#include "tp_op_validate_family_internal.h"

#include <string.h>

#include "tp_op_internal.h"
#include "tp_pack_constraints_internal.h"
#include "tp_project_identity_internal.h"

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

/* atlas.settings.set: check every masked knob against its range. */
static tp_status validate_atlas_settings(const tp_project_atlas *atlas,
                                         const tp_op_atlas_settings *s,
                                         tp_op_reject *rej) {
    if (s->mask == 0) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "atlas.settings.set names no field");
    }
    if ((s->mask & ~(uint32_t)TP_AF_ALL) != 0U) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "",
                             "atlas.settings.set contains unknown field bits");
    }
    const int effective_max_size = (s->mask & TP_AF_MAX_SIZE)
                                       ? s->max_size
                                       : atlas->max_size;
    const tp_pack_atlas_constraint_input raw_input = {
        .max_size = effective_max_size,
        .padding = s->padding,
        .margin = s->margin,
        .extrude = s->extrude,
        .alpha_threshold = s->alpha_threshold,
        .max_vertices = s->max_vertices,
        .shape = s->shape,
        .pixels_per_unit = s->pixels_per_unit,
    };
    const tp_pack_atlas_constraint_facts raw =
        tp_pack_atlas_constraint_facts_of(&raw_input);
    if ((s->mask & TP_AF_MAX_SIZE) && raw.max_size_out_of_range) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "max_size",
                             "max_size = %d must be in [1..%d]", s->max_size,
                             TP_PACK_MAX_PAGE_DIM);
    }
    if (raw.max_size_out_of_range) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "max_size",
                             "effective max_size = %d must be in [1..%d]",
                             effective_max_size, TP_PACK_MAX_PAGE_DIM);
    }
    if ((s->mask & TP_AF_PADDING) &&
        (raw.padding_negative || raw.padding_exceeds_max_size)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "padding",
                             "padding = %d must be in [0..%d]", s->padding,
                             effective_max_size);
    }
    if ((s->mask & TP_AF_MARGIN) &&
        (raw.margin_negative || raw.margin_exceeds_max_size)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "margin",
                             "margin = %d must be in [0..%d]", s->margin,
                             effective_max_size);
    }
    if ((s->mask & TP_AF_EXTRUDE) &&
        (raw.extrude_negative || raw.extrude_exceeds_max_size)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "extrude",
                             "extrude = %d must be in [0..%d]", s->extrude,
                             effective_max_size);
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
    const tp_pack_atlas_constraint_input effective_input = {
        .max_size = effective_max_size,
        .padding = effective_padding,
        .margin = effective_margin,
        .extrude = effective_extrude,
        .alpha_threshold = (s->mask & TP_AF_ALPHA_THRESHOLD)
                               ? s->alpha_threshold
                               : atlas->alpha_threshold,
        .max_vertices = (s->mask & TP_AF_MAX_VERTICES)
                            ? s->max_vertices
                            : atlas->max_vertices,
        .shape = (s->mask & TP_AF_SHAPE) ? s->shape : atlas->shape,
        .pixels_per_unit = (s->mask & TP_AF_PIXELS_PER_UNIT)
                               ? s->pixels_per_unit
                               : atlas->pixels_per_unit,
    };
    const tp_pack_atlas_constraint_facts effective =
        tp_pack_atlas_constraint_facts_of(&effective_input);
    if (effective.padding_negative || effective.padding_exceeds_max_size) {
        const char *field = (s->mask & TP_AF_PADDING) ? "padding"
                                                      : "max_size";
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, field,
            "effective padding = %d must be in [0..%d]",
            effective_padding, effective_max_size);
    }
    if (effective.margin_negative || effective.margin_exceeds_max_size) {
        const char *field = (s->mask & TP_AF_MARGIN) ? "margin"
                                                     : "max_size";
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, field,
            "effective margin = %d must be in [0..%d]", effective_margin,
            effective_max_size);
    }
    if (effective.extrude_negative || effective.extrude_exceeds_max_size) {
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
            const tp_pack_sprite_constraint_input sprite_input = {
                .atlas_max_size = effective_max_size,
                .has_margin =
                    sprite->ov_margin != TP_PROJECT_OV_INHERIT,
                .margin = sprite->ov_margin,
                .has_extrude =
                    sprite->ov_extrude != TP_PROJECT_OV_INHERIT,
                .extrude = sprite->ov_extrude,
            };
            const tp_pack_sprite_constraint_facts sprite_facts =
                tp_pack_sprite_constraint_facts_of(&sprite_input);
            if (sprite_facts.margin_exceeds_max_size) {
                return tp_op__reject(
                    rej, TP_STATUS_OUT_OF_RANGE, "max_size",
                    "max_size = %d is smaller than sprite margin override %d",
                    effective_max_size, sprite->ov_margin);
            }
            if (sprite_facts.extrude_exceeds_max_size) {
                return tp_op__reject(
                    rej, TP_STATUS_OUT_OF_RANGE, "max_size",
                    "max_size = %d is smaller than sprite extrude override %d",
                    effective_max_size, sprite->ov_extrude);
            }
        }
    }
    if ((s->mask & TP_AF_ALPHA_THRESHOLD) &&
        raw.alpha_threshold_out_of_range) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "alpha_threshold",
                             "alpha_threshold = %d must be in [0..%d]",
                             s->alpha_threshold, TP_PACK_ALPHA_MAX);
    }
    if ((s->mask & TP_AF_MAX_VERTICES) &&
        raw.max_vertices_out_of_range) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "max_vertices",
                             "max_vertices = %d must be in [1..%d]",
                             s->max_vertices, TP_PACK_MAX_VERTICES);
    }
    if ((s->mask & TP_AF_SHAPE) && raw.shape_out_of_range) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "shape",
                             "shape = %d must be in [%d..%d]", s->shape,
                             TP_PACK_SHAPE_MIN, TP_PACK_SHAPE_MAX);
    }
    if ((s->mask & TP_AF_PIXELS_PER_UNIT) &&
        raw.pixels_per_unit_out_of_range) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "pixels_per_unit", "pixels_per_unit must be positive finite");
    }
    if (effective.extrude_requires_rect ||
        (effective_extrude > 0 && effective.shape_out_of_range)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "extrude",
                             "extrude > 0 requires shape RECT");
    }
    return TP_STATUS_OK; /* allow_transform / power_of_two are booleans -- any value */
}

