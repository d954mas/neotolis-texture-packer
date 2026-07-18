/* Core-owned saved-project validation. Frontends receive an owned typed report
 * and retain only presentation/exit mapping; no validation rule lives in an adapter. */
#include "tp_core/tp_validate.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_pack_constraints_internal.h"
#include "tp_session_internal.h"
#include "tp_core/tp_srckey.h"
#include "tp_validate_internal.h"
#include "tp_validate_index_internal.h"
#include "tp_validate_report_internal.h"
#include "tp_srckey_internal.h"

static finding_context context_atlas(const tp_project_atlas *atlas) {
    finding_context context = {0};
    context.atlas = atlas ? atlas->name : NULL;
    context.atlas_id = atlas ? atlas->id : tp_id128_nil();
    return context;
}

static finding_context context_source(const tp_project_atlas *atlas,
                                      const tp_project_source *source) {
    finding_context context = context_atlas(atlas);
    context.source = source ? source->path : NULL;
    context.source_id = source ? source->id : tp_id128_nil();
    return context;
}

static finding_context context_sprite(const tp_project_atlas *atlas,
                                      tp_id128 source_id,
                                      const char *sprite) {
    finding_context context = context_atlas(atlas);
    context.source_id = source_id;
    context.sprite = sprite;
    return context;
}

static finding_context context_frame(const tp_project_atlas *atlas,
                                     const tp_project_anim *animation,
                                     const tp_project_frame *frame) {
    finding_context context = context_atlas(atlas);
    context.source_id = frame ? frame->source_ref : tp_id128_nil();
    context.anim = animation ? animation->name : NULL;
    context.animation_id = animation ? animation->id : tp_id128_nil();
    context.frame = frame ? frame->name : NULL;
    return context;
}

static finding_context context_animation(const tp_project_atlas *atlas,
                                         const tp_project_anim *animation) {
    finding_context context = context_atlas(atlas);
    context.anim = animation ? animation->name : NULL;
    context.animation_id = animation ? animation->id : tp_id128_nil();
    return context;
}

static finding_context context_target(const tp_project_atlas *atlas,
                                      const tp_project_target *target) {
    finding_context context = context_atlas(atlas);
    context.target = target ? target->exporter_id : NULL;
    context.target_id = target ? target->id : tp_id128_nil();
    return context;
}

enum {
    TARGET_ISSUE_UNKNOWN_EXPORTER = 1U << 0,
    TARGET_ISSUE_NO_OUT_PATH = 1U << 1,
    TARGET_ISSUE_DUPLICATE_OUT_PATH = 1U << 2,
};

static unsigned target_issue_mask(const char *exporter_id, bool enabled,
                                  const char *out_path, bool path_shared) {
    unsigned mask = tp_exporter_find(exporter_id)
                        ? 0U
                        : TARGET_ISSUE_UNKNOWN_EXPORTER;
    if (enabled) {
        if (!out_path || out_path[0] == '\0') {
            mask |= TARGET_ISSUE_NO_OUT_PATH;
        } else if (path_shared) {
            mask |= TARGET_ISSUE_DUPLICATE_OUT_PATH;
        }
    }
    return mask;
}

static _Thread_local bool s_fail_sprite_index;
void tp_validate__test_fail_sprite_index(bool fail) { s_fail_sprite_index = fail; }
void tp_validate__test_work_reset(void) { tp_validate_work_probes = 0U; }
tp_validate_work_stats tp_validate__test_work_get(void) {
    return (tp_validate_work_stats){tp_validate_work_probes};
}

/* Reports duplicated values in `vals[0..n)` once per distinct duplicate. `code`/
 * `severity` select which check; the shared field is `sprite` (the key or final name). */
static void report_duplicates(validation_builder *fs,
                              const tp_project_atlas *atlas,
                              const char *const *vals,
                              const tp_sprite_ref *refs, int n,
                              const str_index *index,
                              tp_validation_severity severity,
                              const char *code, const char *what) {
    for (int i = 0; i < n; i++) {
        const str_slot *slot = str_index_find(index, vals[i]);
        if (slot && slot->first_index == i && slot->count > 1U) {
            const tp_id128 source_id =
                refs ? refs[i].source_id : tp_id128_nil();
            add_finding(fs, severity, code,
                        context_sprite(atlas, source_id, vals[i]),
                        "%zu sprites %s '%s'", slot->count, what, vals[i]);
        }
    }
}

