/*
 * F2-03 task 1: per-op before/after capture. As the F2-02 commit applies each op to
 * the transactional clone, we snapshot the touched entity's before (pre-apply) and
 * after (post-apply) state + ordering position -- the compact semantic diff, keyed by
 * effect class (C0-02 §6). capture_before also fixes the entry's shape + addressing
 * ids; capture_after fills the created/after data. Either may allocate; on OOM the
 * caller frees the entry and fails the whole commit (model byte-unchanged).
 *
 * The clone is applied op-by-op in order, so a capture reads exactly the region the
 * op touched at that step -- op N sees the effects of ops 1..N-1 (intra-batch deps).
 */

#include <stddef.h>
#include <string.h>

#include "tp_core/tp_names.h"  /* tp_sprite_export_key (override-record bridge key) */
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_srckey.h" /* TP_SRCKEY_MAX */
#include "tp_diff_internal.h"

/* const-read lookups over the public (non-const) id accessors -- capture only
 * reads; the cast mirrors tp_op_validate.c's documented-safe pattern. */
static const tp_project_atlas *find_atlas(const tp_project *p, tp_id128 id) {
    int ai = tp_project_find_atlas_by_id(p, id);
    return ai < 0 ? NULL : &p->atlases[ai];
}
static const tp_project_source *find_source(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_source_by_id((tp_project_atlas *)a, id);
}
static const tp_project_anim *find_anim(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_animation_by_id((tp_project_atlas *)a, id);
}
static const tp_project_target *find_target(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_target_by_id((tp_project_atlas *)a, id);
}
static const tp_project_sprite *find_sprite(const tp_project_atlas *a, const char *bridge) {
    return tp_project_atlas_find_sprite((tp_project_atlas *)a, bridge);
}

static void grab_knobs(tp_diff_knobs *k, const tp_project_atlas *a) {
    k->max_size = a->max_size;
    k->padding = a->padding;
    k->margin = a->margin;
    k->extrude = a->extrude;
    k->alpha_threshold = a->alpha_threshold;
    k->max_vertices = a->max_vertices;
    k->shape = a->shape;
    k->allow_transform = a->allow_transform;
    k->power_of_two = a->power_of_two;
    k->pixels_per_unit = a->pixels_per_unit;
}

static void bridge_of(const char *src_key, char *out, size_t cap) { tp_sprite_export_key(src_key, out, cap); }

/* Snapshot the sparse override record for `bridge` into (present,index,copy). */
static tp_status grab_sprite(const tp_project_atlas *a, const char *bridge, bool *present, int *index,
                             tp_project_sprite *copy) {
    const tp_project_sprite *s = find_sprite(a, bridge);
    if (!s) {
        *present = false;
        *index = -1;
        return TP_STATUS_OK;
    }
    *present = true;
    *index = (int)(s - a->sprites);
    return tp_diff__copy_sprite_fields(s, copy);
}

