#include "tp_core/tp_session.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "tp_core/tp_diff.h"
#include "tp_core/tp_project_lease.h"
#include "tp_core/tp_project_migrate.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_core/tp_srckey.h"
#include "tp_core/tp_transaction.h"
#include "tp_journal_internal.h"
#include "tp_recovery_internal.h"
#include "tp_session_internal.h"
#include "tp_job_owner_internal.h"
#include "tp_txn_internal.h"

#define TP_SESSION_EVENT_CAPACITY 64U

struct tp_session {
#if defined(_WIN32)
    SRWLOCK gate;
#else
    pthread_mutex_t gate;
#endif
    tp_model *model;
    tp_recovery_live *recovery_live;
    tp_project_lease *project_lease;
    tp_session_owned_job *active_job;
    tp_session_identity identity;
    tp_id128 saved_file_fingerprint;
    tp_id128 recovery_token;
    bool has_saved_file_fingerprint;
    bool has_recovery_token;
    bool recovery_healthy;
    bool recovery_required;
    bool discarded;
    tp_journal_io test_recovery_io; /* borrowed fault seam; model owns it */
    uint64_t admission_sequence;
    uint64_t model_generation;
    uint64_t source_generation;
    uint64_t event_sequence;
    tp_session_event events[TP_SESSION_EVENT_CAPACITY];
    size_t event_count;
    size_t event_start;
};

typedef struct tp_snapshot_atlas_storage {
    tp_snapshot_atlas dto;
    tp_snapshot_source *sources;
    tp_snapshot_sprite *sprites;
    tp_snapshot_animation *animations;
    tp_snapshot_frame *frames;
    int *frame_offsets;
    tp_snapshot_target *targets;
} tp_snapshot_atlas_storage;

struct tp_session_snapshot {
    tp_project *project;
    tp_snapshot_atlas_storage *atlases;
    int atlas_count;
    int64_t revision;
    uint64_t admission_sequence;
    uint64_t model_generation;
    uint64_t source_generation;
    uint64_t event_sequence;
    bool dirty;
    bool recovery_healthy;
    tp_session_identity identity;
    tp_id128 saved_file_fingerprint;
    bool has_saved_file_fingerprint;
};

static _Thread_local size_t s_snapshot_allocations;
static _Thread_local size_t s_snapshot_allocation_bytes;

static void gate_lock(const tp_session *session);
static void gate_unlock(const tp_session *session);

static bool recovery_is_healthy(const tp_session *session) {
    if (session->recovery_live) {
        return tp_recovery_live_healthy(session->recovery_live);
    }
    if (tp_model_has_journal(session->model)) {
        return session->recovery_healthy;
    }
    return !session->recovery_required && session->recovery_healthy;
}

void tp_session__test_reset_snapshot_allocations(void) {
    s_snapshot_allocations = 0U;
    s_snapshot_allocation_bytes = 0U;
}

size_t tp_session__test_snapshot_allocation_count(void) {
    return s_snapshot_allocations;
}

size_t tp_session__test_snapshot_allocation_bytes(void) {
    return s_snapshot_allocation_bytes;
}

tp_status tp_session__test_attach_memory_recovery(tp_session *session,
                                                  tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "memory recovery attach requires a session");
    }
    tp_journal_io io = tp_journal_io_memory();
    if (!io.ctx) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "memory recovery I/O allocation failed");
    }
    tp_id128 key;
    static const uint8_t bytes[16] = {
        'n', 't', 'p', 'k', '_', 'r', 'e', 'c',
        'o', 'v', 'e', 'r', 'y', '_', '0', '1'};
    memcpy(key.bytes, bytes, sizeof bytes);
    tp_journal *journal = tp_journal_create(io, key);
    if (!journal) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "memory recovery journal allocation failed");
    }
    tp_status status = tp_session_attach_journal(session, journal, err);
    if (status != TP_STATUS_OK) {
        tp_journal_destroy(journal);
        return status;
    }
    gate_lock(session);
    session->test_recovery_io = io;
    gate_unlock(session);
    return TP_STATUS_OK;
}

