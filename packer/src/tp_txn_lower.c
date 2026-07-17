/*
 * A shape-valid transaction op JSON object ->
 * a typed tp_operation. This is the inverse of tp_operation_encode; it reads
 * ONLY the closed per-kind field vocabulary (tp_op_fields) and derives each SET op's
 * presence mask from which fields are present. Every number routes through the
 * range-checked j_i64 converter (tp_txn_json.h) -- no out-of-range double->int cast.
 *
 * PRE: the caller (tp_txn_parse.c) already ran the shape collect-all (wire known,
 * every field allowed, addressing ids well-formed). Remaining faults here are typed
 * VALUE faults (wrong JSON type / number out of range), reported fail-fast per op.
 */

#include "tp_txn_parse_priv.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_txn_json.h"

static tp_status lower_frame_ref(const cJSON *value, tp_op_sprite_ref *out,
                                 tp_error *err) {
    if (!cJSON_IsObject(value)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "frame must be a {source_id, src_key} object");
    }
    tp_status status = j_opt_shape_id(value, "source_id", TP_ID_KIND_SOURCE,
                                      &out->source_id, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    return j_opt_dup(value, "src_key", &out->src_key, err);
}

/* Read canonical frames[] objects into a fresh array. */
static tp_status lower_frames(const cJSON *oj, tp_op_sprite_ref **out_frames,
                              int *out_n, tp_error *err) {
    *out_frames = NULL;
    *out_n = 0;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(oj, "frames");
    if (!arr) {
        return TP_STATUS_OK;
    }
    if (!cJSON_IsArray(arr)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "\"frames\" must be an array");
    }
    int n = cJSON_GetArraySize(arr);
    if (n == 0) {
        return TP_STATUS_OK;
    }
    tp_op_sprite_ref *frames = (tp_op_sprite_ref *)calloc((size_t)n,
                                                          sizeof *frames);
    if (!frames) {
        return tp_error_set(err, TP_STATUS_OOM, "frames alloc");
    }
    for (int i = 0; i < n; i++) {
        const cJSON *el = cJSON_GetArrayItem(arr, i);
        tp_status status = lower_frame_ref(el, &frames[i], err);
        if (status != TP_STATUS_OK) {
            for (int j = 0; j < i; j++) {
                free(frames[j].src_key);
            }
            free(frames);
            return status;
        }
    }
    *out_frames = frames;
    *out_n = n;
    return TP_STATUS_OK;
}

/* fields[] token array -> sprite override mask. Unknown tokens are ignored (the
 * shape pass validated the `fields` key itself; token content is validated by
 * tp_operation_validate rejecting a mask of 0). */
static uint32_t lower_clear_mask(const cJSON *oj) {
    uint32_t mask = 0;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(oj, "fields");
    if (!cJSON_IsArray(arr)) {
        return 0;
    }
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        const cJSON *el = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(el)) {
            continue;
        }
        const char *t = el->valuestring;
        if (strcmp(t, "origin") == 0) {
            mask |= TP_SPF_ORIGIN;
        } else if (strcmp(t, "slice9") == 0) {
            mask |= TP_SPF_SLICE9;
        } else if (strcmp(t, "shape") == 0) {
            mask |= TP_SPF_SHAPE;
        } else if (strcmp(t, "allow_rotate") == 0) {
            mask |= TP_SPF_ALLOW_ROTATE;
        } else if (strcmp(t, "max_vertices") == 0) {
            mask |= TP_SPF_MAX_VERTICES;
        } else if (strcmp(t, "margin") == 0) {
            mask |= TP_SPF_MARGIN;
        } else if (strcmp(t, "extrude") == 0) {
            mask |= TP_SPF_EXTRUDE;
        }
    }
    return mask;
}

/* TRY: propagate a lowering fault after freeing the partially-filled op. Used only
 * in tp_txn__lower_op where `out` is in scope. */
