#include "gui_session_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_transaction.h"

static char *adapter_dup(const char *text) {
    if (!text) {
        return NULL;
    }
    const size_t length = strlen(text) + 1U;
    char *copy = (char *)malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

static tp_status copy_frame_ref(const tp_op_sprite_ref *input,
                                tp_op_sprite_ref *output, tp_error *err) {
    if (!input || !output || !input->src_key) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "animation frame copy requires a source-key pointer");
    }
    output->source_id = input->source_id;
    output->src_key = adapter_dup(input->src_key);
    if (!output->src_key) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "animation frame reference allocation failed");
    }
    return TP_STATUS_OK;
}

static tp_status apply_atlas_ops(tp_session *session, tp_operation *operations,
                                 int operation_count, int64_t expected_revision,
                                 const char *transaction_id, tp_error *err) {
    if (!session || !operations || operation_count <= 0 || !transaction_id) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid atlas session intent");
    }
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    const int id_length = snprintf(request.id_hex, sizeof request.id_hex, "%s", transaction_id);
    if (id_length < 0 || (size_t)id_length >= sizeof request.id_hex) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "transaction id is too long");
    }
    request.expected_revision = expected_revision;
    request.ops = operations;
    request.op_count = operation_count;

    tp_txn_result result;
    memset(&result, 0, sizeof result);
    const tp_status status = tp_session_apply(session, &request, &result, err);
    if (status != TP_STATUS_OK && result.error_count > 0 &&
        result.errors[0].message[0] != '\0') {
        (void)tp_error_set(err, status, "%s", result.errors[0].message);
    }
    tp_txn_result_free(&result);
    return status;
}

tp_status gui_session_rename_atlas(tp_session *session, tp_id128 atlas_id,
                                   int64_t expected_revision, const char *name,
                                   const char *transaction_id, tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas_id;
    operation.u.atlas_rename.name = (char *)name;
    return apply_atlas_ops(session, &operation, 1, expected_revision, transaction_id, err);
}

tp_status gui_session_create_atlas(tp_session *session, tp_id128 atlas_id,
                                   tp_id128 target_id, int64_t expected_revision,
                                   const char *name, const char *exporter_id,
                                   const char *out_path, bool target_enabled,
                                   const char *transaction_id, tp_error *err) {
    if (!name || !exporter_id || !out_path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "atlas create requires non-null default fields");
    }
    tp_operation operations[2];
    memset(operations, 0, sizeof operations);
    operations[0].kind = TP_OP_ATLAS_CREATE;
    operations[0].atlas_id = atlas_id;
    operations[0].u.atlas_create.name = adapter_dup(name);
    operations[1].kind = TP_OP_TARGET_CREATE;
    operations[1].atlas_id = atlas_id;
    operations[1].u.target_create.target_id = target_id;
    operations[1].u.target_create.exporter_id = adapter_dup(exporter_id);
    operations[1].u.target_create.out_path = adapter_dup(out_path);
    operations[1].u.target_create.enabled = target_enabled;
    if (!operations[0].u.atlas_create.name || !operations[1].u.target_create.exporter_id ||
        !operations[1].u.target_create.out_path) {
        tp_operation_free(&operations[0]);
        tp_operation_free(&operations[1]);
        return tp_error_set(err, TP_STATUS_OOM, "atlas create intent allocation failed");
    }
    const tp_status status = apply_atlas_ops(session, operations, 2, expected_revision,
                                             transaction_id, err);
    tp_operation_free(&operations[0]);
    tp_operation_free(&operations[1]);
    return status;
}

tp_status gui_session_remove_atlas(tp_session *session, tp_id128 atlas_id,
                                   int64_t expected_revision,
                                   const char *transaction_id, tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_REMOVE;
    operation.atlas_id = atlas_id;
    return apply_atlas_ops(session, &operation, 1, expected_revision, transaction_id, err);
}

tp_status gui_session_set_atlas_settings(tp_session *session, tp_id128 atlas_id,
                                         int64_t expected_revision,
                                         const tp_op_atlas_settings *settings,
                                         const char *transaction_id, tp_error *err) {
    if (!settings) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "atlas settings require id and payload");
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_SETTINGS_SET;
    operation.atlas_id = atlas_id;
    operation.u.atlas_settings = *settings;
    return apply_atlas_ops(session, &operation, 1, expected_revision, transaction_id, err);
}

