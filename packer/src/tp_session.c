#include "tp_core/tp_session.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_diff.h"
#include "tp_core/tp_project_lease.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_core/tp_srckey.h"
#include "tp_core/tp_transaction.h"
#include "tp_recovery_live_seam.h"
#include "tp_session_layout.h"
#include "tp_job_owner_internal.h"
#include "tp_model_seam.h"
#include "tp_project_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_mutation_internal.h"

// #region gate & recovery health
bool recovery_is_healthy(const tp_session *session) {
    if (tp_model__recovery_degraded(session->model)) {
        return false;
    }
    if (session->recovery_live) {
        return tp_recovery_live_healthy(session->recovery_live);
    }
    if (tp_model_has_journal(session->model)) {
        return session->recovery_healthy;
    }
    return !session->recovery_required && session->recovery_healthy;
}

static void observe_model_recovery(tp_session *session) {
    if (!tp_model__recovery_degraded(session->model)) {
        return;
    }
    session->recovery_healthy = false;
}

bool tp_session__owns_recovery_live(const tp_session *session,
                                    const tp_recovery_live *live) {
    if (!session || !live) {
        return false;
    }
    gate_lock(session);
    const bool owns = session->recovery_live == live;
    gate_unlock(session);
    return owns;
}

bool tp_session__has_recovery_owner(const tp_session *session) {
    if (!session) {
        return false;
    }
    gate_lock(session);
    const bool has_owner = session->recovery_live != NULL ||
                           tp_model_has_journal(session->model);
    gate_unlock(session);
    return has_owner;
}

const char *tp_session__recovery_journal_path(const tp_session *session) {
    return session && session->recovery_live
               ? tp_recovery_live_journal_path(session->recovery_live)
               : NULL;
}

static bool gate_init(tp_session *session) {
#if defined(_WIN32)
    InitializeSRWLock(&session->gate);
    return true;
#else
    return pthread_mutex_init(&session->gate, NULL) == 0;
#endif
}

static void gate_destroy(tp_session *session) {
#if defined(_WIN32)
    (void)session;
#else
    (void)pthread_mutex_destroy(&session->gate);
#endif
}

void gate_lock(const tp_session *session) {
#if defined(_WIN32)
    AcquireSRWLockExclusive((SRWLOCK *)&session->gate);
#else
    (void)pthread_mutex_lock((pthread_mutex_t *)&session->gate);
#endif
}

void gate_unlock(const tp_session *session) {
#if defined(_WIN32)
    ReleaseSRWLockExclusive((SRWLOCK *)&session->gate);
#else
    (void)pthread_mutex_unlock((pthread_mutex_t *)&session->gate);
#endif
}
// #endregion

// #region event log
static void publish_event(tp_session *session, tp_session_event_kind kind,
                          const char *transaction_id, int64_t revision_before,
                          int64_t revision_after) {
    const uint64_t sequence = ++session->event_sequence;
    size_t slot;
    if (session->event_count < TP_SESSION_EVENT_CAPACITY) {
        slot = (session->event_start + session->event_count) % TP_SESSION_EVENT_CAPACITY;
        session->event_count++;
    } else {
        slot = session->event_start;
        session->event_start = (session->event_start + 1U) % TP_SESSION_EVENT_CAPACITY;
    }
    tp_session_event *event = &session->events[slot];
    memset(event, 0, sizeof *event);
    event->sequence = sequence;
    event->kind = kind;
    event->revision_before = revision_before;
    event->revision_after = revision_after;
    event->admission_sequence = session->admission_sequence;
    event->model_generation = session->model_generation;
    event->source_generation = session->source_generation;
    if (transaction_id) {
        (void)snprintf(event->transaction_id, sizeof event->transaction_id, "%s", transaction_id);
    }
}
// #endregion

