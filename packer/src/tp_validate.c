/* Core-owned saved-project validation. Frontends receive an owned typed report
 * and retain only presentation/exit mapping; no validation rule lives in an adapter. */
#include "tp_core/tp_validate.h"

#include <string.h>

#include "tp_core/tp_project.h"
#include "tp_session_internal.h"
#include "tp_validate_internal.h"
#include "tp_validate_index_internal.h"
#include "tp_validate_report_internal.h"
#include "tp_validate_rules_internal.h"

finding_context context_atlas(const tp_project_atlas *atlas) {
    finding_context context = {0};
    context.atlas = atlas ? atlas->name : NULL;
    context.atlas_id = atlas ? atlas->id : tp_id128_nil();
    return context;
}

finding_context context_source(const tp_project_atlas *atlas,
                                      const tp_project_source *source) {
    finding_context context = context_atlas(atlas);
    context.source = source ? source->path : NULL;
    context.source_id = source ? source->id : tp_id128_nil();
    return context;
}

finding_context context_sprite(const tp_project_atlas *atlas,
                                      tp_id128 source_id,
                                      const char *sprite) {
    finding_context context = context_atlas(atlas);
    context.source_id = source_id;
    context.sprite = sprite;
    return context;
}

finding_context context_frame(const tp_project_atlas *atlas,
                                     const tp_project_anim *animation,
                                     const tp_project_frame *frame) {
    finding_context context = context_atlas(atlas);
    context.source_id = frame ? frame->source_ref : tp_id128_nil();
    context.anim = animation ? animation->name : NULL;
    context.animation_id = animation ? animation->id : tp_id128_nil();
    context.frame = frame ? frame->name : NULL;
    return context;
}

finding_context context_animation(const tp_project_atlas *atlas,
                                         const tp_project_anim *animation) {
    finding_context context = context_atlas(atlas);
    context.anim = animation ? animation->name : NULL;
    context.animation_id = animation ? animation->id : tp_id128_nil();
    return context;
}

finding_context context_target(const tp_project_atlas *atlas,
                                      const tp_project_target *target) {
    finding_context context = context_atlas(atlas);
    context.target = target ? target->exporter_id : NULL;
    context.target_id = target ? target->id : tp_id128_nil();
    return context;
}

void tp_validate__test_work_reset(void) { tp_validate_work_probes = 0U; }
tp_validate_work_stats tp_validate__test_work_get(void) {
    return (tp_validate_work_stats){tp_validate_work_probes};
}

static void validate_atlas(validation_builder *fs, const tp_project *p, int ai,
                           const target_path_index *target_paths) {
    const tp_project_atlas *a = &p->atlases[ai];

    validate_source_domain(fs, p, a);
    validate_sprite_animation_domain(fs, p, ai, a);
    validate_target_settings_domain(fs, p, ai, a, target_paths);
}

static tp_status validate_project(const tp_project *project,
                                  tp_validation_report *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: NULL report");
    }
    memset(out, 0, sizeof *out);
    if (!project) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: NULL project");
    }

    target_path_index target_paths = {0};
    if (!target_path_index_build(project, &target_paths)) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "out of memory indexing validation target paths");
    }

    validation_builder builder = {0};
    for (int atlas_index = 0; atlas_index < project->atlas_count; atlas_index++) {
        validate_atlas(&builder, project, atlas_index, &target_paths);
        if (builder.oom) {
            break;
        }
    }
    target_path_index_free(&target_paths, project);

    return validation_builder_finish(&builder, out, err);
}

tp_status tp_validate_session_snapshot(const tp_session_snapshot *snapshot,
                                       tp_validation_report *out, tp_error *err) {
    if (!snapshot) {
        if (out) {
            memset(out, 0, sizeof *out);
        }
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: NULL snapshot");
    }
    return validate_project(tp_session_snapshot_project_internal(snapshot), out, err);
}

tp_status tp_validate_project_file(const char *path, tp_validation_report *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: NULL report");
    }
    memset(out, 0, sizeof *out);
    if (!path || path[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "validate: empty project path");
    }
    tp_project *project = NULL;
    tp_status status = tp_project_load(path, &project, err);
    if (status == TP_STATUS_OK) {
        status = validate_project(project, out, err);
    }
    tp_project_destroy(project);
    return status;
}
