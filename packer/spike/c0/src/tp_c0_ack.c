/* C0-03 task 2: the commit acknowledgement state machine. Pure transition table
 * encoding master spec §7.1 (apply -> append -> publish; rollback on append
 * failure; not visible until journaled). See tp_c0_ack.h. */

#include "tp_c0/tp_c0_ack.h"

/* No `default` in these switches on purpose: a new enum value trips -Wswitch so
 * the token tables stay in lockstep with the enums. */
const char *tp_c0_ack_phase_id(tp_c0_ack_phase p) {
    switch (p) {
        case TP_C0_ACK_RECEIVED: return "received";
        case TP_C0_ACK_VALIDATED: return "validated";
        case TP_C0_ACK_APPLIED: return "applied";
        case TP_C0_ACK_JOURNALED: return "journaled";
        case TP_C0_ACK_PUBLISHED: return "published";
        case TP_C0_ACK_REJECTED: return "rejected";
        case TP_C0_ACK_ROLLED_BACK: return "rolled_back";
        case TP_C0_ACK_PHASE_COUNT: return "";
    }
    return "unknown";
}

const char *tp_c0_ack_event_id(tp_c0_ack_event e) {
    switch (e) {
        case TP_C0_ACK_EV_VALIDATE_OK: return "validate_ok";
        case TP_C0_ACK_EV_VALIDATE_FAIL: return "validate_fail";
        case TP_C0_ACK_EV_APPLY_OK: return "apply_ok";
        case TP_C0_ACK_EV_APPLY_FAIL: return "apply_fail";
        case TP_C0_ACK_EV_APPEND_OK: return "append_ok";
        case TP_C0_ACK_EV_APPEND_FAIL: return "append_fail";
        case TP_C0_ACK_EV_PUBLISH: return "publish";
        case TP_C0_ACK_EV_COUNT: return "";
    }
    return "unknown";
}

tp_c0_ack_phase tp_c0_ack_next(tp_c0_ack_phase p, tp_c0_ack_event e, bool *legal) {
    tp_c0_ack_phase next = p;
    bool ok = true;
    switch (p) {
        case TP_C0_ACK_RECEIVED:
            if (e == TP_C0_ACK_EV_VALIDATE_OK) {
                next = TP_C0_ACK_VALIDATED;
            } else if (e == TP_C0_ACK_EV_VALIDATE_FAIL) {
                next = TP_C0_ACK_REJECTED;
            } else {
                ok = false;
            }
            break;
        case TP_C0_ACK_VALIDATED:
            if (e == TP_C0_ACK_EV_APPLY_OK) {
                next = TP_C0_ACK_APPLIED;
            } else if (e == TP_C0_ACK_EV_APPLY_FAIL) {
                next = TP_C0_ACK_REJECTED; /* atomic apply failed: nothing applied */
            } else {
                ok = false;
            }
            break;
        case TP_C0_ACK_APPLIED:
            if (e == TP_C0_ACK_EV_APPEND_OK) {
                next = TP_C0_ACK_JOURNALED;
            } else if (e == TP_C0_ACK_EV_APPEND_FAIL) {
                next = TP_C0_ACK_ROLLED_BACK; /* revert the apply; do NOT publish (§7.1) */
            } else {
                ok = false; /* PUBLISH here is illegal: not journaled yet */
            }
            break;
        case TP_C0_ACK_JOURNALED:
            if (e == TP_C0_ACK_EV_PUBLISH) {
                next = TP_C0_ACK_PUBLISHED;
            } else {
                ok = false;
            }
            break;
        case TP_C0_ACK_PUBLISHED:
        case TP_C0_ACK_REJECTED:
        case TP_C0_ACK_ROLLED_BACK:
        case TP_C0_ACK_PHASE_COUNT:
            ok = false; /* terminal (or sentinel): no further transitions */
            break;
    }
    if (legal) {
        *legal = ok;
    }
    return next;
}

bool tp_c0_ack_is_terminal(tp_c0_ack_phase p) {
    return p == TP_C0_ACK_PUBLISHED || p == TP_C0_ACK_REJECTED || p == TP_C0_ACK_ROLLED_BACK;
}

bool tp_c0_ack_is_published(tp_c0_ack_phase p) { return p == TP_C0_ACK_PUBLISHED; }

bool tp_c0_ack_is_rolled_back(tp_c0_ack_phase p) { return p == TP_C0_ACK_ROLLED_BACK; }

bool tp_c0_ack_revision_committed(tp_c0_ack_phase p) { return p == TP_C0_ACK_PUBLISHED; }

tp_c0_ack_phase tp_c0_ack_run(const tp_c0_ack_event *events, int count, bool *ok) {
    tp_c0_ack_phase p = TP_C0_ACK_RECEIVED;
    bool all_ok = true;
    for (int i = 0; i < count && events; i++) {
        bool legal = false;
        p = tp_c0_ack_next(p, events[i], &legal);
        if (!legal) {
            all_ok = false;
            break;
        }
    }
    if (ok) {
        *ok = all_ok;
    }
    return p;
}
