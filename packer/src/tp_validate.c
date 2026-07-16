/* Core-owned saved-project validation. Frontends receive an owned typed report
 * and retain only presentation/exit mapping; no validation rule lives in an adapter. */
#include "tp_core/tp_validate.h"

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_session_internal.h"
#include "tp_core/tp_srckey.h"
#include "tp_validate_internal.h"
#include "tp_srckey_internal.h"
#include "hash/nt_hash.h"

typedef struct {
    tp_validation_finding *v;
    size_t n;
    size_t cap;
    size_t total;
    size_t errors;
    size_t warnings;
    size_t omitted_errors;
    size_t omitted_warnings;
    bool oom;
} validation_builder;

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

static _Thread_local int s_alloc_fail = -1;
static _Thread_local bool s_fail_sprite_index;
static _Thread_local tp_validate_work_stats s_work;
void tp_validate__test_set_alloc_fail(int nth) { s_alloc_fail = nth; }
void tp_validate__test_fail_sprite_index(bool fail) { s_fail_sprite_index = fail; }
void tp_validate__test_work_reset(void) { memset(&s_work, 0, sizeof s_work); }
tp_validate_work_stats tp_validate__test_work_get(void) { return s_work; }

static size_t report_slot_limit(void) {
    size_t by_bytes = (size_t)TP_VALIDATION_REPORT_MAX_BYTES / sizeof(tp_validation_finding);
    return by_bytes < (size_t)TP_VALIDATION_REPORT_MAX_FINDINGS ? by_bytes
                                                                : (size_t)TP_VALIDATION_REPORT_MAX_FINDINGS;
}

static size_t report_ordinary_limit(void) {
    const size_t limit = report_slot_limit();
    return limit > 0U ? limit - 1U : 0U;
}

static bool report_has_room(const validation_builder *fs) {
    return fs->n < report_ordinary_limit();
}

static void add_omitted(validation_builder *fs, tp_validation_severity severity, size_t count) {
    fs->total += count;
    if (severity == TP_VALIDATION_ERROR) {
        fs->errors += count;
        fs->omitted_errors += count;
    } else {
        fs->warnings += count;
        fs->omitted_warnings += count;
    }
}

static tp_validation_finding *findings_new(validation_builder *fs) {
    if (fs->oom) {
        return NULL;
    }
    if (fs->n == fs->cap) {
        const size_t limit = report_slot_limit();
        size_t nc = fs->cap ? fs->cap * 2U : 16U;
        if (nc > limit) {
            nc = limit;
        }
        if (nc <= fs->cap) {
            return NULL;
        }
        if (s_alloc_fail == 0) {
            s_alloc_fail = -1;
            fs->oom = true;
            return NULL;
        }
        if (s_alloc_fail > 0) {
            s_alloc_fail--;
        }
        tp_validation_finding *nv = (tp_validation_finding *)realloc(fs->v, nc * sizeof *nv);
        if (!nv) {
            fs->oom = true;
            return NULL;
        }
        fs->v = nv;
        fs->cap = nc;
    }
    tp_validation_finding *f = &fs->v[fs->n++];
    memset(f, 0, sizeof *f);
    return f;
}

static void set_field(char *dst, size_t cap, const char *v) {
    if (v) {
        (void)snprintf(dst, cap, "%s", v);
    }
}

