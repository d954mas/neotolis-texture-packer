/*
 * The diff-record lifecycle + a MINIMAL in-memory undo/redo
 * history and the model-facing Undo/Redo. A committed transaction pushes ONE record
 * (its ordered per-op diffs + label/author + produced revision); a NEW transaction
 * applied after an Undo discards the redo branch (cursor semantics).
 *
 * Undo/Redo reuse the clone/swap for STAGE-THEN-COMMIT atomicity: the inverse
 * (or forward) diff is applied to a CLONE and swapped in only on FULL success, so an
 * allocation failure or corrupted diff rolls back with the live model, revision,
 * and cursor byte-unchanged. Recovery transition encode/append failure occurs
 * after publication and makes recovery degraded. A successful Undo/Redo bumps the revision by one (a new committed
 * state); dirty stays identity-derived (an Undo to the saved baseline is clean even
 * at a higher revision).
 *
 * HONEST SCOPE: the undo STACK is in-memory session state and is not restored after
 * restart. When a recovery journal is attached, the DOCUMENT STATE produced by each
 * Undo/Redo is recorded via a compact transition while recovery is healthy.
 * Unsupported/oversized transitions degrade recovery rather than surprising the
 * caller with a full checkpoint; recovery starts with a fresh empty history stack.
 */

#include "tp_core/tp_diff.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_project.h"
#include "tp_core/tp_transaction.h"
#include "tp_diff_internal.h"
#include "tp_history_codec_internal.h"
#include "tp_journal_internal.h"
#include "tp_project_internal.h"
#include "tp_txn_internal.h"

/* ---- diff record + per-op entry lifecycle -------------------------------- */

tp_diff_record *tp_diff_record_new(const char *label, const char *author, int op_cap) {
    tp_diff_record *r = (tp_diff_record *)tp_diff__alloc(sizeof *r);
    if (!r) {
        return NULL;
    }
    bool ok = true;
    r->label = tp_diff__dup(label, &ok);
    if (!ok) {
        free(r);
        return NULL;
    }
    r->author = tp_diff__dup(author, &ok);
    if (!ok) {
        free(r->label);
        free(r);
        return NULL;
    }
    r->ops = NULL;
    r->op_count = 0;
    r->op_cap = 0;
    r->revision = 0;
    if (op_cap > 0) {
        r->ops = (tp_diff_op *)tp_diff__alloc((size_t)op_cap * sizeof *r->ops);
        if (!r->ops) {
            free(r->author);
            free(r->label);
            free(r);
            return NULL;
        }
        r->op_cap = op_cap;
    }
    return r;
}

void tp_diff_op_free(tp_diff_op *e) {
    if (!e) {
        return;
    }
    tp_diff__free_elem(e->coll, e->elem); /* NULL-safe: no-op for non-COLL shapes */
    e->elem = NULL;
    free(e->name_before);
    free(e->name_after);
    free(e->path_before);
    free(e->path_after);
    free(e->exporter_before);
    free(e->out_before);
    free(e->exporter_after);
    free(e->out_after);
    tp_diff__free_sprite_fields(&e->spr_before);
    tp_diff__free_sprite_fields(&e->spr_after);
    tp_diff__free_frames(e->frames_before, e->frames_before_count);
    tp_diff__free_frames(e->frames_after, e->frames_after_count);
    e->name_before = e->name_after = e->path_before = e->path_after = NULL;
    e->exporter_before = e->out_before = e->exporter_after = e->out_after = NULL;
    e->frames_before = e->frames_after = NULL;
}

void tp_diff_record_free(tp_diff_record *r) {
    if (!r) {
        return;
    }
    for (int i = 0; i < r->op_count; i++) {
        tp_diff_op_free(&r->ops[i]);
    }
    free(r->ops);
    free(r->label);
    free(r->author);
    free(r);
}

void tp_diff_record_push_op(tp_diff_record *r, tp_diff_op *e) {
    /* op_cap >= op_count guaranteed by the caller (record_new sized to op_count):
     * a plain move, no allocation. */
    r->ops[r->op_count] = *e; /* transfer ownership of the entry's pointers */
    r->op_count++;
}

