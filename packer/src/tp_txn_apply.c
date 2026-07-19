/*
 * The atomic transaction core over the live tp_project model.
 *
 * Mechanism -- transactional CLONE (docs/decisions/0011): idempotency (a seen id ->
 * duplicate_id) -> revision precondition (mismatch rejects ALONE, op_index -1, before
 * any per-op work) -> clone the model -> validate+apply each op to the CLONE op-by-op
 * via tp_operation_apply (so op N may depend on ops 1..N-1) -> on FULL
 * success record the id, swap the clone in (freeing the old model), and bump the
 * revision by exactly 1. On ANY op rejection or allocator failure the clone is
 * discarded and the LIVE model is byte-unchanged (§416). The commit point is an
 * allocation-free pointer swap: provably atomic.
 *
 * SEMANTIC vs SHAPE validation (divergence from a static-table approach,
 * documented in decision 0011): the model-INDEPENDENT shape faults (unknown op,
 * unknown field, malformed *_id) are collected-all in tp_txn_parse.c before any
 * apply. The model-DEPENDENT semantic faults (dangling id, range, name collision)
 * are validated against the PROGRESSIVELY-applied clone -- which is what lets a
 * create-then-use batch validate -- and report the FIRST offending op (a later op's
 * validity depends on the earlier ops succeeding, so a full collect-all is not
 * well-defined once intra-batch dependencies exist).
 */

#include "tp_core/tp_transaction.h"
#include <stdlib.h>
#include <string.h>

#include "tp_diff_internal.h" /* optional per-op diff capture on commit */
#include "tp_encode_internal.h"
#include "tp_history_codec_internal.h" /* compact durable Undo/Redo replay */
#include "tp_journal_internal.h" /* poison the journal from the recovery glue */
#include "tp_model_seam.h"
#include "tp_op_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_txn_internal.h"

/* Reject the in-flight commit -- the single home for the clone/record cleanup-and-fail
 * contract. Frees the partially-captured entry
 * and the diff record, marks `out` rejected at the UNCHANGED revision with one
 * structured error, and discards the clone so the LIVE model stays byte-unchanged.
 * `entry` / `rec` / `clone` may each be NULL (all frees are NULL-safe). */
static void tp_txn__commit_reject(tp_txn_result *out, tp_project *clone, tp_diff_record *rec, tp_diff_op *entry,
                                  char *payload, int64_t revision, int op_index, tp_status code, const char *field,
                                  const char *msg) {
    tp_diff_op_free(entry);   /* NULL-safe: frees a partially-captured before/after entry */
    tp_diff_record_free(rec); /* NULL-safe */
    if (out) {
        tp_txn__result_echo_discard(out);
        out->committed = false;
        out->revision = revision; /* unchanged: nothing committed */
        tp_txn__result_add_error(out, op_index, code, field, msg);
    }
    tp_project_destroy(clone); /* NULL-safe; discard the clone -- live model untouched */
    free(payload);
}

/* ---- the atomic commit path (shared by the typed and JSON entry points) --- *
 * PRE: idempotency + revision precondition already passed. Clones the model, applies
 * each op to the clone, and on FULL success records the id + swaps + bumps revision.
 * Fills `out` committed/rejected. The live model is byte-unchanged unless it returns
 * TP_STATUS_OK. */
