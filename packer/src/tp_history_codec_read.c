#include "tp_history_codec_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_names.h"
#include "tp_project_identity_internal.h"
typedef struct tp_hr {
    const uint8_t *data;
    size_t len;
    size_t off;
    size_t allocated;
    bool oom;
} tp_hr;

static bool hr_bytes(tp_hr *r, void *dst, size_t n) {
    if (n > r->len - r->off) return false;
    if (dst && n) memcpy(dst, r->data + r->off, n);
    r->off += n;
    return true;
}

static bool hr_u8(tp_hr *r, uint8_t *v) { return hr_bytes(r, v, 1U); }

static bool hr_u32(tp_hr *r, uint32_t *v) {
    uint8_t b[4];
    if (!hr_bytes(r, b, sizeof b)) return false;
    *v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
         ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    return true;
}

static bool hr_i32(tp_hr *r, int *v) {
    uint32_t bits = 0;
    if (!hr_u32(r, &bits)) return false;
    *v = (int)(int32_t)bits;
    return true;
}

static bool hr_u16(tp_hr *r, uint16_t *v) {
    uint8_t b[2];
    if (!hr_bytes(r, b, sizeof b)) return false;
    *v = (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
    return true;
}

static bool hr_i16(tp_hr *r, int16_t *v) {
    uint16_t bits = 0;
    if (!hr_u16(r, &bits)) return false;
    *v = (int16_t)bits;
    return true;
}

static bool hr_bool(tp_hr *r, bool *v) {
    uint8_t byte = 0;
    if (!hr_u8(r, &byte) || byte > 1U) return false;
    *v = byte != 0U;
    return true;
}

static bool hr_f32(tp_hr *r, float *v) {
    uint32_t bits = 0;
    if (!hr_u32(r, &bits)) return false;
    memcpy(v, &bits, sizeof bits);
    return isfinite(*v) != 0;
}

static bool hr_id(tp_hr *r, tp_id128 *id) {
    return hr_bytes(r, id->bytes, sizeof id->bytes);
}

static bool hr_string(tp_hr *r, char **out) {
    uint32_t n = 0;
    *out = NULL;
    if (!hr_u32(r, &n)) return false;
    if (n == UINT32_MAX) return true;
    if ((size_t)n > r->len - r->off) return false;
    if (memchr(r->data + r->off, '\0', (size_t)n)) return false;
    if ((size_t)n + 1U > (size_t)TP_HISTORY_MAX_RECORD_BYTES - r->allocated) {
        return false;
    }
    char *s = (char *)malloc((size_t)n + 1U);
    if (!s) {
        r->oom = true;
        return false;
    }
    r->allocated += (size_t)n + 1U;
    memcpy(s, r->data + r->off, (size_t)n);
    s[n] = '\0';
    r->off += (size_t)n;
    *out = s;
    return true;
}

static void *hr_alloc(tp_hr *r, size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    const size_t bytes = count * size;
    if (bytes > (size_t)TP_HISTORY_MAX_RECORD_BYTES - r->allocated) return NULL;
    void *p = calloc(count ? count : 1U, size);
    if (!p) {
        r->oom = true;
        return NULL;
    }
    r->allocated += bytes;
    return p;
}

static bool hr_count(tp_hr *r, int *count) {
    uint32_t value = 0;
    if (!hr_u32(r, &value) || value > (uint32_t)INT_MAX) return false;
    *count = (int)value;
    return true;
}

static bool hr_derived_name(tp_hr *r, const char *key, char **out) {
    if (!key || key[0] == '\0') return false;
    const size_t n = strlen(key) + 1U;
    char *name = (char *)hr_alloc(r, n, 1U);
    if (!name) return false;
    tp_sprite_export_key(key, name, n);
    if (name[0] == '\0') {
        free(name);
        return false;
    }
    *out = name;
    return true;
}

static bool hr_knobs(tp_hr *r, tp_diff_knobs *k) {
    return hr_i32(r, &k->max_size) && hr_i32(r, &k->padding) &&
           hr_i32(r, &k->margin) && hr_i32(r, &k->extrude) &&
           hr_i32(r, &k->alpha_threshold) && hr_i32(r, &k->max_vertices) &&
           hr_i32(r, &k->shape) && hr_bool(r, &k->allow_transform) &&
           hr_bool(r, &k->power_of_two) && hr_f32(r, &k->pixels_per_unit);
}

static bool hr_frame(tp_hr *r, tp_project_frame *frame) {
    return hr_id(r, &frame->source_ref) &&
           !tp_id128_is_nil(frame->source_ref) &&
           hr_string(r, &frame->src_key) && frame->src_key &&
           hr_derived_name(r, frame->src_key, &frame->name);
}

static bool hr_source(tp_hr *r, tp_project_source *source) {
    uint8_t kind = 0;
    if (!hr_id(r, &source->id) || !hr_u8(r, &kind) || kind > 1U ||
        !hr_string(r, &source->path) || !source->path) {
        return false;
    }
    source->kind = (tp_source_kind)kind;
    return true;
}

static bool hr_sprite(tp_hr *r, tp_project_sprite *sprite) {
    if (!hr_id(r, &sprite->source_ref) ||
        tp_id128_is_nil(sprite->source_ref) ||
        !hr_string(r, &sprite->src_key) || !sprite->src_key ||
        !hr_derived_name(r, sprite->src_key, &sprite->name) ||
        !hr_f32(r, &sprite->origin_x) || !hr_f32(r, &sprite->origin_y)) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!hr_u16(r, &sprite->slice9_lrtb[i])) return false;
    }
    return hr_string(r, &sprite->rename) && hr_i16(r, &sprite->ov_shape) &&
           hr_i16(r, &sprite->ov_allow_rotate) &&
           hr_i16(r, &sprite->ov_max_vertices) &&
           hr_i16(r, &sprite->ov_margin) && hr_i16(r, &sprite->ov_extrude);
}

