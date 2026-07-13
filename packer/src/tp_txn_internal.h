#ifndef TP_CORE_SRC_TP_TXN_INTERNAL_H
#define TP_CORE_SRC_TP_TXN_INTERNAL_H

/* F2-02 transaction-engine internals shared across the transaction TUs
 * (project clone / semantic identity / apply / parse / encode / idset). Not a
 * public header -- tests include it from src/ the same way test_op_apply.c
 * includes tp_op_internal.h. */

#include <stdbool.h>

#include "tp_core/tp_transaction.h"

/* F2-04 fix C1: the shared id-set behind a memory idstore (NULL for a foreign store).
 * tp_model_attach_journal uses it to migrate ids the model committed journal-less into
 * the fresh journal's retained-id index. Defined in tp_txn_idset.c. */
struct tp_idset;
const struct tp_idset *tp_txn_idstore_mem_view(const tp_txn_idstore *store);

/* ---- shared between the JSON path (tp_txn_parse.c) and the apply core
 * (tp_txn_apply.c). NOT public: the JSON path runs the idempotency + revision
 * gates in the contract order (structural -> idempotency -> revision -> per-op
 * shape collect-all) and then hands an already-gated, already-shape-checked typed
 * request to the atomic commit. */

/* Reset a result to an empty rejected shell tagged with the transaction id. */
void tp_txn__result_reset(tp_txn_result *out, const char *id_hex);

/* Append one error to a result's dynamic error array. Returns true when the error
 * was stored, false when the grow failed (OOM) and the error had to be dropped --
 * a collect-all caller MUST treat a false as a forced reject so a shape-faulted
 * batch never falsely commits under allocation pressure. */
bool tp_txn__result_add_error(tp_txn_result *out, int op_index, tp_status code, const char *field, const char *msg);

/* True iff `s` is exactly 32 lowercase-hex characters (the 128-bit idempotency
 * transaction-id shape). Shared by the envelope structural decode and the preflight
 * so the two id-format checks cannot drift. NULL-safe (NULL -> false). */
bool tp_txn__is_hex32_lower(const char *s);

/* The shared preflight gate BOTH entry points run before the atomic commit, so the
 * typed path (tp_model_apply) and the JSON path (tp_model_apply_json) cannot drift:
 * (a) validate the transaction id is 32-lowercase-hex; (b) NULL-safe reset the
 * result tagged with the id; (c) idempotency (a seen committed id -> duplicate_id,
 * op_index -1); (d) revision precondition (tp_revision_check, op_index -1). On any
 * fault fills `out` (if non-NULL) with revision = m->revision + the structured error
 * and returns the status; on OK the caller proceeds (JSON: shape collect-all ->
 * lower) to tp_txn__commit_validated. */
tp_status tp_txn__preflight(tp_model *m, const char *id_hex, int64_t expected_revision, tp_txn_result *out,
                            tp_error *err);

/* Clone + apply the batch atomically + record id + swap + bump revision. PRE:
 * idempotency and the revision precondition already passed. Fills `out`
 * committed/rejected; the live model is byte-unchanged unless it returns OK. */
tp_status tp_txn__commit_validated(tp_model *m, const tp_txn_request *req, tp_txn_result *out, tp_error *err);

/* Test-only allocation fault seam for tp_txn__result_add_error (default off / -1).
 * Set N so the (N+1)th add_error grow returns failure once, proving that a dropped
 * shape-error record still forces a reject (the batch never falsely commits). */
void tp_txn__test_set_add_error_fail(int nth);

/* Test-only allocation fault seam for tp_project_clone (implemented in
 * tp_project_clone.c; default off / -1). Set N to make the (N+1)th clone
 * allocation return NULL so a test can prove that a clone failure at any staging
 * depth returns NULL, leaks nothing, and leaves the source untouched. Fires once. */
void tp_project__test_set_clone_alloc_fail(int nth);

/* Number of allocations the LAST tp_project_clone attempt requested (whether it
 * succeeded or was fault-injected). Lets a fault-injection test sweep every clone
 * allocation index without hard-coding the count. */
int tp_project__test_clone_alloc_count(void);

#endif /* TP_CORE_SRC_TP_TXN_INTERNAL_H */
