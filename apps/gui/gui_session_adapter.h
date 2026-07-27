#ifndef NTPACKER_GUI_SESSION_ADAPTER_H
#define NTPACKER_GUI_SESSION_ADAPTER_H

#include <stddef.h>

#include "tp_core/tp_operation.h"
#include "gui_session_client.h"

/* Thin stable-ID -> typed-operation -> session adapter for GUI mutations.
 * It never owns or exposes a model/project pointer. */
tp_status gui_session_rename_atlas(gui_session_client *client, tp_id128 atlas_id,
                                   int64_t expected_revision, const char *name,
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
tp_status gui_session_set_sprite_name(gui_session_client *client, tp_id128 atlas_id,
                                      tp_id128 source_id, const char *source_key,
                                      int64_t expected_revision, const char *name,
                                      tp_error *err);
tp_status gui_session_set_sprite_override(gui_session_client *client, tp_id128 atlas_id,
                                          tp_id128 source_id, const char *source_key,
                                          int64_t expected_revision,
                                          const tp_op_sprite_set *settings,
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
tp_status gui_session_rename_animation(gui_session_client *client, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision, const char *name,
                                       tp_error *err);
tp_status gui_session_set_animation_settings(gui_session_client *client, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision,
                                             const tp_op_anim_settings *settings,
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
                                 gui_session_submit_terminal *out_terminal,
                                 tp_error *err);

/* Copies the presentation value from an owned snapshot. */
tp_status gui_session_copy_atlas_name(const tp_session_snapshot *snapshot,
                                      tp_id128 atlas_id,
                                      char *out, size_t capacity, tp_error *err);

#endif /* NTPACKER_GUI_SESSION_ADAPTER_H */
