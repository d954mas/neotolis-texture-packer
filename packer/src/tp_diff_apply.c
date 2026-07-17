/*
 * Apply one op-diff to restore the BEFORE (Undo) or AFTER (Redo)
 * state. The caller applies a whole record to a CLONE of the live model and swaps it
 * in only on full success (tp_history.c), so a failure here (allocator OR a corrupted
 * diff) discards the clone with the live model byte-unchanged -- STAGE-THEN-COMMIT.
 *
 * UB-clean on a hostile diff: every entity reference is resolved and every index is
 * bounds-checked BEFORE any dereference; a stale/unknown id yields TP_STATUS_NOT_FOUND
 * and an out-of-range position TP_STATUS_OUT_OF_BOUNDS, never a crash.
 */

#include <stddef.h>
#include <stdlib.h>

#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_diff_internal.h"
#include "tp_strutil.h" /* tp_set_owned_dup (shared OOM-safe string-field swap) */

static tp_project_atlas *atlas_of(tp_project *p, tp_id128 id) {
    int ai = tp_project_find_atlas_by_id(p, id);
    return ai < 0 ? NULL : &p->atlases[ai];
}

/* Adapt tp_diff__dup (NULL src -> NULL/ok, real alloc fail -> NULL/!ok) to the "NULL only on
 * failure" dup contract tp_set_owned_dup wants. Every anim name / source path captured in a diff
 * is non-NULL (both are required non-empty fields), so tp_diff__dup's NULL-src branch is
 * unreachable here; routing through it keeps this field swap on the diff alloc fault seam. */
static char *diff_dup_or_null(const char *s) {
    bool ok = true;
    char *d = tp_diff__dup(s, &ok);
    return ok ? d : NULL;
}

static void set_knobs(tp_project_atlas *a, const tp_diff_knobs *k) {
    a->max_size = k->max_size;
    a->padding = k->padding;
    a->margin = k->margin;
    a->extrude = k->extrude;
    a->alpha_threshold = k->alpha_threshold;
    a->max_vertices = k->max_vertices;
    a->shape = k->shape;
    a->allow_transform = k->allow_transform;
    a->power_of_two = k->power_of_two;
    a->pixels_per_unit = k->pixels_per_unit;
}

/* Replace an animation's whole frames list (stage-then-commit: copy new list first
 * so an OOM leaves the old list intact). */
static tp_status replace_frames(tp_project_anim *an, const tp_project_frame *src, int count) {
    tp_project_frame *nf = NULL;
    tp_status st = tp_diff__copy_frames(src, count, &nf);
    if (st != TP_STATUS_OK) {
        return st; /* old list intact */
    }
    tp_diff__free_frames(an->frames, an->frame_count);
    an->frames = nf;
    an->frame_count = count;
    an->frame_cap = count;
    return TP_STATUS_OK;
}

/* Reconcile the sparse sprite array from `oth` (the from-state) to `tgt` (target).
 * Under the strict reverse/forward replay invariant the current array matches `oth`. */
static tp_status reconcile_sprite(tp_project_atlas *a, bool tgt_present, int tgt_index, const tp_project_sprite *tgt,
                                  bool oth_present, int oth_index) {
    if (oth_present && !tgt_present) {
        return tp_diff__remove_sprite_at(a, oth_index);
    }
    if (!oth_present && tgt_present) {
        return tp_diff__insert_sprite(a, tgt_index, tgt);
    }
    if (oth_present && tgt_present) {
        return tp_diff__replace_sprite_at(a, oth_index, tgt); /* in place: oth_index == tgt_index */
    }
    return TP_STATUS_OK; /* both absent: no-op */
}