static bool hr_anim(tp_hr *r, tp_project_anim *anim) {
    if (!hr_id(r, &anim->id) || !hr_string(r, &anim->name) || !anim->name ||
        !hr_count(r, &anim->frame_count)) {
        return false;
    }
    anim->frame_cap = anim->frame_count;
    if (anim->frame_count > 0) {
        anim->frames = (tp_project_frame *)hr_alloc(
            r, (size_t)anim->frame_count, sizeof *anim->frames);
        if (!anim->frames) return false;
        for (int i = 0; i < anim->frame_count; ++i) {
            if (!hr_frame(r, &anim->frames[i])) {
                anim->frame_count = i + 1;
                return false;
            }
        }
    }
    return hr_f32(r, &anim->fps) && hr_i32(r, &anim->playback) &&
           hr_bool(r, &anim->flip_h) && hr_bool(r, &anim->flip_v);
}

static bool hr_target(tp_hr *r, tp_project_target *target) {
    return hr_id(r, &target->id) &&
           hr_string(r, &target->exporter_id) && target->exporter_id &&
           hr_string(r, &target->out_path) && target->out_path &&
           hr_bool(r, &target->enabled);
}

static bool hr_atlas(tp_hr *r, tp_project_atlas *atlas) {
    tp_diff_knobs knobs = {0};
    if (!hr_id(r, &atlas->id) || !hr_string(r, &atlas->name) || !atlas->name ||
        !hr_knobs(r, &knobs)) {
        return false;
    }
    atlas->max_size = knobs.max_size;
    atlas->padding = knobs.padding;
    atlas->margin = knobs.margin;
    atlas->extrude = knobs.extrude;
    atlas->alpha_threshold = knobs.alpha_threshold;
    atlas->max_vertices = knobs.max_vertices;
    atlas->shape = knobs.shape;
    atlas->allow_transform = knobs.allow_transform;
    atlas->power_of_two = knobs.power_of_two;
    atlas->pixels_per_unit = knobs.pixels_per_unit;

    if (!hr_count(r, &atlas->source_count)) return false;
    atlas->source_cap = atlas->source_count;
    if (atlas->source_count > 0) {
        atlas->sources = (tp_project_source *)hr_alloc(
            r, (size_t)atlas->source_count, sizeof *atlas->sources);
        if (!atlas->sources) return false;
        for (int i = 0; i < atlas->source_count; ++i) {
            if (!hr_source(r, &atlas->sources[i])) {
                atlas->source_count = i + 1;
                return false;
            }
        }
    }
    if (!hr_count(r, &atlas->sprite_count)) return false;
    atlas->sprite_cap = atlas->sprite_count;
    if (atlas->sprite_count > 0) {
        atlas->sprites = (tp_project_sprite *)hr_alloc(
            r, (size_t)atlas->sprite_count, sizeof *atlas->sprites);
        if (!atlas->sprites) return false;
        for (int i = 0; i < atlas->sprite_count; ++i) {
            if (!hr_sprite(r, &atlas->sprites[i])) {
                atlas->sprite_count = i + 1;
                return false;
            }
        }
    }
    if (!hr_count(r, &atlas->animation_count)) return false;
    atlas->animation_cap = atlas->animation_count;
    if (atlas->animation_count > 0) {
        atlas->animations = (tp_project_anim *)hr_alloc(
            r, (size_t)atlas->animation_count, sizeof *atlas->animations);
        if (!atlas->animations) return false;
        for (int i = 0; i < atlas->animation_count; ++i) {
            if (!hr_anim(r, &atlas->animations[i])) {
                atlas->animation_count = i + 1;
                return false;
            }
        }
    }
    if (!hr_count(r, &atlas->target_count)) return false;
    atlas->target_cap = atlas->target_count;
    if (atlas->target_count > 0) {
        atlas->targets = (tp_project_target *)hr_alloc(
            r, (size_t)atlas->target_count, sizeof *atlas->targets);
        if (!atlas->targets) return false;
        for (int i = 0; i < atlas->target_count; ++i) {
            if (!hr_target(r, &atlas->targets[i])) {
                atlas->target_count = i + 1;
                return false;
            }
        }
    }
    return true;
}

