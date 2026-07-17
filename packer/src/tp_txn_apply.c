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
#include "tp_core/tp_project_migrate.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "tp_diff_internal.h" /* F2-03: optional per-op diff capture on commit */
#include "tp_encode_internal.h"
#include "tp_idset_internal.h" /* F2-04 fix C1: migrate retained ids into the journal on attach */
#include "tp_journal_internal.h" /* F2-04 fix C3: poison the journal from the recovery glue */
#include "tp_op_internal.h"
#include "tp_project_internal.h"
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
tp_journal *tp_model_journal(tp_model *m) { return m ? m->journal : NULL; }
int64_t tp_model_revision(const tp_model *m) { return m ? m->revision : 0; }

void tp_model__test_set_revision(tp_model *m, int64_t revision) {
    if (m) {
        m->revision = revision;
    }
}

void tp_model__adopt_project(tp_model *model, tp_project *project) {
    if (!model || !project || model->project == project) {
        return;
    }
    tp_project *old = model->project;
    model->project = project;
    tp_project_destroy(old);
}

tp_status tp_model__migrate_sprite_refs(tp_model *model, tp_error *error) {
    if (!model) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "model is required for reference migration");
    }
    return tp_project_migrate_sprite_refs(model->project, error);
}

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
    size_t id_len = 0U;
    if (id_hex) {
        while (id_len < sizeof out->transaction_id - 1U && id_hex[id_len]) {
            id_len++;
        }
        memcpy(out->transaction_id, id_hex, id_len);
    }
    out->transaction_id[id_len] = '\0';
    out->committed = false;
    out->no_change = false;
    out->revision = 0;
    out->ops = NULL;
    out->op_count = 0;
    out->errors = NULL;
    out->error_count = 0;
}

/* Test-only fault seam for the add_error grow: -1 = off; N = the (N+1)th call's
 * grow fails once (then re-arms to off). Lets a test prove a dropped shape-error
 * record still forces a reject. */
static _Thread_local int s_add_error_fail = -1;
static _Thread_local size_t s_test_op_walk_steps = 0U;
static _Thread_local size_t s_test_error_allocations = 0U;
void tp_txn__test_set_add_error_fail(int nth) { s_add_error_fail = nth; }
void tp_txn__test_complexity_reset(void) {
    s_test_op_walk_steps = 0U;
    s_test_error_allocations = 0U;
}
size_t tp_txn__test_op_walk_steps(void) { return s_test_op_walk_steps; }
size_t tp_txn__test_error_allocations(void) { return s_test_error_allocations; }
void tp_txn__test_count_op_walk(size_t steps) { s_test_op_walk_steps += steps; }

/* Append one error (grows the dynamic error array). Returns true when stored; false
 * when the grow failed (OOM). A collect-all caller must treat a false as a forced
 * reject -- a shape-faulted batch must never falsely commit just because its error
 * record could not be stored. */
static bool result_error_store_allowed(void) {
    if (s_add_error_fail >= 0) { /* fault seam: fail this grow once */
        if (s_add_error_fail == 0) {
            s_add_error_fail = -1;
            return false;
        }
        s_add_error_fail--;
    }
    return true;
}

static void result_error_fill(tp_txn_error *e, int op_index, tp_status code,
                              const char *field, const char *msg) {
    e->op_index = op_index;
    e->code = code;
    (void)snprintf(e->field, sizeof e->field, "%s", field ? field : "");
    (void)snprintf(e->message, sizeof e->message, "%s", msg ? msg : "");
}

bool tp_txn__result_add_error(tp_txn_result *out, int op_index, tp_status code, const char *field, const char *msg) {
    if (!out || !result_error_store_allowed()) {
        return false;
    }
    tp_txn_error *n = (tp_txn_error *)realloc(out->errors, (size_t)(out->error_count + 1) * sizeof *n);
    if (!n) {
        return false;
    }
    s_test_error_allocations++;
    out->errors = n;
    tp_txn_error *e = &out->errors[out->error_count];
    result_error_fill(e, op_index, code, field, msg);
    out->error_count++;
    return true;
}

bool tp_txn__result_reserve_errors(tp_txn_result *out, int count) {
    if (!out || count < 0 || out->errors || out->error_count != 0) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if ((size_t)count > SIZE_MAX / sizeof *out->errors) {
        return false;
    }
    out->errors = (tp_txn_error *)malloc((size_t)count * sizeof *out->errors);
    if (!out->errors) {
        return false;
    }
    s_test_error_allocations++;
    return true;
}

