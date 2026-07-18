#include "tp_history_codec_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_names.h"

typedef struct tp_hw {
    uint8_t *data;
    size_t len;
    size_t limit;
    bool overflow;
} tp_hw;



static void hw_bytes(tp_hw *w, const void *src, size_t n) {
    if (w->overflow || n > SIZE_MAX - w->len || w->len + n > w->limit) {
        w->overflow = true;
        return;
    }
    if (w->data && n) memcpy(w->data + w->len, src, n);
    w->len += n;
}

static void hw_u8(tp_hw *w, uint8_t v) { hw_bytes(w, &v, 1U); }

static void hw_u32(tp_hw *w, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 8), (uint8_t)v};
    hw_bytes(w, b, sizeof b);
}

static void hw_i32(tp_hw *w, int v) { hw_u32(w, (uint32_t)(int32_t)v); }

static void hw_u16(tp_hw *w, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    hw_bytes(w, b, sizeof b);
}

static void hw_i16(tp_hw *w, int16_t v) { hw_u16(w, (uint16_t)v); }

static void hw_bool(tp_hw *w, bool v) { hw_u8(w, v ? 1U : 0U); }

static void hw_f32(tp_hw *w, float v) {
    uint32_t bits = 0;
    _Static_assert(sizeof bits == sizeof v, "history codec needs binary32 float");
    memcpy(&bits, &v, sizeof bits);
    hw_u32(w, bits);
}

static void hw_id(tp_hw *w, tp_id128 id) { hw_bytes(w, id.bytes, sizeof id.bytes); }

static void hw_string(tp_hw *w, const char *s) {
    if (!s) {
        hw_u32(w, UINT32_MAX);
        return;
    }
    const size_t n = strlen(s);
    if (n > UINT32_MAX) {
        w->overflow = true;
        return;
    }
    hw_u32(w, (uint32_t)n);
    hw_bytes(w, s, n);
}

static bool hw_resolved_path(tp_hw *w, const tp_project *path_context,
                             const char *path) {
    char absolute[TP_IDENTITY_PATH_MAX];
    if (path_context &&
        (path_context->source_base_dir || path_context->project_dir) && path &&
        path[0] != '\0') {
        if (tp_project_resolve_source_path(path_context, path, absolute,
                                           sizeof absolute) != TP_STATUS_OK) {
            return false;
        }
        path = absolute;
    }
    hw_string(w, path);
    return !w->overflow;
}

static bool hw_count(tp_hw *w, int count) {
    if (count < 0) return false;
    hw_u32(w, (uint32_t)count);
    return !w->overflow;
}

static void hw_knobs(tp_hw *w, const tp_diff_knobs *k) {
    hw_i32(w, k->max_size);
    hw_i32(w, k->padding);
    hw_i32(w, k->margin);
    hw_i32(w, k->extrude);
    hw_i32(w, k->alpha_threshold);
    hw_i32(w, k->max_vertices);
    hw_i32(w, k->shape);
    hw_bool(w, k->allow_transform);
    hw_bool(w, k->power_of_two);
    hw_f32(w, k->pixels_per_unit);
}

static bool hw_frame(tp_hw *w, const tp_project_frame *frame) {
    hw_id(w, frame->source_ref);
    hw_string(w, frame->src_key);
    return !w->overflow;
}

static bool hw_source(tp_hw *w, const tp_project_source *source,
                      const tp_project *path_context) {
    hw_id(w, source->id);
    hw_u8(w, (uint8_t)source->kind);
    return hw_resolved_path(w, path_context, source->path);
}

static bool hw_sprite(tp_hw *w, const tp_project_sprite *sprite) {
    hw_id(w, sprite->source_ref);
    hw_string(w, sprite->src_key);
    hw_f32(w, sprite->origin_x);
    hw_f32(w, sprite->origin_y);
    for (int i = 0; i < 4; ++i) hw_u16(w, sprite->slice9_lrtb[i]);
    hw_string(w, sprite->rename);
    hw_i16(w, sprite->ov_shape);
    hw_i16(w, sprite->ov_allow_rotate);
    hw_i16(w, sprite->ov_max_vertices);
    hw_i16(w, sprite->ov_margin);
    hw_i16(w, sprite->ov_extrude);
    return !w->overflow;
}