static bool hr_elem(tp_hr *r, tp_diff_op *op) {
    size_t size = 0U;
    switch (op->coll) {
        case TP_DIFF_COLL_ATLAS: size = sizeof(tp_project_atlas); break;
        case TP_DIFF_COLL_SOURCE: size = sizeof(tp_project_source); break;
        case TP_DIFF_COLL_ANIM: size = sizeof(tp_project_anim); break;
        case TP_DIFF_COLL_TARGET: size = sizeof(tp_project_target); break;
        case TP_DIFF_COLL_FRAME: size = sizeof(tp_project_frame); break;
        default: return false;
    }
    op->elem = hr_alloc(r, 1U, size);
    if (!op->elem) return false;
    switch (op->coll) {
        case TP_DIFF_COLL_ATLAS:
            return hr_atlas(r, (tp_project_atlas *)op->elem);
        case TP_DIFF_COLL_SOURCE:
            return hr_source(r, (tp_project_source *)op->elem);
        case TP_DIFF_COLL_ANIM:
            return hr_anim(r, (tp_project_anim *)op->elem);
        case TP_DIFF_COLL_TARGET:
            return hr_target(r, (tp_project_target *)op->elem);
        case TP_DIFF_COLL_FRAME:
            return hr_frame(r, (tp_project_frame *)op->elem);
    }
    return false;
}

