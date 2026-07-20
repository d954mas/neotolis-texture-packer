#include "tp_validate_rules_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"
#include "tp_pack_constraints_internal.h"
#include "tp_session_internal.h"

enum {
    TARGET_ISSUE_UNKNOWN_EXPORTER = 1U << 0,
    TARGET_ISSUE_NO_OUT_PATH = 1U << 1,
    TARGET_ISSUE_DUPLICATE_OUT_PATH = 1U << 2,
};

static unsigned target_issue_mask(const char *exporter_id, bool enabled,
                                  const char *out_path, bool path_shared) {
    unsigned mask = tp_exporter_find(exporter_id)
                        ? 0U
                        : TARGET_ISSUE_UNKNOWN_EXPORTER;
    if (enabled) {
        if (!out_path || out_path[0] == '\0') {
            mask |= TARGET_ISSUE_NO_OUT_PATH;
        } else if (path_shared) {
            mask |= TARGET_ISSUE_DUPLICATE_OUT_PATH;
        }
    }
    return mask;
}

void target_path_index_free(target_path_index *index,
                                   const tp_project *project) {
    if (!index) {
        return;
    }
    if (index->by_atlas && project) {
        for (int ai = 0; ai < index->atlas_count; ++ai) {
            if (index->by_atlas[ai]) {
                for (int ti = 0; ti < project->atlases[ai].target_count; ++ti) {
                    free(index->by_atlas[ai][ti]);
                }
            }
            free(index->by_atlas[ai]);
        }
    }
    free(index->by_atlas);
    str_index_free(&index->counts);
    memset(index, 0, sizeof *index);
}

