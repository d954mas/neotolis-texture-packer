#ifndef TP_CORE_SRC_TP_PROJECT_MUTATION_INTERNAL_H
#define TP_CORE_SRC_TP_PROJECT_MUTATION_INTERNAL_H

#include "tp_core/tp_project.h"

/* Low-level project mutation and mutable lookup surface. The operation/session
 * layer owns product mutation authority; these primitives are limited to the
 * core implementation and white-box tests. Borrowed pointers are invalidated by
 * collection mutation or replacement of the owning project. */

/* Atlas. */
tp_status tp_project_add_atlas(tp_project *project, const char *name,
                               int *out_index);
tp_status tp_project_remove_atlas(tp_project *project, int index);
tp_project_atlas *tp_project_get_atlas(tp_project *project, int index);
tp_status tp_project_set_atlas_name(tp_project_atlas *atlas,
                                    const char *name);

/* Source. */
tp_status tp_project_atlas_add_source_kind(tp_project_atlas *atlas,
                                           const char *path,
                                           tp_source_kind kind);
tp_status tp_project_atlas_add_source(tp_project_atlas *atlas,
                                      const char *path);
tp_status tp_project_atlas_remove_source(tp_project_atlas *atlas, int index);
tp_project_source *tp_project_atlas_find_source_by_id(tp_project_atlas *atlas,
                                                       tp_id128 id);
tp_status tp_project_atlas_remove_source_by_id(tp_project_atlas *atlas,
                                                tp_id128 id);

/* Sparse sprite overrides: canonical (source,key) records only. */
tp_project_sprite *tp_project_atlas_find_sprite_by_source_key(
    tp_project_atlas *atlas, tp_id128 source_ref, const char *src_key);
tp_status tp_project_atlas_add_sprite_by_source_key(
    tp_project_atlas *atlas, tp_id128 source_ref, const char *src_key,
    tp_project_sprite **out);
tp_status tp_project_atlas_remove_sprite_by_source_key(
    tp_project_atlas *atlas, tp_id128 source_ref, const char *src_key);
tp_status tp_project_atlas_prune_sprite_by_source_key(
    tp_project_atlas *atlas, tp_id128 source_ref, const char *src_key);
tp_status tp_project_atlas_set_sprite_rename_by_source_key(
    tp_project_atlas *atlas, tp_id128 source_ref, const char *src_key,
    const char *rename);

/* Animation. */
tp_status tp_project_atlas_add_animation(tp_project_atlas *atlas,
                                         const char *name,
                                         tp_project_anim **out);
tp_project_anim *tp_project_atlas_find_animation_by_id(
    tp_project_atlas *atlas, tp_id128 id);
tp_status tp_project_atlas_remove_animation_by_id(tp_project_atlas *atlas,
                                                   tp_id128 id);
tp_status tp_project_anim_add_frame(tp_project_anim *animation,
                                    tp_id128 source_ref,
                                    const char *src_key);
tp_status tp_project_anim_remove_frame(tp_project_anim *animation, int index);
tp_status tp_project_anim_move_frame(tp_project_anim *animation, int index,
                                     int delta);

/* Export target. */
tp_status tp_project_atlas_add_target(tp_project_atlas *atlas,
                                      const char *exporter_id,
                                      const char *out_path,
                                      tp_project_target **out);
tp_status tp_project_atlas_remove_target(tp_project_atlas *atlas, int index);
tp_project_target *tp_project_atlas_find_target_by_id(tp_project_atlas *atlas,
                                                       tp_id128 id);
tp_status tp_project_atlas_remove_target_by_id(tp_project_atlas *atlas,
                                                tp_id128 id);
tp_status tp_project_atlas_set_target(tp_project_atlas *atlas, int index,
                                      const char *exporter_id,
                                      const char *out_path, bool enabled);
tp_status tp_project_atlas_seed_default_target(tp_project *project,
                                                int atlas_index);

#endif /* TP_CORE_SRC_TP_PROJECT_MUTATION_INTERNAL_H */