tp_status tp_txn__commit_validated(tp_model *m, const tp_txn_request *req, tp_txn_result *out, tp_error *err) {
    bool recovery_ready = m->journal && !m->recovery_degraded;
    tp_status recovery_failure = TP_STATUS_OK;
    tp_error recovery_error = {{0}};
    if (recovery_ready) {
        tp_status replay_st = tp_journal__check_append_admission(
            m->journal, (size_t)req->op_count, &recovery_error);
        if (replay_st != TP_STATUS_OK) {
            recovery_failure = replay_st;
        }
        if (recovery_failure == TP_STATUS_OK) {
            tp_status byte_st = tp_journal__check_txn_min_bytes(
                m->journal, &recovery_error);
            if (byte_st != TP_STATUS_OK) {
                recovery_failure = byte_st;
            }
        }
    }
    /* Canonical journal payload admission is shared by typed and JSON callers. */
    bool encodable = true;
    bool structural_create_applied = false;
    for (int i = 0; i < req->op_count; i++) {
        if (!req->ops || !tp_op_info_by_kind(req->ops[i].kind)) {
            encodable = false;
            break;
        }
        tp_op_reject reject;
        const tp_status shape =
            tp_op__validate_encode_shape(&req->ops[i], &reject);
        if (shape != TP_STATUS_OK) {
            tp_txn__commit_reject(out, NULL, NULL, NULL, NULL, m->revision,
                                  i, shape, reject.field, reject.message);
            return tp_error_set(err, shape, "operation %d rejected: %s", i,
                                reject.message);
        }
    }
    size_t payload_len = 0U;
    if (encodable) {
        /* The public transaction byte contract applies with or without a
         * journal. Measure allocation-free before encoding or cloning. */
        if (!tp_txn_request_encoded_size_for_project(
                req, m->project, &payload_len)) {
            tp_txn__commit_reject(out, NULL, NULL, NULL, NULL, m->revision, -1,
                                  TP_STATUS_OUT_OF_BOUNDS, "request",
                                  "canonical transaction request size overflow");
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "canonical transaction request size overflow");
        }
        if (payload_len > (size_t)TP_TXN_MAX_REQUEST_BYTES) {
            tp_txn__commit_reject(out, NULL, NULL, NULL, NULL, m->revision, -1,
                                  TP_STATUS_OUT_OF_BOUNDS, "request",
                                  "canonical transaction request exceeds the byte limit");
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "canonical transaction request has %zu bytes; maximum is %u",
                                payload_len, (unsigned)TP_TXN_MAX_REQUEST_BYTES);
        }
        if (recovery_ready && recovery_failure == TP_STATUS_OK) {
            tp_status byte_st = tp_journal__check_txn_bytes(
                m->journal, payload_len, &recovery_error);
            if (byte_st != TP_STATUS_OK) {
                recovery_failure = byte_st;
            }
        }
    }
    /* A committed result must echo every operation exactly. Reserve the whole
     * result (including one contiguous source-key pool) before journal append
     * or model publication, so OOM is an ordinary unchanged rejection. */
    if (!tp_txn__result_echo_prepare(out, req)) {
        if (out) {
            out->revision = m->revision;
            (void)tp_txn__result_add_error(
                out, -1, TP_STATUS_OOM, "result",
                "could not allocate the exact transaction result echo");
        }
        return tp_error_set(err, TP_STATUS_OOM,
                            "transaction result echo allocation failed");
    }
    bool payload_too_large = false;
    char *payload = encodable && recovery_ready &&
                            recovery_failure == TP_STATUS_OK
                        ? tp_txn_request_encode_bounded_for_project(
                              req, m->project,
                              (size_t)TP_TXN_MAX_REQUEST_BYTES,
                              &payload_too_large)
                        : NULL;
    if (payload_too_large) {
        recovery_failure = TP_STATUS_OUT_OF_BOUNDS;
    }
    if (encodable && recovery_ready && recovery_failure == TP_STATUS_OK &&
        !payload) {
        recovery_failure = TP_STATUS_OOM;
    }
    payload_len = payload ? strlen(payload) : 0U;
    tp_project *clone = tp_project_clone(m->project);
    if (!clone) {
        tp_txn__commit_reject(out, NULL, NULL, NULL, payload, m->revision, -1, TP_STATUS_OOM, "",
                              "could not clone the model (out of memory)");
        return tp_error_set(err, TP_STATUS_OOM, "transaction clone failed");
    }

    /* When a history is attached, capture the per-op semantic diff as each op
     * applies to the clone. The whole diff is built PRE-swap, so a capture allocation
     * failure fails the commit cleanly (clone discarded, live model byte-unchanged) --
     * atomicity is preserved. A NULL history => exactly the history-less code path. */
    tp_diff_record *rec = NULL;
    if (m->history) {
        tp_diff__record_budget_begin(tp_history_record_byte_limit());
        rec = tp_diff_record_new(req->label, req->author, req->op_count);
        if (!rec) {
            const bool over_budget = tp_diff__record_budget_exceeded();
            (void)tp_diff__record_budget_end(NULL);
            const tp_status rst = over_budget ? TP_STATUS_OUT_OF_BOUNDS : TP_STATUS_OOM;
            tp_txn__commit_reject(out, clone, NULL, NULL, payload, m->revision, -1, rst,
                                  over_budget ? "history" : "",
                                  over_budget ? "semantic diff exceeds the history record budget"
                                              : "could not allocate the diff record (out of memory)");
            return tp_error_set(err, rst, over_budget ? "semantic diff exceeds the history record budget"
                                                      : "diff record allocation failed");
        }
    }

    for (int i = 0; i < req->op_count; i++) {
        tp_op_reject rej;
        tp_status st = tp_operation_validate(clone, &req->ops[i], &rej);
        if (st != TP_STATUS_OK) {
            if (rec) {
                (void)tp_diff__record_budget_end(NULL);
            }
            tp_txn__commit_reject(out, clone, rec, NULL, payload,
                                  m->revision, i, st, rej.field, rej.message);
            return tp_error_set(err, st, "operation %d rejected: %s", i,
                                rej.message);
        }

        tp_diff_op entry;
        memset(&entry, 0, sizeof entry);
        if (rec) {
            /* Validation above owns deterministic first-reject. Capture still
             * resolves every entity and bounds-checks every index defensively;
             * a NOT_FOUND/OUT_OF_BOUNDS here indicates capture drift, while OOM
             * remains an allocation fault (op_index -1). */
            tp_status cst = tp_diff_capture_before(clone, &req->ops[i], &entry);
            if (cst != TP_STATUS_OK) {
                const bool over_budget = tp_diff__record_budget_exceeded();
                (void)tp_diff__record_budget_end(NULL);
                if (over_budget) {
                    cst = TP_STATUS_OUT_OF_BOUNDS;
                }
                int op_index = (cst == TP_STATUS_OOM) ? -1 : i;
                const char *cmsg = over_budget ? "semantic diff exceeds the history record budget"
                                   : (cst == TP_STATUS_OOM)
                                       ? "diff capture failed (out of memory)"
                                       : "operation references a missing entity or an out-of-range index";
                tp_txn__commit_reject(out, clone, rec, &entry, payload, m->revision,
                                      over_budget ? -1 : op_index, cst, over_budget ? "history" : "", cmsg);
                return over_budget ? tp_error_set(err, cst, "semantic diff exceeds the history record budget")
                                   : tp_error_set(err, cst, "diff capture (before) failed at op %d", i);
            }
        }
        st = tp_op__apply_prevalidated(clone, &req->ops[i], &rej);
        if (st != TP_STATUS_OK) {
            if (rec) {
                (void)tp_diff__record_budget_end(NULL);
            }
            tp_txn__commit_reject(out, clone, rec, rec ? &entry : NULL, payload, m->revision, i, st, rej.field,
                                  rej.message);
            return tp_error_set(err, st, "operation %d rejected: %s", i, rej.message);
        }
        switch (req->ops[i].kind) {
            case TP_OP_ATLAS_CREATE:
            case TP_OP_SOURCE_ADD:
            case TP_OP_ANIMATION_CREATE:
            case TP_OP_TARGET_CREATE:
                structural_create_applied = true;
                break;
            default:
                break;
        }
        if (rec) {
            tp_status cst = tp_diff_capture_after(clone, &req->ops[i], &entry);
            if (cst != TP_STATUS_OK) {
                const bool over_budget = tp_diff__record_budget_exceeded();
                (void)tp_diff__record_budget_end(NULL);
                if (over_budget) {
                    cst = TP_STATUS_OUT_OF_BOUNDS;
                }
                tp_txn__commit_reject(out, clone, rec, &entry, payload, m->revision, -1, cst,
                                      over_budget ? "history" : "",
                                      over_budget ? "semantic diff exceeds the history record budget"
                                                  : "diff capture failed (out of memory)");
                return over_budget ? tp_error_set(err, cst, "semantic diff exceeds the history record budget")
                                   : tp_error_set(err, cst, "diff capture (after) failed at op %d", i);
            }
            tp_diff_record_push_op(rec, &entry); /* ownership of `entry`'s pointers moves into rec */
        }
    }

    /* Defense in depth at the transaction boundary: create validators reject
     * project-wide ID collisions at the offending op, while this one linear
     * canonical pass guarantees no future create mutator can publish a model
     * that only fails later at Save. Non-create transactions retain the narrow
     * semantic-identity fast path and pay no whole-project validation cost. */
    if (structural_create_applied) {
        tp_error canonical_error = {{0}};
        const tp_status canonical_status =
            tp_project_validate_canonical(clone, &canonical_error);
        if (canonical_status != TP_STATUS_OK) {
            if (rec) {
                (void)tp_diff__record_budget_end(NULL);
            }
            const char *message = canonical_error.msg[0]
                                      ? canonical_error.msg
                                      : "transaction candidate is not canonical";
            tp_txn__commit_reject(out, clone, rec, NULL, payload, m->revision,
                                  -1, canonical_status, "project", message);
            return tp_error_set(err, canonical_status, "%s", message);
        }
    }

    /* Acceptance is not publication. If the fully-applied candidate has the
     * same semantic identity, report a typed no-change outcome and leave every
     * model-side authority untouched: revision, retained ids, journal, history
     * (including a redo branch), dirty anchor, and side-effect coordinator. */
    bool no_change = false;
    bool no_change_known = false;
    if (req->op_count == 1 &&
        req->ops[0].kind != TP_OP_ATLAS_CREATE &&
        req->ops[0].kind != TP_OP_ATLAS_REMOVE) {
        /* Every non-structural single operation is scoped to one atlas. Fold
         * that atlas with the canonical semantic owner instead of traversing
         * every unrelated sprite twice. Multi-op batches retain the full fold
         * because their effects may cancel across operations. */
        const int before_index = tp_project_find_atlas_by_id(
            m->project, req->ops[0].atlas_id);
        const int after_index = tp_project_find_atlas_by_id(
            clone, req->ops[0].atlas_id);
        if (before_index >= 0 && after_index >= 0) {
            const tp_project_atlas *before =
                &m->project->atlases[before_index];
            const tp_operation *only = &req->ops[0];
            if (only->kind == TP_OP_ATLAS_RENAME) {
                no_change_known = true;
                no_change = strcmp(before->name,
                                   only->u.atlas_rename.name) == 0;
            } else if (only->kind == TP_OP_ATLAS_SETTINGS_SET) {
                const tp_op_atlas_settings *s = &only->u.atlas_settings;
                no_change_known = true;
                no_change = true;
#define TP_SETTING_DIFF(BIT, FIELD)                                            \
                if ((s->mask & (BIT)) && before->FIELD != s->FIELD) {          \
                    no_change = false;                                         \
                }
                TP_SETTING_DIFF(TP_AF_MAX_SIZE, max_size)
                TP_SETTING_DIFF(TP_AF_PADDING, padding)
                TP_SETTING_DIFF(TP_AF_MARGIN, margin)
                TP_SETTING_DIFF(TP_AF_EXTRUDE, extrude)
                TP_SETTING_DIFF(TP_AF_ALPHA_THRESHOLD, alpha_threshold)
                TP_SETTING_DIFF(TP_AF_MAX_VERTICES, max_vertices)
                TP_SETTING_DIFF(TP_AF_SHAPE, shape)
                TP_SETTING_DIFF(TP_AF_ALLOW_TRANSFORM, allow_transform)
                TP_SETTING_DIFF(TP_AF_POWER_OF_TWO, power_of_two)
#undef TP_SETTING_DIFF
                /* Float semantic identity follows canonical %.9g text. An
                 * exact equality is a known no-change; a different binary
                 * value needs the canonical atlas fold to catch equal text. */
                if ((s->mask & TP_AF_PIXELS_PER_UNIT) &&
                    before->pixels_per_unit != s->pixels_per_unit) {
                    no_change_known = false;
                }
            }
            if (!no_change_known) {
                no_change = tp_id128_eq(
                    tp_semantic_atlas_identity(m->project, before),
                    tp_semantic_atlas_identity(
                        clone, &clone->atlases[after_index]));
            }
        }
    } else if (req->op_count != 1) {
        no_change = tp_id128_eq(tp_semantic_identity(clone),
                                tp_semantic_identity(m->project));
    }
    if (no_change) {
        if (rec) {
            (void)tp_diff__record_budget_end(NULL);
            tp_diff_record_free(rec);
        }
        tp_project_destroy(clone);
        free(payload);
        if (out) {
            tp_txn__result_echo_discard(out);
            out->committed = false;
            out->no_change = true;
            out->revision = m->revision;
        }
        return TP_STATUS_OK;
    }

    int64_t next_revision = 0;
    tp_status next_st = tp_model__next_revision(m->revision, &next_revision,
                                                err);
    if (next_st != TP_STATUS_OK) {
        if (rec) {
            (void)tp_diff__record_budget_end(NULL);
        }
        tp_txn__commit_reject(out, clone, rec, NULL, payload, m->revision, -1,
                              next_st, "revision",
                              "the model revision cannot be advanced");
        return next_st;
    }

    tp_history_push_plan history_plan;
    memset(&history_plan, 0, sizeof history_plan);
    if (rec && !tp_diff__record_budget_end(&rec->bytes)) {
        tp_txn__commit_reject(out, clone, rec, NULL, payload, m->revision, -1, TP_STATUS_OUT_OF_BOUNDS,
                              "history", "diff byte accounting overflowed");
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "diff byte accounting overflowed");
    }

    /* Prepare history eviction FIRST: ALL fallible work -- clone, capture,
     * record build, history admission -- must happen BEFORE idstore->record, so that once
     * the id is recorded the only remaining steps are the allocation-free swap + revision
     * bump + push. A reserve OOM fails the commit cleanly (id NOT yet recorded -> the same
     * transaction id stays retryable, never poisoned into a permanent DUPLICATE_ID). */
    if (rec) {
        tp_status rvst = tp_history_prepare_push(m->history, rec, &history_plan, err);
        if (rvst != TP_STATUS_OK) {
            const char *message = rvst == TP_STATUS_OOM ? "could not prepare history (out of memory)"
                                                        : "semantic diff exceeds the history budget";
            tp_txn__commit_reject(out, clone, rec, NULL, payload, m->revision, -1, rvst, "history", message);
            if (rvst == TP_STATUS_OOM) {
                return tp_error_set(err, rvst, "history prepare failed");
            }
            return rvst;
        }
    }

    /* Coordinator prepare stages tied side-effects before the live idempotency
     * gate and allocation-free publication. A recovery append is not a commit gate. */
    /* Stage tied side-effects before the gate,
     * once the ops have applied to the clone. A prepare fault rejects with no side-
     * effects staged (no abort needed). */
    if (m->coordinator && m->coordinator->prepare) {
        tp_status ps = m->coordinator->prepare(m->coordinator->ctx, req, err);
        if (ps != TP_STATUS_OK) {
            tp_txn__commit_reject(out, clone, rec, NULL, payload, m->revision, -1, ps, "",
                                  "side-effect prepare failed");
            return ps;
        }
    }

    /* Live idempotency is part of the in-memory commit and remains authoritative
     * even when recovery only has an older durable prefix. Record it as the last
     * fallible gate before the allocation-free model publication. */
    tp_status gate = TP_STATUS_OK;
    if (m->idstore && m->idstore->record) {
        gate = m->idstore->record(m->idstore->ctx, req->id_hex, err);
    }
    if (gate != TP_STATUS_OK) {
        if (m->coordinator && m->coordinator->abort) {
            m->coordinator->abort(m->coordinator->ctx, req);
        }
        tp_txn__commit_reject(out, clone, rec, NULL, payload, m->revision,
                              -1, gate, "",
                              "could not record the transaction id (out of memory)");
        return gate;
    }

    /* The commit: allocation-free pointer swap + one revision bump. */
    tp_model__replace_owned_project(m, clone);
    m->revision = next_revision;

    /* Push the diff (allocation-free): records the produced revision + discards any
     * redo branch (a new transaction after an Undo drops the redo steps). */
    if (rec) {
        rec->revision = m->revision;
        tp_history_push_prepared(m->history, rec, &history_plan);
    }

    /* Publish infallible/idempotent tied side-effects as part of the live
     * commit before attempting subordinate recovery recording. */
    if (m->coordinator && m->coordinator->publish) {
        m->coordinator->publish(m->coordinator->ctx, req);
    }

    /* Recovery follows the complete irreversible live commit. Admission, encoding,
     * append, and sync failures preserve the committed model and make recovery
     * sticky-degraded; later dependent records are deliberately suppressed. */
    if (recovery_ready) {
        if (recovery_failure == TP_STATUS_OK) {
            recovery_failure = tp_journal_append_txn_counted(
                m->journal, req->id_hex, next_revision,
                (const uint8_t *)payload, payload_len,
                (size_t)req->op_count, &recovery_error);
        }
        if (recovery_failure != TP_STATUS_OK) {
            tp_model__degrade_recovery(m, recovery_failure);
        } else {
            tp_model__mark_recovery_durable(m, next_revision);
        }
    }
    free(payload);

    if (out) {
        out->committed = true;
        out->revision = m->revision;
    }
    return TP_STATUS_OK;
}

