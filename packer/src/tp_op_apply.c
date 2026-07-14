/*
 * F2-01 task 2: ID-only apply of ONE validated operation to the tp_project model.
 * Entities are addressed BY STABLE ID (never array index or mutable name); a human
 * selector is resolved to an id at the request edge (tp_op_build_*), not here.
 *
 * STAGE-THEN-COMMIT: apply validates the whole op first (no mutation), then does
 * its work so that an allocator failure BEFORE the commit point leaves the model
 * BYTE-UNCHANGED. Simple ops delegate to the OOM-safe tp_project mutators (a failed
 * grow/dup returns OOM without incrementing a count). The one genuinely COMPOUND op
 * -- animation.create with initial frames -- builds all its frames in a staging
 * buffer first and only splices them in once every allocation has succeeded, so a
 * mid-build failure never half-populates an animation. tp_op__test_set_alloc_fail
 * drives that staging path in the fault-injection test.
 *
 * F2-01/F2-05 boundary: this engine is CORE-TESTED groundwork. The shipping CLI/GUI
 * mutators are NOT routed through it here -- that cutover is F2-05.
 */

#include "tp_core/tp_operation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_names.h"   /* tp_sprite_export_key (override-record bridge key) */
#include "tp_core/tp_project.h"
#include "tp_core/tp_srckey.h"  /* TP_SRCKEY_MAX */
#include "tp_op_internal.h"
#include "tp_strutil.h"         /* tp_set_owned_dup (shared OOM-safe string-field swap) */

/* ---- staging allocation (with a test-only fault seam) -------------------- */
static int s_alloc_fail = -1; /* countdown; -1 disabled. Fires exactly once. */

void tp_op__test_set_alloc_fail(int nth) { s_alloc_fail = nth; }

static void *stage_alloc(size_t n) {
    if (s_alloc_fail == 0) {
        s_alloc_fail = -1;
        return NULL;
    }
    if (s_alloc_fail > 0) {
        s_alloc_fail--;
    }
    return malloc(n);
}

