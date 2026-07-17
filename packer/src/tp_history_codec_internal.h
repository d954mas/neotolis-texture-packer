#ifndef TP_CORE_SRC_TP_HISTORY_CODEC_INTERNAL_H
#define TP_CORE_SRC_TP_HISTORY_CODEC_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_project.h"
#include "tp_diff_internal.h"

#define TP_HISTORY_CODEC_VERSION 1U

typedef enum tp_history_codec_outcome {
    TP_HISTORY_CODEC_OK = 0,
    TP_HISTORY_CODEC_UNSUPPORTED,
    TP_HISTORY_CODEC_OVERSIZED,
    TP_HISTORY_CODEC_ERROR,
} tp_history_codec_outcome;

typedef struct tp_history_transition_blob {
    uint8_t *data;
    size_t len;
    uint32_t op_count;
} tp_history_transition_blob;

tp_status tp_history_transition_encode(
    const tp_diff_record *record, bool reverse,
    const tp_project *path_context, size_t max_bytes,
    tp_history_transition_blob *out, tp_history_codec_outcome *outcome,
    tp_error *err);

tp_status tp_history_transition_validate(const uint8_t *data, size_t len,
                                         uint32_t *op_count, tp_error *err);

tp_status tp_history_transition_apply(tp_project *project, const uint8_t *data,
                                      size_t len, uint32_t *op_count,
                                      tp_error *err);

/* Replay onto a caller-owned disposable project after the complete journal
 * stream has been structurally preflighted. Unlike the ordinary atomic helper,
 * this does not clone: any semantic failure leaves `project` unusable and the
 * caller MUST discard it rather than publish it. */
tp_status tp_history_transition_apply_disposable(
    tp_project *project, const uint8_t *data, size_t len, uint32_t *op_count,
    tp_error *err);

void tp_history_transition_blob_free(tp_history_transition_blob *blob);

#endif
