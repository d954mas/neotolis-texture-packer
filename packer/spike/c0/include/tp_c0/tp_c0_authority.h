#ifndef TP_C0_AUTHORITY_H
#define TP_C0_AUTHORITY_H

/*
 * C0-03 task 3: session-authority vocabulary + a pure permission predicate
 * table. NO OS lock/claim, NO cutover protocol -- just the state names and what
 * each state permits.
 *
 * Master spec §16 (one canonical path -> one cooperating live session; the
 * system must prevent two authoritative writable copies), §18 (one external
 * controller), §19 (GUI is the authoritative host; MCP is a Dev-API client and
 * recovery mirror; on GUI-opens-headless-project ownership TRANSFERS with no
 * merge and no second session; after cutover the old host may not publish a Pack
 * result or accept mutations), §22.2 (the point where authority changes must be
 * singular). The exact process-claim mechanism, proof-of-death, and singular
 * cutover protocol are OPEN per §60 item 2 / §52.5 and are deliberately NOT
 * modelled here.
 *
 * The permission table's shape is the pinned contract: exactly one state (OWNER)
 * permits apply/publish/pack, which is the executable form of "singular
 * authority -- at most one host accepts writes for a session". OBSERVER,
 * TRANSFER_PENDING, and RELEASED are distinct vocabulary points (they differ in
 * the open cutover machine) that all DENY authoritative mutation/publish/pack.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tp_c0_authority_state {
    TP_C0_AUTH_OWNER = 0,         /* authoritative session host (GUI open, or MCP when GUI closed) */
    TP_C0_AUTH_OBSERVER,         /* Dev-API client / recovery mirror: reads & forwards, never authoritative */
    TP_C0_AUTH_TRANSFER_PENDING, /* ownership cutover in flight: old host draining to a safe boundary */
    TP_C0_AUTH_RELEASED,         /* no authority (handed off / session closed) */
    TP_C0_AUTH_STATE_COUNT
} tp_c0_authority_state;

typedef enum tp_c0_authority_cap {
    TP_C0_AUTH_CAP_APPLY = 0, /* accept an authoritative model mutation (transaction commit) */
    TP_C0_AUTH_CAP_PUBLISH,   /* publish a Pack/Export result as authoritative */
    TP_C0_AUTH_CAP_PACK,      /* own/run a Pack job for the session */
    TP_C0_AUTH_CAP_COUNT
} tp_c0_authority_cap;

/* Stable machine tokens (test-pinned). */
const char *tp_c0_authority_id(tp_c0_authority_state s);
const char *tp_c0_authority_cap_id(tp_c0_authority_cap c);

/* The predicate table: does `s` permit capability `c`? Out-of-range -> false. */
bool tp_c0_authority_permits(tp_c0_authority_state s, tp_c0_authority_cap c);

/* True only for OWNER: the single state that accepts authoritative writes. */
bool tp_c0_authority_is_authoritative(tp_c0_authority_state s);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_AUTHORITY_H */