bool tp_txn__result_add_error_reserved(tp_txn_result *out, int capacity, int op_index,
                                       tp_status code, const char *field, const char *msg) {
    if (!out || capacity < 0 || out->error_count >= capacity ||
        !result_error_store_allowed()) {
        return false;
    }
    result_error_fill(&out->errors[out->error_count], op_index, code, field, msg);
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
                                  char *payload, int64_t revision, int op_index, tp_status code, const char *field,
                                  const char *msg) {
    tp_diff_op_free(entry);   /* NULL-safe: frees a partially-captured before/after entry */
    tp_diff_record_free(rec); /* NULL-safe */
    if (out) {
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
    if (m->journal) {
        tp_status replay_st = tp_journal__check_append_admission(
            m->journal, (size_t)req->op_count, err);
        if (replay_st != TP_STATUS_OK) {
            tp_txn__commit_reject(out, NULL, NULL, NULL, NULL, m->revision, -1,
                                  replay_st, "operations",
                                  "journal replay operation budget is exhausted; save/compact is required");
            return replay_st;
        }
        /* Even the smallest TXN frame cannot fit: reject before canonical
         * encoding allocates. The exact request length is checked below. */
        tp_status byte_st = tp_journal__check_txn_min_bytes(m->journal, err);
        if (byte_st != TP_STATUS_OK) {
            tp_txn__commit_reject(out, NULL, NULL, NULL, NULL, m->revision, -1,
                                  byte_st, "request",
                                  "journal byte budget is exhausted; save/compact is required");
            return byte_st;
        }
    }
    /* Canonical journal payload admission is shared by typed and JSON callers. */
    bool encodable = true;
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
        if (m->journal) {
            tp_status byte_st = tp_journal__check_txn_bytes(
                m->journal, payload_len, err);
            if (byte_st != TP_STATUS_OK) {
                tp_txn__commit_reject(
                    out, NULL, NULL, NULL, NULL, m->revision, -1, byte_st,
                    "request",
                    "journal byte budget is exhausted; save/compact is required");
                return byte_st;
            }
        }
    }
    bool payload_too_large = false;
    char *payload = encodable
                        ? tp_txn_request_encode_bounded_for_project(
                              req, m->project,
                              (size_t)TP_TXN_MAX_REQUEST_BYTES,
                              &payload_too_large)
                        : NULL;
    if (payload_too_large) {
        tp_txn__commit_reject(out, NULL, NULL, NULL, NULL, m->revision, -1, TP_STATUS_OUT_OF_BOUNDS,
                              "request", "canonical transaction request exceeds the byte limit");
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "canonical transaction request exceeds the %u-byte maximum",
                            (unsigned)TP_TXN_MAX_REQUEST_BYTES);
    }
    if (encodable && !payload) {
        tp_txn__commit_reject(out, NULL, NULL, NULL, NULL, m->revision, -1, TP_STATUS_OOM, "request",
                              "could not serialize the transaction request (out of memory)");
        return tp_error_set(err, TP_STATUS_OOM, "transaction request serialization failed");
    }
    payload_len = payload ? strlen(payload) : 0U;
    if (payload && payload_len > (size_t)TP_TXN_MAX_REQUEST_BYTES) {
        tp_txn__commit_reject(out, NULL, NULL, NULL, payload, m->revision, -1, TP_STATUS_OUT_OF_BOUNDS,
                              "request", "canonical transaction request exceeds the byte limit");
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "canonical transaction request has %zu bytes; maximum is %u", payload_len,
                            (unsigned)TP_TXN_MAX_REQUEST_BYTES);
    }
    tp_project *clone = tp_project_clone(m->project);
    if (!clone) {
        tp_txn__commit_reject(out, NULL, NULL, NULL, payload, m->revision, -1, TP_STATUS_OOM, "",
                              "could not clone the model (out of memory)");
        return tp_error_set(err, TP_STATUS_OOM, "transaction clone failed");
    }

    /* F2-03: when a history is attached, capture the per-op semantic diff as each op
     * applies to the clone. The whole diff is built PRE-swap, so a capture allocation
     * failure fails the commit cleanly (clone discarded, live model byte-unchanged) --
     * F2-02 atomicity is preserved. A NULL history => exactly the F2-02 code path. */
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
        tp_op_reject rej;
        tp_status st = tp_operation_apply(clone, &req->ops[i], &rej);
        if (st != TP_STATUS_OK) {
            if (rec) {
                (void)tp_diff__record_budget_end(NULL);
            }
            tp_txn__commit_reject(out, clone, rec, rec ? &entry : NULL, payload, m->revision, i, st, rej.field,
                                  rej.message);
            return tp_error_set(err, st, "operation %d rejected: %s", i, rej.message);
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
            tp_txn__commit_reject(out, clone, rec, NULL, payload, m->revision, -1, ps, "",
                                  "side-effect prepare failed");
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
        gate = tp_journal_append_txn_counted(
            m->journal, req->id_hex, next_revision, (const uint8_t *)payload,
            payload_len, (size_t)req->op_count, err);
        gate_msg = "recovery journal append failed (transaction rolled back)";
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
        tp_txn__commit_reject(out, clone, rec, NULL, payload, m->revision, -1, gate, "", gate_msg);
        return gate;
    }
    free(payload);

    /* The commit: allocation-free pointer swap + one revision bump. */
    tp_project_destroy(m->project);
    m->project = clone;
    m->revision = next_revision;

    /* Push the diff (allocation-free): records the produced revision + discards any
     * redo branch (a new transaction after an Undo drops the redo steps). */
    if (rec) {
        rec->revision = m->revision;
        tp_history_push_prepared(m->history, rec, &history_plan);
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

/* R2b/R3: serialize a candidate project into a fresh malloc'd snapshot
 * (*snap / *snap_len, caller frees) and PROVE it round-trips through tp_project_load_buffer
 * before it is handed to a checkpoint. The checkpoint is the load-bearing recovery BASE --
 * format B replays post-checkpoint ops ONTO it (see tp_model_recover) -- so a base that
 * serializes but does NOT reload (e.g. nil structural ids) would silently discard the whole
 * recovered session. Enforcing "the checkpoint must round-trip" HERE in core, below any
 * client's own promotion (e.g. the GUI's ensure_ids), means no caller can persist an
 * unrecoverable base. Shared by tp_model_attach_journal (initial checkpoint) and
 * tp_model_compact_journal (Save-window compaction), and journal-gated Undo/Redo so all
 * checkpoint paths inherit the guarantee. On any
 * failure *snap is NULL/0 and a wrapped error is returned; the caller leaves the journal
 * untouched. */
static tp_status project_checkpoint_snapshot(const tp_project *project, size_t expected_len,
                                             char **snap, size_t *snap_len, tp_error *err) {
    *snap = NULL;
    *snap_len = 0;
    tp_status ss = tp_project_checkpoint_save_buffer(project, snap, snap_len,
                                                     err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    if (*snap_len != expected_len) {
        free(*snap);
        *snap = NULL;
        *snap_len = 0;
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "project changed while its checkpoint was serialized");
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
            char id_hex[TP_IDSET_IDLEN + 1];
            if (!tp_idset_format_at(pre, i, id_hex)) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                    "in-memory retained-id ordering is invalid");
            }
            tp_status ms = tp_journal_seed_retained_id(j, id_hex);
            if (ms != TP_STATUS_OK) {
                return tp_error_set(err, ms, "could not migrate retained ids into the journal (out of memory)");
            }
        }
    }
    /* Initial CHECKPOINT of the current committed state so the journal is self-sufficient for
     * recovery (spec §22.3 checkpoint + journal), via the shared round-trip-proving helper. On
     * failure the model is NOT attached and the caller still owns j. */
    size_t measured_len = 0;
    tp_status ss = tp_project_checkpoint_serialized_size_bounded(
        m->project, SIZE_MAX, &measured_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    ss = tp_journal__check_checkpoint_append_bytes(j, measured_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    char *snap = NULL;
    size_t snap_len = 0;
    ss = project_checkpoint_snapshot(m->project, measured_len,
                                     &snap, &snap_len, err);
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

tp_status tp_model__append_history_checkpoint(tp_model *m, const tp_project *candidate, int64_t revision,
                                              size_t snapshot_bytes, tp_error *err) {
    if (!m || !candidate) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model or history candidate");
    }
    if (!m->journal) {
        return TP_STATUS_OK;
    }
    tp_status admission = tp_journal__check_checkpoint_append_bytes(
        m->journal, snapshot_bytes, err);
    if (admission != TP_STATUS_OK) {
        return admission;
    }

    /* Undo/Redo has no uniformly encodable forward operation (notably inverse remove),
     * so its durable record is a full checkpoint. It is APPENDED, never compacted:
     * the existing clean checkpoint remains intact until this candidate is durable,
     * and record_count advances past the startup scan's unsaved-work threshold. */
    char *snap = NULL;
    size_t snap_len = 0;
    tp_status ss = project_checkpoint_snapshot(candidate, snapshot_bytes,
                                               &snap, &snap_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    tp_status cs = tp_journal_init_checkpoint(m->journal, (const uint8_t *)snap, snap_len, revision, err);
    free(snap);
    return cs;
}

tp_status tp_model__next_revision(int64_t current, int64_t *next, tp_error *err) {
    if (!next) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null next revision output");
    }
    if (current < 0 || current == INT64_MAX) {
        return tp_error_set(err, TP_STATUS_INVALID_REVISION,
                            "model revision cannot advance from %lld", (long long)current);
    }
    *next = current + 1;
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
    size_t measured_len = 0;
    tp_status ss = tp_project_checkpoint_serialized_size_bounded(
        m->project, SIZE_MAX, &measured_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    ss = tp_journal__check_checkpoint_compact_bytes(
        m->journal, measured_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    char *snap = NULL;
    size_t snap_len = 0;
    ss = project_checkpoint_snapshot(m->project, measured_len,
                                     &snap, &snap_len, err);
    if (ss != TP_STATUS_OK) {
        return ss; /* snapshot/probe failed BEFORE any truncate: journal untouched, keeps recovering */
    }
    tp_status cs = tp_journal_compact(m->journal, (const uint8_t *)snap, snap_len, m->revision, err);
    free(snap);
    /* Never auto-detach on a poisoned replacement failure. The journal remains the
     * model's idempotency/durability authority and subsequent mutations fail closed
     * with TP_STATUS_JOURNAL_FAILED until an owner explicitly repairs or replaces it. */
    return cs;
}

tp_status tp_model_set_recovery_metadata(tp_model *m, int64_t timestamp, const char *path, const char *name,
                                         tp_error *err) {
    return tp_model_set_recovery_metadata_ex(m, timestamp, path, name, NULL, err);
}

tp_status tp_model_set_recovery_metadata_ex(tp_model *m, int64_t timestamp, const char *path, const char *name,
                                            const tp_id128 *file_fingerprint, tp_error *err) {
    if (!m) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model");
    }
    if (!m->journal) {
        return TP_STATUS_OK; /* no journal attached (recovery off / journal-less): nothing to record */
    }
    /* Forward the canonical project identity used by recovery Save Original. Same null-journal-tolerant
     * contract as tp_model_compact_journal; NULL path/name are normalized to "". Every durable write
     * failure is returned so the host can detach/remove stale recovery authority. */
    return tp_journal_set_metadata_ex(m->journal, timestamp, path ? path : "", name ? name : "",
                                      file_fingerprint, err);
}

bool tp_model_has_journal(const tp_model *m) {
    /* R5b-2 fix [0]: m->journal is set ONLY after tp_journal_init_checkpoint durably wrote (see
     * tp_model_attach_journal), so a non-NULL journal means the current committed state is durably
     * backed. The GUI adopt-delete guard keys off this: never delete the adopted source when this is
     * false (journal-less attach), because the source is then the sole durable copy. */
    return m && m->journal != NULL;
}

void tp_model_detach_journal(tp_model *m) {
    if (!m || !m->journal) {
        return;
    }
    tp_journal_destroy(m->journal);
    m->journal = NULL;
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
    int *operation_counts = rec.op_count > 0U
                                ? (int *)calloc(rec.op_count, sizeof *operation_counts)
                                : NULL;
    if (rec.op_count > 0U && !operation_counts) {
        if (info) {
            *info = rec;
        } else {
            tp_journal_recovery_free(&rec);
        }
        tp_journal_destroy(j);
        return tp_error_set(err, TP_STATUS_OOM,
                            "journal replay count index allocation failed");
    }
    size_t replay_operations = 0U;
    for (size_t k = 0U; k < rec.op_count; ++k) {
        int operation_count = 0;
        tp_status count_st = tp_txn__count_operations_json_n(
            rec.ops[k].payload, rec.ops[k].payload_len, &operation_count, err);
        if (count_st != TP_STATUS_OK || operation_count < 0 ||
            (size_t)operation_count >
                (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS - replay_operations) {
            const tp_status reject_st = count_st != TP_STATUS_OK
                                            ? count_st
                                            : TP_STATUS_OUT_OF_BOUNDS;
            if (count_st == TP_STATUS_OK) {
                (void)tp_error_set(err, reject_st,
                                   "journal replay operation limit exceeded before materialization");
            }
            if (info) {
                *info = rec;
            } else {
                tp_journal_recovery_free(&rec);
            }
            free(operation_counts);
            tp_journal_destroy(j);
            return reject_st;
        }
        replay_operations += (size_t)operation_count;
        operation_counts[k] = operation_count;
    }
    tp_status seed_st = tp_journal__set_replay_operations(j, replay_operations, err);
    if (seed_st != TP_STATUS_OK) {
        if (info) {
            *info = rec;
        } else {
            tp_journal_recovery_free(&rec);
        }
        free(operation_counts);
        tp_journal_destroy(j);
        return seed_st;
    }
    bool keep_info = (info != NULL);
    if (keep_info) {
        *info = rec; /* transfer the snapshot ownership to the caller */
    }

    tp_status ret = TP_STATUS_OK;
    size_t applied_operations = 0U;
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
                tp_status ds = tp_txn__decode_prechecked_json_n(
                    rec.ops[k].payload, rec.ops[k].payload_len,
                    operation_counts[k], &req, err);
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
                    applied_operations++;
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
    free(operation_counts);
    tp_op__test_apply_count_publish(applied_operations);
    return ret;
}