static bool hr_frames(tp_hr *r, tp_project_frame **out, int *out_count) {
    int count = 0;
    *out = NULL;
    *out_count = 0;
    if (!hr_count(r, &count)) return false;
    if (count == 0) return true;
    tp_project_frame *frames =
        (tp_project_frame *)hr_alloc(r, (size_t)count, sizeof *frames);
    if (!frames) return false;
    *out = frames;
    for (int i = 0; i < count; ++i) {
        *out_count = i + 1;
        if (!hr_frame(r, &frames[i])) return false;
    }
    return true;
}

static bool hr_required_id(tp_hr *r, tp_id128 *id) {
    return hr_id(r, id) && !tp_id128_is_nil(*id);
}

static bool decode_op(tp_hr *r, tp_diff_op *op) {
    uint8_t shape = 0;
    if (!hr_u8(r, &shape) || shape > (uint8_t)TP_DIFF_SHAPE_ANIM_NAME) {
        return false;
    }
    op->shape = (tp_diff_shape)shape;
    switch (op->shape) {
        case TP_DIFF_SHAPE_COLL: {
            uint8_t coll = 0;
            if (!hr_u8(r, &coll) || coll > (uint8_t)TP_DIFF_COLL_FRAME) {
                return false;
            }
            op->coll = (tp_diff_coll)coll;
            return hr_id(r, &op->atlas_id) && hr_id(r, &op->anim_id) &&
                   hr_i32(r, &op->position) && op->position >= 0 &&
                   hr_bool(r, &op->created) && hr_elem(r, op);
        }
        case TP_DIFF_SHAPE_FRAME_MOVE:
            return hr_required_id(r, &op->atlas_id) &&
                   hr_required_id(r, &op->anim_id) &&
                   hr_i32(r, &op->from_index) && op->from_index >= 0 &&
                   hr_i32(r, &op->to_index) && op->to_index >= 0;
        case TP_DIFF_SHAPE_ATLAS_NAME:
            return hr_required_id(r, &op->atlas_id) &&
                   hr_string(r, &op->name_after) && op->name_after &&
                   op->name_after[0] != '\0';
        case TP_DIFF_SHAPE_ATLAS_KNOBS:
            return hr_required_id(r, &op->atlas_id) &&
                   hr_knobs(r, &op->knobs_after);
        case TP_DIFF_SHAPE_SOURCE_PATH:
            return hr_required_id(r, &op->atlas_id) &&
                   hr_required_id(r, &op->entity_id) &&
                   hr_string(r, &op->path_after) && op->path_after &&
                   op->path_after[0] != '\0';
        case TP_DIFF_SHAPE_TARGET_FIELDS:
            return hr_required_id(r, &op->atlas_id) &&
                   hr_required_id(r, &op->entity_id) &&
                   hr_string(r, &op->exporter_after) && op->exporter_after &&
                   op->exporter_after[0] != '\0' &&
                   hr_string(r, &op->out_after) && op->out_after &&
                   op->out_after[0] != '\0' &&
                   hr_bool(r, &op->enabled_after);
        case TP_DIFF_SHAPE_ANIM_SETTINGS:
            return hr_required_id(r, &op->atlas_id) &&
                   hr_required_id(r, &op->anim_id) &&
                   hr_f32(r, &op->anim_after.fps) &&
                   hr_i32(r, &op->anim_after.playback) &&
                   hr_bool(r, &op->anim_after.flip_h) &&
                   hr_bool(r, &op->anim_after.flip_v);
        case TP_DIFF_SHAPE_SPRITE_RECORD:
            if (!hr_required_id(r, &op->atlas_id) ||
                !hr_bool(r, &op->spr_before_present) ||
                !hr_i32(r, &op->spr_before_index) ||
                (op->spr_before_present &&
                 (op->spr_before_index < 0 ||
                  !hr_sprite(r, &op->spr_before))) ||
                !hr_bool(r, &op->spr_after_present) ||
                !hr_i32(r, &op->spr_after_index) ||
                (op->spr_after_present &&
                 (op->spr_after_index < 0 ||
                  !hr_sprite(r, &op->spr_after)))) {
                return false;
            }
            return true;
        case TP_DIFF_SHAPE_FRAMES_LIST:
            return hr_required_id(r, &op->atlas_id) &&
                   hr_required_id(r, &op->anim_id) &&
                   hr_frames(r, &op->frames_after,
                             &op->frames_after_count);
        case TP_DIFF_SHAPE_ANIM_NAME:
            return hr_required_id(r, &op->atlas_id) &&
                   hr_required_id(r, &op->anim_id) &&
                   hr_string(r, &op->name_after) && op->name_after &&
                   op->name_after[0] != '\0';
    }
    return false;
}