static bool hw_anim(tp_hw *w, const tp_project_anim *anim) {
    hw_id(w, anim->id);
    hw_string(w, anim->name);
    if (!hw_count(w, anim->frame_count)) return false;
    for (int i = 0; i < anim->frame_count; ++i) {
        if (!hw_frame(w, &anim->frames[i])) return false;
    }
    hw_f32(w, anim->fps);
    hw_i32(w, anim->playback);
    hw_bool(w, anim->flip_h);
    hw_bool(w, anim->flip_v);
    return !w->overflow;
}

static bool hw_target(tp_hw *w, const tp_project_target *target) {
    hw_id(w, target->id);
    hw_string(w, target->exporter_id);
    hw_string(w, target->out_path);
    hw_bool(w, target->enabled);
    return !w->overflow;
}

static bool hw_atlas(tp_hw *w, const tp_project_atlas *atlas,
                     const tp_project *path_context) {
    hw_id(w, atlas->id);
    hw_string(w, atlas->name);
    tp_diff_knobs knobs = {
        atlas->max_size, atlas->padding, atlas->margin, atlas->extrude,
        atlas->alpha_threshold, atlas->max_vertices, atlas->shape,
        atlas->allow_transform, atlas->power_of_two, atlas->pixels_per_unit};
    hw_knobs(w, &knobs);
    if (!hw_count(w, atlas->source_count)) return false;
    for (int i = 0; i < atlas->source_count; ++i) {
        if (!hw_source(w, &atlas->sources[i], path_context)) return false;
    }
    if (!hw_count(w, atlas->sprite_count)) return false;
    for (int i = 0; i < atlas->sprite_count; ++i) {
        if (!hw_sprite(w, &atlas->sprites[i])) return false;
    }
    if (!hw_count(w, atlas->animation_count)) return false;
    for (int i = 0; i < atlas->animation_count; ++i) {
        if (!hw_anim(w, &atlas->animations[i])) return false;
    }
    if (!hw_count(w, atlas->target_count)) return false;
    for (int i = 0; i < atlas->target_count; ++i) {
        if (!hw_target(w, &atlas->targets[i])) return false;
    }
    return !w->overflow;
}

static bool hw_elem(tp_hw *w, tp_diff_coll coll, const void *elem,
                    const tp_project *path_context) {
    if (!elem) return false;
    switch (coll) {
        case TP_DIFF_COLL_ATLAS:
            return hw_atlas(w, (const tp_project_atlas *)elem, path_context);
        case TP_DIFF_COLL_SOURCE:
            return hw_source(w, (const tp_project_source *)elem, path_context);
        case TP_DIFF_COLL_ANIM:
            return hw_anim(w, (const tp_project_anim *)elem);
        case TP_DIFF_COLL_TARGET:
            return hw_target(w, (const tp_project_target *)elem);
        case TP_DIFF_COLL_FRAME:
            return hw_frame(w, (const tp_project_frame *)elem);
    }
    return false;
}