/* ---- in-memory undo/redo stack ------------------------------------------- */

tp_history *tp_history_create(void) {
    tp_history *h = (tp_history *)calloc(1, sizeof *h);
    if (!h) {
        return NULL;
    }
    h->records = (tp_diff_record **)calloc((size_t)TP_HISTORY_MAX_STEPS, sizeof *h->records);
    if (!h->records) {
        free(h);
        return NULL;
    }
    h->cap = TP_HISTORY_MAX_STEPS;
    return h;
}

void tp_history_destroy(tp_history *h) {
    if (!h) {
        return;
    }
    for (int i = 0; i < h->count; i++) {
        tp_diff_record_free(h->records[i]);
    }
    free(h->records);
    free(h);
}

/* Test-only fault seam: force the next history admission to report OOM
 * once, then re-arm to off. Lets a test prove the failure happens BEFORE the id is
 * recorded, so the same transaction id stays retryable (never poisoned to DUPLICATE_ID). */
static _Thread_local bool s_reserve_fail = false;
static _Thread_local int s_test_max_steps = 0;
static _Thread_local size_t s_test_max_bytes = 0U;
static _Thread_local size_t s_test_max_record_bytes = 0U;
void tp_history__test_fail_next_reserve(void) { s_reserve_fail = true; }

void tp_history__test_set_limits(int max_steps, size_t max_bytes, size_t max_record_bytes) {
    s_test_max_steps = max_steps;
    s_test_max_bytes = max_bytes;
    s_test_max_record_bytes = max_record_bytes;
}

size_t tp_history_record_byte_limit(void) {
    const size_t max_bytes = s_test_max_bytes > 0U ? s_test_max_bytes : (size_t)TP_HISTORY_MAX_BYTES;
    const size_t max_record = s_test_max_record_bytes > 0U ? s_test_max_record_bytes
                                                           : (size_t)TP_HISTORY_MAX_RECORD_BYTES;
    return max_record < max_bytes ? max_record : max_bytes;
}

tp_status tp_history_prepare_push(const tp_history *h, const tp_diff_record *r, tp_history_push_plan *plan,
                                  tp_error *err) {
    if (!h || !r || !plan) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid history push");
    }
    if (s_reserve_fail) {
        s_reserve_fail = false;
        return TP_STATUS_OOM;
    }

    const int max_steps = s_test_max_steps > 0 ? s_test_max_steps : TP_HISTORY_MAX_STEPS;
    const size_t max_bytes = s_test_max_bytes > 0U ? s_test_max_bytes : (size_t)TP_HISTORY_MAX_BYTES;
    const size_t max_record = s_test_max_record_bytes > 0U ? s_test_max_record_bytes
                                                           : (size_t)TP_HISTORY_MAX_RECORD_BYTES;
    if (max_steps > h->cap) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "history step limit exceeds fixed capacity");
    }
    if (r->bytes == 0U || r->bytes > max_record || r->bytes > max_bytes) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "semantic diff has %zu bytes; record maximum is %zu and history maximum is %zu",
                            r->bytes, max_record, max_bytes);
    }

    size_t kept_bytes = h->bytes;
    for (int i = h->pos; i < h->count; i++) {
        kept_bytes -= h->records[i]->bytes; /* redo branch will be discarded after ACK */
    }
    int kept_count = h->pos;
    int drop = 0;
    while (kept_count >= max_steps || kept_bytes > max_bytes - r->bytes) {
        if (drop >= h->pos) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "semantic diff cannot fit history budget");
        }
        kept_bytes -= h->records[drop]->bytes;
        drop++;
        kept_count--;
    }
    plan->drop_oldest = drop;
    plan->final_bytes = kept_bytes + r->bytes;
    return TP_STATUS_OK;
}

