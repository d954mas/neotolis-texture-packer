#include "tp_op_validate_family_internal.h"

#include <string.h>

#include "tp_core/tp_srckey.h"
#include "tp_op_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_srckey_internal.h"

static const tp_project_anim *find_anim(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_animation_by_id((tp_project_atlas *)a, id);
}

/* fps + playback checks shared by animation.create / .settings.set. */
static tp_status validate_anim_knobs(bool check_fps, float fps, bool check_pb, int pb, tp_op_reject *rej) {
    if (check_fps && !tp_project_anim_fps_valid(fps)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "fps", "fps must be positive finite");
    }
    if (check_pb) {
        if (!tp_project_anim_playback_valid(pb)) {
            return tp_op__reject(
                rej, TP_STATUS_OUT_OF_RANGE, "playback",
                "playback = %d must be in [%d..%d]", pb,
                TP_PROJECT_ANIM_PLAYBACK_MIN,
                TP_PROJECT_ANIM_PLAYBACK_MAX);
        }
    }
    return TP_STATUS_OK;
}

static tp_status validate_canonical_frame_key(const char *key,
                                              const char *field,
                                              tp_op_reject *rej) {
    if (!key || key[0] == '\0') {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, field,
                             "frame key must be non-empty");
    }
    tp_error error = {{0}};
    tp_srckey_canonical_result result;
    const tp_status status =
        tp_srckey_check_canonical(key, &result, &error);
    if (status != TP_STATUS_OK) {
        if (result.reason == TP_SRCKEY_CANONICAL_SPELLING_MISMATCH) {
            return tp_op__reject(
                rej, TP_STATUS_INVALID_ARGUMENT, field,
                "frame key must already be normalized as '%s'",
                result.canonical);
        }
        return tp_op__reject(rej, status, field,
                             "frame key is not a valid source-local key: %s",
                             error.msg);
    }
    return TP_STATUS_OK;
}

static tp_status validate_frame_array_shape(const tp_op_sprite_ref *frames,
                                            int n, tp_op_reject *rej) {
    if (n < 0) { /* a negative count would loop &frames[-1] in apply -> heap underflow */
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "frame_count", "frame_count %d must be >= 0", n);
    }
    if (n > 0 && frames == NULL) { /* count claims frames but the array ptr is null -> frames[i] derefs NULL */
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "frames", "frame_count %d but the frames array is null", n);
    }
    return TP_STATUS_OK;
}

tp_status tp_op__validate_encode_shape(const tp_operation *operation,
                                       tp_op_reject *rej) {
    tp_op__reject_ok(rej);
    if (!operation) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "",
                             "null operation");
    }
    if (operation->kind == TP_OP_ANIMATION_CREATE) {
        return validate_frame_array_shape(operation->u.anim_create.frames,
                                          operation->u.anim_create.frame_count,
                                          rej);
    }
    if (operation->kind == TP_OP_ANIMATION_FRAMES_SET) {
        return validate_frame_array_shape(
            operation->u.anim_frames_set.frames,
            operation->u.anim_frames_set.frame_count, rej);
    }
    return TP_STATUS_OK;
}

