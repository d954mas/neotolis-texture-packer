#include "tp_history_codec_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_names.h"
#include "tp_project_identity_internal.h"

typedef struct tp_hw {
    uint8_t *data;
    size_t len;
    size_t limit;
    bool overflow;
} tp_hw;

typedef struct tp_hr {
    const uint8_t *data;
    size_t len;
    size_t off;
    size_t allocated;
    bool oom;
} tp_hr;

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

void tp_history_transition_blob_free(tp_history_transition_blob *blob) {
    if (!blob) return;
    free(blob->data);
    memset(blob, 0, sizeof *blob);
}