/* Appends one finding. NULL context fields are omitted. Never aborts: on OOM the
 * findings list poisons and the caller reports it as an internal error. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 9, 10)))
#endif
static void
add_finding(validation_builder *fs, tp_validation_severity severity, const char *code, const char *atlas, const char *sprite, const char *anim,
            const char *frame, const char *target, const char *fmt, ...) {
    fs->total++;
    if (severity == TP_VALIDATION_ERROR) {
        fs->errors++;
    } else {
        fs->warnings++;
    }
    if (!report_has_room(fs)) {
        if (severity == TP_VALIDATION_ERROR) {
            fs->omitted_errors++;
        } else {
            fs->omitted_warnings++;
        }
        return;
    }
    tp_validation_finding *f = findings_new(fs);
    if (!f) {
        return;
    }
    f->severity = severity;
    (void)snprintf(f->code, sizeof f->code, "%s", code);
    set_field(f->atlas, sizeof f->atlas, atlas);
    set_field(f->sprite, sizeof f->sprite, sprite);
    set_field(f->anim, sizeof f->anim, anim);
    set_field(f->frame, sizeof f->frame, frame);
    set_field(f->target, sizeof f->target, target);
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(f->message, sizeof f->message, fmt, ap);
    va_end(ap);
}

static void add_truncation_summary(validation_builder *fs) {
    const size_t omitted = fs->omitted_errors + fs->omitted_warnings;
    if (omitted == 0U) {
        return;
    }
    tp_validation_finding *f = findings_new(fs);
    if (!f) {
        fs->oom = true;
        return;
    }
    f->severity = fs->omitted_errors > 0U ? TP_VALIDATION_ERROR : TP_VALIDATION_WARNING;
    (void)snprintf(f->code, sizeof f->code, "%s", TP_VALIDATION_CODE_TRUNCATED);
    (void)snprintf(f->message, sizeof f->message,
                   "validation report truncated: omitted %zu of %zu findings (%zu errors, %zu warnings); "
                   "limits are %u findings and %u bytes",
                   omitted, fs->total, fs->omitted_errors, fs->omitted_warnings,
                   (unsigned)TP_VALIDATION_REPORT_MAX_FINDINGS, (unsigned)TP_VALIDATION_REPORT_MAX_BYTES);
}

/* Validation-local borrowed-string index. Slots own no strings and live only for
 * one atlas validation; open addressing keeps the lookup owner here rather than
 * adding a generic map or changing tp_sprite_index's public contract. */
typedef struct {
    const char *key;
    size_t count;
    int first_index;
    int last_index;
} str_slot;

typedef struct {
    str_slot *slots;
    size_t cap;
} str_index;

static bool str_index_init(str_index *index, int expected) {
    memset(index, 0, sizeof *index);
    if (expected <= 0) {
        return true;
    }
    size_t cap = 16U;
    const size_t need = (size_t)expected * 2U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            return false;
        }
        cap *= 2U;
    }
    index->slots = (str_slot *)calloc(cap, sizeof *index->slots);
    if (!index->slots) {
        return false;
    }
    index->cap = cap;
    return true;
}

static void str_index_free(str_index *index) {
    free(index->slots);
    memset(index, 0, sizeof *index);
}

static str_slot *str_index_find(const str_index *index, const char *key) {
    if (!index || index->cap == 0U || !key) {
        return NULL;
    }
    const size_t start = (size_t)nt_hash64_str(key).value & (index->cap - 1U);
    for (size_t i = 0; i < index->cap; i++) {
        s_work.probes++;
        str_slot *slot = &index->slots[(start + i) & (index->cap - 1U)];
        if (!slot->key || strcmp(slot->key, key) == 0) {
            return slot;
        }
    }
    return NULL;
}

static bool str_index_add(str_index *index, const char *key, int value_index) {
    str_slot *slot = str_index_find(index, key);
    if (!slot) {
        return false;
    }
    if (!slot->key) {
        slot->key = key;
        slot->first_index = value_index;
    }
    slot->last_index = value_index;
    slot->count++;
    return true;
}

static bool str_index_build(str_index *index, const char *const *values, int count) {
    if (!str_index_init(index, count)) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (!str_index_add(index, values[i], i)) {
            str_index_free(index);
            return false;
        }
    }
    return true;
}

typedef struct {
    tp_id128 id;
    bool occupied;
} id_slot;

typedef struct {
    id_slot *slots;
    size_t cap;
} id_index;

static bool id_index_init(id_index *index, int expected) {
    memset(index, 0, sizeof *index);
    if (expected <= 0) {
        return true;
    }
    size_t cap = 16U;
    const size_t need = (size_t)expected * 2U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            return false;
        }
        cap *= 2U;
    }
    index->slots = (id_slot *)calloc(cap, sizeof *index->slots);
    if (!index->slots) {
        return false;
    }
    index->cap = cap;
    return true;
}

