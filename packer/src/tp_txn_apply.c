/*
 * F2-02 tasks 1-4: the atomic transaction core over the live tp_project model.
 *
 * Mechanism -- transactional CLONE (docs/decisions/0011): idempotency (a seen id ->
 * duplicate_id) -> revision precondition (mismatch rejects ALONE, op_index -1, before
 * any per-op work) -> clone the model -> validate+apply each op to the CLONE op-by-op
 * via the F2-01 tp_operation_apply (so op N may depend on ops 1..N-1) -> on FULL
 * success record the id, swap the clone in (freeing the old model), and bump the
 * revision by exactly 1. On ANY op rejection or allocator failure the clone is
 * discarded and the LIVE model is byte-unchanged (§416). The commit point is an
 * allocation-free pointer swap: provably atomic.
 *
 * SEMANTIC vs SHAPE validation (divergence from the C0-02 static-table spike,
 * documented in decision 0011): the model-INDEPENDENT shape faults (unknown op,
 * unknown field, malformed *_id) are collected-all in tp_txn_parse.c before any
 * apply. The model-DEPENDENT semantic faults (dangling id, range, name collision)
 * are validated against the PROGRESSIVELY-applied clone -- which is what lets a
 * create-then-use batch validate -- and report the FIRST offending op (a later op's
 * validity depends on the earlier ops succeeding, so a full collect-all is not
 * well-defined once intra-batch dependencies exist).
 */

#include "tp_core/tp_transaction.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "tp_diff_internal.h" /* F2-03: optional per-op diff capture on commit */
#include "tp_idset_internal.h" /* F2-04 fix C1: migrate retained ids into the journal on attach */
#include "tp_journal_internal.h" /* F2-04 fix C3: poison the journal from the recovery glue */
#include "tp_txn_internal.h"

/* ---- model lifecycle ----------------------------------------------------- */

tp_model *tp_model_wrap(tp_project *project) {
    if (!project) {
        return NULL;
    }
    tp_model *m = (tp_model *)calloc(1, sizeof *m);
    if (!m) {
        return NULL;
    }
    m->idstore = tp_txn_idstore_memory_create();
    if (!m->idstore) {
        free(m);
        return NULL;
    }
    m->project = project;
    m->owns_idstore = true;
    m->revision = 0;
    m->saved_identity = tp_semantic_identity(project); /* freshly wrapped == clean */
    return m;
}

tp_model *tp_model_create(void) {
    tp_project *p = tp_project_create();
    if (!p) {
        return NULL;
    }
    tp_model *m = tp_model_wrap(p);
    if (!m) {
        tp_project_destroy(p);
        return NULL;
    }
    return m;
}

void tp_model_destroy(tp_model *m) {
    if (!m) {
        return;
    }
    tp_history_destroy(m->history); /* F2-03: NULL-safe when history was never enabled */
    tp_journal_destroy(m->journal); /* F2-04: NULL-safe when no journal was attached */
    tp_project_destroy(m->project);
    if (m->idstore) {
        if (m->owns_idstore && m->idstore->destroy) {
            m->idstore->destroy(m->idstore->ctx);
        }
        free(m->idstore);
    }
    free(m);
}

tp_project *tp_model_project(tp_model *m) { return m ? m->project : NULL; }
int64_t tp_model_revision(const tp_model *m) { return m ? m->revision : 0; }

bool tp_model_dirty(const tp_model *m) {
    if (!m) {
        return false;
    }
    /* C5: a crash-recovered model is dirty vs the on-disk project file until it is
     * explicitly saved, even when its identity happens to match the recovered baseline. */
    if (m->recovered_unsaved) {
        return true;
    }
    return !tp_id128_eq(tp_semantic_identity(m->project), m->saved_identity);
}

void tp_model_mark_saved(tp_model *m) {
    if (m) {
        m->saved_identity = tp_semantic_identity(m->project); /* re-baseline; revision unchanged */
        m->recovered_unsaved = false;                         /* C5: the save flushed the recovered state */
    }
}

/* ---- revision precondition ----------------------------------------------- */