/* One source path's precomputed comparison keys (Fix B: canonicalize + case-fold
 * ONCE per source, not per pair). `canon` is the tp_srckey canonical NFC key; for a
 * source tp_srckey_normalize rejects (a legitimately absolute or '..'-escaping
 * external source) it falls back to a slash-normalized copy so the pairwise compare
 * still runs without aborting or false-positiving. `fold` is the case-fold of canon
 * (or a copy of canon when the fold would not fit -- degrades to an exact compare). */
typedef struct {
    char *canon;
    char *fold;
} src_key;

/* Tolerant fallback canonical: copy `src` into `out` (properly sized: TP_SRCKEY_MAX,
 * never a fixed 1024) replacing '\\' with '/'. Used when tp_srckey_normalize rejects
 * a path so exact-duplicate detection still works on it. */
static char *validate_strdup(const char *src) {
    const size_t len = strlen(src);
    char *copy = (char *)malloc(len + 1U);
    if (copy) {
        memcpy(copy, src, len + 1U);
    }
    return copy;
}

static char *slash_norm_owned(const char *src) {
    char *out = validate_strdup(src);
    if (!out) {
        return NULL;
    }
    for (char *c = out; *c; ++c) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    return out;
}

static void src_keys_free(src_key *keys, int count) {
    if (keys) {
        for (int i = 0; i < count; ++i) {
            free(keys[i].canon);
            free(keys[i].fold);
        }
    }
    free(keys);
}

typedef struct {
    char ***by_atlas;
    int atlas_count;
    str_index counts;
} target_path_index;

static void target_path_index_free(target_path_index *index,
                                   const tp_project *project) {
    if (!index) {
        return;
    }
    if (index->by_atlas && project) {
        for (int ai = 0; ai < index->atlas_count; ++ai) {
            if (index->by_atlas[ai]) {
                for (int ti = 0; ti < project->atlases[ai].target_count; ++ti) {
                    free(index->by_atlas[ai][ti]);
                }
            }
            free(index->by_atlas[ai]);
        }
    }
    free(index->by_atlas);
    str_index_free(&index->counts);
    memset(index, 0, sizeof *index);
}

static bool target_path_index_build(const tp_project *project,
                                    target_path_index *out) {
    memset(out, 0, sizeof *out);
    size_t total = 0U;
    for (int ai = 0; ai < project->atlas_count; ++ai) {
        const int count = project->atlases[ai].target_count;
        if (count > 0 && total > (size_t)INT_MAX - (size_t)count) {
            return false;
        }
        total += (size_t)(count > 0 ? count : 0);
    }
    out->atlas_count = project->atlas_count;
    out->by_atlas = (char ***)calloc((size_t)project->atlas_count,
                                    sizeof *out->by_atlas);
    char **flat = total ? (char **)calloc(total, sizeof *flat) : NULL;
    if ((project->atlas_count > 0 && !out->by_atlas) || (total > 0U && !flat)) {
        free(flat);
        target_path_index_free(out, project);
        return false;
    }
    int flat_count = 0;
    for (int ai = 0; ai < project->atlas_count; ++ai) {
        const tp_project_atlas *atlas = &project->atlases[ai];
        if (atlas->target_count > 0) {
            out->by_atlas[ai] = (char **)calloc((size_t)atlas->target_count,
                                                sizeof *out->by_atlas[ai]);
            if (!out->by_atlas[ai]) {
                free(flat);
                target_path_index_free(out, project);
                return false;
            }
        }
        for (int ti = 0; ti < atlas->target_count; ++ti) {
            const tp_project_target *target = &atlas->targets[ti];
            if (!target->enabled || !target->out_path ||
                target->out_path[0] == '\0') {
                continue;
            }
            char *key = slash_norm_owned(target->out_path);
            if (!key) {
                free(flat);
                target_path_index_free(out, project);
                return false;
            }
            out->by_atlas[ai][ti] = key;
            flat[flat_count++] = key;
        }
    }
    const bool ok = str_index_build(&out->counts,
                                    (const char *const *)flat, flat_count);
    free(flat);
    if (!ok) {
        target_path_index_free(out, project);
    }
    return ok;
}

/* (a2) §5.3/§5.6 source-path validation (all WARNINGS -- never flips the --strict
 * exit): exact duplicate, cross-platform case-fold collision, portability, and a
 * non-portable absolute/escaping source, via the tp_srckey primitives.
 * Each path is canonicalized + case-folded once into owned full-length storage, then
 * grouped by folded key. Work is linear for distinct keys; colliding pairs are emitted
 * only while the bounded report has room and the remainder is counted without walking
 * every pair. This catches './' / '//' / trailing-slash / NFC spellings and never
 * mistakes two distinct long paths with a shared prefix for a duplicate. */