static id_slot *id_index_find(const id_index *index, tp_id128 id) {
    if (!index || index->cap == 0U) {
        return NULL;
    }
    const size_t start = (size_t)tp_id128_bucket(id) & (index->cap - 1U);
    for (size_t i = 0; i < index->cap; i++) {
        s_work.probes++;
        id_slot *slot = &index->slots[(start + i) & (index->cap - 1U)];
        if (!slot->occupied || tp_id128_eq(slot->id, id)) {
            return slot;
        }
    }
    return NULL;
}

static bool id_index_add(id_index *index, tp_id128 id) {
    id_slot *slot = id_index_find(index, id);
    if (!slot) {
        return false;
    }
    slot->id = id;
    slot->occupied = true;
    return true;
}

static bool id_index_contains(const id_index *index, tp_id128 id) {
    const id_slot *slot = id_index_find(index, id);
    return slot && slot->occupied;
}

static void id_index_free(id_index *index) {
    free(index->slots);
    memset(index, 0, sizeof *index);
}

typedef struct {
    tp_id128 id;
    const char *key;
} id_key_slot;

typedef struct {
    id_key_slot *slots;
    size_t cap;
} id_key_index;

static bool id_key_index_init(id_key_index *index, int expected) {
    memset(index, 0, sizeof *index);
    if (expected <= 0) {
        return true;
    }
    size_t cap = 16U;
    const size_t need = (size_t)expected * 2U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            return false;
        }
        cap *= 2U;
    }
    index->slots = (id_key_slot *)calloc(cap, sizeof *index->slots);
    if (!index->slots) {
        return false;
    }
    index->cap = cap;
    return true;
}

static id_key_slot *id_key_index_find(const id_key_index *index, tp_id128 id, const char *key) {
    if (!index || index->cap == 0U || !key) {
        return NULL;
    }
    const uint64_t hash = tp_id128_bucket(id) ^ (nt_hash64_str(key).value * UINT64_C(0x9e3779b97f4a7c15));
    const size_t start = (size_t)hash & (index->cap - 1U);
    for (size_t i = 0; i < index->cap; i++) {
        s_work.probes++;
        id_key_slot *slot = &index->slots[(start + i) & (index->cap - 1U)];
        if (!slot->key || (tp_id128_eq(slot->id, id) && strcmp(slot->key, key) == 0)) {
            return slot;
        }
    }
    return NULL;
}

static bool id_key_index_add(id_key_index *index, tp_id128 id, const char *key, bool *already_present) {
    id_key_slot *slot = id_key_index_find(index, id, key);
    if (!slot) {
        return false;
    }
    *already_present = slot->key != NULL;
    if (!slot->key) {
        slot->id = id;
        slot->key = key;
    }
    return true;
}

static bool id_key_index_contains(const id_key_index *index, tp_id128 id, const char *key) {
    const id_key_slot *slot = id_key_index_find(index, id, key);
    return slot && slot->key;
}

static void id_key_index_free(id_key_index *index) {
    free(index->slots);
    memset(index, 0, sizeof *index);
}

/* Reports duplicated values in `vals[0..n)` once per distinct duplicate. `code`/
 * `severity` select which check; the shared field is `sprite` (the key or final name). */