static tp_status validate_frames(const tp_project_atlas *atlas,
                                 const tp_op_sprite_ref *frames, int n,
                                 tp_op_reject *rej) {
    const tp_status shape = validate_frame_array_shape(frames, n, rej);
    if (shape != TP_STATUS_OK) {
        return shape;
    }
    for (int i = 0; i < n; i++) {
        if (tp_id128_is_nil(frames[i].source_id)) {
            return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "frames",
                                 "frame %d needs a canonical source id", i);
        }
        if (!tp_op_validate_find_source(atlas, frames[i].source_id)) {
            return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "frames",
                                 "frame %d source does not exist in the atlas", i);
        }
        const tp_status key_status = validate_canonical_frame_key(
            frames[i].src_key, "frames", rej);
        if (key_status != TP_STATUS_OK) {
            return key_status;
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_op_validate_animation_family(
    const tp_project *p, const tp_project_atlas *a,
    const tp_operation *op, tp_op_reject *rej) {
    switch (op->kind) {
        case TP_OP_ANIMATION_CREATE: {
            const tp_op_anim_create *c = &op->u.anim_create;
            tp_status st = TP_STATUS_OK;
            if (tp_id128_is_nil(c->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "anim_id", "animation.create needs a real anim id");
            }
            if (tp_project_has_structural_id(p, c->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "anim_id",
                                     "that structural id already belongs to a project entity");
            }
            if (!c->name || c->name[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "animation name must be non-empty");
            }
            for (int i = 0; i < a->animation_count; ++i) {
                if (a->animations[i].name &&
                    strcmp(a->animations[i].name, c->name) == 0) {
                    return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT,
                                         "name",
                                         "an animation named '%s' already exists",
                                         c->name);
                }
            }
            if ((st = validate_anim_knobs(true, c->fps, true, c->playback, rej))) {
                return st;
            }
            return validate_frames(a, c->frames, c->frame_count, rej);
        }
        case TP_OP_ANIMATION_REMOVE:
            return find_anim(a, op->u.anim_ref.anim_id)
                       ? TP_STATUS_OK
                       : tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
        case TP_OP_ANIMATION_RENAME: {
            const tp_op_anim_rename *r = &op->u.anim_rename;
            const tp_project_anim *an = find_anim(a, r->anim_id);
            if (!an) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (!r->name || r->name[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "animation name must be non-empty");
            }
            /* Reject a collision with ANOTHER animation in the atlas; renaming to the same name
             * (self) is allowed (a no-op) -- mirrors atlas.rename's uniqueness check. This is the
             * policy the GUI enforced client-side (decision 0015); it now lives here so
             * every frontend reuses the one structured reject (invalid_argument + field). */
            for (int i = 0; i < a->animation_count; i++) {
                const tp_project_anim *other = &a->animations[i];
                if (other != an && other->name && strcmp(other->name, r->name) == 0) {
                    return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name",
                                         "an animation named '%s' already exists", r->name);
                }
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ANIMATION_SETTINGS_SET: {
            const tp_op_anim_settings *s = &op->u.anim_settings;
            if (!find_anim(a, s->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (s->mask == 0) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "animation.settings.set names no field");
            }
            if ((s->mask & ~(uint32_t)TP_ANF_ALL) != 0U) {
                return tp_op__reject(
                    rej, TP_STATUS_INVALID_ARGUMENT, "",
                    "animation.settings.set contains unknown field bits");
            }
            return validate_anim_knobs((s->mask & TP_ANF_FPS) != 0, s->fps, (s->mask & TP_ANF_PLAYBACK) != 0,
                                       s->playback, rej);
        }
        case TP_OP_ANIMATION_FRAMES_SET: {
            const tp_op_anim_frames_set *s = &op->u.anim_frames_set;
            if (!find_anim(a, s->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            return validate_frames(a, s->frames, s->frame_count, rej);
        }
        case TP_OP_ANIMATION_FRAME_ADD: {
            const tp_op_anim_frame_add *s = &op->u.anim_frame_add;
            if (!find_anim(a, s->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (tp_id128_is_nil(s->frame.source_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "frame",
                                     "frame needs a canonical source id");
            }
            if (!tp_op_validate_find_source(a, s->frame.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "frame",
                                     "frame source does not exist in the atlas");
            }
            tp_status key_status = validate_canonical_frame_key(
                s->frame.src_key, "frame", rej);
            if (key_status != TP_STATUS_OK) {
                return key_status;
            }
            if (s->index < -1) {
                return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "index", "index must be >= -1 (-1 = append)");
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ANIMATION_FRAME_REMOVE: {
            const tp_op_anim_frame_rm *s = &op->u.anim_frame_rm;
            const tp_project_anim *an = find_anim(a, s->anim_id);
            if (!an) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (s->index < 0 || s->index >= an->frame_count) {
                return tp_op__reject(rej, TP_STATUS_OUT_OF_BOUNDS, "index", "frame index %d out of [0..%d)", s->index,
                                     an->frame_count);
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ANIMATION_FRAME_MOVE: {
            const tp_op_anim_frame_move *s = &op->u.anim_frame_move;
            const tp_project_anim *an = find_anim(a, s->anim_id);
            if (!an) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (s->from_index < 0 || s->from_index >= an->frame_count) {
                return tp_op__reject(rej, TP_STATUS_OUT_OF_BOUNDS, "from_index", "from_index %d out of [0..%d)",
                                     s->from_index, an->frame_count);
            }
            /* to_index is intentionally UNBOUNDED: the CLI `anim move-frame` clamps a large
             * (or negative) destination to the last/first slot; apply clamps identically. */
            return TP_STATUS_OK;
        }

        default:
            return tp_op__reject(rej, TP_STATUS_UNKNOWN_OP, "op",
                                 "operation is not an animation family member");
    }
}