tp_status tp_op_validate_atlas_family(
    const tp_project *project, const tp_project_atlas *atlas,
    const tp_operation *operation, tp_op_reject *reject) {
    if (operation->kind == TP_OP_ATLAS_CREATE) {
        if (tp_id128_is_nil(operation->atlas_id)) {
            return tp_op__reject(
                reject, TP_STATUS_ID_MALFORMED, "atlas_id",
                "atlas.create needs a real atlas id");
        }
        if (tp_project_has_structural_id(project, operation->atlas_id)) {
            return tp_op__reject(
                reject, TP_STATUS_DUPLICATE_ID, "atlas_id",
                "that structural id already belongs to a project entity");
        }
        const char *name = operation->u.atlas_create.name;
        tp_status status = validate_atlas_name_shape(name, reject);
        if (status != TP_STATUS_OK) {
            return status;
        }
        if (find_atlas_by_name(project, name) >= 0) {
            return tp_op__reject(
                reject, TP_STATUS_INVALID_ARGUMENT, "name",
                "an atlas named '%s' already exists", name);
        }
        return TP_STATUS_OK;
    }

    switch (operation->kind) {
        case TP_OP_ATLAS_REMOVE:
            return TP_STATUS_OK;
        case TP_OP_ATLAS_RENAME: {
            const char *name = operation->u.atlas_rename.name;
            tp_status status = validate_atlas_name_shape(name, reject);
            if (status != TP_STATUS_OK) {
                return status;
            }
            const int other = find_atlas_by_name(project, name);
            if (other >= 0 && &project->atlases[other] != atlas) {
                return tp_op__reject(
                    reject, TP_STATUS_INVALID_ARGUMENT, "name",
                    "an atlas named '%s' already exists", name);
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ATLAS_SETTINGS_SET:
            return validate_atlas_settings(
                atlas, &operation->u.atlas_settings, reject);
        default:
            return tp_op__reject(reject, TP_STATUS_UNKNOWN_OP, "op",
                                 "operation is not an atlas family member");
    }
}