tp_status tp_diff_capture_before(const tp_project *pre, const tp_operation *op, tp_diff_op *e) {
    e->kind = op->kind;
    const tp_op_info *info = tp_op_info_by_kind(op->kind);
    e->cls = info ? info->effect : TP_OP_CLASS_SET;
    e->atlas_id = op->atlas_id;
    const tp_project_atlas *a = find_atlas(pre, op->atlas_id); /* NULL only for atlas.create */
    bool ok = true;

    switch (op->kind) {
        case TP_OP_ATLAS_CREATE:
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_ATLAS;
            e->created = true;
            return TP_STATUS_OK; /* entity + position captured after */
        case TP_OP_ATLAS_REMOVE: {
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_ATLAS;
            e->created = false;
            e->position = tp_project_find_atlas_by_id(pre, op->atlas_id);
            return tp_diff__copy_elem(TP_DIFF_COLL_ATLAS, a, &e->elem);
        }
        case TP_OP_ATLAS_RENAME:
            e->shape = TP_DIFF_SHAPE_ATLAS_NAME;
            e->name_before = tp_diff__dup(a->name, &ok);
            return ok ? TP_STATUS_OK : TP_STATUS_OOM;
        case TP_OP_ATLAS_SETTINGS_SET:
            e->shape = TP_DIFF_SHAPE_ATLAS_KNOBS;
            grab_knobs(&e->knobs_before, a);
            return TP_STATUS_OK;

        case TP_OP_SOURCE_ADD:
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_SOURCE;
            e->created = true;
            return TP_STATUS_OK;
        case TP_OP_SOURCE_REMOVE: {
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_SOURCE;
            e->created = false;
            const tp_project_source *s = find_source(a, op->u.source_ref.source_id);
            e->position = (int)(s - a->sources);
            return tp_diff__copy_elem(TP_DIFF_COLL_SOURCE, s, &e->elem);
        }
        case TP_OP_SOURCE_REPLACE: {
            e->shape = TP_DIFF_SHAPE_SOURCE_PATH;
            e->entity_id = op->u.source_ref.source_id;
            const tp_project_source *s = find_source(a, op->u.source_ref.source_id);
            e->path_before = tp_diff__dup(s->path, &ok);
            return ok ? TP_STATUS_OK : TP_STATUS_OOM;
        }

        case TP_OP_SPRITE_OVERRIDE_SET:
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
        case TP_OP_SPRITE_NAME_SET: {
            e->shape = TP_DIFF_SHAPE_SPRITE_RECORD;
            const char *sk = (op->kind == TP_OP_SPRITE_OVERRIDE_SET)     ? op->u.sprite_set.src_key
                             : (op->kind == TP_OP_SPRITE_OVERRIDE_CLEAR) ? op->u.sprite_clear.src_key
                                                                         : op->u.sprite_name.src_key;
            char bridge[TP_SRCKEY_MAX];
            bridge_of(sk, bridge, sizeof bridge);
            return grab_sprite(a, bridge, &e->spr_before_present, &e->spr_before_index, &e->spr_before);
        }

        case TP_OP_ANIMATION_CREATE:
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_ANIM;
            e->created = true;
            return TP_STATUS_OK;
        case TP_OP_ANIMATION_REMOVE: {
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_ANIM;
            e->created = false;
            const tp_project_anim *an = find_anim(a, op->u.anim_ref.anim_id);
            e->position = (int)(an - a->animations);
            return tp_diff__copy_elem(TP_DIFF_COLL_ANIM, an, &e->elem);
        }
        case TP_OP_ANIMATION_SETTINGS_SET: {
            e->shape = TP_DIFF_SHAPE_ANIM_SETTINGS;
            e->anim_id = op->u.anim_settings.anim_id;
            const tp_project_anim *an = find_anim(a, e->anim_id);
            e->anim_before.fps = an->fps;
            e->anim_before.playback = an->playback;
            e->anim_before.flip_h = an->flip_h;
            e->anim_before.flip_v = an->flip_v;
            return TP_STATUS_OK;
        }
        case TP_OP_ANIMATION_FRAMES_SET: {
            e->shape = TP_DIFF_SHAPE_FRAMES_LIST;
            e->anim_id = op->u.anim_frames_set.anim_id;
            const tp_project_anim *an = find_anim(a, e->anim_id);
            e->frames_before_count = an->frame_count;
            return tp_diff__copy_frames(an->frames, an->frame_count, &e->frames_before);
        }
        case TP_OP_ANIMATION_FRAME_ADD:
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_FRAME;
            e->created = true;
            e->anim_id = op->u.anim_frame_add.anim_id;
            return TP_STATUS_OK;
        case TP_OP_ANIMATION_FRAME_REMOVE: {
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_FRAME;
            e->created = false;
            e->anim_id = op->u.anim_frame_rm.anim_id;
            e->position = op->u.anim_frame_rm.index;
            const tp_project_anim *an = find_anim(a, e->anim_id);
            return tp_diff__copy_elem(TP_DIFF_COLL_FRAME, &an->frames[e->position], &e->elem);
        }
        case TP_OP_ANIMATION_FRAME_MOVE: {
            e->shape = TP_DIFF_SHAPE_FRAME_MOVE;
            e->anim_id = op->u.anim_frame_move.anim_id;
            const tp_project_anim *an = find_anim(a, e->anim_id);
            int fc = an->frame_count; /* >= 1: from_index is validated in range */
            int to = op->u.anim_frame_move.to_index;
            if (to < 0) {
                to = 0;
            } else if (to > fc - 1) {
                to = fc - 1;
            }
            e->from_index = op->u.anim_frame_move.from_index;
            e->to_index = to;
            return TP_STATUS_OK;
        }

        case TP_OP_TARGET_CREATE:
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_TARGET;
            e->created = true;
            return TP_STATUS_OK;
        case TP_OP_TARGET_REMOVE: {
            e->shape = TP_DIFF_SHAPE_COLL;
            e->coll = TP_DIFF_COLL_TARGET;
            e->created = false;
            const tp_project_target *t = find_target(a, op->u.target_ref.target_id);
            e->position = (int)(t - a->targets);
            return tp_diff__copy_elem(TP_DIFF_COLL_TARGET, t, &e->elem);
        }
        case TP_OP_TARGET_SET: {
            e->shape = TP_DIFF_SHAPE_TARGET_FIELDS;
            e->entity_id = op->u.target_set.target_id;
            const tp_project_target *t = find_target(a, e->entity_id);
            e->exporter_before = tp_diff__dup(t->exporter_id, &ok);
            if (!ok) {
                return TP_STATUS_OOM;
            }
            e->out_before = tp_diff__dup(t->out_path, &ok);
            if (!ok) {
                return TP_STATUS_OOM;
            }
            e->enabled_before = t->enabled;
            return TP_STATUS_OK;
        }

        case TP_OP_INVALID:
        case TP_OP_KIND_COUNT: break;
    }
    return TP_STATUS_INVALID_ARGUMENT;
}