// #region lifetime
static tp_status session_adopt_owned(tp_project *project, const tp_rng *rng,
                                     tp_session **out, tp_error *err) {
    if (!project || !rng || !rng->fill || !out) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session adopt requires project, rng, and output");
    }
    *out = NULL;

    tp_status status = tp_project_assign_missing_ids(project, rng, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        return status;
    }
    status = tp_project_validate_canonical(project, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        return status;
    }

    tp_session *session = (tp_session *)calloc(1, sizeof *session);
    if (!session) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_OOM, "session allocation failed");
    }
    if (!gate_init(session)) {
        tp_project_destroy(project);
        free(session);
        return tp_error_set(err, TP_STATUS_OOM, "session serialization gate allocation failed");
    }
    status = tp_session_identity_init_unsaved(&session->identity, rng, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        gate_destroy(session);
        free(session);
        return status;
    }
    session->model = tp_model_wrap(project);
    if (!session->model) {
        tp_project_destroy(project);
        gate_destroy(session);
        free(session);
        return tp_error_set(err, TP_STATUS_OOM, "session model allocation failed");
    }
    status = tp_model_enable_history(session->model);
    if (status != TP_STATUS_OK) {
        tp_model_destroy(session->model);
        gate_destroy(session);
        free(session);
        return tp_error_set(err, status, "session history allocation failed");
    }
    session->recovery_healthy = true;
    *out = session;
    return TP_STATUS_OK;
}

tp_status tp_session_adopt_owned(tp_project *project, const tp_rng *rng,
                                 tp_session **out, tp_error *err) {
    return session_adopt_owned(project, rng, out, err);
}

tp_status tp_session_create(const tp_rng *rng, tp_session **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session output is required");
    }
    *out = NULL;
    tp_project *project = tp_project_create();
    if (!project) {
        return tp_error_set(err, TP_STATUS_OOM, "project allocation failed");
    }
    return tp_session_adopt_owned(project, rng, out, err);
}

tp_status tp_session_create_default_project(const tp_rng *rng,
                                            tp_session **out,
                                            tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session output is required");
    }
    *out = NULL;
    tp_project *project = tp_project_create();
    if (!project) {
        return tp_error_set(err, TP_STATUS_OOM, "project allocation failed");
    }
    if (tp_project_atlas_seed_default_target(project, 0) != TP_STATUS_OK) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_OOM,
                            "default project target allocation failed");
    }
    return tp_session_adopt_owned(project, rng, out, err);
}

tp_status tp_session_create_detached_recovery(tp_project *project,
                                              const tp_rng *rng,
                                              tp_id128 recovery_token,
                                              tp_session **out,
                                              tp_error *err) {
    tp_status status = tp_session_adopt_owned(project, rng, out, err);
    if (status == TP_STATUS_OK) {
        (*out)->recovery_token = recovery_token;
        (*out)->has_recovery_token = true;
    }
    return status;
}

