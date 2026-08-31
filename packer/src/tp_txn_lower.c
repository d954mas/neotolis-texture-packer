/*
 * A shape-valid transaction op JSON object ->
 * a typed tp_operation. This is the inverse of tp_operation_encode; it reads
 * ONLY the closed per-kind field vocabulary (tp_op_field_allowed) and derives each SET op's
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
#include "tp_op_internal.h"
#include "tp_txn_json.h"

static tp_status lower_frame_ref(const cJSON *value, tp_op_sprite_ref *out,
                                 tp_error *err) {
    if (!cJSON_IsObject(value)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "frame must be a {source_id, src_key} object");
    }
    for (const cJSON *field = value->child; field; field = field->next) {
        if (strcmp(field->string, "source_id") != 0 &&
            strcmp(field->string, "src_key") != 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "unknown frame field '%s'", field->string);
        }
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

static tp_status require_field(const cJSON *oj, const char *key,
                               tp_error *err) {
    if (!cJSON_GetObjectItemCaseSensitive(oj, key)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "missing required field \"%s\"", key);
    }
    return TP_STATUS_OK;
}

static tp_status j_req_int(const cJSON *oj, const char *key, int *out,
                           tp_error *err) {
    bool present = false;
    tp_status status = j_opt_int(oj, key, out, &present, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    return present ? TP_STATUS_OK
                   : tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                  "missing required field \"%s\"", key);
}

/* fields[] token array -> sprite override mask. The token vocabulary is closed:
 * accepting a token that the encoder cannot reproduce would make journal replay
 * differ from the recorded mutation. */
static tp_status lower_clear_mask(const cJSON *oj, uint32_t *out,
                                  tp_error *err) {
    *out = 0U;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(oj, "fields");
    if (!cJSON_IsArray(arr)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "fields must be an array");
    }
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        const cJSON *el = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(el)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "fields[%d] must be a string", i);
        }
        uint32_t bit = 0U;
        if (!tp_op__sprite_clear_bit(el->valuestring, &bit)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "unknown sprite override field '%s'",
                                el->valuestring);
        }
        *out |= bit;
    }
    return TP_STATUS_OK;
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

/* One registry walk lowers every field-presence SET family: read each row's key
 * in ROW ORDER (which fixes precedence when several fields carry a value fault),
 * derive the presence mask from what was actually present, and enforce the
 * all-or-none arity of a grouped bit at the end of its run. */
static tp_status lower_fields(const cJSON *oj, tp_field_family family,
                              void *payload, uint32_t *mask, tp_error *err) {
    size_t count = 0U;
    const tp_field_row *rows = tp_op_field_rows(family, &count);
    unsigned run_present = 0U;
    unsigned run_total = 0U;
    for (size_t i = 0U; i < count; i++) {
        const tp_field_row *row = &rows[i];
        void *slot = (char *)payload + row->op_off;
        bool present = false;
        tp_status st = TP_STATUS_OK;
        switch (row->type) {
            case TP_FIELD_INT:
            case TP_FIELD_INT_I16:
            case TP_FIELD_INT_U16:
                st = j_opt_int(oj, row->key, (int *)slot, &present, err);
                break;
            case TP_FIELD_BOOL:
                st = j_opt_bool(oj, row->key, (bool *)slot, &present, err);
                break;
            case TP_FIELD_FLOAT:
                st = j_opt_float(oj, row->key, (float *)slot, &present, err);
                break;
            case TP_FIELD_STR:
                st = j_opt_dup(oj, row->key, (char **)slot, err);
                present = (*(char **)slot != NULL); /* a dup result IS the presence */
                break;
        }
        if (st != TP_STATUS_OK) {
            return st;
        }
        run_total++;
        if (present) {
            run_present++;
            *mask |= row->bit;
        }
        if (i + 1U < count && rows[i + 1U].bit == row->bit) {
            continue; /* the run continues */
        }
        if (row->group && run_present != 0U && run_present != run_total) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "%s must be provided together", row->group);
        }
        run_present = 0U;
        run_total = 0U;
    }
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
        case TP_OP_ATLAS_SETTINGS_SET:
            TRY(lower_fields(oj, TP_FIELD_FAMILY_ATLAS, &out->u.atlas_settings,
                             &out->u.atlas_settings.mask, err));
            break;

        case TP_OP_SOURCE_ADD: {
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.source_add.source_id, err));
            TRY(j_opt_dup(oj, "key", &out->u.source_add.key, err));
            const cJSON *kind = cJSON_GetObjectItemCaseSensitive(oj, "kind");
            out->u.source_add.kind = TP_SOURCE_KIND_FOLDER;
            if (!kind) {
                break;
            }
            if (!cJSON_IsString(kind)) {
                TRY(tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                 "\"kind\" must be a string"));
            }
            if (strcmp(kind->valuestring, "file") == 0) {
                out->u.source_add.kind = TP_SOURCE_KIND_FILE;
            } else if (strcmp(kind->valuestring, "folder") != 0) {
                TRY(tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                 "unknown source kind '%s'", kind->valuestring));
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
            TRY(lower_fields(oj, TP_FIELD_FAMILY_SPRITE, &out->u.sprite_set,
                             &out->u.sprite_set.mask, err));
            break;
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.sprite_clear.source_id, err));
            TRY(j_opt_dup(oj, "src_key", &out->u.sprite_clear.src_key, err));
            TRY(lower_clear_mask(oj, &out->u.sprite_clear.mask, err));
            break;
        case TP_OP_SPRITE_NAME_SET:
            TRY(j_opt_shape_id(oj, "source_id", TP_ID_KIND_SOURCE, &out->u.sprite_name.source_id, err));
            TRY(j_opt_dup(oj, "src_key", &out->u.sprite_name.src_key, err));
            TRY(require_field(oj, "name", err));
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
            TRY(lower_fields(oj, TP_FIELD_FAMILY_ANIM, &out->u.anim_settings,
                             &out->u.anim_settings.mask, err));
            break;
        case TP_OP_ANIMATION_FRAMES_SET:
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_frames_set.anim_id, err));
            TRY(require_field(oj, "frames", err));
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
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_frame_rm.anim_id, err));
            TRY(j_req_int(oj, "index", &out->u.anim_frame_rm.index, err));
            break;
        }
        case TP_OP_ANIMATION_FRAME_MOVE: {
            TRY(j_opt_shape_id(oj, "anim_id", TP_ID_KIND_ANIM, &out->u.anim_frame_move.anim_id, err));
            TRY(j_req_int(oj, "from_index", &out->u.anim_frame_move.from_index, err));
            TRY(j_req_int(oj, "to_index", &out->u.anim_frame_move.to_index, err));
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
            TRY(j_opt_shape_id(oj, "target_id", TP_ID_KIND_TARGET, &s->target_id, err));
            TRY(lower_fields(oj, TP_FIELD_FAMILY_TARGET, s, &s->mask, err));
            break;
        }

        case TP_OP_INVALID:
        case TP_OP_KIND_COUNT: return tp_error_set(err, TP_STATUS_UNKNOWN_OP, "un-lowerable op kind");
    }
    return TP_STATUS_OK;
}
