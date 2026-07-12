/* C0-03 task 3: session-authority vocabulary + permission predicate table. Pure
 * tables, no OS claim/cutover (that machine is OPEN per §60 item 2). See
 * tp_c0_authority.h. */

#include "tp_c0/tp_c0_authority.h"

const char *tp_c0_authority_id(tp_c0_authority_state s) {
    switch (s) {
        case TP_C0_AUTH_OWNER: return "owner";
        case TP_C0_AUTH_OBSERVER: return "observer";
        case TP_C0_AUTH_TRANSFER_PENDING: return "transfer_pending";
        case TP_C0_AUTH_RELEASED: return "released";
        case TP_C0_AUTH_STATE_COUNT: return "";
    }
    return "unknown";
}

const char *tp_c0_authority_cap_id(tp_c0_authority_cap c) {
    switch (c) {
        case TP_C0_AUTH_CAP_APPLY: return "apply";
        case TP_C0_AUTH_CAP_PUBLISH: return "publish";
        case TP_C0_AUTH_CAP_PACK: return "pack";
        case TP_C0_AUTH_CAP_COUNT: return "";
    }
    return "unknown";
}

/* Singular authority: OWNER is the ONLY state that permits apply/publish/pack.
 * The three non-owner states deny all three -- that denial IS the contract (no
 * two hosts accept writes for one session, §16/§22.2). */
bool tp_c0_authority_permits(tp_c0_authority_state s, tp_c0_authority_cap c) {
    if (c < 0 || c >= TP_C0_AUTH_CAP_COUNT) {
        return false;
    }
    return s == TP_C0_AUTH_OWNER;
}

bool tp_c0_authority_is_authoritative(tp_c0_authority_state s) { return s == TP_C0_AUTH_OWNER; }
