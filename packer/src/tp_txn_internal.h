#ifndef TP_CORE_SRC_TP_TXN_INTERNAL_H
#define TP_CORE_SRC_TP_TXN_INTERNAL_H

/* Transaction-engine internals shared across the transaction TUs
 * (project clone / semantic identity / apply / parse / encode / idset). Not a
 * public header -- tests include it from src/ the same way test_op_apply.c
 * includes tp_op_internal.h. */

#include <stdbool.h>

#include "tp_core/tp_transaction.h"
#include "tp_model_seam.h"

/* The model-state wrapper's field layout. Private to the owning TUs
 * (tp_txn_apply.c, tp_txn_parse.c, tp_history.c); tp_transaction.h exposes only
 * the opaque handle + read accessors. */
struct tp_model {
    tp_project *project;      /* authoritative immutable view; see generation */
    tp_project_generation *generation; /* NULL => model directly owns project */
    int64_t revision;         /* canonical monotonic counter; runtime, not persisted */
    tp_id128 saved_identity;  /* semantic identity of the saved baseline (dirty anchor) */
    bool recovered_unsaved;   /* recovered state is dirty until explicitly saved */
    tp_txn_idstore *idstore;  /* idempotency retention (owned unless borrowed) */
    bool owns_idstore;
    struct tp_history *history; /* optional owned Undo/Redo history */
    struct tp_journal *journal; /* optional owned acknowledgement journal */
    struct tp_side_effect_coordinator *coordinator; /* optional borrowed hooks */
};

/* Allocation-free publication shared by commit, Undo/Redo, and Save. Takes
 * ownership of project and releases the previous direct/shared generation. */
void tp_model__replace_owned_project(tp_model *model, tp_project *project);

/* The shared id-set behind a memory idstore (NULL for a foreign store).
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

/* Result echo storage is staged before durable acknowledgement and discarded
 * on any commit rejection. These two ownership operations stay below commit. */
bool tp_txn__result_echo_prepare(tp_txn_result *out,
                                 const tp_txn_request *request);
void tp_txn__result_echo_discard(tp_txn_result *out);

/* Append one error to a result's dynamic error array. Returns true when the error
 * was stored, false when the grow failed (OOM) and the error had to be dropped --
 * a collect-all caller MUST treat a false as a forced reject so a shape-faulted
 * batch never falsely commits under allocation pressure. */
bool tp_txn__result_add_error(tp_txn_result *out, int op_index, tp_status code, const char *field, const char *msg);

/* Exact one-allocation collector used after the JSON shape pass has counted its
 * bounded errors. Keeps capacity private to the collector instead of leaking
 * builder state into the public result DTO. */
bool tp_txn__result_reserve_errors(tp_txn_result *out, int count);
bool tp_txn__result_add_error_reserved(tp_txn_result *out, int capacity, int op_index,
                                       tp_status code, const char *field, const char *msg);

/* True iff `s` is exactly 32 lowercase-hex characters (the 128-bit idempotency
 * transaction-id shape). Shared by the envelope structural decode and the preflight
 * so the two id-format checks cannot drift. NULL-safe (NULL -> false). */
bool tp_txn__is_hex32_lower(const char *s);

/* Shared typed/JSON operation-count admission. A negative count is malformed;
 * a count above TP_TXN_MAX_OPS is a resource-bound rejection. Runs before the
 * typed operation array is allocated/lowered or the model is cloned. */
tp_status tp_txn__check_op_count(int op_count, tp_error *err);

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

/* Serialize + round-trip-prove `candidate`, then append it as a durable history
 * CHECKPOINT at `revision` when `m` has a journal. A no-op without a journal.
 * The live model is never touched; tp_history.c calls this as the LAST fallible
 * gate before publishing an Undo/Redo candidate + cursor move. */
tp_status tp_model__append_history_checkpoint(tp_model *m, const tp_project *candidate, int64_t revision,
                                              size_t snapshot_bytes, tp_error *err);

