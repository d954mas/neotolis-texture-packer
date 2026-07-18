#ifndef TP_CORE_SRC_TP_OP_INTERNAL_H
#define TP_CORE_SRC_TP_OP_INTERNAL_H

/* Operation-engine internals shared across the op TUs (catalog / validate /
 * apply / encode / build). Not a public header -- tests include it from src/ the
 * same way test_id.c includes tp_hex.h. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_operation.h"
#include "tp_core/tp_identity.h"

/* Operation-only ranges that do not belong to the shared pack-setting contract.
 * Pack settings use TP_PACK_* constants/predicates from tp_pack.h so operation,
 * project validation, and pack admission cannot drift. */
#define TP_OP_PLAYBACK_MAX 6

/* Fill a structured rejection (task 5): status id + offending closed-vocab field +
 * human context. `field` may be NULL/"" when no single field is at fault. Returns
 * `st` so callers write `return tp_op__reject(rej, TP_STATUS_X, "field", "...")`. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
static inline tp_status tp_op__reject(tp_op_reject *rej, tp_status st, const char *field, const char *fmt, ...) {
    if (rej) {
        rej->status = st;
        rej->field[0] = '\0';
        if (field) {
            (void)snprintf(rej->field, sizeof rej->field, "%s", field);
        }
        rej->message[0] = '\0';
        if (fmt) {
            va_list args;
            va_start(args, fmt);
            (void)vsnprintf(rej->message, sizeof rej->message, fmt, args);
            va_end(args);
        }
    }
    return st;
}

/* Clears a rejection to the OK (no-fault) state. */
static inline void tp_op__reject_ok(tp_op_reject *rej) {
    if (rej) {
        rej->status = TP_STATUS_OK;
        rej->field[0] = '\0';
        rej->message[0] = '\0';
    }
}

/* Test-only staging-allocation fault injection for the stage-then-commit apply
 * path (implemented in tp_op_apply.c; default off / -1). Set N to make the (N+1)th
 * staging allocation return NULL so a test can prove an allocator failure before
 * the commit point leaves the model byte-unchanged. */
void tp_op__test_set_alloc_fail(int nth);

/* Test/benchmark-only thread-local operation-apply counter. reset starts counting
 * attempted applies; take stops and returns the exact count. */
void tp_op__test_apply_count_reset(void);
size_t tp_op__test_apply_count_take(void);
void tp_op__test_apply_count_publish(size_t count);

/* Reject typed shapes that the canonical encoder cannot safely traverse.
 * Model-dependent validation still runs against the progressively applied
 * transaction clone. */
tp_status tp_op__validate_encode_shape(const tp_operation *operation,
                                       tp_op_reject *reject);

/* Produce the operation identity used by validation, mutation, and durable
 * transaction encoding. SOURCE_ADD/REPLACE keys are resolved against the
 * current saved-project directory; every other operation is a shallow copy.
 * `path_buf` is caller-owned scratch and no allocation is performed. */
tp_status tp_op__canonical_view(const tp_project *project,
                                const tp_operation *operation,
                                tp_operation *view, char *path_buf,
                                size_t path_buf_size);

/* Apply an operation already accepted by tp_operation_validate() against the
 * same current project state. Transaction history uses this after validation
 * and before/after capture so attachments cannot change the first rejection.
 * Public and recovery callers must use tp_operation_apply(). */
tp_status tp_op__apply_prevalidated(tp_project *project,
                                    const tp_operation *operation,
                                    tp_op_reject *reject);

typedef struct tp_sprite_clear_field {
    const char *token;
    uint32_t bit;
} tp_sprite_clear_field;

/* One canonical token<->bit vocabulary shared by JSON validation/lowering and
 * encoding. This prevents a field from being accepted but silently dropped by
 * the durable transaction encoder. */
const tp_sprite_clear_field *tp_op__sprite_clear_fields(size_t *count);
bool tp_op__sprite_clear_bit(const char *token, uint32_t *bit);

#endif /* TP_CORE_SRC_TP_OP_INTERNAL_H */
