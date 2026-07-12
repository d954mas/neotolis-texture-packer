#ifndef TP_C0_ACK_H
#define TP_C0_ACK_H

/*
 * C0-03 task 2: the commit acknowledgement boundary as a pure state machine.
 *
 * Master spec §7.1: one commit contract for GUI / MCP / Undo / Redo. A
 * transaction is NOT exposed as committed/visible until it (1) validated,
 * (2) applied atomically to the authoritative session, (3) received a new
 * revision, (4) was appended to the recovery journal. If the journal append
 * fails, the transaction is rolled back and no committed event is published.
 * (§22.1: a success response is sent only after the append.)
 *
 * This pins the ORDERING apply -> append -> publish and the rollback-on-append-
 * failure rule as a transition table. It is not an apply engine: no model, no
 * journal IO -- just contract shapes and a total transition function the F2
 * session queue drives. The machine only ever sees a controlled event sequence
 * from the commit pipeline (not caller/disk input), so an illegal (phase,event)
 * is a programming misuse reported via a `legal` out-flag, not a structured disk
 * error and never an abort.
 *
 *   RECEIVED --validate_ok--> VALIDATED --apply_ok--> APPLIED --append_ok--> JOURNALED --publish--> PUBLISHED
 *      |  validate_fail          |  apply_fail            |  append_fail
 *      v                         v                        v
 *   REJECTED                  REJECTED                 ROLLED_BACK
 *
 * The load-bearing invariant: PUBLISH is legal ONLY from JOURNALED. Publishing
 * before the journal append is impossible by construction, and an APPLIED
 * transaction whose append fails goes to ROLLED_BACK, never PUBLISHED.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tp_c0_ack_phase {
    TP_C0_ACK_RECEIVED = 0, /* accepted into the queue, not yet validated */
    TP_C0_ACK_VALIDATED,    /* full batch validated (revision + per-op) */
    TP_C0_ACK_APPLIED,      /* applied atomically; new revision assigned (not yet durable) */
    TP_C0_ACK_JOURNALED,    /* appended to the recovery journal; safe to publish */
    TP_C0_ACK_PUBLISHED,    /* committed event emitted; visible to all frontends (TERMINAL) */
    TP_C0_ACK_REJECTED,     /* validate/apply failed; nothing applied (TERMINAL) */
    TP_C0_ACK_ROLLED_BACK,  /* append failed after apply; model reverted, not published (TERMINAL) */
    TP_C0_ACK_PHASE_COUNT
} tp_c0_ack_phase;

typedef enum tp_c0_ack_event {
    TP_C0_ACK_EV_VALIDATE_OK = 0,
    TP_C0_ACK_EV_VALIDATE_FAIL,
    TP_C0_ACK_EV_APPLY_OK,
    TP_C0_ACK_EV_APPLY_FAIL,
    TP_C0_ACK_EV_APPEND_OK,
    TP_C0_ACK_EV_APPEND_FAIL,
    TP_C0_ACK_EV_PUBLISH,
    TP_C0_ACK_EV_COUNT
} tp_c0_ack_event;

/* Stable machine tokens (test-pinned, like tp_c0_detail_id). */
const char *tp_c0_ack_phase_id(tp_c0_ack_phase p);
const char *tp_c0_ack_event_id(tp_c0_ack_event e);

/* Total transition. On a legal (phase,event) returns the next phase and sets
 * *legal (if non-NULL) to true. On an illegal combination returns `p` unchanged
 * and sets *legal to false -- never aborts. */
tp_c0_ack_phase tp_c0_ack_next(tp_c0_ack_phase p, tp_c0_ack_event e, bool *legal);

/* Outcome predicates over a terminal (or any) phase. */
bool tp_c0_ack_is_terminal(tp_c0_ack_phase p);
bool tp_c0_ack_is_published(tp_c0_ack_phase p);   /* committed event emitted */
bool tp_c0_ack_is_rolled_back(tp_c0_ack_phase p); /* applied then reverted (append failed) */
/* A new revision is retained/visible ONLY when published; a rollback reverts the
 * revision bump made at apply (§7.1). */
bool tp_c0_ack_revision_committed(tp_c0_ack_phase p);

/* Drive an event sequence from RECEIVED. Returns the resulting phase; sets *ok
 * (if non-NULL) false if any event was illegal from the running phase (and stops
 * advancing at that point). A convenience for fixture-table tests. */
tp_c0_ack_phase tp_c0_ack_run(const tp_c0_ack_event *events, int count, bool *ok);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_ACK_H */
