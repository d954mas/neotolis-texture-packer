#include "tp_core/tp_transaction.h"

#include <inttypes.h>
#include <stdlib.h>

#include "tp_diff_internal.h"
#include "tp_journal_internal.h"
#include "tp_project_generation_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_txn_internal.h"
/* ---- model lifecycle ----------------------------------------------------- */

tp_model *tp_model_wrap(tp_project *project) {
    if (!project) {
        return NULL;
    }
    tp_error canonical_error = {{0}};
    if (tp_project_validate_canonical(project, &canonical_error) !=
        TP_STATUS_OK) {
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

void tp_model_destroy(tp_model *m) {
    if (!m) {
        return;
    }
    tp_history_destroy(m->history); /* NULL-safe when history was never enabled */
    tp_journal_destroy(m->journal); /* NULL-safe when no journal was attached */
    if (m->generation) {
        tp_project_generation_release(m->generation);
    } else {
        tp_project_destroy(m->project);
    }
    if (m->idstore) {
        if (m->owns_idstore && m->idstore->destroy) {
            m->idstore->destroy(m->idstore->ctx);
        }
        free(m->idstore);
    }
    free(m);
}

const tp_project *tp_model_project(const tp_model *m) {
    return m ? m->project : NULL;
}
tp_journal *tp_model_journal(tp_model *m) { return m ? m->journal : NULL; }
int64_t tp_model_revision(const tp_model *m) { return m ? m->revision : 0; }

void tp_model__test_set_revision(tp_model *m, int64_t revision) {
    if (m) {
        m->revision = revision;
    }
}

void tp_model__replace_owned_project(tp_model *model, tp_project *project) {
    if (!model || !project || model->project == project) {
        return;
    }
    tp_project *old = model->project;
    tp_project_generation *old_generation = model->generation;
    model->project = project;
    model->generation = NULL;
    if (old_generation) {
        tp_project_generation_release(old_generation);
    } else {
        tp_project_destroy(old);
    }
}

void tp_model__adopt_project(tp_model *model, tp_project *project) {
    tp_model__replace_owned_project(model, project);
}

tp_status tp_model__retain_project_generation(
    tp_model *model, tp_project_generation **out, tp_error *error) {
    if (!model || !out) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "model generation requires model and output");
    }
    *out = NULL;
    if (!model->generation) {
        tp_project_generation *generation =
            tp_project_generation_create_owned(model->project);
        if (!generation) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "project generation allocation failed");
        }
        model->generation = generation;
    }
    tp_project_generation_retain(model->generation);
    *out = model->generation;
    return TP_STATUS_OK;
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