void tp_session__test_fail_next_recovery_writes(tp_session *session, int count) {
    if (!session) {
        return;
    }
    gate_lock(session);
    tp_journal_io io = session->test_recovery_io;
    gate_unlock(session);
    tp_journal_io_memory__fail_next_writes(io, count);
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

static void *snapshot_calloc(size_t count, size_t size) {
    void *allocation = calloc(count, size);
    if (allocation) {
        s_snapshot_allocations++;
        s_snapshot_allocation_bytes += count * size;
    }
    return allocation;
}

_Static_assert((int)TP_SNAPSHOT_SOURCE_FOLDER == (int)TP_SOURCE_KIND_FOLDER,
               "snapshot folder kind must match project vocabulary");
_Static_assert((int)TP_SNAPSHOT_SOURCE_FILE == (int)TP_SOURCE_KIND_FILE,
               "snapshot file kind must match project vocabulary");

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

static void gate_lock(const tp_session *session) {
#if defined(_WIN32)
    AcquireSRWLockExclusive((SRWLOCK *)&session->gate);
#else
    (void)pthread_mutex_lock((pthread_mutex_t *)&session->gate);
#endif
}

static void gate_unlock(const tp_session *session) {
#if defined(_WIN32)
    ReleaseSRWLockExclusive((SRWLOCK *)&session->gate);
#else
    (void)pthread_mutex_unlock((pthread_mutex_t *)&session->gate);
#endif
}

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

tp_status tp_session_adopt_owned(tp_project *project, const tp_rng *rng, tp_session **out,
                                 tp_error *err) {
    if (!project || !rng || !rng->fill || !out) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session adopt requires project, rng, and output");
    }
    *out = NULL;

    tp_status status = tp_project_promote_ids(project, rng, err);
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
    status = tp_session_adopt_owned(project, rng, &session, err);
    if (status != TP_STATUS_OK) {
        tp_project_lease_release(lease);
        return status;
    }
    status = tp_model__migrate_sprite_refs(session->model, err);
    if (status != TP_STATUS_OK) {
        tp_session_destroy(session);
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
        const bool preserve = !tp_recovery_live_healthy(session->recovery_live) ||
                              (tp_model_dirty(session->model) && !session->discarded);
        (void)tp_recovery_live_finish(session->recovery_live, preserve, NULL);
        tp_recovery_live_destroy(session->recovery_live);
    }
    tp_model_destroy(session->model);
    tp_project_lease_release(session->project_lease);
    gate_destroy(session);
    free(session);
}

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
    if (!recovery_is_healthy(session)) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                            "session recovery is degraded; mutation is unavailable");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    tp_txn_result local_result;
    tp_txn_result *published_result = result ? result : &local_result;
    tp_status status = tp_model_apply(session->model, request,
                                      published_result, err);
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
    if (!recovery_is_healthy(session)) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                            "session recovery is degraded; Undo is unavailable");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    tp_status status = tp_model_undo(session->model, err);
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
    if (!recovery_is_healthy(session)) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                            "session recovery is degraded; Redo is unavailable");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    tp_status status = tp_model_redo(session->model, err);
    if (status == TP_STATUS_OK) {
        session->model_generation++;
        publish_event(session, TP_SESSION_EVENT_REDONE, NULL, revision_before,
                      tp_model_revision(session->model));
    }
    gate_unlock(session);
    return status;
}