bool target_path_index_build(const tp_project *project,
                                    target_path_index *out) {
    memset(out, 0, sizeof *out);
    size_t total = 0U;
    for (int ai = 0; ai < project->atlas_count; ++ai) {
        const int count = project->atlases[ai].target_count;
        if (count > 0 && total > (size_t)INT_MAX - (size_t)count) {
            return false;
        }
        total += (size_t)(count > 0 ? count : 0);
    }
    out->atlas_count = project->atlas_count;
    out->by_atlas = (char ***)calloc((size_t)project->atlas_count,
                                    sizeof *out->by_atlas);
    char **flat = total ? (char **)calloc(total, sizeof *flat) : NULL;
    if ((project->atlas_count > 0 && !out->by_atlas) || (total > 0U && !flat)) {
        free(flat);
        target_path_index_free(out, project);
        return false;
    }
    int flat_count = 0;
    for (int ai = 0; ai < project->atlas_count; ++ai) {
        const tp_project_atlas *atlas = &project->atlases[ai];
        if (atlas->target_count > 0) {
            out->by_atlas[ai] = (char **)calloc((size_t)atlas->target_count,
                                                sizeof *out->by_atlas[ai]);
            if (!out->by_atlas[ai]) {
                free(flat);
                target_path_index_free(out, project);
                return false;
            }
        }
        for (int ti = 0; ti < atlas->target_count; ++ti) {
            const tp_project_target *target = &atlas->targets[ti];
            if (!target->enabled || !target->out_path ||
                target->out_path[0] == '\0') {
                continue;
            }
            char *key = validation_slash_norm_owned(target->out_path);
            if (!key) {
                free(flat);
                target_path_index_free(out, project);
                return false;
            }
            out->by_atlas[ai][ti] = key;
            flat[flat_count++] = key;
        }
    }
    const bool ok = str_index_build(&out->counts,
                                    (const char *const *)flat, flat_count);
    free(flat);
    if (!ok) {
        target_path_index_free(out, project);
    }
    return ok;
}
void validate_target_settings_domain(
    validation_builder *fs, const tp_project *p, int ai,
    const tp_project_atlas *a, const target_path_index *target_paths) {
    /* (f) target integrity.
     *   unknown_exporter   [error]   exporter id tp_exporter_find cannot resolve -- a broken id is bad
     *                                 data regardless of enable state, so reported for EVERY target.
     *   target_no_out_path [error]   an ENABLED target with an empty/NULL out_path can produce no file.
     *   duplicate_out_path [warning] an ENABLED target whose out_path is ALSO another ENABLED target's
     *                                 (they overwrite each other). Project-wide (cross-atlas), slash-
     *                                 normalized, via the shared core detector; the message names the path.
     * The out_path checks gate on `enabled`: only enabled targets export, so a DISABLED (parked) target's
     * empty/duplicate out_path is harmless and must NOT flip `validate --strict` to an error. */
    for (int t = 0; t < a->target_count; t++) {
        const tp_project_target *tg = &a->targets[t];
        const char *path_key = tg->enabled && tg->out_path && tg->out_path[0]
                                   ? target_paths->by_atlas[ai][t]
                                   : NULL;
        const str_slot *path_group = path_key
                                         ? str_index_find(&target_paths->counts,
                                                          path_key)
                                         : NULL;
        const unsigned issues = target_issue_mask(
            tg->exporter_id, tg->enabled, tg->out_path,
            path_group && path_group->count > 1U);
        if ((issues & TARGET_ISSUE_UNKNOWN_EXPORTER) != 0U) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_UNKNOWN_EXPORTER,
                        context_target(a, tg),
                        "target references unknown exporter '%s'", tg->exporter_id);
        }
        if ((issues & TARGET_ISSUE_NO_OUT_PATH) != 0U) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_TARGET_NO_OUT_PATH,
                        context_target(a, tg),
                        "target has no output path -- it cannot produce a file");
        }
        if ((issues & TARGET_ISSUE_DUPLICATE_OUT_PATH) != 0U) {
            add_finding(fs, TP_VALIDATION_WARNING,
                        TP_VALIDATION_CODE_DUPLICATE_OUT_PATH,
                        context_target(a, tg),
                        "two or more targets export to '%s' (they overwrite each other)", tg->out_path);
        }
    }

    /* (g) Pack constraints over the raw project model.  Do this before the
     * project->export settings bridge intentionally clamps non-RECT extrude;
     * validation must diagnose persisted input, not its adapted projection. */
    const tp_pack_atlas_constraint_input atlas_input = {
        .max_size = a->max_size,
        .padding = a->padding,
        .margin = a->margin,
        .extrude = a->extrude,
        .alpha_threshold = a->alpha_threshold,
        .max_vertices = a->max_vertices,
        .shape = a->shape,
        .pixels_per_unit = a->pixels_per_unit,
    };
    const tp_pack_atlas_constraint_facts atlas_facts =
        tp_pack_atlas_constraint_facts_of(&atlas_input);
    if (atlas_facts.max_size_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "max_size = %d is out of range [1..%d]", a->max_size,
                    TP_PACK_MAX_PAGE_DIM);
    }
    const struct {
        const char *name;
        int value;
        bool negative;
        bool exceeds_max_size;
    } spacing[] = {
        {"padding", a->padding, atlas_facts.padding_negative,
         atlas_facts.padding_exceeds_max_size},
        {"margin", a->margin, atlas_facts.margin_negative,
         atlas_facts.margin_exceeds_max_size},
        {"extrude", a->extrude, atlas_facts.extrude_negative,
         atlas_facts.extrude_exceeds_max_size},
    };
    for (size_t i = 0U; i < sizeof spacing / sizeof spacing[0]; ++i) {
        if (spacing[i].negative) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        context_atlas(a), "%s = %d must be >= 0",
                        spacing[i].name, spacing[i].value);
        } else if (!atlas_facts.max_size_out_of_range &&
                   spacing[i].exceeds_max_size) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                        context_atlas(a), "%s = %d must be in [0..%d]",
                        spacing[i].name, spacing[i].value, a->max_size);
        }
    }
    if (atlas_facts.alpha_threshold_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "alpha_threshold = %d is out of range [0..%d]",
                    a->alpha_threshold, TP_PACK_ALPHA_MAX);
    }
    if (atlas_facts.max_vertices_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "max_vertices = %d is out of range [1..%d]",
                    a->max_vertices, TP_PACK_MAX_VERTICES);
    }
    if (atlas_facts.shape_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "shape = %d is out of range [%d..%d]", a->shape,
                    TP_PACK_SHAPE_MIN, TP_PACK_SHAPE_MAX);
    }
    if (atlas_facts.pixels_per_unit_out_of_range) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "pixels_per_unit must be positive and finite");
    }
    if (atlas_facts.extrude_requires_rect) {
        add_finding(fs, TP_VALIDATION_ERROR,
                    TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE,
                    context_atlas(a),
                    "extrude > 0 requires shape RECT");
    }
    for (int i = 0; i < a->sprite_count; ++i) {
        const tp_project_sprite *sprite = &a->sprites[i];
        const bool slice9 = sprite->slice9_lrtb[0] ||
                            sprite->slice9_lrtb[1] ||
                            sprite->slice9_lrtb[2] ||
                            sprite->slice9_lrtb[3];
        const tp_pack_sprite_constraint_input sprite_input = {
            .atlas_max_size = a->max_size,
            .atlas_shape = a->shape,
            .atlas_extrude = a->extrude,
            .has_slice9 = slice9,
            .has_shape = sprite->ov_shape != TP_PROJECT_OV_INHERIT,
            .shape = sprite->ov_shape,
            .has_allow_rotate =
                sprite->ov_allow_rotate != TP_PROJECT_OV_INHERIT,
            .allow_rotate = sprite->ov_allow_rotate,
            .has_max_vertices =
                sprite->ov_max_vertices != TP_PROJECT_OV_INHERIT,
            .max_vertices = sprite->ov_max_vertices,
            .has_margin = sprite->ov_margin != TP_PROJECT_OV_INHERIT,
            .margin = sprite->ov_margin,
            .has_extrude = sprite->ov_extrude != TP_PROJECT_OV_INHERIT,
            .extrude = sprite->ov_extrude,
        };
        const tp_pack_sprite_constraint_facts sprite_facts =
            tp_pack_sprite_constraint_facts_of(&sprite_input);
        const finding_context context =
            context_sprite(a, sprite->source_ref, sprite->name);
        if (!atlas_facts.max_size_out_of_range &&
            sprite_facts.margin_exceeds_max_size) {
            add_finding(
                fs, TP_VALIDATION_ERROR,
                TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, context,
                "sprite ov_margin = %d must not exceed atlas max_size %d",
                sprite->ov_margin, a->max_size);
        }
        if (!atlas_facts.max_size_out_of_range &&
            sprite_facts.extrude_exceeds_max_size) {
            add_finding(
                fs, TP_VALIDATION_ERROR,
                TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, context,
                "sprite ov_extrude = %d must not exceed atlas max_size %d",
                sprite->ov_extrude, a->max_size);
        }
        if (sprite_facts.slice9_shape_conflict) {
            add_finding(fs, TP_VALIDATION_ERROR,
                        TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, context,
                        "sprite ov_shape %d conflicts with slice9; slice9 requires RECT",
                        sprite->ov_shape);
        }
        if (sprite_facts.effective_extrude_requires_rect) {
            const int effective_extrude =
                sprite->ov_extrude != TP_PROJECT_OV_INHERIT
                    ? sprite->ov_extrude
                    : a->extrude;
            add_finding(
                fs, TP_VALIDATION_ERROR,
                TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE, context,
                "sprite effective extrude %d requires effective shape RECT",
                effective_extrude);
        }
    }
}
static void target_issue_add(tp_target_validation_report *out,
                             tp_validation_severity severity,
                             const char *code) {
    if (out->issue_count >= TP_TARGET_VALIDATION_MAX_ISSUES) {
        return;
    }
    tp_target_validation_issue *issue = &out->issues[out->issue_count++];
    issue->severity = severity;
    (void)snprintf(issue->code, sizeof issue->code, "%s", code);
}

