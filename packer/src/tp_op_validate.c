/*
 * Operation payload + reference validation moved INTO core.
 * The range / name / exporter-id / reference rules that were duplicated in
 * apps/cli/cli_mutate.c (CLI-specific) and the apps/gui wrappers now live here,
 * once, producing a STRUCTURED status id + offending field + context (not prose).
 * Pure: never mutates `p`. Bounds-checked, UB-clean on arbitrary payloads.
 */

#include "tp_core/tp_operation.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "tp_core/tp_export.h"  /* tp_exporter_find (exporter-id validation) */
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_srckey.h"
#include "tp_op_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_source_plan_internal.h"
#include "tp_strutil.h"
#include "tp_utf8_internal.h"

/* Reject NaN / +-inf; `pos` also rejects <= 0. No libm: comparisons only. */
static bool finite_any(float v) { return v == v && v >= -FLT_MAX && v <= FLT_MAX; }
static bool finite_pos(float v) { return v > 0.0F && v <= FLT_MAX; }

static const tp_project_atlas *find_atlas(const tp_project *p, tp_id128 id) {
    int ai = tp_project_find_atlas_by_id(p, id);
    return ai < 0 ? NULL : &p->atlases[ai];
}
/* Index of the atlas named `name` (exact, case-sensitive), or -1. Mirrors the CLI's
 * resolve_atlas so name-uniqueness rejections match the CLI oracle byte-for-byte. */
static int find_atlas_by_name(const tp_project *p, const char *name) {
    for (int i = 0; i < p->atlas_count; i++) {
        if (p->atlases[i].name && strcmp(p->atlases[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static tp_status validate_atlas_name_shape(const char *name,
                                           tp_op_reject *rej) {
    if (!name || name[0] == '\0') {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name",
                             "atlas name must be non-empty");
    }
    bool only_dots = true;
    for (const char *c = name; *c; ++c) {
        if (*c == '/' || *c == '\\') {
            return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name",
                                 "atlas name must not contain path separators");
        }
        if (*c != '.') {
            only_dots = false;
        }
    }
    return only_dots
               ? tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name",
                               "atlas name must not be dots-only")
               : TP_STATUS_OK;
}
/* const adapters over the public id-addressed accessors: validate holds a
 * const project; the accessors take non-const and only READ, so the cast is safe. This
 * removes the lookup loops that duplicated the tp_project_atlas_find_*_by_id accessors. */
static const tp_project_source *find_source(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_source_by_id((tp_project_atlas *)a, id);
}
static const tp_project_anim *find_anim(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_animation_by_id((tp_project_atlas *)a, id);
}
static const tp_project_target *find_target(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_target_by_id((tp_project_atlas *)a, id);
}

static bool source_is_referenced(const tp_project_atlas *atlas,
                                 tp_id128 source_id) {
    for (int i = 0; i < atlas->sprite_count; i++) {
        if (tp_id128_eq(atlas->sprites[i].source_ref, source_id)) {
            return true;
        }
    }
    for (int i = 0; i < atlas->animation_count; i++) {
        const tp_project_anim *animation = &atlas->animations[i];
        for (int f = 0; f < animation->frame_count; f++) {
            if (tp_id128_eq(animation->frames[f].source_ref, source_id)) {
                return true;
            }
        }
    }
    return false;
}

/* Range-check one integer knob; returns OK or an OUT_OF_RANGE rejection. */
static tp_status range_i(tp_op_reject *rej, const char *field, long v, long lo, long hi) {
    if (v < lo || v > hi) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, field, "%s = %ld must be in [%ld..%ld]", field, v, lo, hi);
    }
    return TP_STATUS_OK;
}

static tp_status validate_persisted_utf8(const char *value, const char *field,
                                         tp_op_reject *reject) {
    if (!value) {
        return TP_STATUS_OK; /* Required/nullable semantics stay with each op. */
    }
    tp_error error = {{0}};
    const tp_status status = tp_utf8_validate_c_string(
        value, TP_STATUS_INVALID_UTF8, field, &error);
    return status == TP_STATUS_OK
               ? TP_STATUS_OK
               : tp_op__reject(reject, TP_STATUS_INVALID_UTF8, field, "%s",
                               error.msg);
}