static tp_status save_as_locked(tp_session *session, const char *path,
                                tp_session_save_result *result, tp_error *err) {
    if (result) {
        memset(result, 0, sizeof *result);
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
    tp_project *save_project = tp_model_project(session->model);
    tp_project *migrated = NULL;
    if (tp_project_has_pending_sprite_refs(save_project)) {
        migrated = tp_project_clone(save_project);
        if (!migrated) {
            if (destination_lease != session->project_lease) {
                tp_project_lease_release(destination_lease);
            }
            return tp_error_set(err, TP_STATUS_OOM,
                                "legacy reference migration clone failed");
        }
        status = tp_project_migrate_sprite_refs(migrated, err);
        if (status != TP_STATUS_OK) {
            tp_project_destroy(migrated);
            if (destination_lease != session->project_lease) {
                tp_project_lease_release(destination_lease);
            }
            return status;
        }
        save_project = migrated;
    }
    tp_id128 fingerprint;
    status = same_identity
                 ? tp_project_save_if_unchanged(save_project, canonical,
                                                &session->saved_file_fingerprint,
                                                &fingerprint, err)
                 : tp_project_save_with_fingerprint(save_project, canonical,
                                                    &fingerprint, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(migrated);
        if (destination_lease != session->project_lease) {
            tp_project_lease_release(destination_lease);
        }
        return status;
    }
    if (migrated) {
        tp_model__adopt_project(session->model, migrated);
    }
    tp_project_lease *old_lease = session->project_lease;
    session->identity = next_identity;
    session->project_lease = destination_lease;
    session->saved_file_fingerprint = fingerprint;
    session->has_saved_file_fingerprint = true;
    bool recovery_degraded = !recovery_is_healthy(session);
    tp_status recovery_status = recovery_degraded
                                    ? TP_STATUS_JOURNAL_FAILED
                                    : TP_STATUS_OK;
    if (!recovery_degraded && session->recovery_live) {
        tp_error metadata_error = {{0}};
        recovery_status = tp_recovery_live__update_saved_identity(
            session->recovery_live, canonical, &fingerprint, &metadata_error);
        if (recovery_status != TP_STATUS_OK) {
            recovery_degraded = true;
            session->recovery_healthy = false;
        }
    }
    tp_model_mark_saved(session->model);
    session->model_generation++;
    if (!recovery_degraded) {
        tp_error compact_error = {{0}};
        recovery_status = tp_model_compact_journal(session->model, &compact_error);
        if (recovery_status != TP_STATUS_OK) {
            recovery_degraded = true;
            session->recovery_healthy = false;
            tp_recovery_live__mark_degraded(session->recovery_live);
        }
    }
    if (result) {
        result->saved = true;
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

tp_status tp_session_save_as(tp_session *session, const char *path,
                             tp_session_save_result *result, tp_error *err) {
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
    tp_status status = save_as_locked(session, path, result, err);
    gate_unlock(session);
    return status;
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
    result->recovery_status = TP_STATUS_OK;
    gate_lock(session);
    if (!session->has_recovery_token || session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session is not an active detached recovery session");
    }
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_project_path_canonical(
        path, canonical, sizeof canonical, err);
    if (status == TP_STATUS_OK) {
        tp_project *save_project = tp_model_project(session->model);
        tp_project *migrated = NULL;
        if (tp_project_has_pending_sprite_refs(save_project)) {
            migrated = tp_project_clone(save_project);
            status = migrated
                         ? tp_project_migrate_sprite_refs(migrated, err)
                         : tp_error_set(err, TP_STATUS_OOM,
                                        "legacy reference migration clone failed");
            if (status == TP_STATUS_OK) {
                save_project = migrated;
            }
        }
        tp_id128 fingerprint;
        if (status == TP_STATUS_OK) {
            status = expected_fingerprint
                         ? tp_project_save_if_unchanged(save_project, canonical,
                                                        expected_fingerprint,
                                                        &fingerprint, err)
                         : tp_project_save_with_fingerprint(save_project, canonical,
                                                            &fingerprint, err);
        }
        if (status == TP_STATUS_OK) {
            if (migrated) {
                tp_model__adopt_project(session->model, migrated);
                migrated = NULL;
            }
            tp_model_mark_saved(session->model);
            result->saved = true;
            (void)snprintf(result->target_path, sizeof result->target_path,
                           "%s", canonical);
            result->file_fingerprint = fingerprint;
            result->recovery_token = session->recovery_token;
            result->has_recovery_token = true;
        }
        tp_project_destroy(migrated);
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
    tp_status status = save_as_locked(session, session->identity.canonical_path, result, err);
    gate_unlock(session);
    return status;
}

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
    const bool can_undo = tp_model_can_undo(session->model);
    gate_unlock(session);
    return can_undo;
}

bool tp_session_can_redo(const tp_session *session) {
    if (!session) {
        return false;
    }
    gate_lock(session);
    const bool can_redo = tp_model_can_redo(session->model);
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

static tp_status snapshot_materialize(tp_session_snapshot *snapshot,
                                      tp_error *err);

tp_status tp_session_snapshot_create(const tp_session *session,
                                     tp_session_snapshot **out, tp_error *err) {
    if (!session || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot requires session and output");
    }
    *out = NULL;
    tp_session_snapshot *snapshot = (tp_session_snapshot *)snapshot_calloc(1U, sizeof *snapshot);
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_OOM, "snapshot allocation failed");
    }

    /* Only cloning and scalar metadata capture require a consistent admission
     * point. The clone is immutable after publication, so DTO materialization
     * must not keep the single-writer gate held. */
    gate_lock(session);
    tp_project *project = tp_project_clone(tp_model_project(session->model));
    if (!project) {
        gate_unlock(session);
        free(snapshot);
        return tp_error_set(err, TP_STATUS_OOM, "snapshot project clone failed");
    }
    snapshot->project = project;
    snapshot->atlas_count = project->atlas_count;
    snapshot->revision = tp_model_revision(session->model);
    snapshot->admission_sequence = session->admission_sequence;
    snapshot->model_generation = session->model_generation;
    snapshot->source_generation = session->source_generation;
    snapshot->event_sequence = session->event_sequence;
    snapshot->dirty = tp_model_dirty(session->model);
    snapshot->recovery_healthy = recovery_is_healthy(session);
    snapshot->identity = session->identity;
    snapshot->saved_file_fingerprint = session->saved_file_fingerprint;
    snapshot->has_saved_file_fingerprint = session->has_saved_file_fingerprint;
    gate_unlock(session);

    tp_status status = snapshot_materialize(snapshot, err);
    if (status == TP_STATUS_OK) {
        *out = snapshot;
    }
    return status;
}

static tp_status snapshot_materialize(tp_session_snapshot *snapshot,
                                      tp_error *err) {
    tp_project *project = snapshot->project;
    if (project->atlas_count > 0) {
        snapshot->atlases = (tp_snapshot_atlas_storage *)snapshot_calloc(
            (size_t)project->atlas_count, sizeof *snapshot->atlases);
        if (!snapshot->atlases) {
            tp_session_snapshot_destroy(snapshot);
            return tp_error_set(err, TP_STATUS_OOM, "snapshot atlas allocation failed");
        }
    }
    for (int i = 0; i < project->atlas_count; ++i) {
        const tp_project_atlas *source = &project->atlases[i];
        tp_snapshot_atlas_storage *storage = &snapshot->atlases[i];
        tp_snapshot_atlas *atlas = &storage->dto;
        atlas->id = source->id;
        atlas->name = source->name;
        atlas->max_size = source->max_size;
        atlas->padding = source->padding;
        atlas->margin = source->margin;
        atlas->extrude = source->extrude;
        atlas->alpha_threshold = source->alpha_threshold;
        atlas->max_vertices = source->max_vertices;
        atlas->shape = source->shape;
        atlas->allow_transform = source->allow_transform;
        atlas->power_of_two = source->power_of_two;
        atlas->pixels_per_unit = source->pixels_per_unit;
        atlas->source_count = source->source_count;
        atlas->sprite_count = source->sprite_count;
        atlas->animation_count = source->animation_count;
        atlas->target_count = source->target_count;

        if (source->source_count > 0) {
            storage->sources = (tp_snapshot_source *)snapshot_calloc(
                (size_t)source->source_count, sizeof *storage->sources);
        }
        if (source->sprite_count > 0) {
            storage->sprites = (tp_snapshot_sprite *)snapshot_calloc(
                (size_t)source->sprite_count, sizeof *storage->sprites);
        }
        if (source->animation_count > 0) {
            storage->animations = (tp_snapshot_animation *)snapshot_calloc(
                (size_t)source->animation_count, sizeof *storage->animations);
            storage->frame_offsets = (int *)snapshot_calloc(
                (size_t)source->animation_count + 1U, sizeof *storage->frame_offsets);
        }
        if (source->target_count > 0) {
            storage->targets = (tp_snapshot_target *)snapshot_calloc(
                (size_t)source->target_count, sizeof *storage->targets);
        }
        if ((source->source_count > 0 && !storage->sources) ||
            (source->sprite_count > 0 && !storage->sprites) ||
            (source->animation_count > 0 && (!storage->animations || !storage->frame_offsets)) ||
            (source->target_count > 0 && !storage->targets)) {
            tp_session_snapshot_destroy(snapshot);
            return tp_error_set(err, TP_STATUS_OOM, "snapshot nested DTO allocation failed");
        }
        int frame_count = 0;
        for (int j = 0; j < source->animation_count; ++j) {
            storage->frame_offsets[j] = frame_count;
            frame_count += source->animations[j].frame_count;
        }
        if (source->animation_count > 0) {
            storage->frame_offsets[source->animation_count] = frame_count;
        }
        if (frame_count > 0) {
            storage->frames = (tp_snapshot_frame *)snapshot_calloc(
                (size_t)frame_count, sizeof *storage->frames);
            if (!storage->frames) {
                tp_session_snapshot_destroy(snapshot);
                return tp_error_set(err, TP_STATUS_OOM, "snapshot frame DTO allocation failed");
            }
        }
        for (int j = 0; j < source->source_count; ++j) {
            storage->sources[j].id = source->sources[j].id;
            storage->sources[j].kind = (tp_snapshot_source_kind)source->sources[j].kind;
            storage->sources[j].path = source->sources[j].path;
        }
        for (int j = 0; j < source->sprite_count; ++j) {
            const tp_project_sprite *input = &source->sprites[j];
            tp_snapshot_sprite *output = &storage->sprites[j];
            output->id = !tp_id128_is_nil(input->source_ref) && input->src_key
                             ? tp_sprite_id(input->source_ref, input->src_key)
                             : tp_id128_nil();
            output->source_id = input->source_ref;
            output->source_key = input->src_key;
            output->name = input->name;
            output->origin_x = input->origin_x;
            output->origin_y = input->origin_y;
            memcpy(output->slice9_lrtb, input->slice9_lrtb, sizeof output->slice9_lrtb);
            output->rename = input->rename;
            output->override_shape = input->ov_shape;
            output->override_allow_rotate = input->ov_allow_rotate;
            output->override_max_vertices = input->ov_max_vertices;
            output->override_margin = input->ov_margin;
            output->override_extrude = input->ov_extrude;
        }
        for (int j = 0; j < source->animation_count; ++j) {
            const tp_project_anim *input = &source->animations[j];
            tp_snapshot_animation *output = &storage->animations[j];
            output->id = input->id;
            output->name = input->name;
            output->fps = input->fps;
            output->playback = input->playback;
            output->flip_h = input->flip_h;
            output->flip_v = input->flip_v;
            output->frame_count = input->frame_count;
            const int offset = storage->frame_offsets[j];
            for (int k = 0; k < input->frame_count; ++k) {
                storage->frames[offset + k].sprite_id =
                    !tp_id128_is_nil(input->frames[k].source_ref) && input->frames[k].src_key
                        ? tp_sprite_id(input->frames[k].source_ref, input->frames[k].src_key)
                        : tp_id128_nil();
                storage->frames[offset + k].source_id = input->frames[k].source_ref;
                storage->frames[offset + k].source_key = input->frames[k].src_key;
                storage->frames[offset + k].name = input->frames[k].name;
            }
        }
        for (int j = 0; j < source->target_count; ++j) {
            storage->targets[j].id = source->targets[j].id;
            storage->targets[j].exporter_id = source->targets[j].exporter_id;
            storage->targets[j].out_path = source->targets[j].out_path;
            storage->targets[j].enabled = source->targets[j].enabled;
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_session_snapshot_load(const char *path,
                                   tp_session_snapshot **out, tp_error *err) {
    if (!path || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot load requires path and output");
    }
    *out = NULL;
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_project_path_canonical(
        path, canonical, sizeof canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    tp_id128 before;
    status = tp_identity_file_fingerprint(canonical, &before, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_project *project = NULL;
    tp_id128 loaded;
    status = tp_project_load_with_fingerprint(canonical, &project, &loaded, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (!tp_id128_eq(before, loaded)) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "project changed while it was read");
    }
    status = tp_project_migrate_sprite_refs(project, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        return status;
    }

    tp_session_snapshot *snapshot =
        (tp_session_snapshot *)snapshot_calloc(1U, sizeof *snapshot);
    if (!snapshot) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_OOM, "snapshot allocation failed");
    }
    snapshot->project = project;
    snapshot->atlas_count = project->atlas_count;
    snapshot->recovery_healthy = true;
    snapshot->identity.kind = TP_IDENTITY_SAVED;
    (void)snprintf(snapshot->identity.canonical_path,
                   sizeof snapshot->identity.canonical_path, "%s", canonical);
    snapshot->saved_file_fingerprint = loaded;
    snapshot->has_saved_file_fingerprint = true;
    status = snapshot_materialize(snapshot, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    *out = snapshot;
    return TP_STATUS_OK;
}

void tp_session_snapshot_destroy(tp_session_snapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    for (int i = 0; i < snapshot->atlas_count; ++i) {
        free(snapshot->atlases[i].sources);
        free(snapshot->atlases[i].sprites);
        free(snapshot->atlases[i].animations);
        free(snapshot->atlases[i].frames);
        free(snapshot->atlases[i].frame_offsets);
        free(snapshot->atlases[i].targets);
    }
    free(snapshot->atlases);
    tp_project_destroy(snapshot->project);
    free(snapshot);
}

const tp_project *tp_session_snapshot_project_internal(
    const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->project : NULL;
}

int64_t tp_session_snapshot_revision(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->revision : 0;
}

uint64_t tp_session_snapshot_model_generation(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->model_generation : 0U;
}

uint64_t tp_session_snapshot_admission_sequence(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->admission_sequence : 0U;
}

uint64_t tp_session_snapshot_source_generation(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->source_generation : 0U;
}

tp_session_input_token tp_session_snapshot_input_token(
    const tp_session_snapshot *snapshot) {
    const tp_session_input_token token = {
        .model_generation = snapshot ? snapshot->model_generation : 0U,
        .source_generation = snapshot ? snapshot->source_generation : 0U,
    };
    return token;
}

bool tp_session_input_token_equal(tp_session_input_token left,
                                  tp_session_input_token right) {
    return left.model_generation == right.model_generation &&
           left.source_generation == right.source_generation;
}

uint64_t tp_session_snapshot_event_sequence(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->event_sequence : 0U;
}

bool tp_session_snapshot_dirty(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->dirty;
}

bool tp_session_snapshot_recovery_available(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->recovery_healthy;
}

tp_session_identity tp_session_snapshot_identity(const tp_session_snapshot *snapshot) {
    tp_session_identity identity;
    memset(&identity, 0, sizeof identity);
    if (snapshot) {
        identity = snapshot->identity;
    }
    return identity;
}

int tp_session_snapshot_project_schema_version(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->project ? snapshot->project->schema_version : 0;
}

const char *tp_session_snapshot_project_dir(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->project && snapshot->project->project_dir
               ? snapshot->project->project_dir
               : "";
}

const char *tp_session_snapshot_canonical_path(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->identity.kind == TP_IDENTITY_SAVED
               ? snapshot->identity.canonical_path
               : "";
}

bool tp_session_snapshot_saved_file_fingerprint(
    const tp_session_snapshot *snapshot, tp_id128 *out_fingerprint) {
    if (!snapshot || !out_fingerprint || !snapshot->has_saved_file_fingerprint) {
        return false;
    }
    *out_fingerprint = snapshot->saved_file_fingerprint;
    return true;
}

int tp_session_snapshot_atlas_count(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->atlas_count : 0;
}

const tp_snapshot_atlas *tp_session_snapshot_atlas_at(const tp_session_snapshot *snapshot,
                                                      int index) {
    if (!snapshot || index < 0 || index >= snapshot->atlas_count) {
        return NULL;
    }
    return &snapshot->atlases[index].dto;
}

const tp_snapshot_atlas *tp_session_snapshot_atlas_by_id(const tp_session_snapshot *snapshot,
                                                         tp_id128 id) {
    if (!snapshot || tp_id128_is_nil(id)) {
        return NULL;
    }
    for (int i = 0; i < snapshot->atlas_count; ++i) {
        if (tp_id128_eq(snapshot->atlases[i].dto.id, id)) {
            return &snapshot->atlases[i].dto;
        }
    }
    return NULL;
}

static const tp_snapshot_atlas_storage *atlas_storage(const tp_session_snapshot *snapshot,
                                                      tp_id128 atlas_id) {
    if (!snapshot || tp_id128_is_nil(atlas_id)) {
        return NULL;
    }
    for (int i = 0; i < snapshot->atlas_count; ++i) {
        if (tp_id128_eq(snapshot->atlases[i].dto.id, atlas_id)) {
            return &snapshot->atlases[i];
        }
    }
    return NULL;
}

const tp_snapshot_source *tp_session_snapshot_source_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    return atlas && index >= 0 && index < atlas->dto.source_count ? &atlas->sources[index] : NULL;
}

tp_status tp_session_snapshot_source_resolved_at(
    const tp_session_snapshot *snapshot, int atlas_index, int source_index,
    const tp_snapshot_source **out_source, char *out_path, size_t capacity,
    tp_error *err) {
    if (out_source) {
        *out_source = NULL;
    }
    if (out_path && capacity > 0U) {
        out_path[0] = '\0';
    }
    if (!snapshot || !out_source || !out_path || capacity == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid direct snapshot source query");
    }
    if (atlas_index < 0 || atlas_index >= snapshot->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "snapshot atlas index is out of bounds");
    }
    const tp_snapshot_atlas_storage *atlas = &snapshot->atlases[atlas_index];
    if (source_index < 0 || source_index >= atlas->dto.source_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "snapshot source index is out of bounds");
    }
    const tp_snapshot_source *source = &atlas->sources[source_index];
    *out_source = source;
    return tp_project_resolve_source_path(snapshot->project, source->path,
                                          out_path, capacity);
}

const tp_snapshot_source *tp_session_snapshot_source_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 source_id) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(source_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.source_count; ++i) {
        if (tp_id128_eq(atlas->sources[i].id, source_id)) {
            return &atlas->sources[i];
        }
    }
    return NULL;
}