tp_status tp_revision_check(int64_t expected_revision, int64_t current_revision, tp_error *err) {
    if (expected_revision == current_revision) {
        return TP_STATUS_OK;
    }
    if (expected_revision < current_revision) {
        return tp_error_set(err, TP_STATUS_REVISION_CONFLICT, "expected_revision %" PRId64 " < current %" PRId64,
                            expected_revision, current_revision);
    }
    return tp_error_set(err, TP_STATUS_INVALID_REVISION, "expected_revision %" PRId64 " > current %" PRId64,
                        expected_revision, current_revision);
}

/* ---- request / result lifecycle ------------------------------------------ */

void tp_txn_request_free(tp_txn_request *req) {
    if (!req) {
        return;
    }
    for (int i = 0; i < req->op_count; i++) {
        tp_operation_free(&req->ops[i]);
    }
    free(req->ops);
    free(req->label);
    free(req->author);
    free(req);
}

void tp_txn_result_free(tp_txn_result *res) {
    if (!res) {
        return;
    }
    free(res->ops);
    free(res->errors);
    res->ops = NULL;
    res->errors = NULL;
    res->op_count = res->error_count = 0;
}

/* ---- result assembly ----------------------------------------------------- */

void tp_txn__result_reset(tp_txn_result *out, const char *id_hex) {
    if (!out) {
        return;
    }
    out->schema = TP_TXN_SCHEMA;
    (void)snprintf(out->transaction_id, sizeof out->transaction_id, "%s", id_hex ? id_hex : "");
    out->committed = false;
    out->revision = 0;
    out->ops = NULL;
    out->op_count = 0;
    out->errors = NULL;
    out->error_count = 0;
}

/* Test-only fault seam for the add_error grow: -1 = off; N = the (N+1)th call's
 * grow fails once (then re-arms to off). Lets a test prove a dropped shape-error
 * record still forces a reject. */
static int s_add_error_fail = -1;
void tp_txn__test_set_add_error_fail(int nth) { s_add_error_fail = nth; }

/* Append one error (grows the dynamic error array). Returns true when stored; false
 * when the grow failed (OOM). A collect-all caller must treat a false as a forced
 * reject -- a shape-faulted batch must never falsely commit just because its error
 * record could not be stored. */
bool tp_txn__result_add_error(tp_txn_result *out, int op_index, tp_status code, const char *field, const char *msg) {
    if (!out) {
        return false;
    }
    if (s_add_error_fail >= 0) { /* fault seam: fail this grow once */
        if (s_add_error_fail == 0) {
            s_add_error_fail = -1;
            return false;
        }
        s_add_error_fail--;
    }
    tp_txn_error *n = (tp_txn_error *)realloc(out->errors, (size_t)(out->error_count + 1) * sizeof *n);
    if (!n) {
        return false;
    }
    out->errors = n;
    tp_txn_error *e = &out->errors[out->error_count];
    e->op_index = op_index;
    e->code = code;
    (void)snprintf(e->field, sizeof e->field, "%s", field ? field : "");
    (void)snprintf(e->message, sizeof e->message, "%s", msg ? msg : "");
    out->error_count++;
    return true;
}

/* Add one id addressing echo to a committed result op. */
static void addr_id(tp_txn_result_op *ro, const char *key, tp_id_kind idk, tp_id128 id) {
    if (ro->addr_count >= 3) {
        return;
    }
    tp_txn_addr *a = &ro->addr[ro->addr_count++];
    (void)snprintf(a->key, sizeof a->key, "%s", key);
    a->idk = idk;
    a->id = id;
    a->str[0] = '\0';
}

static void addr_str(tp_txn_result_op *ro, const char *key, const char *val) {
    if (ro->addr_count >= 3) {
        return;
    }
    tp_txn_addr *a = &ro->addr[ro->addr_count++];
    (void)snprintf(a->key, sizeof a->key, "%s", key);
    a->idk = TP_ID_KIND_INVALID;
    a->id = tp_id128_nil();
    (void)snprintf(a->str, sizeof a->str, "%s", val ? val : "");
}