tp_status tp_session_open(const char *path, const tp_rng *rng,
                          tp_session **out, tp_error *err) {
    if (!path || !rng || !rng->fill || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session open requires path, rng, and output");
    }
    *out = NULL;
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_project_path_canonical(
        path, canonical, sizeof canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_project_lease *lease = NULL;
    status = tp_project_lease_acquire(canonical, &lease, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_id128 before;
    status = tp_identity_file_fingerprint(canonical, &before, err);
    if (status != TP_STATUS_OK) {
        tp_project_lease_release(lease);
        return status;
    }
    tp_project *project = NULL;
    tp_id128 loaded;
    status = tp_project_load_with_fingerprint(canonical, &project, &loaded, err);
    if (status != TP_STATUS_OK) {
        tp_project_lease_release(lease);
        return status;
    }
    if (!tp_id128_eq(before, loaded)) {
        tp_project_destroy(project);
        tp_project_lease_release(lease);
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "project changed while it was opened");
    }
    tp_session *session = NULL;
    status = session_adopt_owned(project, rng, &session, err);
    if (status != TP_STATUS_OK) {
        tp_project_lease_release(lease);
        return status;
    }
    tp_model_mark_saved(session->model);
    tp_session_identity identity = session->identity;
    status = tp_session_identity_transition_to_path(&identity, canonical, err);
    if (status != TP_STATUS_OK) {
        tp_session_destroy(session);
        tp_project_lease_release(lease);
        return status;
    }
    session->identity = identity;
    session->project_lease = lease;
    session->saved_file_fingerprint = loaded;
    session->has_saved_file_fingerprint = true;
    *out = session;
    return TP_STATUS_OK;
}

void tp_session_destroy(tp_session *session) {
    if (!session) {
        return;
    }
    if (session->active_job) {
        tp_session_owned_job *job = session->active_job;
        session->active_job = NULL;
        job->cancel(job);
        tp_session_job_release_internal(job);
    }
    if (session->recovery_live) {
        const bool preserve = tp_model__recovery_degraded(session->model) ||
                              !tp_recovery_live_healthy(session->recovery_live) ||
                              (tp_model_dirty(session->model) && !session->discarded);
        (void)tp_recovery_live_finish(session->recovery_live, preserve, NULL);
        tp_recovery_live_destroy(session->recovery_live);
    }
    tp_model_destroy(session->model);
    tp_project_lease_release(session->project_lease);
    gate_destroy(session);
    free(session);
}
// #endregion

// #region jobs
void tp_session_owned_job_init(tp_session_owned_job *job,
                               void (*cancel)(tp_session_owned_job *job),
                               void (*destroy)(tp_session_owned_job *job)) {
    if (!job) {
        return;
    }
    atomic_init(&job->refs, 1U);
    job->cancel = cancel;
    job->destroy = destroy;
}

void tp_session_job_release_internal(tp_session_owned_job *job) {
    if (!job) {
        return;
    }
    if (atomic_fetch_sub_explicit(&job->refs, 1U, memory_order_acq_rel) ==
        1U) {
        job->destroy(job);
    }
}

tp_status tp_session_job_attach_internal(tp_session *session,
                                         tp_session_owned_job *job,
                                         tp_error *err) {
    if (!session || !job || !job->cancel || !job->destroy) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session job attach requires a concrete job handle");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session was discarded");
    }
    if (session->active_job) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "a Pack or Export job is already active");
    }
    session->active_job = job;
    gate_unlock(session);
    return TP_STATUS_OK;
}

tp_session_owned_job *tp_session_job_acquire_internal(
    const tp_session *session) {
    if (!session) {
        return NULL;
    }
    gate_lock(session);
    tp_session_owned_job *job = session->active_job;
    if (job) {
        (void)atomic_fetch_add_explicit(&job->refs, 1U,
                                        memory_order_relaxed);
    }
    gate_unlock(session);
    return job;
}

tp_status tp_session_job_detach_internal(tp_session *session,
                                         tp_session_owned_job *expected,
                                         tp_error *err) {
    if (!session || !expected) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session job detach requires session and handle");
    }
    gate_lock(session);
    if (session->active_job != expected) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "session no longer owns that job handle");
    }
    session->active_job = NULL;
    gate_unlock(session);
    return TP_STATUS_OK;
}
// #endregion

// #region recovery integration
tp_status tp_session_attach_journal(tp_session *session, tp_journal *journal,
                                    tp_error *err) {
    if (!session || !journal) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "journal attach requires session and journal");
    }
    gate_lock(session);
    tp_status status = tp_model_attach_journal(session->model, journal, err);
    if (status == TP_STATUS_OK) {
        session->recovery_healthy = true;
    }
    gate_unlock(session);
    return status;
}

tp_status tp_session_attach_recovery_live(tp_session *session,
                                          tp_recovery_live *live,
                                          const tp_recovery_metadata *metadata,
                                          tp_error *err) {
    if (!session || !live || !metadata) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live attach requires session, live handle, and metadata");
    }
    gate_lock(session);
    if (session->recovery_live || tp_model_has_journal(session->model)) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session already has recovery attached");
    }
    session->recovery_live = live;
    tp_status status = tp_recovery_live_attach(live, session->model, metadata, err);
    session->recovery_healthy = status == TP_STATUS_OK;
    gate_unlock(session);
    return status;
}