const tp_snapshot_sprite *tp_session_snapshot_sprite_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    return atlas && index >= 0 && index < atlas->dto.sprite_count ? &atlas->sprites[index] : NULL;
}

const tp_snapshot_sprite *tp_session_snapshot_sprite_at_index(
    const tp_session_snapshot *snapshot, int atlas_index, int sprite_index) {
    if (!snapshot || atlas_index < 0 || atlas_index >= snapshot->atlas_count) {
        return NULL;
    }
    const tp_snapshot_atlas_storage *atlas = &snapshot->atlases[atlas_index];
    return sprite_index >= 0 && sprite_index < atlas->dto.sprite_count
               ? &atlas->sprites[sprite_index]
               : NULL;
}

const tp_snapshot_sprite *tp_session_snapshot_sprite_by_key(const tp_session_snapshot *snapshot,
                                                            tp_id128 atlas_id, tp_id128 source_id,
                                                            const char *source_key) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(source_id) || !source_key) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.sprite_count; ++i) {
        if (tp_id128_eq(atlas->sprites[i].source_id, source_id) && atlas->sprites[i].source_key &&
            strcmp(atlas->sprites[i].source_key, source_key) == 0) {
            return &atlas->sprites[i];
        }
    }
    return NULL;
}

const tp_snapshot_sprite *tp_session_snapshot_sprite_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 sprite_id) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(sprite_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.sprite_count; ++i) {
        if (tp_id128_eq(atlas->sprites[i].id, sprite_id)) {
            return &atlas->sprites[i];
        }
    }
    return NULL;
}