/* ---- shared preflight gate (typed + JSON entry points) ------------------- */

bool tp_txn__is_hex32_lower(const char *s) {
    if (!s) {
        return false;
    }
    size_t len = 0U;
    while (len < 33U && s[len]) {
        len++;
    }
    if (len != 32U) {
        return false;
    }
    for (size_t i = 0U; i < len; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

tp_status tp_txn__check_op_count(int op_count, tp_error *err) {
    if (op_count < 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "transaction operation count cannot be negative");
    }
    if (op_count > TP_TXN_MAX_OPS) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "transaction has %d operations; maximum is %d", op_count, TP_TXN_MAX_OPS);
    }
    return TP_STATUS_OK;
}

tp_status tp_txn__preflight(tp_model *m, const char *id_hex, int64_t expected_revision, tp_txn_result *out,
                            tp_error *err) {
    tp_txn__result_reset(out, id_hex); /* NULL-safe */

    /* (a) transaction id must be 32 lowercase hex (the typed path never validated
     * this before -- an empty/garbage id would commit and then collide). */
    if (!tp_txn__is_hex32_lower(id_hex)) {
        if (out) {
            out->revision = m->revision;
        }
        tp_txn__result_add_error(out, -1, TP_STATUS_ID_MALFORMED, "id", "transaction id must be 32 lowercase hex");
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "transaction id must be 32 lowercase hex");
    }

    /* (b) idempotency: live memory is authoritative. A degraded journal may
     * retain only an older durable prefix; recovery seeds this store when a
     * model is rebuilt. */
    bool dup = m->idstore && m->idstore->contains &&
               m->idstore->contains(m->idstore->ctx, id_hex);
    if (dup) {
        if (out) {
            out->revision = m->revision;
        }
        tp_txn__result_add_error(out, -1, TP_STATUS_DUPLICATE_ID, "id", "transaction id already applied");
        return tp_error_set(err, TP_STATUS_DUPLICATE_ID, "transaction id already applied");
    }

    /* (c) revision precondition: a mismatch rejects ALONE (op_index -1). */
    tp_status rv = tp_revision_check(expected_revision, m->revision, err);
    if (rv != TP_STATUS_OK) {
        if (out) {
            out->revision = m->revision;
        }
        tp_txn__result_add_error(out, -1, rv, "expected_revision", err ? err->msg : "");
        return rv;
    }
    return TP_STATUS_OK;
}