tp_status gui_session_add_sources(tp_session *session, tp_id128 atlas_id,
                                  const tp_id128 *source_ids,
                                  const char *const *paths, int source_count,
                                  tp_snapshot_source_kind kind,
                                  int64_t expected_revision,
                                  const char *transaction_id, tp_error *err) {
    if (!source_ids || !paths || source_count <= 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid source add intent");
    }
    tp_operation *operations = (tp_operation *)calloc((size_t)source_count,
                                                       sizeof *operations);
    if (!operations) {
        return tp_error_set(err, TP_STATUS_OOM, "source add intent allocation failed");
    }
    tp_status status = TP_STATUS_OK;
    for (int i = 0; i < source_count; i++) {
        if (!paths[i]) {
            status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                  "source add requires id and path");
            break;
        }
        tp_operation *operation = &operations[i];
        operation->kind = TP_OP_SOURCE_ADD;
        operation->atlas_id = atlas_id;
        operation->u.source_add.source_id = source_ids[i];
        operation->u.source_add.kind = (tp_source_kind)kind;
        operation->u.source_add.key = adapter_dup(paths[i]);
        if (!operation->u.source_add.key) {
            status = tp_error_set(err, TP_STATUS_OOM,
                                  "source add intent allocation failed");
            break;
        }
    }
    if (status == TP_STATUS_OK) {
        status = apply_atlas_ops(session, operations, source_count,
                                 expected_revision, transaction_id, err);
    }
    for (int i = 0; i < source_count; i++) {
        tp_operation_free(&operations[i]);
    }
    free(operations);
    return status;
}