const tp_snapshot_animation *tp_session_snapshot_animation_at(const tp_session_snapshot *snapshot,
                                                              tp_id128 atlas_id, int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    return atlas && index >= 0 && index < atlas->dto.animation_count ? &atlas->animations[index] : NULL;
}

const tp_snapshot_animation *tp_session_snapshot_animation_by_id(const tp_session_snapshot *snapshot,
                                                                 tp_id128 atlas_id, tp_id128 animation_id) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(animation_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.animation_count; ++i) {
        if (tp_id128_eq(atlas->animations[i].id, animation_id)) {
            return &atlas->animations[i];
        }
    }
    return NULL;
}

const tp_snapshot_frame *tp_session_snapshot_animation_frame_at(const tp_session_snapshot *snapshot,
                                                                tp_id128 atlas_id, tp_id128 animation_id,
                                                                int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || index < 0) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.animation_count; ++i) {
        if (tp_id128_eq(atlas->animations[i].id, animation_id)) {
            return index < atlas->animations[i].frame_count
                       ? &atlas->frames[atlas->frame_offsets[i] + index]
                       : NULL;
        }
    }
    return NULL;
}

const tp_snapshot_frame *tp_session_snapshot_animation_frames(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, int *out_count) {
    if (out_count) {
        *out_count = 0;
    }
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(animation_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.animation_count; ++i) {
        if (tp_id128_eq(atlas->animations[i].id, animation_id)) {
            const int count = atlas->animations[i].frame_count;
            if (out_count) {
                *out_count = count;
            }
            return count > 0 ? &atlas->frames[atlas->frame_offsets[i]] : NULL;
        }
    }
    return NULL;
}