tp_status tp_session_require_recovery(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery requirement needs a session");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session was discarded");
    }
    session->recovery_required = true;
    gate_unlock(session);
    return TP_STATUS_OK;
}
// #endregion

// #region admission & transactions
tp_status tp_session_apply(tp_session *session, const tp_txn_request *request,
                           tp_txn_result *result, tp_error *err) {
    if (!session || !request) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session apply requires session and request");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    tp_txn_result local_result;
    tp_txn_result *published_result = result ? result : &local_result;
    tp_status status = tp_model_apply(session->model, request,
                                      published_result, err);
    observe_model_recovery(session);
    if (status == TP_STATUS_OK && published_result->committed) {
        session->model_generation++;
        publish_event(session, TP_SESSION_EVENT_MODEL_COMMITTED, request->id_hex,
                      revision_before, tp_model_revision(session->model));
    }
    if (!result) {
        tp_txn_result_free(&local_result);
    }
    gate_unlock(session);
    return status;
}

tp_status tp_session_undo(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "undo requires session");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    tp_status status = tp_model_undo(session->model, err);
    observe_model_recovery(session);
    if (status == TP_STATUS_OK) {
        session->model_generation++;
        publish_event(session, TP_SESSION_EVENT_UNDONE, NULL, revision_before,
                      tp_model_revision(session->model));
    }
    gate_unlock(session);
    return status;
}

tp_status tp_session_redo(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "redo requires session");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    tp_status status = tp_model_redo(session->model, err);
    observe_model_recovery(session);
    if (status == TP_STATUS_OK) {
        session->model_generation++;
        publish_event(session, TP_SESSION_EVENT_REDONE, NULL, revision_before,
                      tp_model_revision(session->model));
    }
    gate_unlock(session);
    return status;
}
// #endregion

// #region save & identity
static void remap_save_error_path(tp_status status, const char *public_path,
                                  tp_error *err) {
    if (status == TP_STATUS_FILE_IO_FAILED && err) {
        err->file_io.path = public_path;
    }
}

