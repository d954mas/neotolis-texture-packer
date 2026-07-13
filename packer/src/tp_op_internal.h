#ifndef TP_CORE_SRC_TP_OP_INTERNAL_H
#define TP_CORE_SRC_TP_OP_INTERNAL_H

/* F2-01 operation-engine internals shared across the op TUs (catalog / validate /
 * apply / encode / build). Not a public header -- tests include it from src/ the
 * same way test_id.c includes tp_hex.h. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_operation.h"

/* Numeric ranges enforced by tp_operation_validate. #define (never a const
 * size_t: macos -Wgnu-folding-constant rejects that as a VLA bound). These mirror
 * cli_validate.c / the engine NT_BUILD_MAX_TEXTURE_SIZE; the authoritative clamp
 * still lives in tp_pack -- this is the front-line structured rejection (task 4). */
/* max_size upper bound TRACKS the build-wide texture cap NT_BUILD_MAX_TEXTURE_SIZE
 * (== tp_pack.c's TP_PACK_MAX_PAGE_DIM). This build lifts the engine's mobile-safe
 * 4096 default to 16384 via the root CMake add_compile_definitions, so a hardcoded
 * 4096 here silently DIVERGED from the packer -- rejecting the valid 8192/16384 GUI
 * presets + CLI `set max_size` even though tp_pack can produce those pages
 * (F2-05b-i F4, decision 0015). Deriving it from the same constant makes the op
 * validator neither stricter nor looser than the packer; a build that leaves the
 * engine default in place falls back to 4096. */
#ifndef NT_BUILD_MAX_TEXTURE_SIZE
#define TP_OP_MAX_PAGE_DIM 4096
#else
#define TP_OP_MAX_PAGE_DIM NT_BUILD_MAX_TEXTURE_SIZE
#endif
#define TP_OP_MAX_VERTICES 16
#define TP_OP_ALPHA_MAX 255
#define TP_OP_SHAPE_MAX 2
#define TP_OP_OV_MARGIN_MAX 32767
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

#endif /* TP_CORE_SRC_TP_OP_INTERNAL_H */
