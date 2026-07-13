#ifndef TP_CORE_SRC_TP_TXN_INTERNAL_H
#define TP_CORE_SRC_TP_TXN_INTERNAL_H

/* F2-02 transaction-engine internals shared across the transaction TUs
 * (project clone / semantic identity / apply / parse / encode / idset). Not a
 * public header -- tests include it from src/ the same way test_op_apply.c
 * includes tp_op_internal.h. */

#include <stdbool.h>

#include "tp_core/tp_transaction.h"

/* ---- shared between the JSON path (tp_txn_parse.c) and the apply core
 * (tp_txn_apply.c). NOT public: the JSON path runs the idempotency + revision
 * gates in the contract order (structural -> idempotency -> revision -> per-op
 * shape collect-all) and then hands an already-gated, already-shape-checked typed
 * request to the atomic commit. */

/* Reset a result to an empty rejected shell tagged with the transaction id. */
void tp_txn__result_reset(tp_txn_result *out, const char *id_hex);

/* Append one error to a result's dynamic error array (drops it on grow-OOM). */
void tp_txn__result_add_error(tp_txn_result *out, int op_index, tp_status code, const char *field, const char *msg);

/* Clone + apply the batch atomically + record id + swap + bump revision. PRE:
 * idempotency and the revision precondition already passed. Fills `out`
 * committed/rejected; the live model is byte-unchanged unless it returns OK. */
tp_status tp_txn__commit_validated(tp_model *m, const tp_txn_request *req, tp_txn_result *out, tp_error *err);

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