static void report_duplicates(validation_builder *fs, const char *atlas, const char *const *vals, int n,
                              const str_index *index, tp_validation_severity severity,
                              const char *code, const char *what) {
    for (int i = 0; i < n; i++) {
        const str_slot *slot = str_index_find(index, vals[i]);
        if (slot && slot->first_index == i && slot->count > 1U) {
            add_finding(fs, severity, code, atlas, vals[i], NULL, NULL, NULL, "%zu sprites %s '%s'", slot->count, what,
                        vals[i]);
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
 * non-portable absolute/escaping source, via the promoted tp_srckey primitives.
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
                add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_SOURCE_PORTABILITY, a->name, NULL, NULL, NULL, NULL,
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
                add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_SOURCE_ESCAPES_ROOT, a->name, NULL, NULL, NULL, NULL,
                            "source '%s' is absolute or escapes the project directory (not portable across machines)",
                            path);
            } else {
                add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_SOURCE_PORTABILITY, a->name, NULL, NULL, NULL, NULL,
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
            s_work.probes++;
            if (strcmp(k[i].canon, k[j].canon) == 0) {
                add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_DUPLICATE_SOURCE, a->name, NULL, NULL, NULL, NULL,
                            "source '%s' is listed more than once", a->sources[i].path);
            } else {
                add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_SOURCE_COLLISION, a->name, NULL, NULL, NULL, NULL,
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

static tp_status stored_source_key_status(const char *key) {
    if (!key || !key[0]) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    char normalized[TP_SRCKEY_MAX];
    const tp_status status = tp_srckey_normalize(
        key, normalized, sizeof normalized, NULL);
    if (status != TP_STATUS_OK) {
        return status;
    }
    return strcmp(key, normalized) == 0 ? TP_STATUS_OK
                                        : TP_STATUS_INVALID_ARGUMENT;
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
static void validate_sprite_records(validation_builder *fs, const tp_project_atlas *a, const tp_sprite_index *idx,
                                    const str_index *export_keys) {
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
        if (tp_id128_is_nil(s->source_ref) || !s->src_key) {
            /* PENDING legacy input: the name is migration metadata, not an apply key.
             * Check whether migration can currently reactivate it. */
            int matches = 0;
            if (s->name) {
                const str_slot *slot = str_index_find(export_keys, s->name);
                matches = slot && slot->key ? (int)slot->count : 0;
            }
            if (matches == 0) {
                add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_ORPHAN_SPRITE, a->name, s->name, NULL, NULL, NULL,
                            "sprite override '%s' matches no current sprite "
                            "(orphaned by name; not yet id-validated, re-keys to {source, key} in F2)",
                            s->name ? s->name : "");
            }
            continue;
        }
        const tp_status key_status = stored_source_key_status(s->src_key);
        if (key_status != TP_STATUS_OK) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_INVALID_SPRITE_KEY, a->name,
                        s->name, NULL, NULL, NULL,
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
            add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_SPRITE_BAD_SOURCE, a->name, s->name, NULL, NULL, NULL,
                        "sprite override '%s' references a source id not in this atlas "
                        "(source removed; orphaned, reactivates if the canonical source/key returns)",
                        s->name ? s->name : "");
            continue;
        }
        if (!id_key_index_contains(&live_sprites, s->source_ref, s->src_key)) {
            add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_ORPHAN_SPRITE, a->name, s->name, NULL, NULL, NULL,
                        "sprite override '%s' (key '%s') resolves to no current sprite "
                        "(orphaned; reactivates if the source key returns)",
                        s->name ? s->name : "", s->src_key);
        }
    }
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *si = &a->sprites[i];
        if (tp_id128_is_nil(si->source_ref) || !si->src_key ||
            stored_source_key_status(si->src_key) != TP_STATUS_OK) {
            continue;
        }
        bool duplicate = false;
        if (!id_key_index_add(&seen_overrides, si->source_ref, si->src_key, &duplicate)) {
            fs->oom = true;
            break;
        }
        if (duplicate) {
            add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_DUPLICATE_SPRITE_KEY, a->name, si->src_key, NULL, NULL, NULL,
                        "two sprite overrides share the same (source, key) '%s'", si->src_key);
        }
    }
    for (int an = 0; an < a->animation_count; an++) {
        const tp_project_anim *pa = &a->animations[an];
        for (int f = 0; f < pa->frame_count; f++) {
            const tp_project_frame *fr = &pa->frames[f];
            if (tp_id128_is_nil(fr->source_ref) || !fr->src_key) {
                continue;
            }
            const tp_status key_status = stored_source_key_status(fr->src_key);
            if (key_status != TP_STATUS_OK) {
                add_finding(fs, TP_VALIDATION_ERROR,
                            TP_VALIDATION_CODE_INVALID_FRAME_KEY, a->name,
                            NULL, pa->name, fr->name, NULL,
                            "animation '%s' frame '%s' has invalid canonical key '%s' (%s)",
                            pa->name ? pa->name : "",
                            fr->name ? fr->name : "", fr->src_key,
                            tp_status_id(key_status));
                continue;
            }
            if (!id_index_contains(&source_ids, fr->source_ref)) {
                add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_FRAME_BAD_SOURCE, a->name, NULL, pa->name, fr->name, NULL,
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
        const char *sp = a->sources[s].path;
        char abs[512];
        if (tp_project_resolve_source_path(p, sp, abs, sizeof abs) != TP_STATUS_OK) {
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_MISSING_SOURCE, a->name, NULL, NULL, NULL, NULL,
                        "source '%s' cannot be resolved to an absolute path", sp);
        } else if (!tp_scan_exists(abs)) {
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_MISSING_SOURCE, a->name, NULL, NULL, NULL, NULL,
                        "source '%s' does not exist on disk", sp);
        }
    }
    validate_sources(fs, a); /* (a2) duplicate / case-fold collision / portability */

    /* Build ONE resolved sprite index (single disk scan) and feed BOTH the export-key /
     * dangling-frame checks AND the §5.6 record checks from it (fix [7]). The index mirrors
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
        add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_INPUT_BUILD_FAILED, a->name, NULL, NULL, NULL, NULL, "%s", ierr.msg);
    } else {
        int n = sidx.count;
        if (n == 0) {
            /* (b) an atlas that resolves no sprites packs nothing. */
            add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_EMPTY_ATLAS, a->name, NULL, NULL, NULL, NULL,
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
            report_duplicates(fs, a->name, keys, n, &key_index, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_DUPLICATE_EXPORT_KEY,
                              "map to export key");
            /* (e) two sprites -> one final name: tp_normalize would hard-error. */
            report_duplicates(fs, a->name, finals, n, &final_index, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_EXPORT_NAME_COLLISION,
                              "resolve to export name");
            /* (c) dangling anim frames: canonical refs match the exact source/key
             * pair used by export. Only pending legacy frames consult the migration
             * name, and only a unique match is live. */
            for (int an = 0; an < a->animation_count; an++) {
                const tp_project_anim *pa = &a->animations[an];
                for (int f = 0; f < pa->frame_count; f++) {
                    const tp_project_frame *frame = &pa->frames[f];
                    const char *fr = frame->name ? frame->name : "";
                    bool found = false;
                    int legacy_matches = 0;
                    if (!tp_id128_is_nil(frame->source_ref) && frame->src_key &&
                        stored_source_key_status(frame->src_key) == TP_STATUS_OK) {
                        found = tp_sprite_index_by_source_key(
                                    &sidx, frame->source_ref,
                                    frame->src_key) != NULL;
                    } else if (!tp_id128_is_nil(frame->source_ref) &&
                               frame->src_key) {
                        continue; /* invalid-key finding is emitted by §5.6 below */
                    } else {
                        found = tp_sprite_index_by_export_key(
                                    &sidx, fr, &legacy_matches) != NULL &&
                                legacy_matches == 1;
                    }
                    if (!found) {
                        add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_DANGLING_ANIM_FRAME, a->name, NULL, pa->name, fr, NULL,
                                    legacy_matches > 1
                                        ? "animation '%s' references legacy frame '%s' ambiguously (%d sprites)"
                                        : "animation '%s' references frame '%s' which matches no sprite export key",
                                    pa->name, fr, legacy_matches);
                    }
                }
            }
            /* (h) §5.6 sprite-record integrity over the SAME resolved index. */
            validate_sprite_records(fs, a, &sidx, &key_index);
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
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_UNKNOWN_EXPORTER, a->name, NULL, NULL, NULL, tg->exporter_id,
                        "target references unknown exporter '%s'", tg->exporter_id);
        }
        if ((issues & TARGET_ISSUE_NO_OUT_PATH) != 0U) {
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_TARGET_NO_OUT_PATH, a->name, NULL, NULL, NULL, tg->exporter_id,
                        "target has no output path -- it cannot produce a file");
        }
        if ((issues & TARGET_ISSUE_DUPLICATE_OUT_PATH) != 0U) {
            add_finding(fs, TP_VALIDATION_WARNING, TP_VALIDATION_CODE_DUPLICATE_OUT_PATH, a->name, NULL, NULL, NULL, tg->exporter_id,
                        "two or more targets export to '%s' (they overwrite each other)", tg->out_path);
        }
    }

    /* (g) knob ranges over the export-path settings (clamp applied). */
    tp_pack_settings sset;
    tp_error serr = {0};
    if (tp_project_atlas_to_settings(p, ai, &sset, &serr) == TP_STATUS_OK) {
        if (!tp_pack_max_size_valid(sset.max_size)) {
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        a->name, NULL, NULL, NULL, NULL,
                        "max_size = %d is out of range [1..%d]", sset.max_size,
                        TP_PACK_MAX_PAGE_DIM);
        }
        const struct { const char *name; int value; } nonnegative[] = {
            {"padding", sset.padding}, {"margin", sset.margin},
            {"extrude", sset.extrude}};
        for (size_t i = 0U; i < sizeof nonnegative / sizeof nonnegative[0]; ++i) {
            if (!tp_pack_nonnegative_valid(nonnegative[i].value)) {
                add_finding(fs, TP_VALIDATION_ERROR,
                            TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, a->name,
                            NULL, NULL, NULL, NULL, "%s = %d must be >= 0",
                            nonnegative[i].name, nonnegative[i].value);
            }
        }
        if (!tp_pack_alpha_threshold_valid(sset.alpha_threshold)) {
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        a->name, NULL, NULL, NULL, NULL,
                        "alpha_threshold = %d is out of range [0..%d]",
                        sset.alpha_threshold, TP_PACK_ALPHA_MAX);
        }
        if (!tp_pack_max_vertices_valid(sset.max_vertices)) {
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        a->name, NULL, NULL, NULL, NULL,
                        "max_vertices = %d is out of range [1..%d]",
                        sset.max_vertices, TP_PACK_MAX_VERTICES);
        }
        if (!tp_pack_shape_valid(sset.shape)) {
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        a->name, NULL, NULL, NULL, NULL,
                        "shape = %d is out of range [%d..%d]", sset.shape,
                        TP_PACK_SHAPE_MIN, TP_PACK_SHAPE_MAX);
        }
        if (!tp_pack_pixels_per_unit_valid(sset.pixels_per_unit)) {
            add_finding(fs, TP_VALIDATION_ERROR, TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, a->name, NULL, NULL, NULL, NULL,
                        "pixels_per_unit must be positive and finite");
        }
        if (tp_pack_shape_valid(sset.shape) &&
            tp_pack_nonnegative_valid(sset.extrude) &&
            !tp_pack_extrude_shape_valid(sset.extrude, sset.shape)) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, a->name,
                        NULL, NULL, NULL, NULL,
                        "extrude > 0 requires shape RECT");
        }
    }
}


void tp_validation_report_free(tp_validation_report *report) {
    if (!report) {
        return;
    }
    free(report->findings);
    memset(report, 0, sizeof *report);
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

    add_truncation_summary(&builder);

    if (builder.oom) {
        free(builder.v);
        return tp_error_set(err, TP_STATUS_OOM, "out of memory collecting validation findings");
    }

    out->findings = builder.v;
    out->finding_count = builder.n;
    out->error_count = builder.errors;
    out->warning_count = builder.warnings;
    out->total_finding_count = builder.total;
    out->omitted_finding_count = builder.omitted_errors + builder.omitted_warnings;
    out->truncated = out->omitted_finding_count > 0U;
    return TP_STATUS_OK;
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
