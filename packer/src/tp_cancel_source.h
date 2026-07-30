#ifndef TP_CANCEL_SOURCE_H
#define TP_CANCEL_SOURCE_H

/* The producer side of a tp_cancel_token: a latching adapter over a cancel
 * CHANNEL whose read is destructive.
 *
 * A tp_cancel_token answers "should I stop?" and may be polled by any number of
 * core walks, any number of times. A channel like the job worker's control pipe
 * answers "did a cancel byte arrive since my last read?" exactly once -- the
 * second reader sees nothing. tp_cancel_source owns that asymmetry: `probe` does
 * the ONE destructive read, `observed` latches the first non-NONE reason
 * monotonically, and every later poll (from any consumer) answers from the latch.
 * That is what lets a single channel feed a token AND a terminal decision made
 * long after the last poll.
 *
 * Named without the `_internal` suffix on purpose, like tp_pack_priv.h: it is a
 * private src-only header that the check_boundaries R18 registry scan (which
 * matches `*_internal.h`) deliberately does not cover.
 *
 * NOT thread-safe: one source belongs to one polling thread. */

#include <stdbool.h>
#include <stddef.h> /* NULL */

#include "tp_core/tp_cancel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why the stop was asked for. CONTROL_LOST is a cancel too: a worker that can no
 * longer hear its host must stop, and the caller that folds the terminal is the
 * one that decides how a lost control channel is REPORTED. */
typedef enum tp_cancel_reason {
    TP_CANCEL_REASON_NONE = 0,
    TP_CANCEL_REASON_REQUESTED,
    TP_CANCEL_REASON_CONTROL_LOST
} tp_cancel_reason;

typedef struct tp_cancel_source {
    tp_cancel_reason (*probe)(void *ctx); /* ONE destructive channel read */
    void *ctx;
    tp_cancel_reason observed; /* latched; never returns to NONE */
} tp_cancel_source;

/* tp_cancel_token-compatible poll: answers from the latch, and probes the channel
 * only while nothing has fired. `context` is a tp_cancel_source *. */
bool tp_cancel_source_poll(void *context);

/* The token view of `source`. Hand this to any core walk that takes a token. */
tp_cancel_token tp_cancel_source_token(tp_cancel_source *source);

/* Latch reads. These never touch the channel, so a terminal decision made after
 * the last poll still sees what the polls observed. */
static inline bool tp_cancel_source_fired(const tp_cancel_source *source) {
    return source != NULL && source->observed != TP_CANCEL_REASON_NONE;
}
static inline tp_cancel_reason
tp_cancel_source_reason(const tp_cancel_source *source) {
    return source != NULL ? source->observed : TP_CANCEL_REASON_NONE;
}

#ifdef __cplusplus
}
#endif

#endif /* TP_CANCEL_SOURCE_H */
