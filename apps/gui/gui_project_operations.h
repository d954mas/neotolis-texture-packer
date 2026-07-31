#ifndef NTPACKER_GUI_PROJECT_OPERATIONS_H
#define NTPACKER_GUI_PROJECT_OPERATIONS_H

#include <stddef.h>

#include "tp_core/tp_operation.h"
#include "gui_project.h"

/* Thin stable-ID -> typed-operation -> session adapter for GUI mutations.
 * It never owns or exposes a model/project pointer.
 *
 * Receipt contract. Three intent shapes live here and the difference is visible
 * in the signature:
 *   - DRAFT intents take `identity` + `transaction_id` + `out_terminal`. They
 *     back an editable value owned by the settings-panel draft FSM, which stays
 *     in SUBMITTING until it reads back its OWN typed receipt. For these,
 *     the session submit path guarantees a terminal on every non-OK return
 *     (an empty terminal transaction id means "the session never saw it", and
 *     the draft owner returns the draft to EDITING).
 *   - CREATE intents (`gui_project_operation_create_atlas`, `_create_animation`,
 *     `_create_target`) take `out_terminal` but NOT `identity`/`transaction_id`:
 *     they are not drafts, and nothing stays in SUBMITTING for them, but their
 *     caller reads the committed revision back out of the terminal to resolve
 *     the created object's visible index.
 *   - RECEIPT-FREE intents have no `out_terminal` at all. The set is exactly
 *     these eight: `gui_project_operation_remove_atlas`, `_add_sources`,
 *     `_remove_source`, `_remove_animation`, `_add_animation_frames`,
 *     `_remove_animation_frame`, `_move_animation_frame`, `_remove_target`.
 *     They are discrete commands, not editable values, so they are RECEIPT-FREE
 *     BY CONTRACT: the caller decides from the returned tp_status alone and
 *     there is no draft phase to strand. Do not add receipts to them to "make
 *     it uniform". */
tp_status gui_project_operation_submit_atlas_name(
    tp_session *client, tp_id128 atlas_id,
    int64_t expected_revision, const char *name,
    gui_project_operation_submit_identity identity,
    const char transaction_id[33],
    gui_project_operation_submit_terminal *out_terminal,
    tp_error *err);

tp_status gui_project_operation_create_atlas(tp_session *client, tp_id128 atlas_id,
                                   tp_id128 target_id, int64_t expected_revision,
                                   const char *name, const char *exporter_id,
                                   const char *out_path, bool target_enabled,
                                   gui_project_operation_submit_terminal *out_terminal,
                                   tp_error *err);
tp_status gui_project_operation_remove_atlas(tp_session *client, tp_id128 atlas_id,
                                   int64_t expected_revision,
                                   tp_error *err);
tp_status gui_project_operation_set_atlas_settings(tp_session *client, tp_id128 atlas_id,
                                         int64_t expected_revision,
                                         const tp_op_atlas_settings *settings,
                                         gui_project_operation_submit_identity identity,
                                         const char transaction_id[33],
                                         gui_project_operation_submit_terminal *out_terminal,
                                         tp_error *err);
tp_status gui_project_operation_add_sources(tp_session *client, tp_id128 atlas_id,
                                  const tp_id128 *source_ids,
                                  const char *const *paths, int source_count,
                                  tp_snapshot_source_kind kind,
                                  int64_t expected_revision,
                                  tp_error *err);
tp_status gui_project_operation_remove_source(tp_session *client, tp_id128 atlas_id,
                                    tp_id128 source_id,
                                    int64_t expected_revision,
                                    tp_error *err);
tp_status gui_project_operation_submit_sprite_name(
    tp_session *client, tp_id128 atlas_id,
    tp_id128 source_id, const char *source_key,
    int64_t expected_revision, const char *name,
    gui_project_operation_submit_identity identity,
    const char transaction_id[33],
    gui_project_operation_submit_terminal *out_terminal,
    tp_error *err);