/* Echo an op's wire + addressing ids on a committed result op (no diff -- F2-03). */
static void fill_result_op(tp_txn_result_op *ro, const tp_operation *op) {
    memset(ro, 0, sizeof *ro);
    (void)snprintf(ro->wire, sizeof ro->wire, "%s", tp_op_wire(op->kind));
    addr_id(ro, "atlas_id", TP_ID_KIND_ATLAS, op->atlas_id);
    switch (op->kind) {
        case TP_OP_SOURCE_ADD: addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.source_add.source_id); break;
        case TP_OP_SOURCE_REMOVE:
        case TP_OP_SOURCE_REPLACE: addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.source_ref.source_id); break;
        case TP_OP_SPRITE_OVERRIDE_SET:
            addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.sprite_set.source_id);
            addr_str(ro, "src_key", op->u.sprite_set.src_key);
            break;
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.sprite_clear.source_id);
            addr_str(ro, "src_key", op->u.sprite_clear.src_key);
            break;
        case TP_OP_SPRITE_NAME_SET:
            addr_id(ro, "source_id", TP_ID_KIND_SOURCE, op->u.sprite_name.source_id);
            addr_str(ro, "src_key", op->u.sprite_name.src_key);
            break;
        case TP_OP_ANIMATION_CREATE: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_create.anim_id); break;
        case TP_OP_ANIMATION_REMOVE: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_ref.anim_id); break;
        case TP_OP_ANIMATION_RENAME: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_rename.anim_id); break;
        case TP_OP_ANIMATION_SETTINGS_SET: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_settings.anim_id); break;
        case TP_OP_ANIMATION_FRAMES_SET: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_frames_set.anim_id); break;
        case TP_OP_ANIMATION_FRAME_ADD: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_add.anim_id); break;
        case TP_OP_ANIMATION_FRAME_REMOVE: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_rm.anim_id); break;
        case TP_OP_ANIMATION_FRAME_MOVE: addr_id(ro, "anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_move.anim_id); break;
        case TP_OP_TARGET_CREATE: addr_id(ro, "target_id", TP_ID_KIND_TARGET, op->u.target_create.target_id); break;
        case TP_OP_TARGET_REMOVE: addr_id(ro, "target_id", TP_ID_KIND_TARGET, op->u.target_ref.target_id); break;
        case TP_OP_TARGET_SET: addr_id(ro, "target_id", TP_ID_KIND_TARGET, op->u.target_set.target_id); break;
        case TP_OP_ATLAS_CREATE:
        case TP_OP_ATLAS_REMOVE:
        case TP_OP_ATLAS_RENAME:
        case TP_OP_ATLAS_SETTINGS_SET:
        case TP_OP_INVALID:
        case TP_OP_KIND_COUNT: break; /* atlas ops: atlas_id is the only addressing id */
    }
}

/* Reject the in-flight commit -- the single home for the clone/record cleanup-and-fail
 * contract (fix [4]; was ~7x near-identical blocks). Frees the partially-captured entry
 * and the diff record, marks `out` rejected at the UNCHANGED revision with one
 * structured error, and discards the clone so the LIVE model stays byte-unchanged.
 * `entry` / `rec` / `clone` may each be NULL (all frees are NULL-safe). */
static void tp_txn__commit_reject(tp_txn_result *out, tp_project *clone, tp_diff_record *rec, tp_diff_op *entry,
                                  int64_t revision, int op_index, tp_status code, const char *field, const char *msg) {
    tp_diff_op_free(entry);   /* NULL-safe: frees a partially-captured before/after entry */
    tp_diff_record_free(rec); /* NULL-safe */
    if (out) {
        out->committed = false;
        out->revision = revision; /* unchanged: nothing committed */
        tp_txn__result_add_error(out, op_index, code, field, msg);
    }
    tp_project_destroy(clone); /* NULL-safe; discard the clone -- live model untouched */
}

/* ---- the atomic commit path (shared by the typed and JSON entry points) --- *
 * PRE: idempotency + revision precondition already passed. Clones the model, applies
 * each op to the clone, and on FULL success records the id + swaps + bumps revision.
 * Fills `out` committed/rejected. The live model is byte-unchanged unless it returns
 * TP_STATUS_OK. */