static tp_status save_as_locked(tp_session *session, const char *path,
                                bool create_only,
                                tp_session_save_result *result, tp_error *err) {
    if (result) {
        memset(result, 0, sizeof *result);
        result->file_durability_status = TP_STATUS_OK;
        result->recovery_status = TP_STATUS_OK;
    }
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_project_path_canonical(
        path, canonical, sizeof canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_session_identity next_identity = session->identity;
    status = tp_session_identity_transition_to_path(&next_identity, canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const bool same_identity = session->identity.kind == TP_IDENTITY_SAVED &&
                               tp_identity_path_equal(session->identity.canonical_path,
                                                      next_identity.canonical_path);
    if (same_identity && !session->has_saved_file_fingerprint) {
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "saved-file fingerprint is unavailable; use Save As");
    }
    tp_project_lease *destination_lease = session->project_lease;
    if (!same_identity || !destination_lease) {
        status = tp_project_lease_acquire(canonical, &destination_lease, err);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    tp_project *candidate = tp_project_clone(tp_model_project(session->model));
    if (!candidate) {
        if (destination_lease != session->project_lease) {
            tp_project_lease_release(destination_lease);
        }
        return tp_error_set(err, TP_STATUS_OOM,
                            "save candidate clone failed");
    }
    tp_id128 fingerprint;
    const tp_id128 *expected_fingerprint =
        same_identity ? &session->saved_file_fingerprint : NULL;
    status = tp_project_save_candidate_with_fingerprint(
        candidate, canonical, expected_fingerprint, create_only,
        &fingerprint, err);
    remap_save_error_path(status, path, err);
    const bool file_durability_degraded =
        status == TP_STATUS_FILE_DURABILITY_UNCERTAIN;
    const tp_status file_durability_status =
        file_durability_degraded ? status : TP_STATUS_OK;
    if (status != TP_STATUS_OK && !file_durability_degraded) {
        tp_project_destroy(candidate);
        if (destination_lease != session->project_lease) {
            tp_project_lease_release(destination_lease);
        }
        return status;
    }
    /* Publication already completed for the degraded outcome. From here the
     * candidate, lease, identity, fingerprint, dirty anchor, and Saved event
     * must advance exactly as on the fully durable path. */
    status = TP_STATUS_OK;
    if (file_durability_degraded && err) {
        err->msg[0] = '\0';
    }
    const bool model_was_degraded =
        tp_model__recovery_degraded(session->model);
    bool recovery_degraded = !recovery_is_healthy(session);
    tp_status recovery_status = model_was_degraded
                                    ? tp_model__recovery_status(session->model)
                                    : (recovery_degraded
                                           ? TP_STATUS_JOURNAL_FAILED
                                           : TP_STATUS_OK);
    if (!recovery_degraded && session->recovery_live) {
        tp_error metadata_error = {{0}};
        const tp_status metadata_status = tp_recovery_live__update_saved_identity(
            session->recovery_live, canonical, &fingerprint, &metadata_error);
        if (metadata_status != TP_STATUS_OK) {
            recovery_degraded = true;
            session->recovery_healthy = false;
            if (!model_was_degraded) {
                recovery_status = metadata_status;
                tp_model__degrade_recovery(session->model, metadata_status);
            }
        }
    }
    if (recovery_degraded && !same_identity && session->recovery_live) {
        tp_error cleanup_error = {{0}};
        const tp_status cleanup_status =
            tp_recovery_live_retire(session->recovery_live, &cleanup_error);
        if (cleanup_status != TP_STATUS_OK) {
            /* The destination file is already atomically published. It is now
             * the authoritative saved state even when stale recovery cleanup
             * fails. Report degraded recovery out-of-band, but never return a
             * failure that lies about the completed Save As side effect. */
            recovery_status = cleanup_status;
            session->recovery_healthy = false;
        }
    }
    if (same_identity) {
        tp_project_destroy(candidate);
        candidate = NULL;
    } else {
        tp_model__adopt_project(session->model, candidate);
        candidate = NULL;
        session->model_generation++;
    }
    tp_project_lease *old_lease = session->project_lease;
    session->identity = next_identity;
    session->project_lease = destination_lease;
    session->saved_file_fingerprint = fingerprint;
    session->has_saved_file_fingerprint = true;
    tp_model_mark_saved(session->model);
    const bool can_heal_degraded_model =
        model_was_degraded && tp_model_has_journal(session->model) &&
        (!session->recovery_live ||
         (same_identity &&
          tp_recovery_live_healthy(session->recovery_live)));
    if (!recovery_degraded || can_heal_degraded_model) {
        tp_error compact_error = {{0}};
        const tp_status compact_status = model_was_degraded
                                             ? tp_model__heal_journal(
                                                   session->model,
                                                   &compact_error)
                                             : tp_model_compact_journal(
                                                   session->model,
                                                   &compact_error);
        if (compact_status != TP_STATUS_OK) {
            recovery_degraded = true;
            session->recovery_healthy = false;
            if (!model_was_degraded) {
                recovery_status = compact_status;
                tp_model__degrade_recovery(session->model, compact_status);
            }
        } else {
            recovery_degraded = false;
            recovery_status = TP_STATUS_OK;
            session->recovery_healthy = true;
            if (model_was_degraded && same_identity &&
                session->recovery_live) {
                tp_error metadata_error = {{0}};
                const tp_status metadata_status =
                    tp_recovery_live__update_saved_identity(
                        session->recovery_live, canonical, &fingerprint,
                        &metadata_error);
                if (metadata_status != TP_STATUS_OK) {
                    recovery_degraded = true;
                    recovery_status = metadata_status;
                    session->recovery_healthy = false;
                }
            }
        }
    }
    if (result) {
        result->saved = true;
        result->file_durability_degraded = file_durability_degraded;
        result->file_durability_status = file_durability_status;
        result->recovery_degraded = recovery_degraded;
        result->recovery_status = recovery_status;
        (void)snprintf(result->target_path, sizeof result->target_path, "%s", canonical);
        result->file_fingerprint = fingerprint;
        result->recovery_token = session->recovery_token;
        result->has_recovery_token = session->has_recovery_token;
    }
    const int64_t revision = tp_model_revision(session->model);
    publish_event(session, TP_SESSION_EVENT_SAVED, NULL, revision, revision);
    if (old_lease && old_lease != destination_lease) {
        tp_project_lease_release(old_lease);
    }
    return TP_STATUS_OK;
}

static tp_status session_save_as(tp_session *session, const char *path,
                                 bool create_only,
                                 tp_session_save_result *result,
                                 tp_error *err) {
    if (!session || !path || path[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Save As requires session and destination path");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    tp_status status = save_as_locked(session, path, create_only, result, err);
    gate_unlock(session);
    return status;
}

tp_status tp_session_save_as(tp_session *session, const char *path,
                             tp_session_save_result *result, tp_error *err) {
    return session_save_as(session, path, false, result, err);
}

tp_status tp_session_save_new(tp_session *session, const char *path,
                              tp_session_save_result *result, tp_error *err) {
    return session_save_as(session, path, true, result, err);
}

tp_status tp_session_save_detached_recovery(
    tp_session *session, const char *path,
    const tp_id128 *expected_fingerprint,
    tp_session_save_result *result, tp_error *err) {
    if (!session || !path || path[0] == '\0' || !result) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "detached recovery save requires session, path, and receipt");
    }
    memset(result, 0, sizeof *result);
    result->file_durability_status = TP_STATUS_OK;
    result->recovery_status = TP_STATUS_OK;
    gate_lock(session);
    if (!session->has_recovery_token || session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session is not an active detached recovery session");
    }
    session->admission_sequence++;
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_project_path_canonical(
        path, canonical, sizeof canonical, err);
    if (status == TP_STATUS_OK) {
        tp_project *candidate = tp_project_clone(
            tp_model_project(session->model));
        if (!candidate) {
            status = tp_error_set(err, TP_STATUS_OOM,
                                  "save candidate clone failed");
        }
        tp_id128 fingerprint;
        bool file_durability_degraded = false;
        tp_status file_durability_status = TP_STATUS_OK;
        if (status == TP_STATUS_OK) {
            status = tp_project_save_candidate_with_fingerprint(
                candidate, canonical, expected_fingerprint, false,
                &fingerprint, err);
            remap_save_error_path(status, path, err);
            file_durability_degraded =
                status == TP_STATUS_FILE_DURABILITY_UNCERTAIN;
            file_durability_status = file_durability_degraded
                                         ? status
                                         : TP_STATUS_OK;
        }
        if (status == TP_STATUS_OK || file_durability_degraded) {
            tp_model__adopt_project(session->model, candidate);
            candidate = NULL;
            tp_model_mark_saved(session->model);
            session->model_generation++;
            result->saved = true;
            result->file_durability_degraded =
                file_durability_degraded;
            result->file_durability_status = file_durability_status;
            (void)snprintf(result->target_path, sizeof result->target_path,
                           "%s", canonical);
            result->file_fingerprint = fingerprint;
            result->recovery_token = session->recovery_token;
            result->has_recovery_token = true;
            status = TP_STATUS_OK;
            if (file_durability_degraded && err) {
                err->msg[0] = '\0';
            }
        }
        tp_project_destroy(candidate);
    }
    gate_unlock(session);
    return status;
}

tp_status tp_session_save(tp_session *session, tp_session_save_result *result,
                          tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "Save requires session");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    if (session->identity.kind != TP_IDENTITY_SAVED ||
        session->identity.canonical_path[0] == '\0') {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "unsaved session requires Save As");
    }
    session->admission_sequence++;
    tp_status status = save_as_locked(session, session->identity.canonical_path,
                                      false, result, err);
    gate_unlock(session);
    return status;
}
// #endregion