static void validate_sources(validation_builder *fs, const tp_project_atlas *a) {
    int n = a->source_count;
    if (n <= 0) {
        return;
    }
    src_key *k = (src_key *)calloc((size_t)n, sizeof *k);
    if (!k) {
        fs->oom = true;
        return;
    }
    for (int i = 0; i < n; i++) {
        const char *path = a->sources[i].path ? a->sources[i].path : "";
        char normalized[TP_SRCKEY_MAX];
        tp_error err = {0};
        tp_status st = tp_srckey_normalize(path, normalized,
                                           sizeof normalized, &err);
        if (st == TP_STATUS_OK) {
            unsigned flags = TP_SRCKEY_PORT_OK;
            k[i].canon = validate_strdup(normalized);
            if (tp_srckey_portability(normalized, &flags, NULL) == TP_STATUS_OK && flags != TP_SRCKEY_PORT_OK) {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_SOURCE_PORTABILITY,
                            context_source(a, &a->sources[i]),
                            "source '%s' has non-portable path parts:%s%s%s", path,
                            (flags & TP_SRCKEY_PORT_RESERVED_NAME) ? " reserved-name" : "",
                            (flags & TP_SRCKEY_PORT_INVALID_CHAR) ? " invalid-char" : "",
                            (flags & TP_SRCKEY_PORT_TRAILING_DOT_SPACE) ? " trailing-dot-space" : "");
            }
        } else if (st == TP_STATUS_OOM) {
            fs->oom = true;
            break;
        } else {
            /* A source path is project-relative but MAY legitimately be absolute or
             * escape the project dir (a shared art folder): tp_srckey_normalize
             * rejects those. Surface it as a WARNING and fall back to a tolerant
             * slash-normalized key -- never abort validate, never false-positive a
             * duplicate. Invalid-UTF-8 / over-long paths land in the else branch. */
            if (st == TP_STATUS_KEY_ABSOLUTE || st == TP_STATUS_KEY_TRAVERSAL) {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_SOURCE_ESCAPES_ROOT,
                            context_source(a, &a->sources[i]),
                            "source '%s' is absolute or escapes the project directory (not portable across machines)",
                            path);
            } else {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_SOURCE_PORTABILITY,
                            context_source(a, &a->sources[i]),
                            "source '%s' could not be canonicalized (%s)", path, err.msg);
            }
            k[i].canon = slash_norm_owned(path);
        }
        if (!k[i].canon) {
            fs->oom = true;
            break;
        }
        tp_error fold_err = {0};
        st = tp_srckey__casefold_owned(k[i].canon, &k[i].fold, &fold_err);
        if (st == TP_STATUS_OOM) {
            fs->oom = true;
            break;
        }
        if (st != TP_STATUS_OK) {
            /* Invalid UTF-8 was already surfaced as a portability warning. Keep
             * exact full bytes comparable without inventing a lossy fold. */
            k[i].fold = validate_strdup(k[i].canon);
            if (!k[i].fold) {
                fs->oom = true;
                break;
            }
        }
    }
    if (fs->oom) {
        src_keys_free(k, n);
        return;
    }
    /* Group by the case-folded key and retain prior indices in source order. This
     * emits the exact same small-report pair order as the old nested scan. Once
     * the materialization budget is full, the remaining same-fold pairs are
     * counted in O(1) per source and represented by the truncation summary. */
    str_index fold_groups = {0};
    int *next = (int *)malloc((size_t)n * sizeof *next);
    if (!next || !str_index_init(&fold_groups, n)) {
        free(next);
        str_index_free(&fold_groups);
        src_keys_free(k, n);
        fs->oom = true;
        return;
    }
    for (int i = 0; i < n; i++) {
        next[i] = -1;
    }
    for (int i = 0; i < n; i++) {
        str_slot *group = str_index_find(&fold_groups, k[i].fold);
        if (!group) {
            fs->oom = true;
            break;
        }
        const size_t previous = group->key ? group->count : 0U;
        size_t visited = 0U;
        int j = group->key ? group->first_index : -1;
        while (visited < previous && report_has_room(fs)) {
            tp_validate_work_probes++;
            if (strcmp(k[i].canon, k[j].canon) == 0) {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_DUPLICATE_SOURCE,
                            context_source(a, &a->sources[i]),
                            "source '%s' is listed more than once", a->sources[i].path);
            } else {
                add_finding(fs, TP_VALIDATION_WARNING,
                            TP_VALIDATION_CODE_SOURCE_COLLISION,
                            context_source(a, &a->sources[i]),
                            "sources '%s' and '%s' collide case-insensitively (cross-platform name clash)",
                            a->sources[j].path, a->sources[i].path);
            }
            visited++;
            j = next[j];
        }
        if (visited < previous) {
            add_omitted(fs, TP_VALIDATION_WARNING, previous - visited);
        }
        if (!group->key) {
            group->key = k[i].fold;
            group->first_index = i;
        } else {
            next[group->last_index] = i;
        }
        group->last_index = i;
        group->count++;
    }
    free(next);
    str_index_free(&fold_groups);
    src_keys_free(k, n);
}