#define TRY(EXPR)                       \
    do {                                \
        tp_status _st = (EXPR);         \
        if (_st != TP_STATUS_OK) {      \
            tp_operation_free(out);     \
            return _st;                 \
        }                               \
    } while (0)

static tp_status lower_atlas_settings(const cJSON *oj, tp_op_atlas_settings *s, tp_error *err) {
    bool pr = false;
    tp_status st;
    if ((st = j_opt_int(oj, "max_size", &s->max_size, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_MAX_SIZE;
    if ((st = j_opt_int(oj, "padding", &s->padding, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_PADDING;
    if ((st = j_opt_int(oj, "margin", &s->margin, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_MARGIN;
    if ((st = j_opt_int(oj, "extrude", &s->extrude, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_EXTRUDE;
    if ((st = j_opt_int(oj, "alpha_threshold", &s->alpha_threshold, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_ALPHA_THRESHOLD;
    if ((st = j_opt_int(oj, "max_vertices", &s->max_vertices, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_MAX_VERTICES;
    if ((st = j_opt_int(oj, "shape", &s->shape, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_SHAPE;
    if ((st = j_opt_bool(oj, "allow_transform", &s->allow_transform, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_ALLOW_TRANSFORM;
    if ((st = j_opt_bool(oj, "power_of_two", &s->power_of_two, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_POWER_OF_TWO;
    if ((st = j_opt_float(oj, "pixels_per_unit", &s->pixels_per_unit, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_AF_PIXELS_PER_UNIT;
    return TP_STATUS_OK;
}

static tp_status lower_sprite_set(const cJSON *oj, tp_op_sprite_set *s, tp_error *err) {
    tp_status st;
    bool pr = false;
    bool ox = false, oy = false;
    if ((st = j_opt_float(oj, "origin_x", &s->origin_x, &ox, err)) != TP_STATUS_OK) return st;
    if ((st = j_opt_float(oj, "origin_y", &s->origin_y, &oy, err)) != TP_STATUS_OK) return st;
    if (ox || oy) s->mask |= TP_SPF_ORIGIN;
    bool any9 = false;
    const char *k9[4] = {"slice9_l", "slice9_r", "slice9_t", "slice9_b"};
    for (int i = 0; i < 4; i++) {
        int v = 0;
        if ((st = j_opt_int(oj, k9[i], &v, &pr, err)) != TP_STATUS_OK) return st;
        if (pr) {
            any9 = true;
        }
        s->slice9[i] = v;
    }
    if (any9) s->mask |= TP_SPF_SLICE9;
    if ((st = j_opt_int(oj, "ov_shape", &s->ov_shape, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_SPF_SHAPE;
    if ((st = j_opt_int(oj, "ov_allow_rotate", &s->ov_allow_rotate, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_SPF_ALLOW_ROTATE;
    if ((st = j_opt_int(oj, "ov_max_vertices", &s->ov_max_vertices, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_SPF_MAX_VERTICES;
    if ((st = j_opt_int(oj, "ov_margin", &s->ov_margin, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_SPF_MARGIN;
    if ((st = j_opt_int(oj, "ov_extrude", &s->ov_extrude, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_SPF_EXTRUDE;
    return TP_STATUS_OK;
}

static tp_status lower_anim_settings(const cJSON *oj, tp_op_anim_settings *s, tp_error *err) {
    tp_status st;
    bool pr = false;
    if ((st = j_opt_float(oj, "fps", &s->fps, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_ANF_FPS;
    if ((st = j_opt_int(oj, "playback", &s->playback, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_ANF_PLAYBACK;
    if ((st = j_opt_bool(oj, "flip_h", &s->flip_h, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_ANF_FLIP_H;
    if ((st = j_opt_bool(oj, "flip_v", &s->flip_v, &pr, err)) != TP_STATUS_OK) return st;
    if (pr) s->mask |= TP_ANF_FLIP_V;
    return TP_STATUS_OK;
}

tp_status tp_txn__lower_op(const cJSON *oj, tp_operation *out, tp_error *err) {
    memset(out, 0, sizeof *out);
    const cJSON *wire = cJSON_GetObjectItemCaseSensitive(oj, "op");
    if (!cJSON_IsString(wire)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "operation missing string \"op\"");
    }
    out->kind = tp_op_kind_from_wire(wire->valuestring);
    if (out->kind == TP_OP_INVALID) {
        return tp_error_set(err, TP_STATUS_UNKNOWN_OP, "unknown operation '%s'", wire->valuestring);
    }
    /* atlas_id is on every op (the parent atlas; the NEW atlas for atlas.create). */
    TRY(j_opt_shape_id(oj, "atlas_id", TP_ID_KIND_ATLAS, &out->atlas_id, err));

    switch (out->kind) {
        case TP_OP_ATLAS_CREATE: TRY(j_opt_dup(oj, "name", &out->u.atlas_create.name, err)); break;
        case TP_OP_ATLAS_REMOVE: break;
        case TP_OP_ATLAS_RENAME: TRY(j_opt_dup(oj, "name", &out->u.atlas_rename.name, err)); break;
        case TP_OP_ATLAS_SETTINGS_SET: TRY(lower_atlas_settings(oj, &out->u.atlas_settings, err)); break;

        case TP_OP_SOURCE_ADD: {
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.source_add.source_id, err));
            TRY(j_opt_dup(oj, "key", &out->u.source_add.key, err));
            const cJSON *kind = cJSON_GetObjectItemCaseSensitive(oj, "kind");
            out->u.source_add.kind = TP_SOURCE_KIND_FOLDER;
            if (cJSON_IsString(kind) && strcmp(kind->valuestring, "file") == 0) {
                out->u.source_add.kind = TP_SOURCE_KIND_FILE;
            }
            break;
        }
        case TP_OP_SOURCE_REMOVE:
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.source_ref.source_id, err));
            break;
        case TP_OP_SOURCE_REPLACE:
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.source_ref.source_id, err));
            TRY(j_opt_dup(oj, "key", &out->u.source_ref.key, err));
            break;

        case TP_OP_SPRITE_OVERRIDE_SET:
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.sprite_set.source_id, err));
            TRY(j_opt_dup(oj, "src_key", &out->u.sprite_set.src_key, err));
            TRY(lower_sprite_set(oj, &out->u.sprite_set, err));
            break;
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.sprite_clear.source_id, err));
            TRY(j_opt_dup(oj, "src_key", &out->u.sprite_clear.src_key, err));
            out->u.sprite_clear.mask = lower_clear_mask(oj);
            break;
        case TP_OP_SPRITE_NAME_SET:
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.sprite_name.source_id, err));
            TRY(j_opt_dup(oj, "src_key", &out->u.sprite_name.src_key, err));
            TRY(j_opt_dup(oj, "name", &out->u.sprite_name.name, err)); /* NULL clears the rename */
            break;

        case TP_OP_ANIMATION_CREATE: {
            tp_op_anim_create *c = &out->u.anim_create;
            bool pr = false;
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &c->anim_id, err));
            TRY(j_opt_dup(oj, "name", &c->name, err));
            c->fps = TP_PROJECT_ANIM_FPS_DEFAULT; /* preserved when "fps" is absent */
            TRY(j_opt_float(oj, "fps", &c->fps, &pr, err));
            TRY(j_opt_int(oj, "playback", &c->playback, &pr, err));
            TRY(j_opt_bool(oj, "flip_h", &c->flip_h, &pr, err));
            TRY(j_opt_bool(oj, "flip_v", &c->flip_v, &pr, err));
            TRY(lower_frames(oj, &c->frames, &c->frame_count, err));
            break;
        }
        case TP_OP_ANIMATION_REMOVE:
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_ref.anim_id, err));
            break;
        case TP_OP_ANIMATION_RENAME:
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_rename.anim_id, err));
            TRY(j_opt_dup(oj, "name", &out->u.anim_rename.name, err));
            break;
        case TP_OP_ANIMATION_SETTINGS_SET:
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_settings.anim_id, err));
            TRY(lower_anim_settings(oj, &out->u.anim_settings, err));
            break;
        case TP_OP_ANIMATION_FRAMES_SET:
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_frames_set.anim_id, err));
            TRY(lower_frames(oj, &out->u.anim_frames_set.frames, &out->u.anim_frames_set.frame_count, err));
            break;
        case TP_OP_ANIMATION_FRAME_ADD: {
            bool pr = false;
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_frame_add.anim_id, err));
            const cJSON *frame = cJSON_GetObjectItemCaseSensitive(oj, "frame");
            TRY(lower_frame_ref(frame, &out->u.anim_frame_add.frame, err));
            out->u.anim_frame_add.index = -1; /* default append */
            TRY(j_opt_int(oj, "index", &out->u.anim_frame_add.index, &pr, err));
            break;
        }
        case TP_OP_ANIMATION_FRAME_REMOVE: {
            bool pr = false;
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_frame_rm.anim_id, err));
            TRY(j_opt_int(oj, "index", &out->u.anim_frame_rm.index, &pr, err));
            break;
        }
        case TP_OP_ANIMATION_FRAME_MOVE: {
            bool pr = false;
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_frame_move.anim_id, err));
            TRY(j_opt_int(oj, "from_index", &out->u.anim_frame_move.from_index, &pr, err));
            TRY(j_opt_int(oj, "to_index", &out->u.anim_frame_move.to_index, &pr, err));
            break;
        }

        case TP_OP_TARGET_CREATE: {
            bool pr = false;
            TRY(j_opt_shape_id(oj, "target_id", TP_ID_KIND_TARGET, &out->u.target_create.target_id, err));
            TRY(j_opt_dup(oj, "exporter_id", &out->u.target_create.exporter_id, err));
            TRY(j_opt_dup(oj, "out_path", &out->u.target_create.out_path, err));
            out->u.target_create.enabled = true;
            TRY(j_opt_bool(oj, "enabled", &out->u.target_create.enabled, &pr, err));
            break;
        }
        case TP_OP_TARGET_REMOVE:
            TRY(j_opt_shape_id(oj, "target_id", TP_ID_KIND_TARGET, &out->u.target_ref.target_id, err));
            break;
        case TP_OP_TARGET_SET: {
            /* R2a: JSON target.set is FIELD-PRESENCE, like every other SET op (atlas.settings.set,
             * sprite.override.set, animation.settings.set): the mask reflects exactly which of
             * exporter_id / out_path / enabled are present, so tp_operation_encode -> decode round-trips
             * faithfully. The diff-recovery journal serializes committed ops via tp_txn_request_encode
             * (which emits ONLY masked fields) and replays them on recovery; the old full-replace pin
             * re-added fields the sparse wire form never carried -> silent data loss on replay. validate
             * is already mask-aware (each field is checked only when its bit is set; mask==0 is rejected),
             * and a full object (all three fields) still yields TP_TF_ALL, so the pre-R2a contract is a
             * strict subset. `out` was memset at the top of this function, so mask starts at 0. */
            tp_op_target_set *s = &out->u.target_set;
            bool pr = false;
            TRY(j_opt_shape_id(oj, "target_id", TP_ID_KIND_TARGET, &s->target_id, err));
            TRY(j_opt_dup(oj, "exporter_id", &s->exporter_id, err));
            if (s->exporter_id) s->mask |= TP_TF_EXPORTER; /* string presence == a non-NULL dup result */
            TRY(j_opt_dup(oj, "out_path", &s->out_path, err));
            if (s->out_path) s->mask |= TP_TF_OUT_PATH;
            TRY(j_opt_bool(oj, "enabled", &s->enabled, &pr, err));
            if (pr) s->mask |= TP_TF_ENABLED;
            break;
        }

        case TP_OP_INVALID:
        case TP_OP_KIND_COUNT: return tp_error_set(err, TP_STATUS_UNKNOWN_OP, "un-lowerable op kind");
    }
    return TP_STATUS_OK;
}