tp_status tp_txn__commit_validated(tp_model *m, const tp_txn_request *req, tp_txn_result *out, tp_error *err) {
    tp_project *clone = tp_project_clone(m->project);
    if (!clone) {
        tp_txn__commit_reject(out, NULL, NULL, NULL, m->revision, -1, TP_STATUS_OOM, "",
                              "could not clone the model (out of memory)");
        return tp_error_set(err, TP_STATUS_OOM, "transaction clone failed");
    }

    /* F2-03: when a history is attached, capture the per-op semantic diff as each op
     * applies to the clone. The whole diff is built PRE-swap, so a capture allocation
     * failure fails the commit cleanly (clone discarded, live model byte-unchanged) --
     * F2-02 atomicity is preserved. A NULL history => exactly the F2-02 code path. */
    tp_diff_record *rec = NULL;
    if (m->history) {
        rec = tp_diff_record_new(req->label, req->author, req->op_count);
        if (!rec) {
            tp_txn__commit_reject(out, clone, NULL, NULL, m->revision, -1, TP_STATUS_OOM, "",
                                  "could not allocate the diff record (out of memory)");
            return tp_error_set(err, TP_STATUS_OOM, "diff record allocation failed");
        }
    }

    for (int i = 0; i < req->op_count; i++) {
        tp_diff_op entry;
        memset(&entry, 0, sizeof entry);
        if (rec) {
            /* Capture runs BEFORE tp_operation_apply validates, so capture itself must
             * resolve every entity + bounds-check every index before any deref (fix
             * [1]/[2]) -- a dangling id / out-of-range index yields a structured status,
             * never a crash. A NOT_FOUND/OUT_OF_BOUNDS is an op-content rejection at op i
             * (the same status the history-LESS apply path returns for this input); an OOM
             * is an allocation fault (op_index -1, the F2-02 convention). */
            tp_status cst = tp_diff_capture_before(clone, &req->ops[i], &entry);
            if (cst != TP_STATUS_OK) {
                int op_index = (cst == TP_STATUS_OOM) ? -1 : i;
                const char *cmsg = (cst == TP_STATUS_OOM)
                                       ? "diff capture failed (out of memory)"
                                       : "operation references a missing entity or an out-of-range index";
                tp_txn__commit_reject(out, clone, rec, &entry, m->revision, op_index, cst, "", cmsg);
                return tp_error_set(err, cst, "diff capture (before) failed at op %d", i);
            }
        }
        tp_op_reject rej;
        tp_status st = tp_operation_apply(clone, &req->ops[i], &rej);
        if (st != TP_STATUS_OK) {
            tp_txn__commit_reject(out, clone, rec, rec ? &entry : NULL, m->revision, i, st, rej.field, rej.message);
            return tp_error_set(err, st, "operation %d rejected: %s", i, rej.message);
        }
        if (rec) {
            tp_status cst = tp_diff_capture_after(clone, &req->ops[i], &entry);
            if (cst != TP_STATUS_OK) {
                tp_txn__commit_reject(out, clone, rec, &entry, m->revision, -1, cst, "",
                                      "diff capture failed (out of memory)");
                return tp_error_set(err, cst, "diff capture (after) failed at op %d", i);
            }
            tp_diff_record_push_op(rec, &entry); /* ownership of `entry`'s pointers moves into rec */
        }
    }

    /* Reserve the history slot FIRST (fix [3]): ALL fallible work -- clone, capture,
     * record build, history reserve -- must happen BEFORE idstore->record, so that once
     * the id is recorded the only remaining steps are the allocation-free swap + revision
     * bump + push. A reserve OOM fails the commit cleanly (id NOT yet recorded -> the same
     * transaction id stays retryable, never poisoned into a permanent DUPLICATE_ID). */
    if (rec) {
        tp_status rvst = tp_history_reserve(m->history);
        if (rvst != TP_STATUS_OK) {
            tp_txn__commit_reject(out, clone, rec, NULL, m->revision, -1, rvst, "",
                                  "could not reserve a history slot (out of memory)");
            return tp_error_set(err, rvst, "history reserve failed");
        }
    }

    /* The commit ACKNOWLEDGEMENT gate (spec §7.1): validate/stage -> apply -> APPEND ->
     * publish. The LAST fallible step before the allocation-free swap is either the
     * durable journal append (journal-backed) or the in-memory id record (journal-less,
     * EXACTLY the F2-02/03 path). Everything after a successful gate -- swap + revision++
     * + diff push + side-effect publish -- is allocation-free, so "gate passed => commit
     * cannot fail" holds. Whichever gate is used registers the transaction id ONLY on
     * success, so a rolled-back txn never poisons the id into a permanent DUPLICATE_ID. */
    /* (i) Coordinator prepare -- stage tied side-effects (B1 Extract) BEFORE the gate,
     * once the ops have applied to the clone. A prepare fault rejects with no side-
     * effects staged (no abort needed). */
    if (m->coordinator && m->coordinator->prepare) {
        tp_status ps = m->coordinator->prepare(m->coordinator->ctx, req, err);
        if (ps != TP_STATUS_OK) {
            tp_txn__commit_reject(out, clone, rec, NULL, m->revision, -1, ps, "", "side-effect prepare failed");
            return ps;
        }
    }

    /* (ii) The gate itself: journal-backed durably appends this transaction's SERIALIZED
     * OPERATION (format B -- a tp_txn_request_encode blob), so recovery re-runs apply from
     * the last checkpoint (load the checkpoint base, replay the post-checkpoint op-payloads);
     * journal-less records the id in the in-memory idstore (EXACTLY the F2-02/03 path).
     * Either way the transaction id is registered ONLY on success, so a rolled-back txn
     * never poisons the id into a permanent DUPLICATE_ID. (Checkpoints, written on attach and
     * on the R3 compaction cadence, remain full snapshots -- the durable replay baseline.) */
    tp_status gate = TP_STATUS_OK;
    const char *gate_msg = "";
    if (m->journal) {
        /* The op request already validated + applied to the clone above, so encode() fails
         * only on OOM (an INVALID-kind op is impossible here). The journal payload is OPAQUE
         * bytes to the log -- no journal API change. */
        char *payload = tp_txn_request_encode(req);
        if (!payload) {
            gate = tp_error_set(err, TP_STATUS_OOM, "could not serialize the operation for the recovery journal");
            gate_msg = "could not serialize the operation for the journal";
        } else {
            gate = tp_journal_append_txn(m->journal, req->id_hex, m->revision + 1, (const uint8_t *)payload,
                                         strlen(payload), err);
            gate_msg = "recovery journal append failed (transaction rolled back)";
        }
        free(payload);
    } else if (m->idstore && m->idstore->record) {
        gate = m->idstore->record(m->idstore->ctx, req->id_hex, err);
        gate_msg = "could not record the transaction id (out of memory)";
    }
    if (gate != TP_STATUS_OK) {
        /* Exact rollback: discard clone, live model byte-unchanged, no acknowledgement,
         * abort tied side-effects, txn retryable. */
        if (m->coordinator && m->coordinator->abort) {
            m->coordinator->abort(m->coordinator->ctx, req);
        }
        tp_txn__commit_reject(out, clone, rec, NULL, m->revision, -1, gate, "", gate_msg);
        return gate;
    }

    /* The commit: allocation-free pointer swap + one revision bump. */
    tp_project_destroy(m->project);
    m->project = clone;
    m->revision += 1;

    /* Push the diff (allocation-free): records the produced revision + discards any
     * redo branch (a new transaction after an Undo drops the redo steps). */
    if (rec) {
        rec->revision = m->revision;
        tp_history_push_reserved(m->history, rec);
    }

    /* (iii) Publish tied side-effects now that the transaction is durably acknowledged.
     * BOUNDARY (ADR 0013 §D1, B1-02): publish() has no durability of its own -- a crash
     * BETWEEN the acknowledged append and this call leaves the txn committed+retained
     * while its side-effects are unpublished, and a resubmit returns DUPLICATE_ID so
     * publish never re-runs. The coordinator is a no-op until B1 wires Extract, so this
     * is not a live bug now; recovery-time re-drive / publish idempotency is B1-02's
     * responsibility (recovery exposes the retained set for B1 to reconcile staged
     * side-effects). We deliberately do NOT build 2-phase crash-durability here. */
    if (m->coordinator && m->coordinator->publish) {
        m->coordinator->publish(m->coordinator->ctx, req);
    }

    if (out) {
        out->committed = true;
        out->revision = m->revision;
        if (req->op_count > 0) {
            out->ops = (tp_txn_result_op *)calloc((size_t)req->op_count, sizeof *out->ops);
            if (out->ops) {
                for (int i = 0; i < req->op_count; i++) {
                    fill_result_op(&out->ops[i], &req->ops[i]);
                }
                out->op_count = req->op_count;
            }
            /* An echo alloc failure does not un-commit: the transaction IS applied.
             * out->op_count stays 0 (the committed status + new revision still hold). */
        }
    }
    return TP_STATUS_OK;
}