tp_status gui_project_operation_set_sprite_override(tp_session *client, tp_id128 atlas_id,
                                          tp_id128 source_id, const char *source_key,
                                          int64_t expected_revision,
                                          const tp_op_sprite_set *settings,
                                          gui_project_operation_submit_identity identity,
                                          const char transaction_id[33],
                                          gui_project_operation_submit_terminal *out_terminal,
                                          tp_error *err);
tp_status gui_project_operation_clear_sprite_override(
    tp_session *client, tp_id128 atlas_id,
    tp_id128 source_id, const char *source_key,
    int64_t expected_revision, uint32_t mask,
    gui_project_operation_submit_identity identity,
    const char transaction_id[33],
    gui_project_operation_submit_terminal *out_terminal,
    tp_error *err);

tp_status gui_project_operation_create_animation(tp_session *client, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision, const char *name,
                                       const tp_op_sprite_ref *frames,
                                       int frame_count,
                                       gui_project_operation_submit_terminal *out_terminal,
                                       tp_error *err);
tp_status gui_project_operation_remove_animation(tp_session *client, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision,
                                       tp_error *err);
tp_status gui_project_operation_submit_animation_name(
    tp_session *client, tp_id128 atlas_id,
    tp_id128 animation_id, int64_t expected_revision,
    const char *name, gui_project_operation_submit_identity identity,
    const char transaction_id[33],
    gui_project_operation_submit_terminal *out_terminal,
    tp_error *err);
tp_status gui_project_operation_set_animation_settings(tp_session *client, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision,
                                             const tp_op_anim_settings *settings,
                                             gui_project_operation_submit_identity identity,
                                             const char transaction_id[33],
                                             gui_project_operation_submit_terminal *out_terminal,
                                             tp_error *err);
tp_status gui_project_operation_add_animation_frames(tp_session *client, tp_id128 atlas_id,
                                           tp_id128 animation_id,
                                           int64_t expected_revision,
                                           const tp_op_sprite_ref *frames,
                                           int frame_count,
                                           tp_error *err);
tp_status gui_project_operation_remove_animation_frame(tp_session *client, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision, int frame_index,
                                             tp_error *err);
tp_status gui_project_operation_move_animation_frame(tp_session *client, tp_id128 atlas_id,
                                           tp_id128 animation_id,
                                           int64_t expected_revision, int from_index,
                                           int to_index,
                                           tp_error *err);

tp_status gui_project_operation_create_target(tp_session *client, tp_id128 atlas_id,
                                    tp_id128 target_id, int64_t expected_revision,
                                    const char *exporter_id, const char *out_path,
                                    bool enabled,
                                    gui_project_operation_submit_terminal *out_terminal,
                                    tp_error *err);
tp_status gui_project_operation_remove_target(tp_session *client, tp_id128 atlas_id,
                                    tp_id128 target_id, int64_t expected_revision,
                                    tp_error *err);
tp_status gui_project_operation_set_target(tp_session *client, tp_id128 atlas_id,
                                 tp_id128 target_id, int64_t expected_revision,
                                 const tp_op_target_set *settings,
                                 gui_project_operation_submit_identity identity,
                                 const char transaction_id[33],
                                 gui_project_operation_submit_terminal *out_terminal,
                                 tp_error *err);
tp_status gui_project_operation_submit_target_out_path(
    tp_session *client, tp_id128 atlas_id,
    tp_id128 target_id, int64_t expected_revision,
    const char *out_path,
    gui_project_operation_submit_identity identity,
    const char transaction_id[33],
    gui_project_operation_submit_terminal *out_terminal,
    tp_error *err);

/* Copies the presentation value from an owned snapshot. */
tp_status gui_project_operation_copy_atlas_name(const tp_session_snapshot *snapshot,
                                      tp_id128 atlas_id,
                                      char *out, size_t capacity, tp_error *err);

#endif /* NTPACKER_GUI_PROJECT_OPERATIONS_H */
