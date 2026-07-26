#ifndef TP_CORE_TP_CANCEL_H
#define TP_CORE_TP_CANCEL_H

/* Optional cooperative-cancel token threaded through the long, interruptible core
 * walks (the recursive folder scan and the pack-input build that drives it). The
 * callback shape mirrors the pack cancel (tp_pack_cancel_poll): cancel_requested(ctx)
 * returns true when the caller has asked to stop. A NULL token -- or a token whose
 * cancel_requested is NULL -- means "never cancel", so callers that never cancel pass
 * NULL and stay untouched (the non-cancellable wrappers do exactly this). The token is
 * polled cooperatively at bounded points (per directory entry / per source); it does
 * not abort an I/O already in flight, and a cancelled walk frees its partial result and
 * reports TP_STATUS_CANCELLED. */

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