/* ---- shared preflight gate (typed + JSON entry points) ------------------- */

bool tp_txn__is_hex32_lower(const char *s) {
    if (!s) {
        return false;
    }
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return n == 32;
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

    /* (b) idempotency: a re-submitted committed id rejects (model unchanged). When a
     * journal is attached its retained-id index is the idempotency authority (§7.2:
     * recoverable across restart); otherwise the in-memory idstore answers (F2-02). */
    bool dup = m->journal ? tp_journal_contains(m->journal, id_hex)
                          : (m->idstore && m->idstore->contains && m->idstore->contains(m->idstore->ctx, id_hex));
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
    /* Shared preflight: id format + idempotency + revision precondition. */
    tp_status pf = tp_txn__preflight(m, req->id_hex, req->expected_revision, out, err);
    if (pf != TP_STATUS_OK) {
        return pf;
    }
    /* Clone + apply atomically. */
    return tp_txn__commit_validated(m, req, out, err);
}

/* ---- F2-04 side-effect coordinator + model <-> journal glue -------------- */

tp_side_effect_coordinator tp_side_effect_coordinator_noop(void) {
    tp_side_effect_coordinator c;
    c.ctx = NULL;
    c.prepare = NULL;
    c.publish = NULL;
    c.abort = NULL;
    return c;
}

