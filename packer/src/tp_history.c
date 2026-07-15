/*
 * F2-03 tasks 2/3/5: the diff-record lifecycle + a MINIMAL in-memory undo/redo
 * history and the model-facing Undo/Redo. A committed transaction pushes ONE record
 * (its ordered per-op diffs + label/author + produced revision); a NEW transaction
 * applied after an Undo discards the redo branch (cursor semantics).
 *
 * Undo/Redo reuse the F2-02 clone/swap for STAGE-THEN-COMMIT atomicity: the inverse
 * (or forward) diff is applied to a CLONE and swapped in only on FULL success, so an
 * allocation failure, corrupted diff, checkpoint serialization fault, or durable
 * journal-append failure rolls back with the live model, revision, and cursor
 * byte-unchanged. A successful Undo/Redo bumps the revision by one (a new committed
 * state); dirty stays identity-derived (an Undo to the saved baseline is clean even
 * at a higher revision).
 *
 * HONEST SCOPE: the undo STACK is in-memory session state and is not restored after
 * restart. When a recovery journal is attached, the DOCUMENT STATE produced by each
 * Undo/Redo is crash-durable via an appended checkpoint; recovery starts with a fresh
 * empty history stack.
 */

#include "tp_core/tp_diff.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_project.h"
#include "tp_core/tp_transaction.h"
#include "tp_diff_internal.h"
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
    return h; /* records=NULL, count=cap=pos=0 */
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

/* Test-only fault seam (fix [3] pin): force the next history-slot grow to report OOM
 * once, then re-arm to off. Lets a test prove a reserve OOM happens BEFORE the id is
 * recorded, so the same transaction id stays retryable (never poisoned to DUPLICATE_ID). */
static bool s_reserve_fail = false;
void tp_history__test_fail_next_reserve(void) { s_reserve_fail = true; }

tp_status tp_history_reserve(tp_history *h) {
    if (h->cap >= h->count + 1) {
        return TP_STATUS_OK;
    }
    if (s_reserve_fail) {
        s_reserve_fail = false;
        return TP_STATUS_OOM;
    }
    int ncap = (h->cap == 0) ? 8 : (h->cap * 2);
    tp_diff_record **n = (tp_diff_record **)tp_diff__alloc((size_t)ncap * sizeof *n);
    if (!n) {
        return TP_STATUS_OOM;
    }
    if (h->records && h->count > 0) {
        memcpy(n, h->records, (size_t)h->count * sizeof *n);
    }
    free(h->records);
    h->records = n;
    h->cap = ncap;
    return TP_STATUS_OK;
}

void tp_history_push_reserved(tp_history *h, tp_diff_record *r) {
    /* discard the redo branch (records above the cursor) */
    for (int i = h->pos; i < h->count; i++) {
        tp_diff_record_free(h->records[i]);
        h->records[i] = NULL;
    }
    h->count = h->pos;
    h->records[h->count] = r; /* cap ensured by tp_history_reserve */
    h->count++;
    h->pos++;
}

tp_diff_record *tp_history_undo_record(tp_history *h) { return h->pos > 0 ? h->records[h->pos - 1] : NULL; }
tp_diff_record *tp_history_redo_record(tp_history *h) { return h->pos < h->count ? h->records[h->pos] : NULL; }
void tp_history_commit_undo(tp_history *h) { h->pos--; }
void tp_history_commit_redo(tp_history *h) { h->pos++; }

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
    tp_project *clone = tp_project_clone(m->project);
    if (!clone) {
        return tp_error_set(err, TP_STATUS_OOM, "undo: clone failed");
    }
    st = tp_diff_record_apply(clone, r, /*reverse=*/true, err);
    if (st != TP_STATUS_OK) {
        tp_project_destroy(clone); /* rollback: live model byte-unchanged */
        return st;
    }
    st = tp_model__append_history_checkpoint(m, clone, revision, err);
    if (st != TP_STATUS_OK) {
        tp_project_destroy(clone); /* durable gate rejected: model + revision + cursor unchanged */
        return st;
    }
    tp_project_destroy(m->project);
    m->project = clone;
    m->revision = revision;
    tp_history_commit_undo(m->history);
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
    tp_project *clone = tp_project_clone(m->project);
    if (!clone) {
        return tp_error_set(err, TP_STATUS_OOM, "redo: clone failed");
    }
    st = tp_diff_record_apply(clone, r, /*reverse=*/false, err);
    if (st != TP_STATUS_OK) {
        tp_project_destroy(clone);
        return st;
    }
    st = tp_model__append_history_checkpoint(m, clone, revision, err);
    if (st != TP_STATUS_OK) {
        tp_project_destroy(clone);
        return st;
    }
    tp_project_destroy(m->project);
    m->project = clone;
    m->revision = revision;
    tp_history_commit_redo(m->history);
    return TP_STATUS_OK;
}