static bool encode_op(tp_hw *w, const tp_diff_op *op, bool reverse,
                      const tp_project *path_context) {
    hw_u8(w, (uint8_t)op->shape);
    switch (op->shape) {
        case TP_DIFF_SHAPE_COLL:
            hw_u8(w, (uint8_t)op->coll);
            hw_id(w, op->atlas_id);
            hw_id(w, op->anim_id);
            hw_i32(w, op->position);
            hw_bool(w, reverse ? !op->created : op->created);
            return hw_elem(w, op->coll, op->elem, path_context);
        case TP_DIFF_SHAPE_FRAME_MOVE:
            hw_id(w, op->atlas_id);
            hw_id(w, op->anim_id);
            hw_i32(w, reverse ? op->to_index : op->from_index);
            hw_i32(w, reverse ? op->from_index : op->to_index);
            return !w->overflow;
        case TP_DIFF_SHAPE_ATLAS_NAME:
            hw_id(w, op->atlas_id);
            hw_string(w, reverse ? op->name_before : op->name_after);
            return !w->overflow;
        case TP_DIFF_SHAPE_ATLAS_KNOBS:
            hw_id(w, op->atlas_id);
            hw_knobs(w, reverse ? &op->knobs_before : &op->knobs_after);
            return !w->overflow;
        case TP_DIFF_SHAPE_SOURCE_PATH:
            hw_id(w, op->atlas_id);
            hw_id(w, op->entity_id);
            return hw_resolved_path(w, path_context,
                                    reverse ? op->path_before : op->path_after);
        case TP_DIFF_SHAPE_TARGET_FIELDS:
            hw_id(w, op->atlas_id);
            hw_id(w, op->entity_id);
            hw_string(w, reverse ? op->exporter_before : op->exporter_after);
            hw_string(w, reverse ? op->out_before : op->out_after);
            hw_bool(w, reverse ? op->enabled_before : op->enabled_after);
            return !w->overflow;
        case TP_DIFF_SHAPE_ANIM_SETTINGS:
            hw_id(w, op->atlas_id);
            hw_id(w, op->anim_id);
            {
                const tp_diff_anim_settings *s =
                    reverse ? &op->anim_before : &op->anim_after;
                hw_f32(w, s->fps);
                hw_i32(w, s->playback);
                hw_bool(w, s->flip_h);
                hw_bool(w, s->flip_v);
            }
            return !w->overflow;
        case TP_DIFF_SHAPE_SPRITE_RECORD: {
            hw_id(w, op->atlas_id);
            const bool before_present =
                reverse ? op->spr_after_present : op->spr_before_present;
            const int before_index =
                reverse ? op->spr_after_index : op->spr_before_index;
            const tp_project_sprite *before =
                reverse ? &op->spr_after : &op->spr_before;
            const bool after_present =
                reverse ? op->spr_before_present : op->spr_after_present;
            const int after_index =
                reverse ? op->spr_before_index : op->spr_after_index;
            const tp_project_sprite *after =
                reverse ? &op->spr_before : &op->spr_after;
            hw_bool(w, before_present);
            hw_i32(w, before_index);
            if (before_present && !hw_sprite(w, before)) return false;
            hw_bool(w, after_present);
            hw_i32(w, after_index);
            if (after_present && !hw_sprite(w, after)) return false;
            return !w->overflow;
        }
        case TP_DIFF_SHAPE_FRAMES_LIST: {
            hw_id(w, op->atlas_id);
            hw_id(w, op->anim_id);
            const tp_project_frame *frames =
                reverse ? op->frames_before : op->frames_after;
            const int count =
                reverse ? op->frames_before_count : op->frames_after_count;
            if (!hw_count(w, count)) return false;
            for (int i = 0; i < count; ++i) {
                if (!hw_frame(w, &frames[i])) return false;
            }
            return !w->overflow;
        }
        case TP_DIFF_SHAPE_ANIM_NAME:
            hw_id(w, op->atlas_id);
            hw_id(w, op->anim_id);
            hw_string(w, reverse ? op->name_before : op->name_after);
            return !w->overflow;
        default:
            return false;
    }
}