void tp_model_set_coordinator(tp_model *m, tp_side_effect_coordinator *c) {
    if (m) {
        m->coordinator = c;
    }
}

/* R2b/R3: serialize the model's live committed project into a fresh malloc'd snapshot
 * (*snap / *snap_len, caller frees) and PROVE it round-trips through tp_project_load_buffer
 * before it is handed to a checkpoint. The checkpoint is the load-bearing recovery BASE --
 * format B replays post-checkpoint ops ONTO it (see tp_model_recover) -- so a base that
 * serializes but does NOT reload (e.g. nil structural ids) would silently discard the whole
 * recovered session. Enforcing "the checkpoint must round-trip" HERE in core, below any
 * client's own promotion (e.g. the GUI's ensure_ids), means no caller can persist an
 * unrecoverable base. Shared by tp_model_attach_journal (initial checkpoint) and
 * tp_model_compact_journal (Save-window compaction) so BOTH inherit the guarantee. On any
 * failure *snap is NULL/0 and a wrapped error is returned; the caller leaves the journal
 * untouched. */
static tp_status model_checkpoint_snapshot(tp_model *m, char **snap, size_t *snap_len, tp_error *err) {
    *snap = NULL;
    *snap_len = 0;
    tp_status ss = tp_project_save_buffer(m->project, snap, snap_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    tp_project *probe = NULL;
    tp_status vs = tp_project_load_buffer(*snap, *snap_len, &probe, err);
    if (vs != TP_STATUS_OK) {
        free(*snap);
        *snap = NULL;
        *snap_len = 0;
        return tp_error_set(err, vs, "recovery checkpoint does not round-trip "
                                     "(promote structural ids before checkpointing)");
    }
    tp_project_destroy(probe);
    return TP_STATUS_OK;
}

tp_status tp_model_attach_journal(tp_model *m, tp_journal *j, tp_error *err) {
    if (!m || !j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model or journal");
    }
    if (m->journal) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "a journal is already attached");
    }
    /* C1: migrate ids the model already committed journal-less (recorded only in the
     * in-memory idstore) into the fresh journal's retained-id index BEFORE the initial
     * checkpoint. Otherwise the post-attach idempotency authority (tp_journal_contains)
     * would not know those ids and a legitimate re-submit would double-apply (§7.2). The
     * seed happens pre-checkpoint so the checkpoint's durable id-list carries them too,
     * and recovery sees them. (A foreign idstore we cannot enumerate -> no migration.) */
    const tp_idset *pre = tp_txn_idstore_mem_view(m->idstore);
    if (pre) {
        int pre_count = tp_idset_count(pre);
        for (int i = 0; i < pre_count; i++) {
            tp_status ms = tp_journal_seed_retained_id(j, tp_idset_at(pre, i));
            if (ms != TP_STATUS_OK) {
                return tp_error_set(err, ms, "could not migrate retained ids into the journal (out of memory)");
            }
        }
    }
    /* Initial CHECKPOINT of the current committed state so the journal is self-sufficient for
     * recovery (spec §22.3 checkpoint + journal), via the shared round-trip-proving helper. On
     * failure the model is NOT attached and the caller still owns j. */
    char *snap = NULL;
    size_t snap_len = 0;
    tp_status ss = model_checkpoint_snapshot(m, &snap, &snap_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    tp_status cs = tp_journal_init_checkpoint(j, (const uint8_t *)snap, snap_len, m->revision, err);
    free(snap);
    if (cs != TP_STATUS_OK) {
        return cs;
    }
    m->journal = j; /* ownership transferred */
    return TP_STATUS_OK;
}