tp_status tp_diff_capture_after(const tp_project *post, const tp_operation *op, tp_diff_op *e) {
    const tp_project_atlas *a = find_atlas(post, op->atlas_id);
    bool ok = true;

    switch (op->kind) {
        case TP_OP_ATLAS_CREATE: {
            int idx = tp_project_find_atlas_by_id(post, op->atlas_id);
            e->position = idx;
            return tp_diff__copy_elem(TP_DIFF_COLL_ATLAS, &post->atlases[idx], &e->elem);
        }
        case TP_OP_ATLAS_RENAME:
            e->name_after = tp_diff__dup(a->name, &ok);
            return ok ? TP_STATUS_OK : TP_STATUS_OOM;
        case TP_OP_ATLAS_SETTINGS_SET:
            grab_knobs(&e->knobs_after, a);
            return TP_STATUS_OK;

        case TP_OP_SOURCE_ADD: {
            const tp_project_source *s = find_source(a, op->u.source_add.source_id);
            e->position = (int)(s - a->sources);
            return tp_diff__copy_elem(TP_DIFF_COLL_SOURCE, s, &e->elem);
        }
        case TP_OP_SOURCE_REPLACE: {
            const tp_project_source *s = find_source(a, op->u.source_ref.source_id);
            e->path_after = tp_diff__dup(s->path, &ok);
            return ok ? TP_STATUS_OK : TP_STATUS_OOM;
        }

        case TP_OP_SPRITE_OVERRIDE_SET:
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
        case TP_OP_SPRITE_NAME_SET: {
            const char *sk = (op->kind == TP_OP_SPRITE_OVERRIDE_SET)     ? op->u.sprite_set.src_key
                             : (op->kind == TP_OP_SPRITE_OVERRIDE_CLEAR) ? op->u.sprite_clear.src_key
                                                                         : op->u.sprite_name.src_key;
            char bridge[TP_SRCKEY_MAX];
            bridge_of(sk, bridge, sizeof bridge);
            return grab_sprite(a, bridge, &e->spr_after_present, &e->spr_after_index, &e->spr_after);
        }

        case TP_OP_ANIMATION_CREATE: {
            const tp_project_anim *an = find_anim(a, op->u.anim_create.anim_id);
            e->position = (int)(an - a->animations);
            return tp_diff__copy_elem(TP_DIFF_COLL_ANIM, an, &e->elem);
        }
        case TP_OP_ANIMATION_SETTINGS_SET: {
            const tp_project_anim *an = find_anim(a, e->anim_id);
            e->anim_after.fps = an->fps;
            e->anim_after.playback = an->playback;
            e->anim_after.flip_h = an->flip_h;
            e->anim_after.flip_v = an->flip_v;
            return TP_STATUS_OK;
        }
        case TP_OP_ANIMATION_FRAMES_SET: {
            const tp_project_anim *an = find_anim(a, e->anim_id);
            e->frames_after_count = an->frame_count;
            return tp_diff__copy_frames(an->frames, an->frame_count, &e->frames_after);
        }
        case TP_OP_ANIMATION_FRAME_ADD: {
            const tp_project_anim *an = find_anim(a, e->anim_id);
            int fc = an->frame_count; /* post-add: >= 1 */
            int idx = op->u.anim_frame_add.index;
            if (idx < 0 || idx > fc - 1) {
                idx = fc - 1; /* append / clamp -- matches tp_operation_apply */
            }
            e->position = idx;
            return tp_diff__copy_elem(TP_DIFF_COLL_FRAME, &an->frames[idx], &e->elem);
        }

        case TP_OP_TARGET_CREATE: {
            const tp_project_target *t = find_target(a, op->u.target_create.target_id);
            e->position = (int)(t - a->targets);
            return tp_diff__copy_elem(TP_DIFF_COLL_TARGET, t, &e->elem);
        }
        case TP_OP_TARGET_SET: {
            const tp_project_target *t = find_target(a, e->entity_id);
            e->exporter_after = tp_diff__dup(t->exporter_id, &ok);
            if (!ok) {
                return TP_STATUS_OOM;
            }
            e->out_after = tp_diff__dup(t->out_path, &ok);
            if (!ok) {
                return TP_STATUS_OOM;
            }
            e->enabled_after = t->enabled;
            return TP_STATUS_OK;
        }

        /* REMOVE + MOVE: fully captured before-apply; nothing to record after. */
        case TP_OP_ATLAS_REMOVE:
        case TP_OP_SOURCE_REMOVE:
        case TP_OP_ANIMATION_REMOVE:
        case TP_OP_ANIMATION_FRAME_REMOVE:
        case TP_OP_ANIMATION_FRAME_MOVE:
        case TP_OP_TARGET_REMOVE: return TP_STATUS_OK;

        case TP_OP_INVALID:
        case TP_OP_KIND_COUNT: break;
    }
    return TP_STATUS_INVALID_ARGUMENT;
}