const tp_snapshot_target *tp_session_snapshot_target_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    return atlas && index >= 0 && index < atlas->dto.target_count ? &atlas->targets[index] : NULL;
}

const tp_snapshot_target *tp_session_snapshot_target_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 target_id) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(target_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.target_count; ++i) {
        if (tp_id128_eq(atlas->targets[i].id, target_id)) {
            return &atlas->targets[i];
        }
    }
    return NULL;
}

tp_status tp_session_snapshot_resolve_path(const tp_session_snapshot *snapshot,
                                           tp_id128 atlas_id, tp_id128 source_id,
                                           char *out, size_t capacity, tp_error *err) {
    if (!snapshot || !out || capacity == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid snapshot path query");
    }
    const int atlas_index = tp_project_find_atlas_by_id(snapshot->project, atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "snapshot atlas id was not found");
    }
    tp_project_atlas *atlas = &snapshot->project->atlases[atlas_index];
    tp_project_source *source = tp_project_atlas_find_source_by_id(atlas, source_id);
    if (!source) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "snapshot source id was not found");
    }
    return tp_project_resolve_source_path(snapshot->project, source->path, out,
                                          capacity);
}

tp_status tp_session_snapshot_resolve_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_scope,
    tp_selector_kind want, const char *selector, tp_selector_result *out,
    tp_selector_candidates *candidates, tp_error *err) {
    if (!snapshot || !snapshot->project || !selector || !out ||
        want <= TP_SEL_NONE || want > TP_SEL_TARGET) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot selector needs a snapshot, structural kind, selector, and output");
    }
    const tp_project *project = snapshot->project;
    tp_project scoped;
    int atlas_index = -1;
    if (!tp_id128_is_nil(atlas_scope)) {
        atlas_index = tp_project_find_atlas_by_id(project, atlas_scope);
        if (atlas_index < 0) {
            return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                "selector atlas scope does not exist");
        }
        scoped = *project;
        scoped.atlases = &project->atlases[atlas_index];
        scoped.atlas_count = 1;
        project = &scoped;
    }

    char qualified[TP_IDENTITY_PATH_MAX + 16U];
    const char *resolved_selector = selector;
    const char *kind_token = tp_selector_kind_token(want);
    const size_t kind_token_len = strlen(kind_token);
    const bool already_qualified =
        strncmp(selector, kind_token, kind_token_len) == 0 &&
        selector[kind_token_len] == ':';
    tp_id_kind id_kind = TP_ID_KIND_INVALID;
    tp_id128 parsed_id = tp_id128_nil();
    if (tp_id_parse(selector, &id_kind, &parsed_id, NULL) != TP_STATUS_OK &&
        !already_qualified) {
        const int written = snprintf(qualified, sizeof qualified, "%s:%s",
                                     kind_token, selector);
        if (written < 0 || (size_t)written >= sizeof qualified) {
            return tp_error_set(err, TP_STATUS_OUT_OF_RANGE,
                                "selector is too long");
        }
        resolved_selector = qualified;
    }
    tp_status status = tp_selector_resolve(project, resolved_selector, NULL, -1,
                                            out, candidates, err);
    if (status == TP_STATUS_OK && out->kind != want) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "selector resolved to %s, expected %s",
                            tp_selector_kind_token(out->kind),
                            tp_selector_kind_token(want));
    }
    if (atlas_index >= 0) {
        if (status == TP_STATUS_OK) {
            out->atlas_index = atlas_index;
        }
        if (candidates) {
            for (int i = 0; i < candidates->count; ++i) {
                candidates->v[i].atlas_index = atlas_index;
            }
        }
    }
    return status;
}

