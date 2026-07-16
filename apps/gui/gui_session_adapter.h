#ifndef NTPACKER_GUI_SESSION_ADAPTER_H
#define NTPACKER_GUI_SESSION_ADAPTER_H

#include <stddef.h>

#include "tp_core/tp_session.h"
#include "tp_core/tp_operation.h"

/* Thin GUI intent adapter for the first M1 cutover family. Atlas indexes are
 * resolved against an owned snapshot before the canonical ID transaction is
 * admitted. The adapter never owns or exposes a model/project pointer. */
tp_status gui_session_rename_atlas(tp_session *session, tp_id128 atlas_id,
                                   int64_t expected_revision, const char *name,
                                   const char *transaction_id,
                                   tp_error *err);

tp_status gui_session_create_atlas(tp_session *session, tp_id128 atlas_id,
                                   tp_id128 target_id, int64_t expected_revision,
                                   const char *name, const char *exporter_id,
                                   const char *out_path, bool target_enabled,
                                   const char *transaction_id, tp_error *err);
tp_status gui_session_remove_atlas(tp_session *session, tp_id128 atlas_id,
                                   int64_t expected_revision,
                                   const char *transaction_id, tp_error *err);
tp_status gui_session_set_atlas_settings(tp_session *session, tp_id128 atlas_id,
                                         int64_t expected_revision,
                                         const tp_op_atlas_settings *settings,
                                         const char *transaction_id, tp_error *err);
tp_status gui_session_add_sources(tp_session *session, tp_id128 atlas_id,
                                  const tp_id128 *source_ids,
                                  const char *const *paths, int source_count,
                                  tp_snapshot_source_kind kind,
                                  int64_t expected_revision,
                                  const char *transaction_id, tp_error *err);
tp_status gui_session_remove_source(tp_session *session, tp_id128 atlas_id,
                                    tp_id128 source_id,
                                    int64_t expected_revision,
                                    const char *transaction_id, tp_error *err);
tp_status gui_session_set_sprite_name(tp_session *session, tp_id128 atlas_id,
                                      tp_id128 source_id, const char *source_key,
                                      int64_t expected_revision, const char *name,
                                      const char *transaction_id, tp_error *err);
tp_status gui_session_set_sprite_override(tp_session *session, tp_id128 atlas_id,
                                          tp_id128 source_id, const char *source_key,
                                          int64_t expected_revision,
                                          const tp_op_sprite_set *settings,
                                          const char *transaction_id, tp_error *err);

tp_status gui_session_create_animation(tp_session *session, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision, const char *name,
                                       const char *const *frames, int frame_count,
                                       const char *transaction_id, tp_error *err);
tp_status gui_session_remove_animation(tp_session *session, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision,
                                       const char *transaction_id, tp_error *err);
tp_status gui_session_rename_animation(tp_session *session, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision, const char *name,
                                       const char *transaction_id, tp_error *err);
tp_status gui_session_set_animation_settings(tp_session *session, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision,
                                             const tp_op_anim_settings *settings,
                                             const char *transaction_id,
                                             tp_error *err);
tp_status gui_session_add_animation_frames(tp_session *session, tp_id128 atlas_id,
                                           tp_id128 animation_id,
                                           int64_t expected_revision,
                                           const char *const *frames, int frame_count,
                                           const char *transaction_id, tp_error *err);
tp_status gui_session_remove_animation_frame(tp_session *session, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision, int frame_index,
                                             const char *transaction_id, tp_error *err);
tp_status gui_session_move_animation_frame(tp_session *session, tp_id128 atlas_id,
                                           tp_id128 animation_id,
                                           int64_t expected_revision, int from_index,
                                           int to_index, const char *transaction_id,
                                           tp_error *err);

tp_status gui_session_create_target(tp_session *session, tp_id128 atlas_id,
                                    tp_id128 target_id, int64_t expected_revision,
                                    const char *exporter_id, const char *out_path,
                                    bool enabled, const char *transaction_id,
                                    tp_error *err);
tp_status gui_session_remove_target(tp_session *session, tp_id128 atlas_id,
                                    tp_id128 target_id, int64_t expected_revision,
                                    const char *transaction_id, tp_error *err);
tp_status gui_session_set_target(tp_session *session, tp_id128 atlas_id,
                                 tp_id128 target_id, int64_t expected_revision,
                                 const tp_op_target_set *settings,
                                 const char *transaction_id, tp_error *err);

/* Copies the presentation value from an owned snapshot. */
tp_status gui_session_copy_atlas_name(const tp_session_snapshot *snapshot,
                                      tp_id128 atlas_id,
                                      char *out, size_t capacity, tp_error *err);

#endif /* NTPACKER_GUI_SESSION_ADAPTER_H */