/* ---- typed entry point --------------------------------------------------- */

tp_status tp_model_apply(tp_model *m, const tp_txn_request *req, tp_txn_result *out, tp_error *err) {
    if (!m || !req) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model or request");
    }
    /* Structural resource admission precedes idempotency/revision and clone work,
     * matching the JSON envelope path. The result is still a normal structured
     * rejection tagged with the caller's transaction id/current revision. */
    tp_status count_st = tp_txn__check_op_count(req->op_count, err);
    if (count_st != TP_STATUS_OK) {
        tp_txn__result_reset(out, req->id_hex);
        if (out) {
            out->revision = m->revision;
            tp_txn__result_add_error(out, -1, count_st, "operations", err ? err->msg : "");
        }
        return count_st;
    }
    if (req->op_count > 0 && !req->ops) {
        const tp_status pointer_st = tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "transaction operations are null for a positive operation count");
        tp_txn__result_reset(out, req->id_hex);
        if (out) {
            out->revision = m->revision;
            tp_txn__result_add_error(out, -1, pointer_st, "operations",
                                     err ? err->msg : "transaction operations are null");
        }
        return pointer_st;
    }
    /* Shared preflight: id format + idempotency + revision precondition. */
    tp_status pf = tp_txn__preflight(m, req->id_hex, req->expected_revision, out, err);
    if (pf != TP_STATUS_OK) {
        return pf;
    }
    /* Clone + apply atomically. */
    return tp_txn__commit_validated(m, req, out, err);
}
