#include "tp_validate_rules_internal.h"

#include <stdlib.h>

#include "tp_core/tp_sprite_index.h"
#include "tp_core/tp_srckey.h"
#include "tp_srckey_internal.h"
#include "tp_validate_index_internal.h"
#include "tp_validate_internal.h"

static _Thread_local bool s_fail_sprite_index;

void tp_validate__test_fail_sprite_index(bool fail) {
    s_fail_sprite_index = fail;
}

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
void validate_sprite_animation_domain(validation_builder *fs,
                                      const tp_project *p, int ai,
                                      const tp_project_atlas *a) {
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
}