tp_status tp_diff_op_apply(tp_project *clone, const tp_diff_op *e, bool reverse, tp_error *err) {
    switch (e->shape) {
        case TP_DIFF_SHAPE_COLL: {
            /* created: undo removes / redo inserts. removed: undo inserts / redo removes. */
            bool do_insert = e->created ? !reverse : reverse;
            switch (e->coll) {
                case TP_DIFF_COLL_ATLAS:
                    return do_insert ? tp_diff__insert_atlas(clone, e->position, (const tp_project_atlas *)e->elem)
                                     : tp_diff__remove_atlas(clone, e->position);
                case TP_DIFF_COLL_SOURCE: {
                    tp_project_atlas *a = atlas_of(clone, e->atlas_id);
                    if (!a) {
                        return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for source op");
                    }
                    return do_insert ? tp_diff__insert_source(a, e->position, (const tp_project_source *)e->elem)
                                     : tp_diff__remove_source(a, e->position);
                }
                case TP_DIFF_COLL_ANIM: {
                    tp_project_atlas *a = atlas_of(clone, e->atlas_id);
                    if (!a) {
                        return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for anim op");
                    }
                    return do_insert ? tp_diff__insert_anim(a, e->position, (const tp_project_anim *)e->elem)
                                     : tp_diff__remove_anim(a, e->position);
                }
                case TP_DIFF_COLL_TARGET: {
                    tp_project_atlas *a = atlas_of(clone, e->atlas_id);
                    if (!a) {
                        return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for target op");
                    }
                    return do_insert ? tp_diff__insert_target(a, e->position, (const tp_project_target *)e->elem)
                                     : tp_diff__remove_target(a, e->position);
                }
                case TP_DIFF_COLL_FRAME: {
                    tp_project_atlas *a = atlas_of(clone, e->atlas_id);
                    if (!a) {
                        return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for frame op");
                    }
                    tp_project_anim *an = tp_project_atlas_find_animation_by_id(a, e->anim_id);
                    if (!an) {
                        return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no animation for frame op");
                    }
                    return do_insert ? tp_diff__insert_frame(an, e->position, (const tp_project_frame *)e->elem)
                                     : tp_diff__remove_frame_at(an, e->position);
                }
            }
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "diff: bad collection");
        }

        case TP_DIFF_SHAPE_FRAME_MOVE: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for frame.move");
            }
            tp_project_anim *an = tp_project_atlas_find_animation_by_id(a, e->anim_id);
            if (!an) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no animation for frame.move");
            }
            int src = reverse ? e->to_index : e->from_index;
            int dst = reverse ? e->from_index : e->to_index;
            if (src < 0 || src >= an->frame_count || dst < 0 || dst >= an->frame_count) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "diff: frame.move index out of range");
            }
            return tp_project_anim_move_frame(an, src, dst - src);
        }

        case TP_DIFF_SHAPE_ATLAS_NAME: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for rename");
            }
            return tp_project_set_atlas_name(a, reverse ? e->name_before : e->name_after);
        }
        case TP_DIFF_SHAPE_ATLAS_KNOBS: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for settings");
            }
            set_knobs(a, reverse ? &e->knobs_before : &e->knobs_after);
            return TP_STATUS_OK;
        }
        case TP_DIFF_SHAPE_SOURCE_PATH: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for source.replace");
            }
            tp_project_source *s = tp_project_atlas_find_source_by_id(a, e->entity_id);
            if (!s) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no source for source.replace");
            }
            if (tp_set_owned_dup(&s->path, reverse ? e->path_before : e->path_after, diff_dup_or_null) != TP_STATUS_OK) {
                return tp_error_set(err, TP_STATUS_OOM, "diff: source path dup failed");
            }
            return TP_STATUS_OK;
        }
        case TP_DIFF_SHAPE_TARGET_FIELDS: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for target.set");
            }
            tp_project_target *t = tp_project_atlas_find_target_by_id(a, e->entity_id);
            if (!t) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no target for target.set");
            }
            int index = (int)(t - a->targets);
            return tp_project_atlas_set_target(a, index, reverse ? e->exporter_before : e->exporter_after,
                                               reverse ? e->out_before : e->out_after,
                                               reverse ? e->enabled_before : e->enabled_after);
        }
        case TP_DIFF_SHAPE_ANIM_NAME: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for anim.rename");
            }
            tp_project_anim *an = tp_project_atlas_find_animation_by_id(a, e->anim_id);
            if (!an) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no animation for anim.rename");
            }
            if (tp_set_owned_dup(&an->name, reverse ? e->name_before : e->name_after, diff_dup_or_null) !=
                TP_STATUS_OK) {
                return tp_error_set(err, TP_STATUS_OOM, "diff: anim name dup failed");
            }
            return TP_STATUS_OK;
        }
        case TP_DIFF_SHAPE_ANIM_SETTINGS: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for anim.settings");
            }
            tp_project_anim *an = tp_project_atlas_find_animation_by_id(a, e->anim_id);
            if (!an) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no animation for anim.settings");
            }
            const tp_diff_anim_settings *s = reverse ? &e->anim_before : &e->anim_after;
            an->fps = s->fps;
            an->playback = s->playback;
            an->flip_h = s->flip_h;
            an->flip_v = s->flip_v;
            return TP_STATUS_OK;
        }
        case TP_DIFF_SHAPE_SPRITE_RECORD: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for sprite op");
            }
            bool tgt_present = reverse ? e->spr_before_present : e->spr_after_present;
            int tgt_index = reverse ? e->spr_before_index : e->spr_after_index;
            const tp_project_sprite *tgt = reverse ? &e->spr_before : &e->spr_after;
            bool oth_present = reverse ? e->spr_after_present : e->spr_before_present;
            int oth_index = reverse ? e->spr_after_index : e->spr_before_index;
            return reconcile_sprite(a, tgt_present, tgt_index, tgt, oth_present, oth_index);
        }
        case TP_DIFF_SHAPE_FRAMES_LIST: {
            tp_project_atlas *a = atlas_of(clone, e->atlas_id);
            if (!a) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no atlas for frames.set");
            }
            tp_project_anim *an = tp_project_atlas_find_animation_by_id(a, e->anim_id);
            if (!an) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND, "diff: no animation for frames.set");
            }
            return replace_frames(an, reverse ? e->frames_before : e->frames_after,
                                  reverse ? e->frames_before_count : e->frames_after_count);
        }
    }
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "diff: unknown shape");
}

tp_status tp_diff_record_apply(tp_project *clone, const tp_diff_record *r, bool reverse, tp_error *err) {
    if (reverse) {
        for (int i = r->op_count - 1; i >= 0; i--) {
            tp_status st = tp_diff_op_apply(clone, &r->ops[i], true, err);
            if (st != TP_STATUS_OK) {
                return st;
            }
        }
    } else {
        for (int i = 0; i < r->op_count; i++) {
            tp_status st = tp_diff_op_apply(clone, &r->ops[i], false, err);
            if (st != TP_STATUS_OK) {
                return st;
            }
        }
    }
    return TP_STATUS_OK;
}