static tp_status validate_exporter_id(const char *value,
                                      tp_op_reject *reject) {
    tp_error error = {0};
    const tp_status status = tp_exporter_id_validate(value, &error);
    return status == TP_STATUS_OK
               ? TP_STATUS_OK
               : tp_op__reject(reject, status, "exporter_id", "%s",
                               error.msg);
}

/* Validate every string that the active operation can persist before path
 * canonicalization, mutation, or integer/string encoding. Unmasked payload
 * storage is deliberately ignored: field-presence means it is not data. */
static tp_status validate_operation_utf8(const tp_operation *operation,
                                         tp_op_reject *reject) {
    tp_status status = TP_STATUS_OK;
#define CHECK_UTF8(value, field)                                                \
    do {                                                                        \
        status = validate_persisted_utf8((value), (field), reject);             \
        if (status != TP_STATUS_OK) {                                            \
            return status;                                                      \
        }                                                                       \
    } while (0)
    switch (operation->kind) {
        case TP_OP_ATLAS_CREATE:
            CHECK_UTF8(operation->u.atlas_create.name, "name");
            break;
        case TP_OP_ATLAS_RENAME:
            CHECK_UTF8(operation->u.atlas_rename.name, "name");
            break;
        case TP_OP_SOURCE_ADD:
            CHECK_UTF8(operation->u.source_add.key, "key");
            break;
        case TP_OP_SOURCE_REPLACE:
            CHECK_UTF8(operation->u.source_ref.key, "key");
            break;
        case TP_OP_SPRITE_OVERRIDE_SET:
            CHECK_UTF8(operation->u.sprite_set.src_key, "src_key");
            break;
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            CHECK_UTF8(operation->u.sprite_clear.src_key, "src_key");
            break;
        case TP_OP_SPRITE_NAME_SET:
            CHECK_UTF8(operation->u.sprite_name.src_key, "src_key");
            CHECK_UTF8(operation->u.sprite_name.name, "name");
            break;
        case TP_OP_ANIMATION_CREATE:
            CHECK_UTF8(operation->u.anim_create.name, "name");
            if (operation->u.anim_create.frames &&
                operation->u.anim_create.frame_count > 0) {
                for (int i = 0; i < operation->u.anim_create.frame_count; i++) {
                    CHECK_UTF8(operation->u.anim_create.frames[i].src_key,
                               "frames");
                }
            }
            break;
        case TP_OP_ANIMATION_RENAME:
            CHECK_UTF8(operation->u.anim_rename.name, "name");
            break;
        case TP_OP_ANIMATION_FRAMES_SET:
            if (operation->u.anim_frames_set.frames &&
                operation->u.anim_frames_set.frame_count > 0) {
                for (int i = 0;
                     i < operation->u.anim_frames_set.frame_count; i++) {
                    CHECK_UTF8(
                        operation->u.anim_frames_set.frames[i].src_key,
                        "frames");
                }
            }
            break;
        case TP_OP_ANIMATION_FRAME_ADD:
            CHECK_UTF8(operation->u.anim_frame_add.frame.src_key, "frame");
            break;
        case TP_OP_TARGET_CREATE:
            CHECK_UTF8(operation->u.target_create.exporter_id, "exporter_id");
            CHECK_UTF8(operation->u.target_create.out_path, "out_path");
            break;
        case TP_OP_TARGET_SET:
            if (operation->u.target_set.mask & TP_TF_EXPORTER) {
                CHECK_UTF8(operation->u.target_set.exporter_id, "exporter_id");
            }
            if (operation->u.target_set.mask & TP_TF_OUT_PATH) {
                CHECK_UTF8(operation->u.target_set.out_path, "out_path");
            }
            break;
        case TP_OP_INVALID:
        case TP_OP_ATLAS_REMOVE:
        case TP_OP_ATLAS_SETTINGS_SET:
        case TP_OP_SOURCE_REMOVE:
        case TP_OP_ANIMATION_REMOVE:
        case TP_OP_ANIMATION_SETTINGS_SET:
        case TP_OP_ANIMATION_FRAME_REMOVE:
        case TP_OP_ANIMATION_FRAME_MOVE:
        case TP_OP_TARGET_REMOVE:
        case TP_OP_KIND_COUNT:
            break;
    }
#undef CHECK_UTF8
    return TP_STATUS_OK;
}

