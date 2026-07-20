#include "tp_op_validate_family_internal.h"

#include <float.h>
#include <string.h>

#include "tp_core/tp_pack.h"
#include "tp_op_internal.h"
#include "tp_pack_constraints_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_source_plan_internal.h"

static bool finite_any(float v) { return v == v && v >= -FLT_MAX && v <= FLT_MAX; }

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
    const tp_pack_sprite_constraint_input input = {
        .atlas_max_size = atlas->max_size,
        .has_shape = (s->mask & TP_SPF_SHAPE) &&
                     s->ov_shape != TP_PROJECT_OV_INHERIT,
        .shape = s->ov_shape,
        .has_allow_rotate = (s->mask & TP_SPF_ALLOW_ROTATE) &&
                            s->ov_allow_rotate != TP_PROJECT_OV_INHERIT,
        .allow_rotate = s->ov_allow_rotate,
        .has_max_vertices = (s->mask & TP_SPF_MAX_VERTICES) &&
                            s->ov_max_vertices != TP_PROJECT_OV_INHERIT,
        .max_vertices = s->ov_max_vertices,
        .has_margin = (s->mask & TP_SPF_MARGIN) &&
                      s->ov_margin != TP_PROJECT_OV_INHERIT,
        .margin = s->ov_margin,
        .has_extrude = (s->mask & TP_SPF_EXTRUDE) &&
                       s->ov_extrude != TP_PROJECT_OV_INHERIT,
        .extrude = s->ov_extrude,
    };
    const tp_pack_sprite_constraint_facts facts =
        tp_pack_sprite_constraint_facts_of(&input);
    if ((s->mask & TP_SPF_SHAPE) && s->ov_shape != TP_PROJECT_OV_INHERIT &&
        facts.shape_not_wire_representable) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_shape",
            "ov_shape = %d must be in [%d..%d]", s->ov_shape,
            TP_PACK_SHAPE_MIN, TP_PACK_SHAPE_MAX);
    }
    if ((s->mask & TP_SPF_ALLOW_ROTATE) &&
        s->ov_allow_rotate != TP_PROJECT_OV_INHERIT &&
        facts.allow_rotate_not_wire_representable) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_allow_rotate",
            "ov_allow_rotate = %d must be 0 (force no-rotate) or -1 (inherit)",
            s->ov_allow_rotate);
    }
    if ((s->mask & TP_SPF_MAX_VERTICES) &&
        s->ov_max_vertices != TP_PROJECT_OV_INHERIT &&
        facts.max_vertices_not_wire_representable) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "ov_max_vertices",
                             "ov_max_vertices = %d must be in [1..%d]",
                             s->ov_max_vertices, TP_PACK_MAX_VERTICES);
    }
    if ((s->mask & TP_SPF_MARGIN) && s->ov_margin != TP_PROJECT_OV_INHERIT &&
        facts.margin_not_wire_representable) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_margin",
            "ov_margin = %d must be in [1..%d]", s->ov_margin, UINT8_MAX);
    }
    if ((s->mask & TP_SPF_MARGIN) &&
        s->ov_margin != TP_PROJECT_OV_INHERIT &&
        facts.margin_exceeds_max_size) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_margin",
            "ov_margin = %d must not exceed atlas max_size %d", s->ov_margin,
            atlas->max_size);
    }
    if ((s->mask & TP_SPF_EXTRUDE) && s->ov_extrude != TP_PROJECT_OV_INHERIT &&
        facts.extrude_not_wire_representable) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_extrude",
            "ov_extrude = %d must be in [1..%d]", s->ov_extrude,
            UINT8_MAX);
    }
    if ((s->mask & TP_SPF_EXTRUDE) &&
        s->ov_extrude != TP_PROJECT_OV_INHERIT &&
        facts.extrude_exceeds_max_size) {
        return tp_op__reject(
            rej, TP_STATUS_OUT_OF_RANGE, "ov_extrude",
            "ov_extrude = %d must not exceed atlas max_size %d",
            s->ov_extrude, atlas->max_size);
    }
    return TP_STATUS_OK;
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
tp_status tp_op_validate_source_sprite_family(
    const tp_project *project, const tp_project_atlas *atlas,
    const tp_operation *operation, tp_op_reject *reject) {
    switch (operation->kind) {
        case TP_OP_SOURCE_ADD:
            if (tp_id128_is_nil(operation->u.source_add.source_id)) {
                return tp_op__reject(reject, TP_STATUS_ID_MALFORMED, "source_id", "source.add needs a real source id");
            }
            if (tp_project_has_structural_id(project, operation->u.source_add.source_id)) {
                return tp_op__reject(reject, TP_STATUS_DUPLICATE_ID, "source_id",
                                     "that structural id already belongs to a project entity");
            }
            if (!operation->u.source_add.key || operation->u.source_add.key[0] == '\0') {
                return tp_op__reject(reject, TP_STATUS_INVALID_ARGUMENT, "key", "source path must be non-empty");
            }
            /* The apply mutator dedupes a matching '/'-normalized path (count unchanged),
             * which would strand the op's source_id. The id-contract ("create a NEW source
             * with THIS id at this path") cannot honor a path that already belongs to another
             * source, so reject the conflict here. (The CLI adapter pre-checks/skips to
             * preserve the CLI's silent-dedupe UX.) */
            tp_status path_status = validate_unique_source_path(
                project, atlas, operation->u.source_add.key, tp_id128_nil(), reject);
            if (path_status != TP_STATUS_OK) {
                return path_status;
            }
            /* kind must be a currently-valid enum value {FOLDER=0, FILE=1}: apply stores kind verbatim and
             * it re-serializes by token, so an out-of-range kind (99, or the reserved ATLAS=2) would
             * validate, live in the model, then reload as "folder" -> live-vs-disk divergence. Epic B1 adds
             * TP_SOURCE_KIND_ATLAS=2 -- WIDEN the upper bound to 2 when it lands (a loud, testable failure
             * here beats a silent folder-coercion). range_i keeps kind consistent with every other bounded
             * knob (shape/playback/alpha) -> OUT_OF_RANGE. */
            return range_i(reject, "kind", (long)operation->u.source_add.kind, 0, 1);
        case TP_OP_SOURCE_REMOVE:
            if (!tp_op_validate_find_source(atlas, operation->u.source_ref.source_id)) {
                return tp_op__reject(reject, TP_STATUS_NOT_FOUND, "source_id",
                                     "no source with that id in the atlas");
            }
            return source_is_referenced(atlas, operation->u.source_ref.source_id)
                       ? tp_op__reject(
                             reject, TP_STATUS_INVALID_ARGUMENT, "source_id",
                             "source is still referenced by sprite overrides or animation frames")
                       : TP_STATUS_OK;
        case TP_OP_SOURCE_REPLACE:
            if (!tp_op_validate_find_source(atlas, operation->u.source_ref.source_id)) {
                return tp_op__reject(reject, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!operation->u.source_ref.key || operation->u.source_ref.key[0] == '\0') {
                return tp_op__reject(reject, TP_STATUS_INVALID_ARGUMENT, "key", "source path must be non-empty");
            }
            return validate_unique_source_path(
                project, atlas, operation->u.source_ref.key, operation->u.source_ref.source_id, reject);

        case TP_OP_SPRITE_OVERRIDE_SET:
            if (tp_id128_is_nil(operation->u.sprite_set.source_id)) {
                return tp_op__reject(reject, TP_STATUS_ID_MALFORMED, "source_id",
                                     "sprite operation needs a canonical source id");
            }
            if (!tp_op_validate_find_source(atlas, operation->u.sprite_set.source_id)) {
                return tp_op__reject(reject, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!operation->u.sprite_set.src_key || operation->u.sprite_set.src_key[0] == '\0') {
                return tp_op__reject(reject, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            return validate_sprite_set(atlas, &operation->u.sprite_set, reject);
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            if (tp_id128_is_nil(operation->u.sprite_clear.source_id)) {
                return tp_op__reject(reject, TP_STATUS_ID_MALFORMED, "source_id",
                                     "sprite operation needs a canonical source id");
            }
            if (!tp_op_validate_find_source(atlas, operation->u.sprite_clear.source_id)) {
                return tp_op__reject(reject, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!operation->u.sprite_clear.src_key || operation->u.sprite_clear.src_key[0] == '\0') {
                return tp_op__reject(reject, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            if (operation->u.sprite_clear.mask == 0) {
                return tp_op__reject(reject, TP_STATUS_INVALID_ARGUMENT, "fields", "sprite.override.clear names no field");
            }
            if ((operation->u.sprite_clear.mask & ~(uint32_t)TP_SPF_ALL) != 0U) {
                return tp_op__reject(
                    reject, TP_STATUS_INVALID_ARGUMENT, "fields",
                    "sprite.override.clear contains unknown field bits");
            }
            return TP_STATUS_OK;
        case TP_OP_SPRITE_NAME_SET:
            if (tp_id128_is_nil(operation->u.sprite_name.source_id)) {
                return tp_op__reject(reject, TP_STATUS_ID_MALFORMED, "source_id",
                                     "sprite operation needs a canonical source id");
            }
            if (!tp_op_validate_find_source(atlas, operation->u.sprite_name.source_id)) {
                return tp_op__reject(reject, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!operation->u.sprite_name.src_key || operation->u.sprite_name.src_key[0] == '\0') {
                return tp_op__reject(reject, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            return TP_STATUS_OK; /* name == NULL clears the rename (valid) */
        default:
            return tp_op__reject(reject, TP_STATUS_UNKNOWN_OP, "op",
                                 "operation is not a source/sprite family member");
    }
}