void tp_history_push_prepared(tp_history *h, tp_diff_record *r, const tp_history_push_plan *plan) {
    /* No mutation happened during prepare. Redo and FIFO eviction become visible
     * together at the live commit point. */
    for (int i = h->pos; i < h->count; i++) {
        tp_diff_record_free(h->records[i]);
        h->records[i] = NULL;
    }
    for (int i = 0; i < plan->drop_oldest; i++) {
        tp_diff_record_free(h->records[i]);
        h->records[i] = NULL;
    }
    const int kept = h->pos - plan->drop_oldest;
    if (kept > 0 && plan->drop_oldest > 0) {
        memmove(h->records, h->records + plan->drop_oldest, (size_t)kept * sizeof *h->records);
    }
    for (int i = kept; i < h->count; i++) {
        h->records[i] = NULL;
    }
    h->records[kept] = r;
    h->count = kept + 1;
    h->pos = h->count;
    h->bytes = plan->final_bytes;
}

tp_diff_record *tp_history_undo_record(tp_history *h) { return h->pos > 0 ? h->records[h->pos - 1] : NULL; }
tp_diff_record *tp_history_redo_record(tp_history *h) { return h->pos < h->count ? h->records[h->pos] : NULL; }
void tp_history_commit_undo(tp_history *h) {
    h->pos--;
}
void tp_history_commit_redo(tp_history *h) {
    h->pos++;
}

/* ---- model-facing history API (tp_diff.h) -------------------------------- */

tp_status tp_model_enable_history(tp_model *m) {
    if (!m) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (m->history) {
        return TP_STATUS_OK; /* idempotent */
    }
    m->history = tp_history_create();
    return m->history ? TP_STATUS_OK : TP_STATUS_OOM;
}

bool tp_model_has_history(const tp_model *m) { return m && m->history; }

struct tp_history *tp_model_history(tp_model *m) { return m ? m->history : NULL; }

bool tp_model_can_undo(const tp_model *m) { return m && m->history && m->history->pos > 0; }
bool tp_model_can_redo(const tp_model *m) { return m && m->history && m->history->pos < m->history->count; }
int tp_model_undo_depth(const tp_model *m) { return (m && m->history) ? m->history->pos : 0; }
int tp_model_redo_depth(const tp_model *m) {
    return (m && m->history) ? (m->history->count - m->history->pos) : 0;
}

const char *tp_model_undo_label(const tp_model *m) {
    if (!m || !m->history || m->history->pos <= 0) {
        return NULL;
    }
    return m->history->records[m->history->pos - 1]->label;
}
const char *tp_model_undo_author(const tp_model *m) {
    if (!m || !m->history || m->history->pos <= 0) {
        return NULL;
    }
    return m->history->records[m->history->pos - 1]->author;
}

typedef struct tp_history_durable_transition {
    tp_history_transition_blob blob;
    tp_history_codec_outcome outcome;
} tp_history_durable_transition;

/* Measure and admit the ordinary compact record before building the mutable
 * candidate. The semantic diff already contains both sides of every effect, so
 * this is exact and does not depend on applying Undo/Redo first. */
static tp_status prepare_history_transition(
    tp_model *m, const tp_diff_record *record, bool reverse,
    tp_history_durable_transition *out, tp_error *err) {
    memset(out, 0, sizeof *out);
    out->outcome = TP_HISTORY_CODEC_ERROR;
    if (!m->journal || m->recovery_degraded) return TP_STATUS_OK;
    tp_status status = tp_history_transition_encode(
        record, reverse, m->project,
        (size_t)TP_JOURNAL_MAX_RECORD_BYTES -
            (size_t)TP_JRN_HISTORY_FIXED,
        &out->blob, &out->outcome, err);
    if (status != TP_STATUS_OK) return status;
    if (out->outcome == TP_HISTORY_CODEC_OK) {
        status = tp_journal__check_history_append_bytes(
            m->journal, out->blob.len, out->blob.op_count, err);
        if (status != TP_STATUS_OK) {
            tp_history_transition_blob_free(&out->blob);
        }
        return status;
    }
    if (out->outcome == TP_HISTORY_CODEC_UNSUPPORTED ||
        out->outcome == TP_HISTORY_CODEC_OVERSIZED)
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "history transition is not journal-encodable");
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "history transition encoding failed");
}