tp_status tp_model_compact_journal(tp_model *m, tp_error *err) {
    if (!m) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model");
    }
    if (!m->journal) {
        return TP_STATUS_OK; /* no journal attached (recovery off / journal-less): nothing to compact */
    }
    /* R3 (plan S18 R): compact the recovery journal to one fresh checkpoint == the current committed
     * state -- the Save-window reset. Uses the SAME round-trip-proving snapshot helper as attach, so a
     * compacted checkpoint is guaranteed loadable (it becomes the recovery base). On any snapshot/probe
     * failure the journal is left UNCHANGED (keeps its larger-but-correct checkpoint+ops and still
     * recovers); the compaction primitive itself fails closed on a truncate failure (poison preserved). */
    char *snap = NULL;
    size_t snap_len = 0;
    tp_status ss = model_checkpoint_snapshot(m, &snap, &snap_len, err);
    if (ss != TP_STATUS_OK) {
        return ss; /* snapshot/probe failed BEFORE any truncate: journal untouched, keeps recovering */
    }
    tp_status cs = tp_journal_compact(m->journal, (const uint8_t *)snap, snap_len, m->revision, err);
    free(snap);
    if (cs != TP_STATUS_OK) {
        /* A poisoned journal here means the BROKEN-STORE case: the truncate removed the old checkpoint
         * but the fresh one could not be written, so the store is checkpoint-LESS and appends onto it
         * would be unrecoverable. Poison would make every subsequent commit REJECT (append refused),
         * leaving the app unable to edit. DETACH the journal and continue JOURNAL-LESS (still editable,
         * recovery disabled for the session) rather than append unrecoverable records. A benign truncate
         * FAILURE leaves the store byte-intact + the journal healthy (NOT poisoned) -> keep it as-is. */
        if (tp_journal__is_poisoned(m->journal)) {
            tp_journal_destroy(m->journal);
            m->journal = NULL;
        }
        return cs;
    }
    return TP_STATUS_OK;
}

tp_status tp_model_set_recovery_metadata(tp_model *m, int64_t timestamp, const char *path, const char *name,
                                         tp_error *err) {
    if (!m) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model");
    }
    if (!m->journal) {
        return TP_STATUS_OK; /* no journal attached (recovery off / journal-less): nothing to record */
    }
    /* R5b-1: forward the project identity to the attached journal so a startup scan can list the crashed
     * project by path + name + time. Same null-journal-tolerant contract as tp_model_compact_journal; NULL
     * path/name are normalised to "" (never passed as NULL). A write failure is returned to the caller for
     * the soft channel -- metadata is informational and must never fail an edit or Save. */
    return tp_journal_set_metadata(m->journal, timestamp, path ? path : "", name ? name : "", err);
}