/* Checked monotonic revision increment shared by transaction commit and Undo/Redo.
 * Rejects corrupted negative/MAX revisions before any staging or signed overflow. */
tp_status tp_model__next_revision(int64_t current, int64_t *next, tp_error *err);

/* Test-only allocation fault seam for tp_txn__result_add_error (default off / -1).
 * Set N so the (N+1)th add_error grow returns failure once, proving that a dropped
 * shape-error record still forces a reject (the batch never falsely commits). */
void tp_txn__test_set_add_error_fail(int nth);

/* Fails the (N+1)th exact result-echo allocation once. Echo reservation runs
 * before durable acknowledgement, so the transaction must remain unchanged. */
void tp_txn__test_set_result_echo_fail(int nth);

/* Deterministic complexity probes for the bounded JSON path. The op-walk counter
 * counts linked-list node inspections across structural/shape/lowering passes;
 * the error-allocation counter counts backing-array grow allocations. */
void tp_txn__test_complexity_reset(void);
size_t tp_txn__test_op_walk_steps(void);
size_t tp_txn__test_error_allocations(void);
void tp_txn__test_count_op_walk(size_t steps);

/* Canonical request sizing/encoding probes. A byte-cap rejection must perform
 * the exact sizing pass with zero allocations and must not enter the allocating
 * encoder at all. */
void tp_txn__test_encode_stats_reset(void);
size_t tp_txn__test_request_encode_calls(void);
size_t tp_txn__test_last_measure_allocations(void);

/* Run the allocation-free JSON operation-count preflight and report input-byte
 * inspections. The scanner is single-pass; adversarial nesting tests pin work <=
 * input length instead of relying on wall-clock timing. */
tp_status tp_txn__test_json_precheck(const char *json, size_t json_len, size_t *steps,
                                     tp_error *err);

/* Allocation-free exact operation count for one bounded canonical request span.
 * Recovery uses this for its aggregate replay admission before cJSON lowering or
 * any operation is applied. */
tp_status tp_txn__count_operations_json_n(const char *json, size_t json_len,
                                          int *count_out, tp_error *err);

/* Recovery-only lowering after the same payload span already passed the exact
 * byte/count preflight. Skips that duplicate lexical scan, but verifies the
 * parsed operation count before returning a request. */
tp_status tp_txn__decode_prechecked_json_n(const char *json, size_t json_len,
                                           int expected_op_count,
                                           tp_txn_request **out,
                                           tp_error *err);

/* Test-only allocation fault seam for tp_project_clone (implemented in
 * tp_project_clone.c; default off / -1). Set N to make the (N+1)th clone
 * allocation return NULL so a test can prove that a clone failure at any staging
 * depth returns NULL, leaks nothing, and leaves the source untouched. Fires once. */
void tp_project__test_set_clone_alloc_fail(int nth);

/* Number of allocations the LAST tp_project_clone attempt requested (whether it
 * succeeded or was fault-injected). Lets a fault-injection test sweep every clone
 * allocation index without hard-coding the count. */
int tp_project__test_clone_alloc_count(void);

/* Sum of successful allocation request sizes owned by the last successful
 * clone. For a completed clone this is its exact payload live-byte count
 * (allocator metadata excluded), not an estimate from allocation count. */
size_t tp_project__test_clone_allocation_bytes(void);

/* Exact semantic identity for one atlas, using the same field/order rules as
 * tp_semantic_identity. The single-operation commit path uses it to detect a
 * true no-change without folding unrelated atlases. */
tp_id128 tp_semantic_atlas_identity(const tp_project *project,
                                    const tp_project_atlas *atlas);

/* Test-only revision seam: forces the canonical revision to an arbitrary value
 * (e.g. INT64_MAX) so a test can prove the overflow guard rejects further
 * commits/history transitions without going through TP_TXN_MAX_OPS commits. */
void tp_model__test_set_revision(tp_model *m, int64_t revision);

#endif /* TP_CORE_SRC_TP_TXN_INTERNAL_H */
