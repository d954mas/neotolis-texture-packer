#ifndef TP_CORE_SRC_TP_ENCODE_INTERNAL_H
#define TP_CORE_SRC_TP_ENCODE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_operation.h"
#include "tp_core/tp_transaction.h"

struct tp_sb;

/* Internal bounded variants used by transaction admission. `max_bytes == 0`
 * means unlimited. A size rejection returns NULL with *too_large=true; semantic
 * validity and canonical formatting stay owned by the normal encoders. */
char *tp_operation_encode_bounded(const tp_operation *op, size_t max_bytes, bool *too_large);
char *tp_txn_request_encode_bounded(const tp_txn_request *req, size_t max_bytes, bool *too_large);

/* Shared canonical emitters/sizers. The sizing path writes to a count-only
 * builder, so durable byte admission can be exact without allocating. */
bool tp_operation_emit_canonical(struct tp_sb *sb, const tp_operation *op,
                                 int depth, bool trailing_newline);
bool tp_txn_request_encoded_size(const tp_txn_request *req, size_t *size_out);
char *tp_txn_request_encode_bounded_for_project(
    const tp_txn_request *req, const tp_project *project, size_t max_bytes,
    bool *too_large);
bool tp_txn_request_encoded_size_for_project(const tp_txn_request *req,
                                             const tp_project *project,
                                             size_t *size_out);

#endif