/* (h) §5.6 sprite-record integrity, over the resolved index `idx`:
 *   MIGRATED records (stored {source,key}) are id-checked:
 *     sprite_bad_source / frame_bad_source            a stored source id absent from the
 *         atlas; a per-sprite override -> orphan [warning] (its source was removed on a
 *         routine edit; reactivates only if its canonical source/key returns,
 *         §5.2/§5.6);
 *         a frame -> [error] (a frame targeting a gone source breaks the animation, and
 *         tp_normalize hard-errors on it -- also flagged as dangling_anim_frame);
 *     orphan_sprite                        [warning] a valid (source,key) that resolves
 *         to no current sprite (stored orphan; reactivates when the key returns);
 *     duplicate_sprite_key                 [warning] two records sharing one (source,key).
 *   PENDING legacy records have no stored {source,key} to id-check, so validation uses
 *   their migration lookup name only to report orphan/ambiguity. Normal pack/apply never
 *   uses that name as an authoritative fallback. */
static void validate_sprite_records(validation_builder *fs,
                                    const tp_project_atlas *a,
                                    const tp_sprite_index *idx) {
    id_index source_ids = {0};
    id_key_index live_sprites = {0};
    id_key_index seen_overrides = {0};
    bool index_ok = id_index_init(&source_ids, a->source_count) &&
                    id_key_index_init(&live_sprites, idx->count) &&
                    id_key_index_init(&seen_overrides, a->sprite_count);
    for (int i = 0; index_ok && i < a->source_count; i++) {
        index_ok = id_index_add(&source_ids, a->sources[i].id);
    }
    for (int i = 0; index_ok && i < idx->count; i++) {
        bool existed = false;
        index_ok = id_key_index_add(&live_sprites, idx->refs[i].source_id, idx->refs[i].source_key, &existed);
    }
    if (!index_ok) {
        fs->oom = true;
        id_index_free(&source_ids);
        id_key_index_free(&live_sprites);
        id_key_index_free(&seen_overrides);
        return;
    }
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *s = &a->sprites[i];
        const tp_status key_status =
            tp_srckey_validate_canonical(s->src_key, NULL);
        if (key_status != TP_STATUS_OK) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_INVALID_SPRITE_KEY,
                        context_sprite(a, s->source_ref, s->name),
                        "sprite override '%s' has invalid canonical key '%s' (%s)",
                        s->name ? s->name : "", s->src_key,
                        tp_status_id(key_status));
            continue;
        }
        if (!id_index_contains(&source_ids, s->source_ref)) {
            /* The owning source was removed from the atlas (a routine edit). This is an
             * ORPHAN, not a hard error: the override applies to nothing now and reactivates
             * only if its canonical source/key returns (§5.2/§5.6). WARNING, so a plain
             * source-removal does not flip `validate --strict` (exit 7). */
            add_finding(fs, TP_VALIDATION_WARNING,
                        TP_VALIDATION_CODE_SPRITE_BAD_SOURCE,
                        context_sprite(a, s->source_ref, s->name),
                        "sprite override '%s' references a source id not in this atlas "
                        "(source removed; orphaned, reactivates if the canonical source/key returns)",
                        s->name ? s->name : "");
            continue;
        }
        if (!id_key_index_contains(&live_sprites, s->source_ref, s->src_key)) {
            add_finding(fs, TP_VALIDATION_WARNING,
                        TP_VALIDATION_CODE_ORPHAN_SPRITE,
                        context_sprite(a, s->source_ref, s->name),
                        "sprite override '%s' (key '%s') resolves to no current sprite "
                        "(orphaned; reactivates if the source key returns)",
                        s->name ? s->name : "", s->src_key);
        }
    }
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *si = &a->sprites[i];
        if (tp_srckey_validate_canonical(si->src_key, NULL) !=
            TP_STATUS_OK) {
            continue;
        }
        bool duplicate = false;
        if (!id_key_index_add(&seen_overrides, si->source_ref, si->src_key, &duplicate)) {
            fs->oom = true;
            break;
        }
        if (duplicate) {
            add_finding(fs, TP_VALIDATION_WARNING,
                        TP_VALIDATION_CODE_DUPLICATE_SPRITE_KEY,
                        context_sprite(a, si->source_ref, si->src_key),
                        "two sprite overrides share the same (source, key) '%s'", si->src_key);
        }
    }
    for (int an = 0; an < a->animation_count; an++) {
        const tp_project_anim *pa = &a->animations[an];
        if (!tp_project_anim_fps_valid(pa->fps)) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        context_animation(a, pa),
                        "animation '%s' fps must be positive and finite",
                        pa->name ? pa->name : "");
        }
        if (!tp_project_anim_playback_valid(pa->playback)) {
            add_finding(
                fs, TP_VALIDATION_ERROR,
                TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                context_animation(a, pa),
                "animation '%s' playback = %d is out of range [%d..%d]",
                pa->name ? pa->name : "", pa->playback,
                TP_PROJECT_ANIM_PLAYBACK_MIN,
                TP_PROJECT_ANIM_PLAYBACK_MAX);
        }
        for (int f = 0; f < pa->frame_count; f++) {
            const tp_project_frame *fr = &pa->frames[f];
            const tp_status key_status =
                tp_srckey_validate_canonical(fr->src_key, NULL);
            if (key_status != TP_STATUS_OK) {
                add_finding(fs, TP_VALIDATION_ERROR,
                            TP_VALIDATION_CODE_INVALID_FRAME_KEY,
                            context_frame(a, pa, fr),
                            "animation '%s' frame '%s' has invalid canonical key '%s' (%s)",
                            pa->name ? pa->name : "",
                            fr->name ? fr->name : "", fr->src_key,
                            tp_status_id(key_status));
                continue;
            }
            if (!id_index_contains(&source_ids, fr->source_ref)) {
                add_finding(fs, TP_VALIDATION_ERROR,
                            TP_VALIDATION_CODE_FRAME_BAD_SOURCE,
                            context_frame(a, pa, fr),
                            "animation '%s' frame '%s' references a source id not in this atlas",
                            pa->name ? pa->name : "", fr->name ? fr->name : "");
            }
        }
    }
    id_index_free(&source_ids);
    id_key_index_free(&live_sprites);
    id_key_index_free(&seen_overrides);
}