// #region session commands
tp_status tp_session_discard(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "Discard requires session");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return TP_STATUS_OK;
    }
    session->admission_sequence++;
    session->discarded = true;
    const int64_t revision = tp_model_revision(session->model);
    publish_event(session, TP_SESSION_EVENT_DISCARDED, NULL, revision, revision);
    gate_unlock(session);
    return TP_STATUS_OK;
}

tp_status tp_session_invalidate_sources(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "source invalidation requires session");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    session->source_generation++;
    const int64_t revision = tp_model_revision(session->model);
    publish_event(session, TP_SESSION_EVENT_SOURCE_RUNTIME_CHANGED, NULL, revision, revision);
    gate_unlock(session);
    return TP_STATUS_OK;
}
// #endregion

// #region queries
int64_t tp_session_revision(const tp_session *session) {
    if (!session) {
        return 0;
    }
    gate_lock(session);
    int64_t revision = tp_model_revision(session->model);
    gate_unlock(session);
    return revision;
}

bool tp_session_recovery_available(const tp_session *session) {
    if (!session) {
        return false;
    }
    gate_lock(session);
    const bool available = recovery_is_healthy(session);
    gate_unlock(session);
    return available;
}

bool tp_session_can_undo(const tp_session *session) {
    if (!session) {
        return false;
    }
    gate_lock(session);
    const bool can_undo = !session->discarded &&
                          tp_model_can_undo(session->model);
    gate_unlock(session);
    return can_undo;
}