static tp_status emit_record(tp_hw *w, const tp_diff_record *record,
                             bool reverse, const tp_project *path_context,
                             tp_history_codec_outcome *outcome, tp_error *err) {
    if (!record || record->op_count < 0 ||
        record->op_count > TP_TXN_MAX_OPS ||
        (record->op_count > 0 && !record->ops)) {
        *outcome = TP_HISTORY_CODEC_ERROR;
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid semantic history record");
    }
    /* A future effect class is the ONLY semantic reason to allow the
     * full-checkpoint fallback. Check it before writing so a tiny byte budget
     * cannot accidentally classify an unknown shape as merely oversized. */
    for (int i = 0; i < record->op_count; ++i) {
        if (record->ops[i].shape < TP_DIFF_SHAPE_COLL ||
            record->ops[i].shape > TP_DIFF_SHAPE_ANIM_NAME) {
            *outcome = TP_HISTORY_CODEC_UNSUPPORTED;
            return TP_STATUS_OK;
        }
    }
    hw_u32(w, TP_HISTORY_CODEC_VERSION);
    hw_u32(w, (uint32_t)record->op_count);
    for (int n = 0; n < record->op_count; ++n) {
        const int i = reverse ? record->op_count - 1 - n : n;
        if (!encode_op(w, &record->ops[i], reverse, path_context)) {
            if (w->overflow) {
                *outcome = TP_HISTORY_CODEC_OVERSIZED;
                return TP_STATUS_OK;
            }
            *outcome = TP_HISTORY_CODEC_ERROR;
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "invalid semantic history operation %d", i);
        }
    }
    if (w->overflow) {
        *outcome = TP_HISTORY_CODEC_OVERSIZED;
        return TP_STATUS_OK;
    }
    *outcome = TP_HISTORY_CODEC_OK;
    return TP_STATUS_OK;
}

tp_status tp_history_transition_encode(
    const tp_diff_record *record, bool reverse,
    const tp_project *path_context, size_t max_bytes,
    tp_history_transition_blob *out, tp_history_codec_outcome *outcome,
    tp_error *err) {
    if (!out || !outcome) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "null history codec output");
    }
    memset(out, 0, sizeof *out);
    *outcome = TP_HISTORY_CODEC_ERROR;
    /* Keep the durable form within the same per-history-record ceiling as its
     * in-memory semantic source. A larger journal frame is reserved for the
     * explicit checkpoint fallback, not an unbounded compact decode. */
    const size_t codec_limit =
        max_bytes < (size_t)TP_HISTORY_MAX_RECORD_BYTES
            ? max_bytes
            : (size_t)TP_HISTORY_MAX_RECORD_BYTES;
    tp_hw count = {.limit = codec_limit};
    /* The same path context is used in both passes. */
    tp_status status = emit_record(&count, record, reverse, path_context,
                                   outcome, err);
    if (status != TP_STATUS_OK || *outcome != TP_HISTORY_CODEC_OK) return status;
    uint8_t *data = (uint8_t *)malloc(count.len ? count.len : 1U);
    if (!data) {
        *outcome = TP_HISTORY_CODEC_ERROR;
        return tp_error_set(err, TP_STATUS_OOM,
                            "history transition allocation failed");
    }
    tp_hw write = {.data = data, .limit = count.len};
    status = emit_record(&write, record, reverse, path_context, outcome, err);
    if (status != TP_STATUS_OK || *outcome != TP_HISTORY_CODEC_OK ||
        write.len != count.len) {
        free(data);
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                   "history transition changed while encoding");
    }
    /* Encoding a known shape must never produce bytes the decoder rejects.
     * This catches malformed diff-owned values (for example a missing required
     * name) at the writer boundary; only unsupported/oversized records may use
     * the checkpoint fallback. */
    uint32_t validated_op_count = 0U;
    status = tp_history_transition_validate(data, write.len,
                                            &validated_op_count, err);
    if (status != TP_STATUS_OK ||
        validated_op_count != (uint32_t)record->op_count) {
        free(data);
        *outcome = TP_HISTORY_CODEC_ERROR;
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                  "history transition count changed while validating");
    }
    out->data = data;
    out->len = write.len;
    out->op_count = (uint32_t)record->op_count;
    return TP_STATUS_OK;
}

void tp_history_transition_blob_free(tp_history_transition_blob *blob) {
    if (!blob) return;
    free(blob->data);
    memset(blob, 0, sizeof *blob);
}