tp_status tp_session_snapshot_resolve_sprite_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *selector, tp_selector_result *out, tp_id128 *out_source_id,
    char *out_source_key, size_t source_key_capacity,
    tp_selector_candidates *candidates, tp_error *err) {
    if (!snapshot || !snapshot->project || tp_id128_is_nil(atlas_id) ||
        !selector || !out || !out_source_id || !out_source_key ||
        source_key_capacity == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "sprite selector needs a snapshot, atlas, selector, and outputs");
    }
    out_source_key[0] = '\0';
    *out_source_id = tp_id128_nil();
    const int atlas_index = tp_project_find_atlas_by_id(snapshot->project,
                                                        atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "sprite selector atlas does not exist");
    }

    /* An explicit source-id compound is already a unique structural address and
     * must remain usable while its file is missing (orphan metadata is retained
     * until the source key returns). Normalize it without consulting the disk. */
    const char *compound_colon = strchr(selector, ':');
    if (compound_colon) {
        const size_t prefix_length = (size_t)(compound_colon - selector);
        if (prefix_length < TP_ID_TEXT_CAP) {
            char prefix[TP_ID_TEXT_CAP];
            memcpy(prefix, selector, prefix_length);
            prefix[prefix_length] = '\0';
            tp_id_kind kind = TP_ID_KIND_INVALID;
            tp_id128 source_id = tp_id128_nil();
            if (tp_id_parse(prefix, &kind, &source_id, NULL) == TP_STATUS_OK &&
                kind == TP_ID_KIND_SOURCE) {
                if (!tp_session_snapshot_source_by_id(snapshot, atlas_id,
                                                      source_id)) {
                    return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                        "sprite selector source does not exist in the atlas");
                }
                tp_status normalize_status = tp_srckey_normalize(
                    compound_colon + 1, out_source_key, source_key_capacity,
                    err);
                if (normalize_status != TP_STATUS_OK) {
                    return normalize_status;
                }
                out->kind = TP_SEL_SPRITE;
                out->id = tp_sprite_id(source_id, out_source_key);
                out->atlas_index = atlas_index;
                *out_source_id = source_id;
                return TP_STATUS_OK;
            }
        }
    }

    /* A persisted orphan can also be addressed by its deterministic sprite id;
     * unlike a human key, this does not need a live index to be unambiguous. */
    tp_id128 parsed_sprite = tp_id128_nil();
    if (tp_sprite_id_parse(selector, &parsed_sprite, NULL) == TP_STATUS_OK) {
        const tp_snapshot_sprite *persisted =
            tp_session_snapshot_sprite_by_id(snapshot, atlas_id, parsed_sprite);
        if (persisted) {
            if (!tp_session_snapshot_source_by_id(snapshot, atlas_id,
                                                  persisted->source_id)) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                    "persisted sprite source does not exist in the atlas");
            }
            const size_t key_length = strlen(persisted->source_key);
            if (key_length >= source_key_capacity) {
                return tp_error_set(err, TP_STATUS_OUT_OF_RANGE,
                                    "resolved sprite key exceeds output capacity");
            }
            out->kind = TP_SEL_SPRITE;
            out->id = persisted->id;
            out->atlas_index = atlas_index;
            *out_source_id = persisted->source_id;
            memcpy(out_source_key, persisted->source_key, key_length + 1U);
            return TP_STATUS_OK;
        }
    }

    tp_sprite_index index;
    tp_status status = tp_sprite_index_build(snapshot->project, atlas_index,
                                              &index, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    char qualified[TP_IDENTITY_PATH_MAX + 16U];
    const char *resolved_selector = selector;
    bool canonical_form = false;
    if (tp_sprite_id_parse(selector, &parsed_sprite, NULL) == TP_STATUS_OK ||
        strncmp(selector, "sprite:", sizeof("sprite:") - 1U) == 0) {
        canonical_form = true;
    } else {
        const char *colon = strchr(selector, ':');
        if (colon) {
            const size_t prefix_length = (size_t)(colon - selector);
            if (prefix_length < TP_ID_TEXT_CAP) {
                char prefix[TP_ID_TEXT_CAP];
                memcpy(prefix, selector, prefix_length);
                prefix[prefix_length] = '\0';
                tp_id_kind kind = TP_ID_KIND_INVALID;
                tp_id128 parsed_source = tp_id128_nil();
                canonical_form =
                    tp_id_parse(prefix, &kind, &parsed_source, NULL) ==
                        TP_STATUS_OK &&
                    kind == TP_ID_KIND_SOURCE;
            }
        }
    }
    if (!canonical_form) {
        const int written = snprintf(qualified, sizeof qualified, "sprite:%s",
                                     selector);
        if (written < 0 || (size_t)written >= sizeof qualified) {
            tp_sprite_index_free(&index);
            return tp_error_set(err, TP_STATUS_OUT_OF_RANGE,
                                "sprite selector is too long");
        }
        resolved_selector = qualified;
    }

    status = tp_selector_resolve(snapshot->project, resolved_selector, &index,
                                 atlas_index, out, candidates, err);
    if (status == TP_STATUS_OK && out->kind != TP_SEL_SPRITE) {
        status = tp_error_set(err, TP_STATUS_NOT_FOUND,
                              "selector resolved to %s, expected sprite",
                              tp_selector_kind_token(out->kind));
    }
    if (status == TP_STATUS_OK) {
        const tp_sprite_ref *ref = tp_sprite_index_by_id(&index, out->id);
        if (!ref) {
            status = tp_error_set(err, TP_STATUS_NOT_FOUND,
                                  "resolved sprite is no longer present");
        } else {
            const size_t key_length = strlen(ref->source_key);
            if (key_length >= source_key_capacity) {
                status = tp_error_set(err, TP_STATUS_OUT_OF_RANGE,
                                      "resolved sprite key exceeds output capacity");
            } else {
                *out_source_id = ref->source_id;
                memcpy(out_source_key, ref->source_key, key_length + 1U);
            }
        }
    }
    tp_sprite_index_free(&index);
    return status;
}