static void validate_atlas(validation_builder *fs, const tp_project *p, int ai,
                           const target_path_index *target_paths) {
    const tp_project_atlas *a = &p->atlases[ai];

    /* (a) missing sources -- walk per-source so the finding names the offender. */
    for (int s = 0; s < a->source_count; s++) {
        const tp_project_source *source = &a->sources[s];
        const char *sp = source->path;
        char abs[TP_IDENTITY_PATH_MAX];
        if (tp_project_resolve_source_path(p, sp, abs, sizeof abs) != TP_STATUS_OK) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_MISSING_SOURCE,
                        context_source(a, source),
                        "source '%s' cannot be resolved to an absolute path", sp);
        } else if (!tp_scan_exists(abs)) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_MISSING_SOURCE,
                        context_source(a, source),
                        "source '%s' does not exist on disk", sp);
        }
    }
    validate_sources(fs, a); /* (a2) duplicate / case-fold collision / portability */

    /* Build ONE resolved sprite index (single disk scan) and feed BOTH the export-key /
     * dangling-frame checks AND the §5.6 record checks from it. The index mirrors
     * tp_pack_input_build's iteration EXACTLY (same sources, same order, same raw names), so
     * ref[i].export_key equals the export key of pack desc[i] -- the validate output is
     * identical to the old two-scan version, at one scan instead of two. */
    tp_sprite_index sidx = {0};
    tp_error ierr = {0};
    const tp_status index_status = s_fail_sprite_index
                                       ? TP_STATUS_OOM
                                       : tp_sprite_index_build(p, ai, &sidx, &ierr);
    if (index_status == TP_STATUS_OOM) {
        fs->oom = true;
    } else if (index_status != TP_STATUS_OK) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_INPUT_BUILD_FAILED,
                    context_atlas(a), "%s", ierr.msg);
    } else {
        int n = sidx.count;
        if (n == 0) {
            /* (b) an atlas that resolves no sprites packs nothing. */
            add_finding(fs, TP_VALIDATION_WARNING,
                        TP_VALIDATION_CODE_EMPTY_ATLAS, context_atlas(a),
                        "atlas has no usable sprites (no images resolved from its sources)");
        }
        /* keys[]/finals[] BORROW the index's export keys (and any project rename string) --
         * no per-desc allocation. finals default to the key; a project rename replaces the
         * FIRST desc whose key matches, exactly as build_norm_opts / export does. */
        const char **keys = NULL;
        const char **finals = NULL;
        str_index key_index = {0};
        str_index final_index = {0};
        bool alloc_ok = true;
        if (n > 0) {
            keys = (const char **)calloc((size_t)n, sizeof *keys);
            finals = (const char **)calloc((size_t)n, sizeof *finals);
            alloc_ok = keys && finals;
            for (int i = 0; alloc_ok && i < n; i++) {
                keys[i] = sidx.refs[i].export_key;
                finals[i] = sidx.refs[i].export_key;
            }
            alloc_ok = str_index_build(&key_index, keys, n);
            for (int si = 0; alloc_ok && si < a->sprite_count; si++) {
                const tp_project_sprite *ps = &a->sprites[si];
                if (!ps->rename || ps->rename[0] == '\0') {
                    continue;
                }
                const str_slot *match = str_index_find(&key_index, ps->name);
                if (match && match->key) {
                    finals[match->first_index] = ps->rename;
                }
            }
            if (alloc_ok) {
                alloc_ok = str_index_build(&final_index, finals, n);
            }
        }
        if (!alloc_ok) {
            fs->oom = true;
        } else {
            /* (d) two descs -> one export key: per-sprite overrides become ambiguous. */
            report_duplicates(fs, a, keys, sidx.refs, n, &key_index,
                              TP_VALIDATION_WARNING,
                              TP_VALIDATION_CODE_DUPLICATE_EXPORT_KEY,
                              "map to export key");
            /* (e) two sprites -> one final name: tp_normalize would hard-error. */
            report_duplicates(fs, a, finals, sidx.refs, n, &final_index,
                              TP_VALIDATION_ERROR,
                              TP_VALIDATION_CODE_EXPORT_NAME_COLLISION,
                              "resolve to export name");
            /* (c) dangling animation frames use their canonical source/key. */
            for (int an = 0; an < a->animation_count; an++) {
                const tp_project_anim *pa = &a->animations[an];
                for (int f = 0; f < pa->frame_count; f++) {
                    const tp_project_frame *frame = &pa->frames[f];
                    const char *fr = frame->name ? frame->name : "";
                    bool found = false;
                    if (tp_srckey_validate_canonical(frame->src_key, NULL) ==
                        TP_STATUS_OK) {
                        found = tp_sprite_index_by_source_key(
                                    &sidx, frame->source_ref,
                                    frame->src_key) != NULL;
                    } else {
                        continue; /* invalid-key finding is emitted by §5.6 below */
                    }
                    if (!found) {
                        add_finding(fs, TP_VALIDATION_ERROR,
                                    TP_VALIDATION_CODE_DANGLING_ANIM_FRAME,
                                    context_frame(a, pa, frame),
                                    "animation '%s' references frame '%s' which matches no canonical sprite",
                                    pa->name, fr);
                    }
                }
            }
            /* (h) §5.6 sprite-record integrity over the SAME resolved index. */
            validate_sprite_records(fs, a, &sidx);
        }
        free((void *)keys);
        free((void *)finals);
        str_index_free(&key_index);
        str_index_free(&final_index);
    }
    tp_sprite_index_free(&sidx);

    /* (f) target integrity.
     *   unknown_exporter   [error]   exporter id tp_exporter_find cannot resolve -- a broken id is bad
     *                                 data regardless of enable state, so reported for EVERY target.
     *   target_no_out_path [error]   an ENABLED target with an empty/NULL out_path can produce no file.
     *   duplicate_out_path [warning] an ENABLED target whose out_path is ALSO another ENABLED target's
     *                                 (they overwrite each other). Project-wide (cross-atlas), slash-
     *                                 normalized, via the shared core detector; the message names the path.
     * The out_path checks gate on `enabled`: only enabled targets export, so a DISABLED (parked) target's
     * empty/duplicate out_path is harmless and must NOT flip `validate --strict` to an error. */
    for (int t = 0; t < a->target_count; t++) {
        const tp_project_target *tg = &a->targets[t];
        const char *path_key = tg->enabled && tg->out_path && tg->out_path[0]
                                   ? target_paths->by_atlas[ai][t]
                                   : NULL;
        const str_slot *path_group = path_key
                                         ? str_index_find(&target_paths->counts,
                                                          path_key)
                                         : NULL;
        const unsigned issues = target_issue_mask(
            tg->exporter_id, tg->enabled, tg->out_path,
            path_group && path_group->count > 1U);
        if ((issues & TARGET_ISSUE_UNKNOWN_EXPORTER) != 0U) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_UNKNOWN_EXPORTER,
                        context_target(a, tg),
                        "target references unknown exporter '%s'", tg->exporter_id);
        }
        if ((issues & TARGET_ISSUE_NO_OUT_PATH) != 0U) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_TARGET_NO_OUT_PATH,
                        context_target(a, tg),
                        "target has no output path -- it cannot produce a file");
        }
        if ((issues & TARGET_ISSUE_DUPLICATE_OUT_PATH) != 0U) {
            add_finding(fs, TP_VALIDATION_WARNING,
                        TP_VALIDATION_CODE_DUPLICATE_OUT_PATH,
                        context_target(a, tg),
                        "two or more targets export to '%s' (they overwrite each other)", tg->out_path);
        }
    }

    /* (g) Pack constraints over the raw project model.  Do this before the
     * project->export settings bridge intentionally clamps non-RECT extrude;
     * validation must diagnose persisted input, not its adapted projection. */
    const tp_pack_atlas_constraint_input atlas_input = {
        .max_size = a->max_size,
        .padding = a->padding,
        .margin = a->margin,
        .extrude = a->extrude,
        .alpha_threshold = a->alpha_threshold,
        .max_vertices = a->max_vertices,
        .shape = a->shape,
        .pixels_per_unit = a->pixels_per_unit,
    };
    const tp_pack_atlas_constraint_facts atlas_facts =
        tp_pack_atlas_constraint_facts_of(&atlas_input);
    if (atlas_facts.max_size_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "max_size = %d is out of range [1..%d]", a->max_size,
                    TP_PACK_MAX_PAGE_DIM);
    }
    const struct {
        const char *name;
        int value;
        bool negative;
        bool exceeds_max_size;
    } spacing[] = {
        {"padding", a->padding, atlas_facts.padding_negative,
         atlas_facts.padding_exceeds_max_size},
        {"margin", a->margin, atlas_facts.margin_negative,
         atlas_facts.margin_exceeds_max_size},
        {"extrude", a->extrude, atlas_facts.extrude_negative,
         atlas_facts.extrude_exceeds_max_size},
    };
    for (size_t i = 0U; i < sizeof spacing / sizeof spacing[0]; ++i) {
        if (spacing[i].negative) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        context_atlas(a), "%s = %d must be >= 0",
                        spacing[i].name, spacing[i].value);
        } else if (!atlas_facts.max_size_out_of_range &&
                   spacing[i].exceeds_max_size) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        context_atlas(a), "%s = %d must be in [0..%d]",
                        spacing[i].name, spacing[i].value, a->max_size);
        }
    }
    if (atlas_facts.alpha_threshold_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "alpha_threshold = %d is out of range [0..%d]",
                    a->alpha_threshold, TP_PACK_ALPHA_MAX);
    }
    if (atlas_facts.max_vertices_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "max_vertices = %d is out of range [1..%d]",
                    a->max_vertices, TP_PACK_MAX_VERTICES);
    }
    if (atlas_facts.shape_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "shape = %d is out of range [%d..%d]", a->shape,
                    TP_PACK_SHAPE_MIN, TP_PACK_SHAPE_MAX);
    }
    if (atlas_facts.pixels_per_unit_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "pixels_per_unit must be positive and finite");
    }
    if (atlas_facts.extrude_requires_rect) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "extrude > 0 requires shape RECT");
    }
    for (int i = 0; i < a->sprite_count; ++i) {
        const tp_project_sprite *sprite = &a->sprites[i];
        const bool slice9 = sprite->slice9_lrtb[0] ||
                            sprite->slice9_lrtb[1] ||
                            sprite->slice9_lrtb[2] ||
                            sprite->slice9_lrtb[3];
        const tp_pack_sprite_constraint_input sprite_input = {
            .atlas_max_size = a->max_size,
            .atlas_shape = a->shape,
            .atlas_extrude = a->extrude,
            .has_slice9 = slice9,
            .has_shape = sprite->ov_shape != TP_PROJECT_OV_INHERIT,
            .shape = sprite->ov_shape,
            .has_allow_rotate =
                sprite->ov_allow_rotate != TP_PROJECT_OV_INHERIT,
            .allow_rotate = sprite->ov_allow_rotate,
            .has_max_vertices =
                sprite->ov_max_vertices != TP_PROJECT_OV_INHERIT,
            .max_vertices = sprite->ov_max_vertices,
            .has_margin = sprite->ov_margin != TP_PROJECT_OV_INHERIT,
            .margin = sprite->ov_margin,
            .has_extrude = sprite->ov_extrude != TP_PROJECT_OV_INHERIT,
            .extrude = sprite->ov_extrude,
        };
        const tp_pack_sprite_constraint_facts sprite_facts =
            tp_pack_sprite_constraint_facts_of(&sprite_input);
        const finding_context context =
            context_sprite(a, sprite->source_ref, sprite->name);
        if (!atlas_facts.max_size_out_of_range &&
            sprite_facts.margin_exceeds_max_size) {
            add_finding(
                fs, TP_VALIDATION_ERROR,
                TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, context,
                "sprite ov_margin = %d must not exceed atlas max_size %d",
                sprite->ov_margin, a->max_size);
        }
        if (!atlas_facts.max_size_out_of_range &&
            sprite_facts.extrude_exceeds_max_size) {
            add_finding(
                fs, TP_VALIDATION_ERROR,
                TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, context,
                "sprite ov_extrude = %d must not exceed atlas max_size %d",
                sprite->ov_extrude, a->max_size);
        }
        if (sprite_facts.slice9_shape_conflict) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, context,
                        "sprite ov_shape %d conflicts with slice9; slice9 requires RECT",
                        sprite->ov_shape);
        }
        if (sprite_facts.effective_extrude_requires_rect) {
            const int effective_extrude =
                sprite->ov_extrude != TP_PROJECT_OV_INHERIT
                    ? sprite->ov_extrude
                    : a->extrude;
            add_finding(
                fs, TP_VALIDATION_ERROR,
                TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, context,
                "sprite effective extrude %d requires effective shape RECT",
                effective_extrude);
        }
    }
}


