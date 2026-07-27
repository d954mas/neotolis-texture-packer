#ifndef NTPACKER_GUI_SESSION_ADAPTER_H
#define NTPACKER_GUI_SESSION_ADAPTER_H

#include <stddef.h>

#include "tp_core/tp_operation.h"
#include "gui_session_client.h"

/* Thin stable-ID -> typed-operation -> session adapter for GUI mutations.
 * It never owns or exposes a model/project pointer.
 *
 * Receipt contract. Two intent shapes live here and the difference is visible
 * in the signature:
 *   - DRAFT intents take `identity` + `transaction_id` + `out_terminal`. They
 *     back an editable value owned by the settings-panel draft FSM, which stays
 *     in SUBMITTING until it reads back its OWN typed receipt. For these,
 *     gui_session_client_submit guarantees a terminal on every non-OK return
 *     (an empty terminal transaction id means "the session never saw it", and
 *     the draft owner returns the draft to EDITING).
 *   - STRUCTURAL intents (create/remove atlas, source, target, animation and
 *     animation-frame add/remove/move) have NO `out_terminal`: they are discrete
 *     commands, not editable values, so they are RECEIPT-FREE BY CONTRACT. Their
 *     caller decides from the returned tp_status alone and there is no draft
 *     phase to strand. Do not add receipts to them to "make it uniform". */
tp_status gui_session_submit_atlas_name(
    gui_session_client *client, tp_id128 atlas_id,
    int64_t expected_revision, const char *name,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *out_terminal,
    tp_error *err);

tp_status gui_session_create_atlas(gui_session_client *client, tp_id128 atlas_id,
                                   tp_id128 target_id, int64_t expected_revision,
                                   const char *name, const char *exporter_id,
                                   const char *out_path, bool target_enabled,
                                   gui_session_submit_terminal *out_terminal,
                                   tp_error *err);
tp_status gui_session_remove_atlas(gui_session_client *client, tp_id128 atlas_id,
                                   int64_t expected_revision,
                                   tp_error *err);
tp_status gui_session_set_atlas_settings(gui_session_client *client, tp_id128 atlas_id,
                                         int64_t expected_revision,
                                         const tp_op_atlas_settings *settings,
                                         gui_session_submit_identity identity,
                                         const char transaction_id[33],
                                         gui_session_submit_terminal *out_terminal,
                                         tp_error *err);
tp_status gui_session_add_sources(gui_session_client *client, tp_id128 atlas_id,
                                  const tp_id128 *source_ids,
                                  const char *const *paths, int source_count,
                                  tp_snapshot_source_kind kind,
                                  int64_t expected_revision,
                                  tp_error *err);
tp_status gui_session_remove_source(gui_session_client *client, tp_id128 atlas_id,
                                    tp_id128 source_id,
                                    int64_t expected_revision,
                                    tp_error *err);
tp_status gui_session_submit_sprite_name(
    gui_session_client *client, tp_id128 atlas_id,
    tp_id128 source_id, const char *source_key,
    int64_t expected_revision, const char *name,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *out_terminal,
    tp_error *err);
tp_status gui_session_set_sprite_override(gui_session_client *client, tp_id128 atlas_id,
                                          tp_id128 source_id, const char *source_key,
                                          int64_t expected_revision,
                                          const tp_op_sprite_set *settings,
                                          gui_session_submit_identity identity,
                                          const char transaction_id[33],
                                          gui_session_submit_terminal *out_terminal,
                                          tp_error *err);

tp_status gui_session_create_animation(gui_session_client *client, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision, const char *name,
                                       const tp_op_sprite_ref *frames,
                                       int frame_count,
                                       gui_session_submit_terminal *out_terminal,
                                       tp_error *err);
tp_status gui_session_remove_animation(gui_session_client *client, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision,
                                       tp_error *err);
tp_status gui_session_submit_animation_name(
    gui_session_client *client, tp_id128 atlas_id,
    tp_id128 animation_id, int64_t expected_revision,
    const char *name, gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *out_terminal,
    tp_error *err);
tp_status gui_session_set_animation_settings(gui_session_client *client, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision,
                                             const tp_op_anim_settings *settings,
                                             gui_session_submit_identity identity,
                                             const char transaction_id[33],
                                             gui_session_submit_terminal *out_terminal,
                                             tp_error *err);
tp_status gui_session_add_animation_frames(gui_session_client *client, tp_id128 atlas_id,
                                           tp_id128 animation_id,
                                           int64_t expected_revision,
                                           const tp_op_sprite_ref *frames,
                                           int frame_count,
                                           tp_error *err);
tp_status gui_session_remove_animation_frame(gui_session_client *client, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision, int frame_index,
                                             tp_error *err);
tp_status gui_session_move_animation_frame(gui_session_client *client, tp_id128 atlas_id,
                                           tp_id128 animation_id,
                                           int64_t expected_revision, int from_index,
                                           int to_index,
                                           tp_error *err);

tp_status gui_session_create_target(gui_session_client *client, tp_id128 atlas_id,
                                    tp_id128 target_id, int64_t expected_revision,
                                    const char *exporter_id, const char *out_path,
                                    bool enabled,
                                    gui_session_submit_terminal *out_terminal,
                                    tp_error *err);
tp_status gui_session_remove_target(gui_session_client *client, tp_id128 atlas_id,
                                    tp_id128 target_id, int64_t expected_revision,
                                    tp_error *err);
tp_status gui_session_set_target(gui_session_client *client, tp_id128 atlas_id,
                                 tp_id128 target_id, int64_t expected_revision,
                                 const tp_op_target_set *settings,
                                 gui_session_submit_identity identity,
                                 const char transaction_id[33],
                                 gui_session_submit_terminal *out_terminal,
                                 tp_error *err);
tp_status gui_session_submit_target_out_path(
    gui_session_client *client, tp_id128 atlas_id,
    tp_id128 target_id, int64_t expected_revision,
    const char *out_path,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *out_terminal,
    tp_error *err);

/* Copies the presentation value from an owned snapshot. */
tp_status gui_session_copy_atlas_name(const tp_session_snapshot *snapshot,
                                      tp_id128 atlas_id,
                                      char *out, size_t capacity, tp_error *err);

#endif /* NTPACKER_GUI_SESSION_ADAPTER_H */