/* atlas.settings.set: check every masked knob against its range. */
static tp_status validate_atlas_settings(const tp_project_atlas *atlas,
                                         const tp_op_atlas_settings *s,
                                         tp_op_reject *rej) {
    tp_status st = TP_STATUS_OK;
    if (s->mask == 0) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "atlas.settings.set names no field");
    }
    if ((s->mask & ~(uint32_t)TP_AF_ALL) != 0U) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "",
                             "atlas.settings.set contains unknown field bits");
    }
    if ((s->mask & TP_AF_MAX_SIZE) && !tp_pack_max_size_valid(s->max_size)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "max_size",
                             "max_size = %d must be in [1..%d]", s->max_size,
                             TP_PACK_MAX_PAGE_DIM);
    }
    const int effective_max_size = (s->mask & TP_AF_MAX_SIZE)
                                       ? s->max_size
                                       : atlas->max_size;
    if (!tp_pack_max_size_valid(effective_max_size)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "max_size",
                             "effective max_size = %d must be in [1..%d]",
                             effective_max_size, TP_PACK_MAX_PAGE_DIM);
    }
    if ((s->mask & TP_AF_PADDING) &&
        (st = range_i(rej, "padding", s->padding, 0,
                      effective_max_size))) {
        return st;
    }
    if ((s->mask & TP_AF_MARGIN) &&
        (st = range_i(rej, "margin", s->margin, 0,
                      effective_max_size))) {
        return st;
    }
    if ((s->mask & TP_AF_EXTRUDE) &&
        (st = range_i(rej, "extrude", s->extrude, 0,
                      effective_max_size))) {
        return st;
    }

    const int effective_padding = (s->mask & TP_AF_PADDING)
                                      ? s->padding
                                      : atlas->padding;
    const int effective_margin = (s->mask & TP_AF_MARGIN)
                                     ? s->margin
                                     : atlas->margin;
    const int effective_extrude = (s->mask & TP_AF_EXTRUDE)
                                      ? s->extrude
                                      : atlas->extrude;
    if (effective_padding < 0 || effective_padding > effective_max_size) {
        const char *field = (s->mask & TP_AF_PADDING) ? "padding"
                                                      : "max_size";
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, field,
            "effective padding = %d must be in [0..%d]",
            effective_padding, effective_max_size);
    }
    if (effective_margin < 0 || effective_margin > effective_max_size) {
        const char *field = (s->mask & TP_AF_MARGIN) ? "margin"
                                                     : "max_size";
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, field,
            "effective margin = %d must be in [0..%d]", effective_margin,
            effective_max_size);
    }
    if (effective_extrude < 0 || effective_extrude > effective_max_size) {
        const char *field = (s->mask & TP_AF_EXTRUDE) ? "extrude"
                                                      : "max_size";
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, field,
            "effective extrude = %d must be in [0..%d]",
            effective_extrude, effective_max_size);
    }
    if (s->mask & TP_AF_MAX_SIZE) {
        for (int i = 0; i < atlas->sprite_count; ++i) {
            const tp_project_sprite *sprite = &atlas->sprites[i];
            if (sprite->ov_margin != TP_PROJECT_OV_INHERIT &&
                sprite->ov_margin > effective_max_size) {
                return tp_op__reject(
                    rej, TP_STATUS_OUT_OF_RANGE, "max_size",
                    "max_size = %d is smaller than sprite margin override %d",
                    effective_max_size, sprite->ov_margin);
            }
            if (sprite->ov_extrude != TP_PROJECT_OV_INHERIT &&
                sprite->ov_extrude > effective_max_size) {
                return tp_op__reject(
                    rej, TP_STATUS_OUT_OF_RANGE, "max_size",
                    "max_size = %d is smaller than sprite extrude override %d",
                    effective_max_size, sprite->ov_extrude);
            }
        }
    }
    if ((s->mask & TP_AF_ALPHA_THRESHOLD) &&
        !tp_pack_alpha_threshold_valid(s->alpha_threshold)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "alpha_threshold",
                             "alpha_threshold = %d must be in [0..%d]",
                             s->alpha_threshold, TP_PACK_ALPHA_MAX);
    }
    if ((s->mask & TP_AF_MAX_VERTICES) &&
        !tp_pack_max_vertices_valid(s->max_vertices)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "max_vertices",
                             "max_vertices = %d must be in [1..%d]",
                             s->max_vertices, TP_PACK_MAX_VERTICES);
    }
    if ((s->mask & TP_AF_SHAPE) && !tp_pack_shape_valid(s->shape)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "shape",
                             "shape = %d must be in [%d..%d]", s->shape,
                             TP_PACK_SHAPE_MIN, TP_PACK_SHAPE_MAX);
    }
    if ((s->mask & TP_AF_PIXELS_PER_UNIT) &&
        !tp_pack_pixels_per_unit_valid(s->pixels_per_unit)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "pixels_per_unit", "pixels_per_unit must be positive finite");
    }
    const int effective_shape = (s->mask & TP_AF_SHAPE) ? s->shape
                                                        : atlas->shape;
    if (!tp_pack_extrude_shape_valid(effective_extrude, effective_shape)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "extrude",
                             "extrude > 0 requires shape RECT");
    }
    return TP_STATUS_OK; /* allow_transform / power_of_two are booleans -- any value */
}