tp_status gui_session_remove_source(tp_session *session, tp_id128 atlas_id,
                                    tp_id128 source_id,
                                    int64_t expected_revision,
                                    const char *transaction_id, tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SOURCE_REMOVE;
    operation.atlas_id = atlas_id;
    operation.u.source_ref.source_id = source_id;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_set_sprite_name(tp_session *session, tp_id128 atlas_id,
                                      tp_id128 source_id, const char *source_key,
                                      int64_t expected_revision, const char *name,
                                      const char *transaction_id, tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_NAME_SET;
    operation.atlas_id = atlas_id;
    operation.u.sprite_name.source_id = source_id;
    operation.u.sprite_name.src_key = (char *)source_key;
    operation.u.sprite_name.name = (char *)name;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_set_sprite_override(tp_session *session, tp_id128 atlas_id,
                                          tp_id128 source_id, const char *source_key,
                                          int64_t expected_revision,
                                          const tp_op_sprite_set *settings,
                                          const char *transaction_id, tp_error *err) {
    if (!settings) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "sprite settings require structural reference and payload");
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_OVERRIDE_SET;
    operation.atlas_id = atlas_id;
    operation.u.sprite_set = *settings;
    operation.u.sprite_set.source_id = source_id;
    operation.u.sprite_set.src_key = (char *)source_key;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_create_animation(tp_session *session, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision, const char *name,
                                       const tp_op_sprite_ref *frames,
                                       int frame_count,
                                       const char *transaction_id, tp_error *err) {
    if (!name || frame_count < 0 || (frame_count > 0 && !frames)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid animation create intent");
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_CREATE;
    operation.atlas_id = atlas_id;
    operation.u.anim_create.anim_id = animation_id;
    operation.u.anim_create.name = adapter_dup(name);
    operation.u.anim_create.fps = TP_PROJECT_ANIM_FPS_DEFAULT;
    operation.u.anim_create.playback = TP_PROJECT_ANIM_PLAYBACK_DEFAULT;
    if (frame_count > 0) {
        operation.u.anim_create.frames = (tp_op_sprite_ref *)calloc(
            (size_t)frame_count, sizeof *operation.u.anim_create.frames);
    }
    if (!operation.u.anim_create.name ||
        (frame_count > 0 && !operation.u.anim_create.frames)) {
        tp_operation_free(&operation);
        return tp_error_set(err, TP_STATUS_OOM,
                            "animation create intent allocation failed");
    }
    operation.u.anim_create.frame_count = frame_count;
    tp_status status = TP_STATUS_OK;
    for (int i = 0; i < frame_count; ++i) {
        status = copy_frame_ref(&frames[i],
                                &operation.u.anim_create.frames[i], err);
        if (status != TP_STATUS_OK) {
            tp_operation_free(&operation);
            return status;
        }
    }
    status = apply_atlas_ops(session, &operation, 1, expected_revision,
                             transaction_id, err);
    tp_operation_free(&operation);
    return status;
}

tp_status gui_session_remove_animation(tp_session *session, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision,
                                       const char *transaction_id, tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_REMOVE;
    operation.atlas_id = atlas_id;
    operation.u.anim_ref.anim_id = animation_id;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_rename_animation(tp_session *session, tp_id128 atlas_id,
                                       tp_id128 animation_id,
                                       int64_t expected_revision, const char *name,
                                       const char *transaction_id, tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_RENAME;
    operation.atlas_id = atlas_id;
    operation.u.anim_rename.anim_id = animation_id;
    operation.u.anim_rename.name = (char *)name;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_set_animation_settings(tp_session *session, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision,
                                             const tp_op_anim_settings *settings,
                                             const char *transaction_id,
                                             tp_error *err) {
    if (!settings) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "animation settings require structural ids and payload");
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_SETTINGS_SET;
    operation.atlas_id = atlas_id;
    operation.u.anim_settings = *settings;
    operation.u.anim_settings.anim_id = animation_id;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_add_animation_frames(tp_session *session, tp_id128 atlas_id,
                                           tp_id128 animation_id,
                                           int64_t expected_revision,
                                           const tp_op_sprite_ref *frames,
                                           int frame_count,
                                           const char *transaction_id, tp_error *err) {
    if (!frames || frame_count <= 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid animation frame-add intent");
    }
    tp_operation *operations = (tp_operation *)calloc((size_t)frame_count,
                                                       sizeof *operations);
    if (!operations) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "animation frame-add intent allocation failed");
    }
    tp_status status = TP_STATUS_OK;
    for (int i = 0; i < frame_count; i++) {
        operations[i].kind = TP_OP_ANIMATION_FRAME_ADD;
        operations[i].atlas_id = atlas_id;
        operations[i].u.anim_frame_add.anim_id = animation_id;
        operations[i].u.anim_frame_add.index = -1;
        status = copy_frame_ref(&frames[i],
                                &operations[i].u.anim_frame_add.frame, err);
        if (status != TP_STATUS_OK) {
            break;
        }
    }
    if (status == TP_STATUS_OK) {
        status = apply_atlas_ops(session, operations, frame_count,
                                 expected_revision, transaction_id, err);
    }
    for (int i = 0; i < frame_count; i++) {
        tp_operation_free(&operations[i]);
    }
    free(operations);
    return status;
}

tp_status gui_session_remove_animation_frame(tp_session *session, tp_id128 atlas_id,
                                             tp_id128 animation_id,
                                             int64_t expected_revision, int frame_index,
                                             const char *transaction_id, tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_FRAME_REMOVE;
    operation.atlas_id = atlas_id;
    operation.u.anim_frame_rm.anim_id = animation_id;
    operation.u.anim_frame_rm.index = frame_index;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_move_animation_frame(tp_session *session, tp_id128 atlas_id,
                                           tp_id128 animation_id,
                                           int64_t expected_revision, int from_index,
                                           int to_index, const char *transaction_id,
                                           tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_FRAME_MOVE;
    operation.atlas_id = atlas_id;
    operation.u.anim_frame_move.anim_id = animation_id;
    operation.u.anim_frame_move.from_index = from_index;
    operation.u.anim_frame_move.to_index = to_index;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_create_target(tp_session *session, tp_id128 atlas_id,
                                    tp_id128 target_id, int64_t expected_revision,
                                    const char *exporter_id, const char *out_path,
                                    bool enabled, const char *transaction_id,
                                    tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_TARGET_CREATE;
    operation.atlas_id = atlas_id;
    operation.u.target_create.target_id = target_id;
    operation.u.target_create.exporter_id = (char *)exporter_id;
    operation.u.target_create.out_path = (char *)out_path;
    operation.u.target_create.enabled = enabled;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_remove_target(tp_session *session, tp_id128 atlas_id,
                                    tp_id128 target_id, int64_t expected_revision,
                                    const char *transaction_id, tp_error *err) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_TARGET_REMOVE;
    operation.atlas_id = atlas_id;
    operation.u.target_ref.target_id = target_id;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_set_target(tp_session *session, tp_id128 atlas_id,
                                 tp_id128 target_id, int64_t expected_revision,
                                 const tp_op_target_set *settings,
                                 const char *transaction_id, tp_error *err) {
    if (!settings) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "target settings require structural ids and payload");
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_TARGET_SET;
    operation.atlas_id = atlas_id;
    operation.u.target_set = *settings;
    operation.u.target_set.target_id = target_id;
    return apply_atlas_ops(session, &operation, 1, expected_revision,
                           transaction_id, err);
}

tp_status gui_session_copy_atlas_name(const tp_session_snapshot *snapshot,
                                      tp_id128 atlas_id, char *out,
                                      size_t capacity, tp_error *err) {
    if (!snapshot || tp_id128_is_nil(atlas_id) || !out || capacity == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid atlas name query");
    }
    out[0] = '\0';
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_by_id(snapshot, atlas_id);
    if (!atlas || !atlas->name) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "atlas id was not found");
    }
    const size_t length = strlen(atlas->name);
    if (length >= capacity) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas name output is too small");
    }
    memcpy(out, atlas->name, length + 1U);
    return TP_STATUS_OK;
}