static tp_status append_history_transition(
    tp_model *m, const tp_project *candidate, int64_t revision,
    const tp_history_durable_transition *prepared, tp_error *err) {
    (void)candidate;
    if (!m->journal || m->recovery_degraded) return TP_STATUS_OK;
    if (prepared->outcome == TP_HISTORY_CODEC_OK) {
        return tp_journal_append_history_counted(
            m->journal, revision, prepared->blob.data, prepared->blob.len,
            prepared->blob.op_count, err);
    }
    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                        "invalid prepared history transition");
}

tp_status tp_model_undo(tp_model *m, tp_error *err) {
    if (!m || !m->history) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "undo: model has no history");
    }
    tp_diff_record *r = tp_history_undo_record(m->history);
    if (!r) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "nothing to undo");
    }
    int64_t revision = 0;
    tp_status st = tp_model__next_revision(m->revision, &revision, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    tp_history_durable_transition durable;
    memset(&durable, 0, sizeof durable);
    const bool recovery_ready = m->journal && !m->recovery_degraded;
    tp_error recovery_error = {{0}};
    tp_status recovery_status = recovery_ready
                                    ? prepare_history_transition(
                                          m, r, true, &durable,
                                          &recovery_error)
                                    : TP_STATUS_OK;
    tp_project *clone = tp_project_clone(m->project);
    if (!clone) {
        tp_history_transition_blob_free(&durable.blob);
        return tp_error_set(err, TP_STATUS_OOM, "undo: clone failed");
    }
    st = tp_diff_record_apply(clone, r, /*reverse=*/true, err);
    if (st != TP_STATUS_OK) {
        tp_project_destroy(clone); /* rollback: live model byte-unchanged */
        tp_history_transition_blob_free(&durable.blob);
        return st;
    }
    tp_model__replace_owned_project(m, clone);
    m->revision = revision;
    tp_history_commit_undo(m->history);
    if (recovery_ready && recovery_status == TP_STATUS_OK) {
        recovery_status = append_history_transition(
            m, m->project, revision, &durable, &recovery_error);
    }
    tp_history_transition_blob_free(&durable.blob);
    if (recovery_ready && recovery_status != TP_STATUS_OK) {
        tp_model__degrade_recovery(m, recovery_status);
    } else if (recovery_ready) {
        tp_model__mark_recovery_durable(m, revision);
    }
    return TP_STATUS_OK;
}

tp_status tp_model_redo(tp_model *m, tp_error *err) {
    if (!m || !m->history) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "redo: model has no history");
    }
    tp_diff_record *r = tp_history_redo_record(m->history);
    if (!r) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "nothing to redo");
    }
    int64_t revision = 0;
    tp_status st = tp_model__next_revision(m->revision, &revision, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    tp_history_durable_transition durable;
    memset(&durable, 0, sizeof durable);
    const bool recovery_ready = m->journal && !m->recovery_degraded;
    tp_error recovery_error = {{0}};
    tp_status recovery_status = recovery_ready
                                    ? prepare_history_transition(
                                          m, r, false, &durable,
                                          &recovery_error)
                                    : TP_STATUS_OK;
    tp_project *clone = tp_project_clone(m->project);
    if (!clone) {
        tp_history_transition_blob_free(&durable.blob);
        return tp_error_set(err, TP_STATUS_OOM, "redo: clone failed");
    }
    st = tp_diff_record_apply(clone, r, /*reverse=*/false, err);
    if (st != TP_STATUS_OK) {
        tp_project_destroy(clone);
        tp_history_transition_blob_free(&durable.blob);
        return st;
    }
    tp_model__replace_owned_project(m, clone);
    m->revision = revision;
    tp_history_commit_redo(m->history);
    if (recovery_ready && recovery_status == TP_STATUS_OK) {
        recovery_status = append_history_transition(
            m, m->project, revision, &durable, &recovery_error);
    }
    tp_history_transition_blob_free(&durable.blob);
    if (recovery_ready && recovery_status != TP_STATUS_OK) {
        tp_model__degrade_recovery(m, recovery_status);
    } else if (recovery_ready) {
        tp_model__mark_recovery_durable(m, revision);
    }
    return TP_STATUS_OK;
}
