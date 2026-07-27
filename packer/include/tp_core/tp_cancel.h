#ifndef TP_CORE_TP_CANCEL_H
#define TP_CORE_TP_CANCEL_H

/* The ONE cooperative-cancel token type in the library, threaded through every long,
 * interruptible walk (the recursive folder scan, the pack-input build that drives it,
 * the pack decode loop, and the build-worker wait loop). cancel_requested(ctx) returns
 * true when the caller has asked to stop. A NULL token -- or a token whose
 * cancel_requested is NULL -- means "never cancel", so callers that never cancel pass
 * NULL and stay untouched (the non-cancellable wrappers do exactly this). The token is
 * polled cooperatively at bounded points (per directory entry / per source / per wait
 * slice); it does not abort an I/O already in flight, and a cancelled walk frees its
 * partial result and reports TP_STATUS_CANCELLED.
 *
 * CANCEL NAMING MAP -- one concept, five physical forms, each forced by a boundary:
 *   1. tp_cancel_token          -- the in-process QUESTION ("should I stop?"). A
 *                                  function-pointer pair because the asker and the
 *                                  answerer are in different modules.
 *   2. terminal_claim           -- the host-side RACE WINNER (tp_job): a cancel and a
 *                                  worker terminal can arrive together, so exactly one
 *                                  of them must claim the job's single terminal slot.
 *   3. TP_PROC_CANCEL_BYTE      -- the cross-PROCESS wire form: a host cannot call a
 *                                  child's function pointer, so the request crosses the
 *                                  control pipe as one byte.
 *   4. tp_cancel_source         -- the LATCH over a destructive-read channel
 *                                  (packer/src/tp_cancel_source.h): a pipe read answers
 *                                  once, a token must answer every time.
 *   5. TP_STATUS_CANCELLED /    -- the RESULT forms: a status on the call that stopped,
 *      TP_SESSION_JOB_CANCELLED    a job state on the session's terminal record.
 * A cancelled operation is never reported as success: it is TP_STATUS_CANCELLED. */

#include <stdbool.h>
#include <stddef.h> /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_cancel_token {
    bool (*cancel_requested)(void *ctx); /* NULL => never cancel */
    void *ctx;
} tp_cancel_token;

/* NULL-safe: true only when a token with a non-NULL callback reports cancellation. */
static inline bool tp_cancel_requested(const tp_cancel_token *token) {
    return token != NULL && token->cancel_requested != NULL &&
           token->cancel_requested(token->ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_CANCEL_H */