static tp_status validate_project(const tp_project *project,
                                  tp_validation_report *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: NULL report");
    }
    memset(out, 0, sizeof *out);
    if (!project) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: NULL project");
    }

    target_path_index target_paths = {0};
    if (!target_path_index_build(project, &target_paths)) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "out of memory indexing validation target paths");
    }

    validation_builder builder = {0};
    for (int atlas_index = 0; atlas_index < project->atlas_count; atlas_index++) {
        validate_atlas(&builder, project, atlas_index, &target_paths);
        if (builder.oom) {
            break;
        }
    }
    target_path_index_free(&target_paths, project);

    return validation_builder_finish(&builder, out, err);
}

tp_status tp_validate_session_snapshot(const tp_session_snapshot *snapshot,
                                       tp_validation_report *out, tp_error *err) {
    if (!snapshot) {
        if (out) {
            memset(out, 0, sizeof *out);
        }
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: NULL snapshot");
    }
    return validate_project(tp_session_snapshot_project_internal(snapshot), out, err);
}

static void target_issue_add(tp_target_validation_report *out,
                             tp_validation_severity severity,
                             const char *code) {
    if (out->issue_count >= TP_TARGET_VALIDATION_MAX_ISSUES) {
        return;
    }
    tp_target_validation_issue *issue = &out->issues[out->issue_count++];
    issue->severity = severity;
    (void)snprintf(issue->code, sizeof issue->code, "%s", code);
}