bool tp_session_can_redo(const tp_session *session) {
    if (!session) {
        return false;
    }
    gate_lock(session);
    const bool can_redo = !session->discarded &&
                          tp_model_can_redo(session->model);
    gate_unlock(session);
    return can_redo;
}

int tp_session_undo_depth(const tp_session *session) {
    if (!session) {
        return 0;
    }
    gate_lock(session);
    const int depth = tp_model_undo_depth(session->model);
    gate_unlock(session);
    return depth;
}

int tp_session_redo_depth(const tp_session *session) {
    if (!session) {
        return 0;
    }
    gate_lock(session);
    const int depth = tp_model_redo_depth(session->model);
    gate_unlock(session);
    return depth;
}

uint64_t tp_session_event_sequence(const tp_session *session) {
    if (!session) {
        return 0U;
    }
    gate_lock(session);
    uint64_t sequence = session->event_sequence;
    gate_unlock(session);
    return sequence;
}

tp_status tp_session_events_after(const tp_session *session, uint64_t after_sequence,
                                  tp_session_event *out, size_t capacity,
                                  size_t *out_count, bool *out_resync_required,
                                  tp_error *err) {
    if (!session || !out_count || !out_resync_required || (capacity > 0U && !out)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid event query");
    }
    *out_count = 0U;
    *out_resync_required = false;
    gate_lock(session);
    if (after_sequence > session->event_sequence) {
        *out_resync_required = true;
        gate_unlock(session);
        return TP_STATUS_OK;
    }
    if (after_sequence == session->event_sequence) {
        gate_unlock(session);
        return TP_STATUS_OK;
    }
    const uint64_t oldest = session->event_count > 0U
                                ? session->events[session->event_start].sequence
                                : session->event_sequence + 1U;
    if (after_sequence + 1U < oldest) {
        *out_resync_required = true;
        gate_unlock(session);
        return TP_STATUS_OK;
    }
    for (size_t i = 0U; i < session->event_count && *out_count < capacity; ++i) {
        const size_t slot = (session->event_start + i) % TP_SESSION_EVENT_CAPACITY;
        if (session->events[slot].sequence > after_sequence) {
            out[*out_count] = session->events[slot];
            (*out_count)++;
        }
    }
    gate_unlock(session);
    return TP_STATUS_OK;
}
// #endregion