/* sprite.override.set: range-check each masked override field (INHERIT passes). */
static tp_status validate_sprite_set(const tp_project_atlas *atlas,
                                     const tp_op_sprite_set *s,
                                     tp_op_reject *rej) {
    tp_status st = TP_STATUS_OK;
    if (s->mask == 0) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "sprite.override.set names no field");
    }
    if ((s->mask & ~(uint32_t)TP_SPF_ALL) != 0U) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "",
                             "sprite.override.set contains unknown field bits");
    }
    if ((s->mask & TP_SPF_ORIGIN) && (!finite_any(s->origin_x) || !finite_any(s->origin_y))) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "origin_x", "origin must be finite");
    }
    if (s->mask & TP_SPF_SLICE9) {
        static const char *const fields[4] = {
            "slice9_l", "slice9_r", "slice9_t", "slice9_b"};
        for (int i = 0; i < 4; i++) {
            if ((st = range_i(rej, fields[i], s->slice9[i], 0,
                              UINT16_MAX)) != TP_STATUS_OK) {
                return st;
            }
        }
    }
    if ((s->mask & TP_SPF_SHAPE) && s->ov_shape != TP_PROJECT_OV_INHERIT &&
        (st = range_i(rej, "ov_shape", s->ov_shape, TP_PACK_SHAPE_MIN,
                      TP_PACK_SHAPE_MAX))) {
        return st;
    }
    if ((s->mask & TP_SPF_ALLOW_ROTATE) &&
        s->ov_allow_rotate != TP_PROJECT_OV_INHERIT &&
        s->ov_allow_rotate != 0) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_allow_rotate",
            "ov_allow_rotate = %d must be 0 (force no-rotate) or -1 (inherit)",
            s->ov_allow_rotate);
    }
    if ((s->mask & TP_SPF_MAX_VERTICES) &&
        s->ov_max_vertices != TP_PROJECT_OV_INHERIT &&
        !tp_pack_max_vertices_valid(s->ov_max_vertices)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "ov_max_vertices",
                             "ov_max_vertices = %d must be in [1..%d]",
                             s->ov_max_vertices, TP_PACK_MAX_VERTICES);
    }
    if ((s->mask & TP_SPF_MARGIN) && s->ov_margin != TP_PROJECT_OV_INHERIT &&
        (st = range_i(rej, "ov_margin", s->ov_margin, 1, UINT8_MAX))) {
        return st;
    }
    if ((s->mask & TP_SPF_MARGIN) &&
        s->ov_margin != TP_PROJECT_OV_INHERIT &&
        s->ov_margin > atlas->max_size) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_margin",
            "ov_margin = %d must not exceed atlas max_size %d", s->ov_margin,
            atlas->max_size);
    }
    if ((s->mask & TP_SPF_EXTRUDE) && s->ov_extrude != TP_PROJECT_OV_INHERIT &&
        (st = range_i(rej, "ov_extrude", s->ov_extrude, 1, UINT8_MAX))) {
        return st;
    }
    if ((s->mask & TP_SPF_EXTRUDE) &&
        s->ov_extrude != TP_PROJECT_OV_INHERIT &&
        s->ov_extrude > atlas->max_size) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_extrude",
            "ov_extrude = %d must not exceed atlas max_size %d",
            s->ov_extrude, atlas->max_size);
    }
    return TP_STATUS_OK;
}