bool tp_session_snapshot_target_out_path_shared(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 target_id, const char *out_path) {
    if (!snapshot || !out_path) {
        return false;
    }
    const int atlas_index = tp_project_find_atlas_by_id(snapshot->project,
                                                        atlas_id);
    if (atlas_index < 0) {
        return false;
    }
    const tp_project_target *target = tp_project_atlas_find_target_by_id(
        &snapshot->project->atlases[atlas_index], target_id);
    return target && tp_project_out_path_shared(snapshot->project, out_path,
                                                target);
}

tp_status tp_session_snapshot_next_atlas_defaults(
    const tp_session_snapshot *snapshot, char *name, size_t name_cap,
    char *out_path, size_t out_path_cap, const char **exporter_id,
    bool *target_enabled, tp_error *err) {
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "atlas defaults need a snapshot");
    }
    return tp_project_next_atlas_defaults(snapshot->project, name, name_cap,
                                          out_path, out_path_cap, exporter_id,
                                          target_enabled, err);
}

tp_status tp_session_snapshot_next_animation_name(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, const char *base,
    char *name, size_t name_cap, tp_error *err) {
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "animation defaults need a snapshot");
    }
    return tp_project_next_animation_name(snapshot->project, atlas_id, base,
                                          name, name_cap, err);
}

tp_status tp_session_snapshot_target_defaults(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char **exporter_id, char *out_path, size_t out_path_cap,
    bool *enabled, tp_error *err) {
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "target defaults need a snapshot");
    }
    return tp_project_target_defaults(snapshot->project, atlas_id, exporter_id,
                                      out_path, out_path_cap, enabled, err);
}

tp_status tp_session_snapshot_resolve_frame(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, const char *selector, int *out_index,
    tp_error *err) {
    if (!snapshot || !selector || !out_index) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "frame query needs snapshot, selector, and output");
    }
    char *end = NULL;
    const long parsed = strtol(selector, &end, 10);
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_by_id(snapshot, atlas_id, animation_id);
    if (!animation) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "animation was not found");
    }
    if (end != selector && *end == '\0') {
        if (parsed >= 0 && parsed < animation->frame_count) {
            *out_index = (int)parsed;
            return TP_STATUS_OK;
        }
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "animation has no frame at index %ld", parsed);
    }
    int match = -1;
    int count = 0;
    for (int i = 0; i < animation->frame_count; ++i) {
        const tp_snapshot_frame *frame =
            tp_session_snapshot_animation_frame_at(snapshot, atlas_id,
                                                    animation_id, i);
        if (frame && frame->name && strcmp(frame->name, selector) == 0) {
            match = i;
            count++;
        }
    }
    if (count > 1) {
        return tp_error_set(err, TP_STATUS_AMBIGUOUS_SELECTOR,
                            "frame selector '%s' is ambiguous (%d matches)",
                            selector, count);
    }
    if (count == 1) {
        *out_index = match;
        return TP_STATUS_OK;
    }
    return tp_error_set(err, TP_STATUS_NOT_FOUND,
                        "animation has no frame '%s'", selector);
}

tp_status tp_session_snapshot_resolve_target(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *selector, const tp_snapshot_target **out, tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (!snapshot || !selector || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "target query needs snapshot, selector, and output");
    }
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_by_id(snapshot,
                                                                     atlas_id);
    if (!atlas) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "target atlas was not found");
    }
    char *end = NULL;
    const long parsed = strtol(selector, &end, 10);
    if (end != selector && *end == '\0') {
        const tp_snapshot_target *target =
            parsed >= 0 && parsed < atlas->target_count
                ? tp_session_snapshot_target_at(snapshot, atlas_id, (int)parsed)
                : NULL;
        if (target) {
            *out = target;
            return TP_STATUS_OK;
        }
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "atlas has no target at index %ld", parsed);
    }
    tp_selector_result resolved;
    tp_status status = tp_session_snapshot_resolve_selector(
        snapshot, atlas_id, TP_SEL_TARGET, selector, &resolved, NULL, err);
    if (status == TP_STATUS_OK) {
        *out = tp_session_snapshot_target_by_id(snapshot, atlas_id, resolved.id);
        return *out ? TP_STATUS_OK
                    : tp_error_set(err, TP_STATUS_NOT_FOUND,
                                   "resolved target was not present in the snapshot");
    }
    if (status != TP_STATUS_NOT_FOUND) {
        return status;
    }
    int match = -1;
    int count = 0;
    for (int i = 0; i < atlas->target_count; ++i) {
        const tp_snapshot_target *target = tp_session_snapshot_target_at(
            snapshot, atlas_id, i);
        if (target && target->exporter_id &&
            strcmp(target->exporter_id, selector) == 0) {
            match = i;
            count++;
        }
    }
    if (count > 1) {
        return tp_error_set(err, TP_STATUS_AMBIGUOUS_SELECTOR,
                            "target selector '%s' is ambiguous (%d matches)",
                            selector, count);
    }
    if (count == 1) {
        *out = tp_session_snapshot_target_at(snapshot, atlas_id, match);
        return TP_STATUS_OK;
    }
    return tp_error_set(err, TP_STATUS_NOT_FOUND,
                        "atlas has no target '%s'", selector);
}

tp_status tp_session_snapshot_serialize(const tp_session_snapshot *snapshot,
                                        char **out, size_t *out_len,
                                        tp_error *err) {
    if (!snapshot || !out || !out_len) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot serialization requires snapshot and outputs");
    }
    return tp_project_save_buffer(snapshot->project, out, out_len, err);
}

tp_id128 tp_session_snapshot_semantic_identity(
    const tp_session_snapshot *snapshot) {
    return snapshot ? tp_semantic_identity(snapshot->project) : tp_id128_nil();
}