tp_status tp_validate_session_snapshot_target(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 target_id, tp_target_validation_report *out, tp_error *err) {
    if (!snapshot || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "target validation needs snapshot and output");
    }
    memset(out, 0, sizeof *out);
    const tp_snapshot_target *target = tp_session_snapshot_target_by_id(
        snapshot, atlas_id, target_id);
    if (!target) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "validation target was not found");
    }
    const bool shared = target->enabled && target->out_path &&
                        target->out_path[0] != '\0' &&
                        tp_session_snapshot_target_out_path_shared(
                            snapshot, atlas_id, target_id, target->out_path);
    const unsigned issues = target_issue_mask(
        target->exporter_id, target->enabled, target->out_path, shared);
    if ((issues & TARGET_ISSUE_UNKNOWN_EXPORTER) != 0U) {
        target_issue_add(out, TP_VALIDATION_ERROR,
                         TP_VALIDATION_CODE_UNKNOWN_EXPORTER);
    }
    if ((issues & TARGET_ISSUE_NO_OUT_PATH) != 0U) {
        target_issue_add(out, TP_VALIDATION_ERROR,
                         TP_VALIDATION_CODE_TARGET_NO_OUT_PATH);
    }
    if ((issues & TARGET_ISSUE_DUPLICATE_OUT_PATH) != 0U) {
        target_issue_add(out, TP_VALIDATION_WARNING,
                         TP_VALIDATION_CODE_DUPLICATE_OUT_PATH);
    }
    return TP_STATUS_OK;
}