/* fps + playback checks shared by animation.create / .settings.set. */
static tp_status validate_anim_knobs(bool check_fps, float fps, bool check_pb, int pb, tp_op_reject *rej) {
    if (check_fps && !finite_pos(fps)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "fps", "fps must be positive finite");
    }
    if (check_pb) {
        return range_i(rej, "playback", pb, 0, TP_OP_PLAYBACK_MAX);
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
    char normalized[TP_SRCKEY_MAX];
    tp_error error = {{0}};
    const tp_status status = tp_srckey_normalize(
        key, normalized, sizeof normalized, &error);
    if (status != TP_STATUS_OK) {
        return tp_op__reject(rej, status, field,
                             "frame key is not a valid source-local key: %s",
                             error.msg);
    }
    if (strcmp(key, normalized) != 0) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, field,
                             "frame key must already be normalized as '%s'",
                             normalized);
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
        if (!find_source(atlas, frames[i].source_id)) {
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

tp_status tp_op__canonical_view(const tp_project *project,
                                const tp_operation *operation,
                                tp_operation *view, char *path_buf,
                                size_t path_buf_size) {
    if (!project || !operation || !view || !path_buf || path_buf_size == 0U) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    *view = *operation;
    char **key = NULL;
    if (operation->kind == TP_OP_SOURCE_ADD) {
        key = &view->u.source_add.key;
    } else if (operation->kind == TP_OP_SOURCE_REPLACE) {
        key = &view->u.source_ref.key;
    }
    if (!key || !*key || (*key)[0] == '\0' || !project->project_dir) {
        return TP_STATUS_OK;
    }
    tp_status status = tp_project_resolve_path(project, *key, path_buf,
                                               path_buf_size);
    if (status == TP_STATUS_OK) {
        *key = path_buf;
    }
    return status;
}

static tp_status atlas_has_effective_source_path(
    const tp_project *project, const tp_project_atlas *atlas,
    const char *candidate, tp_id128 except_id, bool *out_found,
    tp_error *err) {
    *out_found = false;
    tp_source_path_identity wanted;
    tp_status status = tp_source_path_identity_from_input(candidate, &wanted,
                                                          err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    for (int i = 0; i < atlas->source_count; ++i) {
        const tp_project_source *source = &atlas->sources[i];
        if (!tp_id128_is_nil(except_id) && tp_id128_eq(source->id, except_id)) {
            continue;
        }
        tp_source_path_identity existing;
        status = tp_source_path_identity_from_stored(
            project, source->path, wanted.has_canonical, &existing, err);
        if (status != TP_STATUS_OK) {
            return status;
        }
        if (tp_source_path_identity_equal_text(
                wanted.absolute, wanted.has_canonical ? wanted.canonical : NULL,
                existing.absolute,
                existing.has_canonical ? existing.canonical : NULL)) {
            *out_found = true;
            return TP_STATUS_OK;
        }
    }
    return TP_STATUS_OK;
}

static tp_status validate_unique_source_path(
    const tp_project *project, const tp_project_atlas *atlas,
    const char *candidate, tp_id128 except_id, tp_op_reject *reject) {
    bool duplicate = false;
    tp_error error = {{0}};
    const tp_status status = atlas_has_effective_source_path(
        project, atlas, candidate, except_id, &duplicate, &error);
    if (status != TP_STATUS_OK) {
        return tp_op__reject(
            reject, status, "key", "source path is invalid: %s",
            error.msg[0] ? error.msg : "identity failed");
    }
    return duplicate
               ? tp_op__reject(
                     reject, TP_STATUS_INVALID_ARGUMENT, "key",
                     "%s source with path '%s' already exists in the atlas",
                     tp_id128_is_nil(except_id) ? "a" : "another", candidate)
               : TP_STATUS_OK;
}

tp_status tp_operation_validate(const tp_project *p, const tp_operation *op, tp_op_reject *rej) {
    tp_op__reject_ok(rej);
    if (!p || !op) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "null project or operation");
    }
    if (tp_op_info_by_kind(op->kind) == NULL) {
        return tp_op__reject(rej, TP_STATUS_UNKNOWN_OP, "op", "operation kind %d is not in the catalog", (int)op->kind);
    }
    tp_status utf8_status = validate_operation_utf8(op, rej);
    if (utf8_status != TP_STATUS_OK) {
        return utf8_status;
    }
    tp_operation canonical;
    char canonical_path[TP_IDENTITY_PATH_MAX];
    tp_status canonical_status = tp_op__canonical_view(
        p, op, &canonical, canonical_path, sizeof canonical_path);
    if (canonical_status != TP_STATUS_OK) {
        return tp_op__reject(rej, canonical_status, "key",
                             "source path cannot be resolved against the project");
    }
    op = &canonical;
    /* atlas.create is the one op that must NOT already resolve its atlas_id. */
    if (op->kind == TP_OP_ATLAS_CREATE) {
        if (tp_id128_is_nil(op->atlas_id)) {
            return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "atlas_id", "atlas.create needs a real atlas id");
        }
        if (tp_project_has_structural_id(p, op->atlas_id)) {
            return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "atlas_id",
                                 "that structural id already belongs to a project entity");
        }
        const char *nm = op->u.atlas_create.name;
        tp_status name_status = validate_atlas_name_shape(nm, rej);
        if (name_status != TP_STATUS_OK) {
            return name_status;
        }
        if (find_atlas_by_name(p, nm) >= 0) { /* CLI `atlas add` rejects a duplicate name */
            return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "an atlas named '%s' already exists", nm);
        }
        return TP_STATUS_OK;
    }

    /* Every other op addresses an existing parent atlas. */
    const tp_project_atlas *a = find_atlas(p, op->atlas_id);
    if (!a) {
        return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "atlas_id", "no atlas with that id");
    }

    switch (op->kind) {
        case TP_OP_ATLAS_REMOVE: return TP_STATUS_OK;
        case TP_OP_ATLAS_RENAME: {
            const char *nm = op->u.atlas_rename.name;
            tp_status name_status = validate_atlas_name_shape(nm, rej);
            if (name_status != TP_STATUS_OK) {
                return name_status;
            }
            /* CLI `atlas rename` rejects a collision with ANOTHER atlas; renaming to the
             * same name (self) is allowed (a no-op). */
            int other = find_atlas_by_name(p, nm);
            if (other >= 0 && &p->atlases[other] != a) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "an atlas named '%s' already exists", nm);
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ATLAS_SETTINGS_SET:
            return validate_atlas_settings(a, &op->u.atlas_settings, rej);

        case TP_OP_SOURCE_ADD:
            if (tp_id128_is_nil(op->u.source_add.source_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "source_id", "source.add needs a real source id");
            }
            if (tp_project_has_structural_id(p, op->u.source_add.source_id)) {
                return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "source_id",
                                     "that structural id already belongs to a project entity");
            }
            if (!op->u.source_add.key || op->u.source_add.key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "key", "source path must be non-empty");
            }
            /* The apply mutator dedupes a matching '/'-normalized path (count unchanged),
             * which would strand the op's source_id. The id-contract ("create a NEW source
             * with THIS id at this path") cannot honor a path that already belongs to another
             * source, so reject the conflict here. (The CLI adapter pre-checks/skips to
             * preserve the CLI's silent-dedupe UX.) */
            tp_status path_status = validate_unique_source_path(
                p, a, op->u.source_add.key, tp_id128_nil(), rej);
            if (path_status != TP_STATUS_OK) {
                return path_status;
            }
            /* kind must be a currently-valid enum value {FOLDER=0, FILE=1}: apply stores kind verbatim and
             * it re-serializes by token, so an out-of-range kind (99, or the reserved ATLAS=2) would
             * validate, live in the model, then reload as "folder" -> live-vs-disk divergence. Epic B1 adds
             * TP_SOURCE_KIND_ATLAS=2 -- WIDEN the upper bound to 2 when it lands (a loud, testable failure
             * here beats a silent folder-coercion). range_i keeps kind consistent with every other bounded
             * knob (shape/playback/alpha) -> OUT_OF_RANGE. */
            return range_i(rej, "kind", (long)op->u.source_add.kind, 0, 1);
        case TP_OP_SOURCE_REMOVE:
            if (!find_source(a, op->u.source_ref.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id",
                                     "no source with that id in the atlas");
            }
            return source_is_referenced(a, op->u.source_ref.source_id)
                       ? tp_op__reject(
                             rej, TP_STATUS_INVALID_ARGUMENT, "source_id",
                             "source is still referenced by sprite overrides or animation frames")
                       : TP_STATUS_OK;
        case TP_OP_SOURCE_REPLACE:
            if (!find_source(a, op->u.source_ref.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!op->u.source_ref.key || op->u.source_ref.key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "key", "source path must be non-empty");
            }
            return validate_unique_source_path(
                p, a, op->u.source_ref.key, op->u.source_ref.source_id, rej);

        case TP_OP_SPRITE_OVERRIDE_SET:
            if (tp_id128_is_nil(op->u.sprite_set.source_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "source_id",
                                     "sprite operation needs a canonical source id");
            }
            if (!find_source(a, op->u.sprite_set.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!op->u.sprite_set.src_key || op->u.sprite_set.src_key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            return validate_sprite_set(a, &op->u.sprite_set, rej);
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            if (tp_id128_is_nil(op->u.sprite_clear.source_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "source_id",
                                     "sprite operation needs a canonical source id");
            }
            if (!find_source(a, op->u.sprite_clear.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!op->u.sprite_clear.src_key || op->u.sprite_clear.src_key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            if (op->u.sprite_clear.mask == 0) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "fields", "sprite.override.clear names no field");
            }
            if ((op->u.sprite_clear.mask & ~(uint32_t)TP_SPF_ALL) != 0U) {
                return tp_op__reject(
                    rej, TP_STATUS_INVALID_ARGUMENT, "fields",
                    "sprite.override.clear contains unknown field bits");
            }
            return TP_STATUS_OK;
        case TP_OP_SPRITE_NAME_SET:
            if (tp_id128_is_nil(op->u.sprite_name.source_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "source_id",
                                     "sprite operation needs a canonical source id");
            }
            if (!find_source(a, op->u.sprite_name.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!op->u.sprite_name.src_key || op->u.sprite_name.src_key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            return TP_STATUS_OK; /* name == NULL clears the rename (valid) */

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
            if (!find_source(a, s->frame.source_id)) {
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

        case TP_OP_TARGET_CREATE: {
            const tp_op_target_create *t = &op->u.target_create;
            if (tp_id128_is_nil(t->target_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "target_id", "target.create needs a real target id");
            }
            if (tp_project_has_structural_id(p, t->target_id)) {
                return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "target_id",
                                     "that structural id already belongs to a project entity");
            }
            tp_status exporter_status =
                validate_exporter_id(t->exporter_id, rej);
            if (exporter_status != TP_STATUS_OK) {
                return exporter_status;
            }
            if (!tp_exporter_find(t->exporter_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "exporter_id", "unknown exporter id '%s'",
                                     t->exporter_id ? t->exporter_id : "");
            }
            if (!t->out_path || t->out_path[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "out_path", "out_path must be non-empty");
            }
            return TP_STATUS_OK;
        }
        case TP_OP_TARGET_REMOVE:
            return find_target(a, op->u.target_ref.target_id)
                       ? TP_STATUS_OK
                       : tp_op__reject(rej, TP_STATUS_NOT_FOUND, "target_id", "no target with that id in the atlas");
        case TP_OP_TARGET_SET: {
            const tp_op_target_set *t = &op->u.target_set;
            if (!find_target(a, t->target_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "target_id", "no target with that id in the atlas");
            }
            /* Masked set (mirrors atlas.settings.set): validate a field ONLY when its mask bit
             * is set. An all-zero mask names no field -> reject (matches atlas/anim settings). */
            if (t->mask == 0) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "target.set names no field");
            }
            if ((t->mask & ~(uint32_t)TP_TF_ALL) != 0U) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "",
                                     "target.set contains unknown field bits");
            }
            if (t->mask & TP_TF_EXPORTER) {
                tp_status exporter_status =
                    validate_exporter_id(t->exporter_id, rej);
                if (exporter_status != TP_STATUS_OK) {
                    return exporter_status;
                }
                if (!tp_exporter_find(t->exporter_id)) {
                    return tp_op__reject(
                        rej, TP_STATUS_NOT_FOUND, "exporter_id",
                        "unknown exporter id '%s'", t->exporter_id);
                }
            }
            if ((t->mask & TP_TF_OUT_PATH) && (!t->out_path || t->out_path[0] == '\0')) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "out_path", "out_path must be non-empty");
            }
            return TP_STATUS_OK;
        }

        case TP_OP_INVALID:
        case TP_OP_ATLAS_CREATE: /* handled above */
        case TP_OP_KIND_COUNT: break;
    }
    return tp_op__reject(rej, TP_STATUS_UNKNOWN_OP, "op", "operation kind %d is not applicable", (int)op->kind);
}