static char *stage_strdup(const char *s) {
    size_t n = strlen(s) + 1U;
    char *d = (char *)stage_alloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

/* ---- id-addressed lookups (non-const, for mutation) --------------------- */
static tp_project_atlas *atlas_by_id(tp_project *p, tp_id128 id) {
    int ai = tp_project_find_atlas_by_id(p, id);
    return ai < 0 ? NULL : &p->atlases[ai];
}

/* The sprite-override RECORD key (the stored `name` field that the tp_project sprite
 * lookups match on). Two record shapes:
 *   - PENDING (nil source_id): a name-keyed override added BEFORE any source scan --
 *     what the file-oriented CLI `sprite set`/`unset` builds. It stores under the key
 *     the user typed VERBATIM (no dir/ext stripping), byte-identical to the pre-cutover
 *     inline CLI (which did add_sprite/remove_sprite/set_sprite_rename on the raw key).
 *     Verbatim storage is also what lets `sprite unset hero.png` clear a pre-existing
 *     verbatim "hero.png" record instead of keying on the stripped "hero" and silently
 *     missing it. (Pre-cutover parity; the pack/export match of an ext-carrying pending
 *     key is a separate, unchanged question -- see the report / ADR 0014.)
 *   - SOURCE-ATTACHED (real source_id): the src_key is a source-local path, so the
 *     record keys under the export bridge (strip dir+ext) that the pack/export path
 *     (tp_input.c: find_sprite(export_key(scanned_name))) resolves against.
 * tp_sprite_export_key stays the single owner of ext-stripping (boundary R1). */
static void sprite_store_key(tp_id128 source_id, const char *src_key, char *out, size_t cap) {
    if (tp_id128_is_nil(source_id)) {
        (void)snprintf(out, cap, "%s", src_key ? src_key : "");
    } else {
        tp_sprite_export_key(src_key, out, cap);
    }
}

/* ---- override field set / clear ----------------------------------------- */
static void sprite_apply_set(tp_project_sprite *s, const tp_op_sprite_set *o) {
    if (o->mask & TP_SPF_ORIGIN) {
        s->origin_x = o->origin_x;
        s->origin_y = o->origin_y;
    }
    if (o->mask & TP_SPF_SLICE9) {
        for (int k = 0; k < 4; k++) {
            s->slice9_lrtb[k] = o->slice9[k];
        }
    }
    if (o->mask & TP_SPF_SHAPE) {
        s->ov_shape = o->ov_shape;
    }
    if (o->mask & TP_SPF_ALLOW_ROTATE) {
        s->ov_allow_rotate = o->ov_allow_rotate;
    }
    if (o->mask & TP_SPF_MAX_VERTICES) {
        s->ov_max_vertices = o->ov_max_vertices;
    }
    if (o->mask & TP_SPF_MARGIN) {
        s->ov_margin = o->ov_margin;
    }
    if (o->mask & TP_SPF_EXTRUDE) {
        s->ov_extrude = o->ov_extrude;
    }
}

static void sprite_apply_clear(tp_project_sprite *s, uint32_t mask) {
    if (mask & TP_SPF_ORIGIN) {
        s->origin_x = TP_PROJECT_ORIGIN_DEFAULT;
        s->origin_y = TP_PROJECT_ORIGIN_DEFAULT;
    }
    if (mask & TP_SPF_SLICE9) {
        for (int k = 0; k < 4; k++) {
            s->slice9_lrtb[k] = 0;
        }
    }
    if (mask & TP_SPF_SHAPE) {
        s->ov_shape = TP_PROJECT_OV_INHERIT;
    }
    if (mask & TP_SPF_ALLOW_ROTATE) {
        s->ov_allow_rotate = TP_PROJECT_OV_INHERIT;
    }
    if (mask & TP_SPF_MAX_VERTICES) {
        s->ov_max_vertices = TP_PROJECT_OV_INHERIT;
    }
    if (mask & TP_SPF_MARGIN) {
        s->ov_margin = TP_PROJECT_OV_INHERIT;
    }
    if (mask & TP_SPF_EXTRUDE) {
        s->ov_extrude = TP_PROJECT_OV_INHERIT;
    }
}

/* ---- animation.create / .frames.set staging ------------------------------ *
 * Build every frame in a staging array (through the fault seam) BEFORE touching
 * the model. On any allocation failure, free the partial staging and return OOM --
 * the model is never touched. Returns TP_STATUS_OK (out owns *out_frames), else
 * TP_STATUS_OOM. n == 0 yields (NULL, 0), a valid empty frame list. */
static tp_status stage_frames(char *const *names, int n, tp_project_frame **out_frames) {
    *out_frames = NULL;
    if (n <= 0) {
        return TP_STATUS_OK;
    }
    tp_project_frame *fr = (tp_project_frame *)stage_alloc((size_t)n * sizeof *fr);
    if (!fr) {
        return TP_STATUS_OOM;
    }
    memset(fr, 0, (size_t)n * sizeof *fr);
    for (int i = 0; i < n; i++) {
        fr[i].name = stage_strdup(names[i]); /* PENDING frame: keyed by name (matches the CLI) */
        if (!fr[i].name) {
            for (int j = 0; j <= i; j++) {
                free(fr[j].name);
            }
            free(fr);
            return TP_STATUS_OOM;
        }
    }
    *out_frames = fr;
    return TP_STATUS_OK;
}

static tp_status apply_anim_create(tp_project_atlas *a, const tp_op_anim_create *c) {
    tp_project_frame *frames = NULL;
    tp_status st = stage_frames(c->frames, c->frame_count, &frames); /* staging: model untouched */
    if (st != TP_STATUS_OK) {
        return st;
    }
    tp_project_anim *an = NULL;
    st = tp_project_atlas_add_animation(a, c->name, &an); /* model grow + name dup; OOM-safe */
    if (st != TP_STATUS_OK) {
        for (int i = 0; i < c->frame_count; i++) {
            free(frames[i].name);
        }
        free(frames);
        return st;
    }
    /* Commit: no more allocations can fail. */
    an->id = c->anim_id;
    an->id_synthetic = false;
    an->fps = c->fps;
    an->playback = c->playback;
    an->flip_h = c->flip_h;
    an->flip_v = c->flip_v;
    an->frames = frames;
    an->frame_count = c->frame_count;
    an->frame_cap = c->frame_count;
    return TP_STATUS_OK;
}

static tp_status apply_anim_frames_set(tp_project_anim *an, const tp_op_anim_frames_set *c) {
    tp_project_frame *frames = NULL;
    tp_status st = stage_frames(c->frames, c->frame_count, &frames);
    if (st != TP_STATUS_OK) {
        return st; /* old frame list intact */
    }
    for (int i = 0; i < an->frame_count; i++) { /* commit: free old, swap in new */
        free(an->frames[i].name);
        free(an->frames[i].src_key);
    }
    free(an->frames);
    an->frames = frames;
    an->frame_count = c->frame_count;
    an->frame_cap = c->frame_count;
    return TP_STATUS_OK;
}

/* animation.rename: dup the new name then swap in (stage-then-commit at field granularity,
 * so an OOM leaves the old name -- and the whole model -- byte-unchanged). No dedicated
 * tp_project mutator exists; the OOM-safe field swap is tp_set_owned_dup, fed stage_strdup so
 * the fault seam still drives it (exactly like apply_source_replace). */
static tp_status apply_anim_rename(tp_project_atlas *a, const tp_op_anim_rename *o) {
    tp_project_anim *an = tp_project_atlas_find_animation_by_id(a, o->anim_id);
    if (!an) {
        return TP_STATUS_NOT_FOUND;
    }
    return tp_set_owned_dup(&an->name, o->name, stage_strdup);
}

/* source.replace (reserved): repath a source in place. Own dup so a failure leaves
 * the old path intact (stage-then-commit at field granularity, shared with anim.rename). */
static tp_status apply_source_replace(tp_project_atlas *a, const tp_op_source_ref *o) {
    tp_project_source *src = tp_project_atlas_find_source_by_id(a, o->source_id);
    if (!src) {
        return TP_STATUS_NOT_FOUND;
    }
    return tp_set_owned_dup(&src->path, o->key, stage_strdup);
}

tp_status tp_operation_apply(tp_project *p, const tp_operation *op, tp_op_reject *rej) {
    tp_status st = tp_operation_validate(p, op, rej);
    if (st != TP_STATUS_OK) {
        return st; /* rejected: model untouched */
    }
    tp_op__reject_ok(rej);

    switch (op->kind) {
        case TP_OP_ATLAS_CREATE: {
            int idx = -1;
            st = tp_project_add_atlas(p, op->u.atlas_create.name, &idx);
            if (st == TP_STATUS_OK) {
                p->atlases[idx].id = op->atlas_id; /* deterministic: the op owns the id */
                p->atlases[idx].id_synthetic = false;
            }
            break;
        }
        case TP_OP_ATLAS_REMOVE:
            st = tp_project_remove_atlas(p, tp_project_find_atlas_by_id(p, op->atlas_id));
            break;
        case TP_OP_ATLAS_RENAME:
            st = tp_project_set_atlas_name(atlas_by_id(p, op->atlas_id), op->u.atlas_rename.name);
            break;
        case TP_OP_ATLAS_SETTINGS_SET: {
            tp_project_atlas *a = atlas_by_id(p, op->atlas_id);
            const tp_op_atlas_settings *s = &op->u.atlas_settings;
            if (s->mask & TP_AF_MAX_SIZE) {
                a->max_size = s->max_size;
            }
            if (s->mask & TP_AF_PADDING) {
                a->padding = s->padding;
            }
            if (s->mask & TP_AF_MARGIN) {
                a->margin = s->margin;
            }
            if (s->mask & TP_AF_EXTRUDE) {
                a->extrude = s->extrude;
            }
            if (s->mask & TP_AF_ALPHA_THRESHOLD) {
                a->alpha_threshold = s->alpha_threshold;
            }
            if (s->mask & TP_AF_MAX_VERTICES) {
                a->max_vertices = s->max_vertices;
            }
            if (s->mask & TP_AF_SHAPE) {
                a->shape = s->shape;
            }
            if (s->mask & TP_AF_ALLOW_TRANSFORM) {
                a->allow_transform = s->allow_transform;
            }
            if (s->mask & TP_AF_POWER_OF_TWO) {
                a->power_of_two = s->power_of_two;
            }
            if (s->mask & TP_AF_PIXELS_PER_UNIT) {
                a->pixels_per_unit = s->pixels_per_unit;
            }
            st = TP_STATUS_OK;
            break;
        }

        case TP_OP_SOURCE_ADD: {
            tp_project_atlas *a = atlas_by_id(p, op->atlas_id);
            int before = a->source_count;
            st = tp_project_atlas_add_source_kind(a, op->u.source_add.key, op->u.source_add.kind);
            if (st == TP_STATUS_OK) {
                if (a->source_count > before) {
                    a->sources[a->source_count - 1].id = op->u.source_add.source_id;
                    a->sources[a->source_count - 1].id_synthetic = false;
                } else {
                    /* validate rejects a duplicate path, so a dedupe no-op is unreachable here;
                     * refuse to "commit" one -- it would strand the op's source_id (a false id). */
                    st = TP_STATUS_INVALID_ARGUMENT;
                }
            }
            break;
        }
        case TP_OP_SOURCE_REMOVE:
            st = tp_project_atlas_remove_source_by_id(atlas_by_id(p, op->atlas_id), op->u.source_ref.source_id);
            break;
        case TP_OP_SOURCE_REPLACE:
            st = apply_source_replace(atlas_by_id(p, op->atlas_id), &op->u.source_ref);
            break;

        case TP_OP_SPRITE_OVERRIDE_SET: {
            tp_project_atlas *a = atlas_by_id(p, op->atlas_id);
            char bridge[TP_SRCKEY_MAX];
            sprite_store_key(op->u.sprite_set.source_id, op->u.sprite_set.src_key, bridge, sizeof bridge);
            tp_project_sprite *s = NULL;
            st = tp_project_atlas_add_sprite(a, bridge, &s);
            if (st == TP_STATUS_OK) {
                sprite_apply_set(s, &op->u.sprite_set);
                (void)tp_project_atlas_prune_sprite(a, bridge); /* keep storage sparse (matches the CLI) */
            }
            break;
        }
        case TP_OP_SPRITE_OVERRIDE_CLEAR: {
            tp_project_atlas *a = atlas_by_id(p, op->atlas_id);
            char bridge[TP_SRCKEY_MAX];
            sprite_store_key(op->u.sprite_clear.source_id, op->u.sprite_clear.src_key, bridge, sizeof bridge);
            if (op->u.sprite_clear.mask == TP_SPF_ALL) { /* sprite unset: drop the whole record (idempotent) */
                st = tp_project_atlas_remove_sprite(a, bridge);
                if (st == TP_STATUS_OUT_OF_BOUNDS) {
                    st = TP_STATUS_OK; /* absent == already cleared */
                }
            } else {
                tp_project_sprite *s = tp_project_atlas_find_sprite(a, bridge);
                if (s) {
                    sprite_apply_clear(s, op->u.sprite_clear.mask);
                    (void)tp_project_atlas_prune_sprite(a, bridge);
                }
                st = TP_STATUS_OK;
            }
            break;
        }
        case TP_OP_SPRITE_NAME_SET: {
            tp_project_atlas *a = atlas_by_id(p, op->atlas_id);
            char bridge[TP_SRCKEY_MAX];
            sprite_store_key(op->u.sprite_name.source_id, op->u.sprite_name.src_key, bridge, sizeof bridge);
            st = tp_project_atlas_set_sprite_rename(a, bridge, op->u.sprite_name.name);
            break;
        }

        case TP_OP_ANIMATION_CREATE:
            st = apply_anim_create(atlas_by_id(p, op->atlas_id), &op->u.anim_create);
            break;
        case TP_OP_ANIMATION_REMOVE:
            st = tp_project_atlas_remove_animation_by_id(atlas_by_id(p, op->atlas_id), op->u.anim_ref.anim_id);
            break;
        case TP_OP_ANIMATION_RENAME:
            st = apply_anim_rename(atlas_by_id(p, op->atlas_id), &op->u.anim_rename);
            break;
        case TP_OP_ANIMATION_SETTINGS_SET: {
            tp_project_anim *an = tp_project_atlas_find_animation_by_id(atlas_by_id(p, op->atlas_id),
                                                                       op->u.anim_settings.anim_id);
            const tp_op_anim_settings *s = &op->u.anim_settings;
            if (s->mask & TP_ANF_FPS) {
                an->fps = s->fps;
            }
            if (s->mask & TP_ANF_PLAYBACK) {
                an->playback = s->playback;
            }
            if (s->mask & TP_ANF_FLIP_H) {
                an->flip_h = s->flip_h;
            }
            if (s->mask & TP_ANF_FLIP_V) {
                an->flip_v = s->flip_v;
            }
            st = TP_STATUS_OK;
            break;
        }
        case TP_OP_ANIMATION_FRAMES_SET:
            st = apply_anim_frames_set(
                tp_project_atlas_find_animation_by_id(atlas_by_id(p, op->atlas_id), op->u.anim_frames_set.anim_id),
                &op->u.anim_frames_set);
            break;
        case TP_OP_ANIMATION_FRAME_ADD: {
            tp_project_anim *an = tp_project_atlas_find_animation_by_id(atlas_by_id(p, op->atlas_id),
                                                                       op->u.anim_frame_add.anim_id);
            st = tp_project_anim_add_frame(an, op->u.anim_frame_add.frame);
            if (st == TP_STATUS_OK && op->u.anim_frame_add.index >= 0) {
                int last = an->frame_count - 1; /* append then move into place (move clamps) */
                (void)tp_project_anim_move_frame(an, last, op->u.anim_frame_add.index - last);
            }
            break;
        }
        case TP_OP_ANIMATION_FRAME_REMOVE:
            st = tp_project_anim_remove_frame(
                tp_project_atlas_find_animation_by_id(atlas_by_id(p, op->atlas_id), op->u.anim_frame_rm.anim_id),
                op->u.anim_frame_rm.index);
            break;
        case TP_OP_ANIMATION_FRAME_MOVE: {
            const tp_op_anim_frame_move *m = &op->u.anim_frame_move;
            tp_project_anim *an = tp_project_atlas_find_animation_by_id(atlas_by_id(p, op->atlas_id), m->anim_id);
            /* Clamp to_index into [0, frame_count-1] BEFORE the subtraction: validate leaves it
             * unbounded (CLI parity), and an arbitrary client int could overflow `to - from`.
             * from_index is validated in range, so frame_count >= 1 here. move_frame re-clamps. */
            int to = m->to_index;
            if (to < 0) {
                to = 0;
            } else if (to > an->frame_count - 1) {
                to = an->frame_count - 1;
            }
            st = tp_project_anim_move_frame(an, m->from_index, to - m->from_index);
            break;
        }

        case TP_OP_TARGET_CREATE: {
            tp_project_atlas *a = atlas_by_id(p, op->atlas_id);
            tp_project_target *t = NULL;
            st = tp_project_atlas_add_target(a, op->u.target_create.exporter_id, op->u.target_create.out_path, &t);
            if (st == TP_STATUS_OK) {
                t->id = op->u.target_create.target_id;
                t->id_synthetic = false;
                t->enabled = op->u.target_create.enabled;
            }
            break;
        }
        case TP_OP_TARGET_REMOVE:
            st = tp_project_atlas_remove_target_by_id(atlas_by_id(p, op->atlas_id), op->u.target_ref.target_id);
            break;
        case TP_OP_TARGET_SET: {
            tp_project_atlas *a = atlas_by_id(p, op->atlas_id);
            tp_project_target *t = tp_project_atlas_find_target_by_id(a, op->u.target_set.target_id);
            const tp_op_target_set *s = &op->u.target_set;
            /* Masked MERGE (mirrors atlas.settings.set's in-place per-field mutation): apply only the
             * flagged fields; an unmasked field keeps the target's CURRENT value. Stage-then-commit at
             * field granularity -- dup BOTH masked strings FIRST (through the fault seam), then free the
             * old + swap, so an OOM leaves the target byte-unchanged AND we never pass t->out_path back
             * into a self-freeing setter (the use-after-free the old whole-record set_target risked).
             * validate (run above) guarantees the masked strings are non-NULL, so stage_strdup is safe. */
            char *new_eid = NULL;
            char *new_op = NULL;
            if (s->mask & TP_TF_EXPORTER) {
                new_eid = stage_strdup(s->exporter_id);
                if (!new_eid) {
                    st = TP_STATUS_OOM;
                    break;
                }
            }
            if (s->mask & TP_TF_OUT_PATH) {
                new_op = stage_strdup(s->out_path);
                if (!new_op) {
                    free(new_eid);
                    st = TP_STATUS_OOM;
                    break;
                }
            }
            /* Commit: no more allocations can fail. */
            if (s->mask & TP_TF_EXPORTER) {
                free(t->exporter_id);
                t->exporter_id = new_eid;
            }
            if (s->mask & TP_TF_OUT_PATH) {
                free(t->out_path);
                t->out_path = new_op;
            }
            if (s->mask & TP_TF_ENABLED) {
                t->enabled = s->enabled;
            }
            st = TP_STATUS_OK;
            break;
        }

        case TP_OP_INVALID:
        case TP_OP_KIND_COUNT:
        default: st = TP_STATUS_UNKNOWN_OP; break;
    }

    if (st != TP_STATUS_OK) {
        return tp_op__reject(rej, st, "", "apply failed: %s", tp_status_str(st));
    }
    return TP_STATUS_OK;
}