static tp_status decode_and_apply(tp_project *project, const uint8_t *data,
                                  size_t len, bool apply,
                                  uint32_t *op_count, tp_error *err) {
    if ((!data && len) || !op_count) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid history transition input");
    }
    tp_hr r = {.data = data, .len = len};
    uint32_t version = 0, count = 0;
    if (!hr_u32(&r, &version) || !hr_u32(&r, &count)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "truncated history transition header");
    }
    if (version != TP_HISTORY_CODEC_VERSION) {
        return tp_error_set(err, TP_STATUS_BAD_VERSION,
                            "unsupported history transition version %u", version);
    }
    if (count > (uint32_t)TP_TXN_MAX_OPS) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "history transition operation count is too large");
    }
    for (uint32_t i = 0; i < count; ++i) {
        tp_diff_op op;
        memset(&op, 0, sizeof op);
        if (!decode_op(&r, &op)) {
            tp_diff_op_free(&op);
            return tp_error_set(
                err, r.oom ? TP_STATUS_OOM : TP_STATUS_INVALID_ARGUMENT,
                "invalid history transition operation %u", i);
        }
        if (apply) {
            tp_status status = tp_diff_op_apply(project, &op, false, err);
            tp_diff_op_free(&op);
            if (status != TP_STATUS_OK) return status;
        } else {
            tp_diff_op_free(&op);
        }
    }
    if (r.off != r.len) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "history transition has trailing bytes");
    }
    *op_count = count;
    return TP_STATUS_OK;
}

tp_status tp_history_transition_validate(const uint8_t *data, size_t len,
                                         uint32_t *op_count, tp_error *err) {
    return decode_and_apply(NULL, data, len, false, op_count, err);
}

tp_status tp_history_transition_apply(tp_project *project, const uint8_t *data,
                                      size_t len, uint32_t *op_count,
                                      tp_error *err) {
    uint32_t count = 0;
    if (!project) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "null project for history transition");
    }
    tp_project *staged = tp_project_clone(project);
    if (!staged) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "history transition staging clone failed");
    }
    tp_status status = decode_and_apply(staged, data, len, true, &count, err);
    if (status == TP_STATUS_OK) {
        /* Decoding enforces wire shape only. Validate the completed staged
         * graph once before the atomic swap so a CRC-valid hostile transition
         * cannot inject noncanonical keys, derived names, or dangling refs. */
        status = tp_project_validate_canonical(staged, err);
    }
    if (status != TP_STATUS_OK) {
        tp_project_destroy(staged);
        return status;
    }
    tp_project old = *project;
    *project = *staged;
    *staged = old;
    tp_project_destroy(staged);
    if (op_count) *op_count = count;
    return TP_STATUS_OK;
}

tp_status tp_history_transition_apply_disposable(
    tp_project *project, const uint8_t *data, size_t len, uint32_t *op_count,
    tp_error *err) {
    uint32_t count = 0U;
    if (!project) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "null disposable project for history transition");
    }
    /* Recovery preflights every frame before it constructs this project. This
     * intentionally avoids a full clone per HISTORY record; on a semantic
     * replay failure the caller destroys the whole recovery candidate. */
    tp_status status = decode_and_apply(project, data, len, true, &count, err);
    if (status == TP_STATUS_OK && op_count) {
        *op_count = count;
    }
    return status;
}