tp_status tp_validate_session_snapshot_target(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 target_id, tp_target_validation_report *out, tp_error *err) {
    if (!snapshot || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "target validation needs snapshot and output");
    }
    memset(out, 0, sizeof *out);
    const tp_snapshot_target *target = tp_session_snapshot_target_by_id(
        snapshot, atlas_id, target_id);
    if (!target) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "validation target was not found");
    }
    const bool shared = target->enabled && target->out_path &&
                        target->out_path[0] != '\0' &&
                        tp_session_snapshot_target_out_path_shared(
                            snapshot, atlas_id, target_id, target->out_path);
    const unsigned issues = target_issue_mask(
        target->exporter_id, target->enabled, target->out_path, shared);
    if ((issues & TARGET_ISSUE_UNKNOWN_EXPORTER) != 0U) {
        target_issue_add(out, TP_VALIDATION_ERROR,
                         TP_VALIDATION_CODE_UNKNOWN_EXPORTER);
    }
    if ((issues & TARGET_ISSUE_NO_OUT_PATH) != 0U) {
        target_issue_add(out, TP_VALIDATION_ERROR,
                         TP_VALIDATION_CODE_TARGET_NO_OUT_PATH);
    }
    if ((issues & TARGET_ISSUE_DUPLICATE_OUT_PATH) != 0U) {
        target_issue_add(out, TP_VALIDATION_WARNING,
                         TP_VALIDATION_CODE_DUPLICATE_OUT_PATH);
    }
    return TP_STATUS_OK;
}

tp_status tp_validate_project_file(const char *path, tp_validation_report *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: NULL report");
    }
    memset(out, 0, sizeof *out);
    if (!path || path[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: empty project path");
    }
    tp_project *project = NULL;
    tp_status status = tp_project_load(path, &project, err);
    if (status == TP_STATUS_OK) {
        status = validate_project(project, out, err);
    }
    tp_project_destroy(project);
    return status;
}
