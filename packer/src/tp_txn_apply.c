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
    return !tp_id128_eq(tp_semantic_identity(m->project), m->saved_identity);
}

void tp_model_mark_saved(tp_model *m) {
    if (m) {
        m->saved_identity = tp_semantic_identity(m->project); /* re-baseline; revision unchanged */
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

/* ---- the atomic commit path (shared by the typed and JSON entry points) --- *
 * PRE: idempotency + revision precondition already passed. Clones the model, applies
 * each op to the clone, and on FULL success records the id + swaps + bumps revision.
 * Fills `out` committed/rejected. The live model is byte-unchanged unless it returns
 * TP_STATUS_OK. */
tp_status tp_txn__commit_validated(tp_model *m, const tp_txn_request *req, tp_txn_result *out, tp_error *err) {
    tp_project *clone = tp_project_clone(m->project);
    if (!clone) {
        if (out) {
            out->committed = false;
            out->revision = m->revision;
            tp_txn__result_add_error(out, -1, TP_STATUS_OOM, "", "could not clone the model (out of memory)");
        }
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
            if (out) {
                out->committed = false;
                out->revision = m->revision;
                tp_txn__result_add_error(out, -1, TP_STATUS_OOM, "", "could not allocate the diff record (out of memory)");
            }
            tp_project_destroy(clone);
            return tp_error_set(err, TP_STATUS_OOM, "diff record allocation failed");
        }
    }

    for (int i = 0; i < req->op_count; i++) {
        tp_diff_op entry;
        memset(&entry, 0, sizeof entry);
        if (rec) {
            tp_status cst = tp_diff_capture_before(clone, &req->ops[i], &entry);
            if (cst != TP_STATUS_OK) {
                tp_diff_op_free(&entry);
                tp_diff_record_free(rec);
                if (out) {
                    out->committed = false;
                    out->revision = m->revision;
                    tp_txn__result_add_error(out, -1, cst, "", "diff capture failed (out of memory)");
                }
                tp_project_destroy(clone);
                return tp_error_set(err, cst, "diff capture (before) failed at op %d", i);
            }
        }
        tp_op_reject rej;
        tp_status st = tp_operation_apply(clone, &req->ops[i], &rej);
        if (st != TP_STATUS_OK) {
            if (rec) {
                tp_diff_op_free(&entry);
                tp_diff_record_free(rec);
            }
            if (out) {
                out->committed = false;
                out->revision = m->revision; /* unchanged: nothing committed */
                tp_txn__result_add_error(out, i, st, rej.field, rej.message);
            }
            tp_project_destroy(clone); /* discard: live model untouched */
            return tp_error_set(err, st, "operation %d rejected: %s", i, rej.message);
        }
        if (rec) {
            tp_status cst = tp_diff_capture_after(clone, &req->ops[i], &entry);
            if (cst != TP_STATUS_OK) {
                tp_diff_op_free(&entry);
                tp_diff_record_free(rec);
                if (out) {
                    out->committed = false;
                    out->revision = m->revision;
                    tp_txn__result_add_error(out, -1, cst, "", "diff capture failed (out of memory)");
                }
                tp_project_destroy(clone);
                return tp_error_set(err, cst, "diff capture (after) failed at op %d", i);
            }
            tp_diff_record_push_op(rec, &entry); /* ownership of `entry`'s pointers moves into rec */
        }
    }

    /* Full success. Record the id BEFORE the swap so that a record OOM discards the
     * clone with the live model still byte-unchanged (the swap after a successful
     * record is allocation-free and cannot fail). */
    if (m->idstore && m->idstore->record) {
        tp_status rst = m->idstore->record(m->idstore->ctx, req->id_hex, err);
        if (rst != TP_STATUS_OK) {
            tp_diff_record_free(rec); /* NULL-safe */
            if (out) {
                out->committed = false;
                out->revision = m->revision;
                tp_txn__result_add_error(out, -1, rst, "", "could not record the transaction id (out of memory)");
            }
            tp_project_destroy(clone);
            return rst;
        }
    }

    /* Reserve the history slot PRE-swap so the post-swap push is allocation-free: a
     * reserve OOM fails the commit cleanly (clone discarded, model byte-unchanged). */
    if (rec) {
        tp_status rvst = tp_history_reserve(m->history);
        if (rvst != TP_STATUS_OK) {
            tp_diff_record_free(rec);
            if (out) {
                out->committed = false;
                out->revision = m->revision;
                tp_txn__result_add_error(out, -1, rvst, "", "could not reserve a history slot (out of memory)");
            }
            tp_project_destroy(clone);
            return tp_error_set(err, rvst, "history reserve failed");
        }
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

    /* (b) idempotency: a re-submitted committed id rejects (model unchanged). */
    if (m->idstore && m->idstore->contains && m->idstore->contains(m->idstore->ctx, id_hex)) {
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