tp_status tp_model_recover(tp_journal_io io, tp_id128 key, tp_model **out, tp_journal_recovery *info, tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (info) {
        memset(info, 0, sizeof *info); /* always in a freeable state, even on an early hard fault */
    }
    tp_journal *j = tp_journal_create(io, key);
    if (!j) {
        return tp_error_set(err, TP_STATUS_OOM, "could not create journal for recovery");
    }
    tp_journal_recovery rec;
    tp_status rc = tp_journal_recover(j, &rec, err);
    if (rc != TP_STATUS_OK) {
        tp_journal_destroy(j);
        return rc;
    }
    /* Clean a torn/incomplete TAIL so continued appends stay recoverable (never guess
     * past the last good record). C2: a MID-STREAM corruption (a bad record with valid
     * records STILL after it) is NOT truncated -- that would physically delete those
     * trailing acknowledged records. Recover up to the last good record and PRESERVE the
     * file; tp_journal_recover has already poisoned the journal against appends behind
     * the corruption. A torn tail, or a single trailing corrupt record, is safe to drop.
     * C3: if the tail-clean truncate itself fails, poison the journal -- a still-present
     * bad record must never hide a later acknowledged append. */
    bool clean_tail = (rec.status == TP_JOURNAL_RECOVERY_TRUNCATED) ||
                      (rec.status == TP_JOURNAL_RECOVERY_CORRUPT && !rec.mid_stream_corrupt);
    if (clean_tail) {
        if (io.truncate(io.ctx, rec.stop_offset) != 0) {
            tp_journal__poison(j);
        }
    }
    bool keep_info = (info != NULL);
    if (keep_info) {
        *info = rec; /* transfer the snapshot ownership to the caller */
    }

    tp_status ret = TP_STATUS_OK;
    bool j_consumed = false; /* set once a model takes ownership of j */
    if (rec.records_recovered > 0 && rec.snapshot && rec.snapshot_len > 0) {
        /* Format B: load the last CHECKPOINT snapshot as the base, then REPLAY the post-checkpoint
         * TXN op-payloads onto it in commit order -- the SAME tp_operation_apply, in the SAME order,
         * as the commit path (tp_txn__commit_validated), so the replayed project reaches exactly the
         * committed state. A replay-time decode/apply failure is a HARD fault (the op was already
         * acknowledged at commit): surface it as a non-OK recover, never silently skip an op. */
        tp_project *p = NULL;
        tp_status ls = tp_project_load_buffer(rec.snapshot, rec.snapshot_len, &p, err); /* base checkpoint */
        if (ls != TP_STATUS_OK) {
            ret = ls; /* durable checkpoint snapshot did not load (real corruption despite a valid crc) */
        } else {
            for (size_t k = 0; ls == TP_STATUS_OK && k < rec.op_count; k++) {
                tp_txn_request *req = NULL;
                tp_status ds = tp_txn_request_decode(rec.ops[k].payload, &req, err);
                if (ds != TP_STATUS_OK) {
                    ls = ds; /* a durable op-payload did not decode */
                    break;
                }
                for (int i = 0; i < req->op_count; i++) {
                    tp_op_reject rej;
                    memset(&rej, 0, sizeof rej);
                    tp_status as = tp_operation_apply(p, &req->ops[i], &rej); /* identical to the commit apply */
                    if (as != TP_STATUS_OK) {
                        ls = tp_error_set(err, as, "recovery replay of transaction %zu op %d rejected: %s", k, i,
                                          rej.message);
                        break;
                    }
                }
                tp_txn_request_free(req);
            }
            if (ls != TP_STATUS_OK) {
                tp_project_destroy(p);
                ret = ls;
            } else {
                tp_model *rm = tp_model_wrap(p);
                if (!rm) {
                    tp_project_destroy(p);
                    ret = tp_error_set(err, TP_STATUS_OOM, "could not wrap the recovered project");
                } else {
                    rm->revision = rec.revision; /* the FINAL recovered revision (last record's) */
                    rm->saved_identity = tp_semantic_identity(p);
                    rm->recovered_unsaved = true; /* C5: recovered state is ahead of the project file -> DIRTY */
                    rm->journal = j;              /* owns j; its index is already seeded by recover */
                    j_consumed = true;
                    if (out) {
                        *out = rm;
                    } else {
                        tp_model_destroy(rm); /* also destroys j */
                    }
                }
            }
        }
    }
    /* Nothing rebuilt (empty / bad-header / stale-key / torn-first / load-fail): destroy
     * j here so the caller falls back to loading the project file. */
    if (!j_consumed) {
        tp_journal_destroy(j);
    }
    if (!keep_info) {
        tp_journal_recovery_free(&rec);
    }
    return ret;
}
