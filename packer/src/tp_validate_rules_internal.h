#ifndef TP_VALIDATE_RULES_INTERNAL_H
#define TP_VALIDATE_RULES_INTERNAL_H

#include "tp_core/tp_project.h"
#include "tp_validate_index_internal.h"
#include "tp_validate_report_internal.h"

finding_context context_atlas(const tp_project_atlas *atlas);
finding_context context_source(const tp_project_atlas *atlas,
                               const tp_project_source *source);
finding_context context_sprite(const tp_project_atlas *atlas,
                               tp_id128 source_id, const char *sprite);
finding_context context_frame(const tp_project_atlas *atlas,
                              const tp_project_anim *animation,
                              const tp_project_frame *frame);
finding_context context_animation(const tp_project_atlas *atlas,
                                  const tp_project_anim *animation);
finding_context context_target(const tp_project_atlas *atlas,
                               const tp_project_target *target);

char *validation_slash_norm_owned(const char *source);
void validate_source_domain(validation_builder *builder,
                            const tp_project *project,
                            const tp_project_atlas *atlas);
void validate_sprite_animation_domain(validation_builder *builder,
                                      const tp_project *project,
                                      int atlas_index,
                                      const tp_project_atlas *atlas);

typedef struct target_path_index {
    char ***by_atlas;
    int atlas_count;
    str_index counts;
} target_path_index;

bool target_path_index_build(const tp_project *project,
                             target_path_index *out);
void target_path_index_free(target_path_index *index,
                            const tp_project *project);
void validate_target_settings_domain(
    validation_builder *builder, const tp_project *project, int atlas_index,
    const tp_project_atlas *atlas, const target_path_index *target_paths);

#endif /* TP_VALIDATE_RULES_INTERNAL_H */
