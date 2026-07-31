#include "tp_core/tp_session.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "core/nt_assert.h"
#include "tp_diff.h"
#include "tp_project_lease.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_core/tp_srckey.h"
#include "tp_core/tp_transaction.h"
#include "tp_recovery_live_seam.h"
#include "tp_session_layout.h"
#include "tp_source_runtime_internal.h"
#include "tp_session_snapshot_internal.h"
#include "tp_job_owner_internal.h"
#include "tp_model_seam.h"
#include "tp_project_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_mutation_internal.h"

// #region owner thread & recovery health
void tp_session__assert_owner_thread(const tp_session *session) {
    NT_ASSERT(session != NULL);
    NT_ASSERT(thrd_equal(session->owner_thread, thrd_current()));
}

static bool recovery_is_healthy(const tp_session *session) {
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

/* Recovery health has TWO independent authorities: the model counts durability
 * and degradation, the session counts owner attachment/requirement. The DTO
 * exposes ONE `generation`, so it must move when either half moves -- reporting
 * only the model half made owner-side transitions (attach_journal,
 * attach_recovery_live, require_recovery) change `available`/`degraded` while
 * `generation` stayed put, which is exactly the staleness the counter exists to
 * rule out. Both halves are monotonic and saturate at UINT64_MAX, so the sum is
 * monotonic too; it saturates rather than wrapping. */
static uint64_t recovery_health_generation(const tp_session *session) {
    NT_ASSERT(session != NULL);
    const uint64_t model_half =
        tp_model__recovery_health_generation(session->model);
    const uint64_t owner_half = session->recovery_owner_generation;
    if (model_half > UINT64_MAX - owner_half) {
        return UINT64_MAX;
    }
    return model_half + owner_half;
}

tp_session_recovery_health tp_session_recovery_health_query(
    const tp_session *session) {
    tp_session_recovery_health health = {
        .notice_id = TP_SESSION_NOTICE_RECOVERY_DEGRADED,
        .first_cause = TP_STATUS_OK,
    };
    if (!session) {
        return health;
    }
    tp_session__assert_owner_thread(session);
    const bool model_degraded =
        tp_model__recovery_degraded(session->model);
    const bool owner_degraded =
        (session->recovery_live &&
         !tp_recovery_live_healthy(session->recovery_live)) ||
        (tp_model_has_journal(session->model) &&
         !session->recovery_healthy);
    health.available = recovery_is_healthy(session);
    health.degraded = model_degraded || owner_degraded;
    if (model_degraded) {
        health.first_cause = tp_model__recovery_status(session->model);
    } else if (owner_degraded) {
        health.first_cause = TP_STATUS_JOURNAL_FAILED;
    }
    health.has_last_durable_revision =
        tp_model__recovery_durable_revision(
            session->model, &health.last_durable_revision);
    /* No append/checkpoint API currently receives a trustworthy timestamp.
     * Keep the time explicitly unknown instead of sampling a global clock. */
    health.has_last_durable_time = false;
    health.last_durable_time = 0;
    health.generation = recovery_health_generation(session);
    return health;
}

static void observe_model_recovery(tp_session *session) {
    if (!tp_model__recovery_degraded(session->model)) {
        return;
    }
    session->recovery_healthy = false;
}

static void bump_recovery_owner_generation(tp_session *session) {
    NT_ASSERT(session != NULL);
    if (session->recovery_owner_generation < UINT64_MAX) {
        session->recovery_owner_generation++;
    }
}

bool tp_session__owns_recovery_live(const tp_session *session,
                                    const tp_recovery_live *live) {
    if (!session || !live) {
        return false;
    }
    tp_session__assert_owner_thread(session);
    return session->recovery_live == live;
}

bool tp_session__has_recovery_owner(const tp_session *session) {
    if (!session) {
        return false;
    }
    tp_session__assert_owner_thread(session);
    return session->recovery_live != NULL ||
           tp_model_has_journal(session->model);
}

const char *tp_session__recovery_journal_path(const tp_session *session) {
    if (!session) {
        return NULL;
    }
    tp_session__assert_owner_thread(session);
    return session->recovery_live
               ? tp_recovery_live_journal_path(session->recovery_live)
               : NULL;
}
// #endregion

// #region event log
static void publish_event(tp_session *session, tp_session_event_kind kind,
                          const char *transaction_id, int64_t revision_before,
                          int64_t revision_after, const char *label,
                          const char *author) {
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
    if (label) {
        (void)snprintf(event->label, sizeof event->label, "%s", label);
    }
    if (author) {
        (void)snprintf(event->author, sizeof event->author, "%s", author);
    }
}
// #endregion

// #region visible history markers
/* Session-owned NON-undoable rows (Save checkpoints §9.2, runtime refreshes §9.3).
 * The model's undo stack owns the edit rows and the cursor; these markers only
 * annotate it. All helpers run on the owner thread. */
static char *history_path_dup(const char *path) {
    if (!path) {
        return NULL;
    }
    const size_t len = strlen(path) + 1U;
    char *copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, path, len);
    }
    return copy;
}

static void history_marker_drop_at(tp_session *session, size_t index) {
    free(session->markers[index].path);
    const size_t tail = session->marker_count - index - 1U;
    if (tail > 0U) {
        memmove(&session->markers[index], &session->markers[index + 1U],
                tail * sizeof session->markers[0]);
    }
    session->marker_count--;
    session->markers[session->marker_count].path = NULL;
}

static void history_markers_clear(tp_session *session) {
    for (size_t i = 0U; i < session->marker_count; i++) {
        free(session->markers[i].path);
        session->markers[i].path = NULL;
    }
    session->marker_count = 0U;
}

/* Append a marker at the live cursor tip. A NULL path is a refresh marker (never
 * allocates); a checkpoint whose path dup fails is silently skipped -- visible
 * History is advisory session state and the Save/refresh already succeeded. FIFO
 * evicts the oldest marker when the fixed cap is reached. */
static void history_marker_append(tp_session *session, tp_session_history_kind kind,
                                  tp_id128 state_identity, const char *path) {
    char *path_copy = history_path_dup(path);
    if (path && !path_copy) {
        return; /* best-effort: drop the row rather than fail the command */
    }
    if (session->marker_count == (size_t)TP_SESSION_HISTORY_MARKER_CAP) {
        history_marker_drop_at(session, 0U);
    }
    tp_session_history_marker *marker = &session->markers[session->marker_count++];
    marker->kind = kind;
    marker->anchor_pos = tp_model_history_position(session->model);
    marker->revision = tp_model_revision(session->model);
    marker->state_identity = state_identity;
    marker->path = path_copy;
}

static void history_record_checkpoint(tp_session *session, const char *canonical) {
    history_marker_append(session, TP_SESSION_HISTORY_SAVE_CHECKPOINT,
                          tp_semantic_identity(tp_model_project(session->model)),
                          canonical);
}

static void history_record_refresh(tp_session *session) {
    history_marker_append(session, TP_SESSION_HISTORY_RUNTIME_REFRESH,
                          tp_id128_nil(), NULL);
}

/* Reconcile markers with the edit stack after a new edit commits. `pos_before` is
 * the cursor before the push, `pos_after` after it. A new edit (1) discards the
 * redo branch -- drop markers anchored above `pos_before` -- and (2) may FIFO-evict
 * `drop_oldest = pos_before + 1 - pos_after` front records -- drop markers anchored
 * inside that evicted window and shift the rest down. Undo/Redo never call this:
 * they move only the cursor, so markers (and their undone/undoable projection)
 * follow for free. Iterating high->low keeps mid-list compaction safe. */
static void history_markers_after_commit(tp_session *session, int pos_before,
                                         int pos_after) {
    for (size_t i = session->marker_count; i-- > 0U;) {
        if (session->markers[i].anchor_pos > pos_before) {
            history_marker_drop_at(session, i);
        }
    }
    const int drop_oldest = pos_before + 1 - pos_after;
    if (drop_oldest <= 0) {
        return;
    }
    for (size_t i = session->marker_count; i-- > 0U;) {
        if (session->markers[i].anchor_pos < drop_oldest) {
            history_marker_drop_at(session, i);
        } else {
            session->markers[i].anchor_pos -= drop_oldest;
        }
    }
}

static void history_fill_edit(const tp_session *session, int index, int pos,
                              tp_session_history_entry *out) {
    tp_model_history_entry entry;
    (void)tp_model_history_entry_at(session->model, index, &entry);
    memset(out, 0, sizeof *out);
    out->kind = TP_SESSION_HISTORY_EDIT;
    out->revision = entry.revision;
    if (entry.label) {
        (void)snprintf(out->label, sizeof out->label, "%s", entry.label);
    }
    if (entry.author) {
        (void)snprintf(out->author, sizeof out->author, "%s", entry.author);
    }
    (void)snprintf(out->transaction_id, sizeof out->transaction_id, "%s",
                   entry.transaction_id ? entry.transaction_id : "");
    out->undoable = index < pos;
    out->undone = index >= pos;
}

static void history_fill_marker(const tp_session *session, size_t mi, int pos,
                                tp_session_history_entry *out) {
    const tp_session_history_marker *marker = &session->markers[mi];
    memset(out, 0, sizeof *out);
    out->kind = marker->kind;
    out->revision = marker->revision;
    out->state_identity = marker->state_identity;
    if (marker->path) {
        (void)snprintf(out->path, sizeof out->path, "%s", marker->path);
    }
    out->undoable = false;
    out->undone = marker->anchor_pos > pos;
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
    /* The creating thread becomes the owner. Everything below asserts against
     * it, so the capture must precede the first entry-point call. */
    session->owner_thread = thrd_current();
    status = tp_session_identity_init_unsaved(&session->identity, rng, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        free(session);
        return status;
    }
    session->model = tp_model_wrap(project);
    if (!session->model) {
        tp_project_destroy(project);
        free(session);
        return tp_error_set(err, TP_STATUS_OOM, "session model allocation failed");
    }
    status = tp_model_enable_history(session->model);
    if (status != TP_STATUS_OK) {
        tp_model_destroy(session->model);
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
    tp_session__assert_owner_thread(session);
    if (session->active_job) {
        tp_session_owned_job *job = session->active_job;
        session->active_job = NULL;
        job->cancel(job);
        tp_session_job_release_internal(job);
    }
    tp_session_snapshot_destroy(session->view_snapshot);
    tp_source_runtime_destroy(session->source_projection);
    if (session->recovery_live) {
        const bool preserve = tp_model__recovery_degraded(session->model) ||
                              !tp_recovery_live_healthy(session->recovery_live) ||
                              session->file_durability_uncertain ||
                              (tp_model_dirty(session->model) && !session->discarded);
        (void)tp_recovery_live_finish(session->recovery_live, preserve, NULL);
        tp_recovery_live_destroy(session->recovery_live);
    }
    history_markers_clear(session);
    tp_model_destroy(session->model);
    tp_project_lease_release(session->project_lease);
    free(session);
}
// #endregion

// #region jobs
static bool session_job_targets_exist(
    const tp_session *session,
    const tp_session_job_descriptor *descriptor) {
    const tp_project *project =
        tp_model_project(session->model);
    for (size_t index = 0U;
         index < descriptor->target_count;
         ++index) {
        const tp_session_job_target *target =
            &descriptor->targets[index];
        const tp_project_atlas *atlas =
            tp_project_atlas_by_id(
                project, target->atlas_id);
        if (!atlas) {
            return false;
        }
        if (target->kind ==
            TP_SESSION_JOB_TARGET_EXPORT_TARGET) {
            if (tp_id128_is_nil(target->id) ||
                !tp_project_atlas_target_by_id(
                    atlas, target->id)) {
                return false;
            }
        } else if (target->kind !=
                   TP_SESSION_JOB_TARGET_ATLAS) {
            return false;
        }
    }
    return true;
}

static void session_job_state_from_sample(
    tp_session *session,
    const tp_session_owned_job *job,
    const tp_session_job_sample *sample) {
    tp_session_job_observed_state *state =
        &session->observed_job_state;
    const tp_session_job_descriptor *descriptor =
        &job->observation_descriptor;
    state->present = true;
    state->session_instance_generation =
        descriptor->session_instance_generation;
    state->request_id = descriptor->request_id;
    state->kind = descriptor->kind;
    state->state = sample->state;
    state->current = sample->current;
    state->total = sample->total;
    state->cancellation_requested =
        sample->cancellation_requested;
    state->terminal =
        sample->state != TP_SESSION_JOB_RUNNING;
    state->base_input_token =
        descriptor->base_input_token;
    state->terminal_status =
        sample->terminal_status;
    state->terminal_error =
        sample->terminal_error;
}

static bool session_job_state_equal(
    const tp_session_job_observed_state *left,
    const tp_session_job_observed_state *right) {
    return left->present == right->present &&
           left->session_instance_generation ==
               right->session_instance_generation &&
           left->request_id == right->request_id &&
           left->kind == right->kind &&
           left->state == right->state &&
           left->current == right->current &&
           left->total == right->total &&
           left->cancellation_requested ==
               right->cancellation_requested &&
           left->terminal == right->terminal &&
           left->result_accepted ==
               right->result_accepted &&
           left->rejection == right->rejection &&
           tp_session_input_token_equal(
               left->base_input_token,
               right->base_input_token) &&
           left->terminal_status ==
               right->terminal_status &&
           strcmp(left->terminal_error.msg,
                  right->terminal_error.msg) == 0 &&
           left->terminal_error.file_io.phase ==
               right->terminal_error.file_io.phase &&
           strcmp(left->terminal_error.file_io.path,
                  right->terminal_error.file_io.path) == 0 &&
           left->terminal_error.file_io.native_code ==
               right->terminal_error.file_io.native_code;
}

static bool session_recovery_health_equal(
    const tp_session_recovery_health *left,
    const tp_session_recovery_health *right) {
    return left->notice_id == right->notice_id &&
           left->available == right->available &&
           left->degraded == right->degraded &&
           left->first_cause == right->first_cause &&
           left->has_last_durable_revision ==
               right->has_last_durable_revision &&
           left->last_durable_revision ==
               right->last_durable_revision &&
           left->has_last_durable_time ==
               right->has_last_durable_time &&
           left->last_durable_time ==
               right->last_durable_time &&
           left->generation == right->generation;
}

tp_status tp_session_update(
    tp_session *session,
    tp_session_job_result *optional_owned_completion,
    tp_error *err) {
    if (!session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "session update requires session");
    }
    tp_session__assert_owner_thread(session);
    if (optional_owned_completion) {
        memset(optional_owned_completion, 0,
               sizeof *optional_owned_completion);
    }
    const tp_status snapshot_status =
        tp_session_view__refresh_snapshot(
            session, err);
    if (snapshot_status != TP_STATUS_OK) {
        return snapshot_status;
    }

    const tp_session_job_observed_state prior_task =
        session->observed_job_state;
    const tp_session_recovery_health prior_recovery =
        session->view.recovery_health;
    tp_session_owned_job *job =
        tp_session_job_acquire_internal(session);
    if (job) {
        if (job->pump) {
            job->pump(job);
        }
        tp_session_job_sample sample = {0};
        if (job->observe &&
            job->observe(job, &sample)) {
            if (sample.state ==
                TP_SESSION_JOB_RUNNING) {
                session_job_state_from_sample(
                    session, job, &sample);
            } else {
                tp_session_job_rejection rejection =
                    TP_SESSION_JOB_REJECTION_NONE;
                if (sample.cancellation_requested ||
                    sample.state ==
                        TP_SESSION_JOB_CANCELLED) {
                    rejection =
                        TP_SESSION_JOB_REJECTION_CANCELLED;
                } else if (
                    job->observation_descriptor.kind ==
                        TP_SESSION_JOB_REFRESH &&
                    job->observation_descriptor.base_input_token
                            .model_generation !=
                        session->model_generation) {
                    rejection =
                        TP_SESSION_JOB_REJECTION_INPUT_CHANGED;
                } else if (!session_job_targets_exist(
                               session,
                               &job->observation_descriptor)) {
                    rejection =
                        TP_SESSION_JOB_REJECTION_TARGET_DELETED;
                }
                if (rejection !=
                        TP_SESSION_JOB_REJECTION_NONE &&
                    sample.terminal_result &&
                    job->release_payload) {
                    job->release_payload(job);
                    (void)job->observe(job, &sample);
                    session_job_state_from_sample(
                        session, job, &sample);
                }
                tp_session_snapshot *prepared_refresh_snapshot =
                    NULL;
                if (rejection ==
                        TP_SESSION_JOB_REJECTION_NONE &&
                    sample.terminal_result &&
                    sample.terminal_result->kind ==
                        TP_SESSION_JOB_REFRESH &&
                    sample.terminal_result->state ==
                        TP_SESSION_JOB_SUCCEEDED &&
                    sample.terminal_result->refresh.projection) {
                    const tp_status prepare_status =
                        tp_session_view__prepare_source_refresh(
                            session,
                            &prepared_refresh_snapshot, err);
                    if (prepare_status != TP_STATUS_OK) {
                        tp_session_job_release_internal(job);
                        return prepare_status;
                    }
                }
                session_job_state_from_sample(
                    session, job, &sample);
                session->observed_job_state.rejection =
                    rejection;
                session->observed_job_state
                    .result_accepted =
                    rejection ==
                        TP_SESSION_JOB_REJECTION_NONE &&
                    sample.terminal_result != NULL;
                if (rejection ==
                        TP_SESSION_JOB_REJECTION_NONE &&
                    sample.terminal_result &&
                    sample.terminal_result->kind ==
                        TP_SESSION_JOB_REFRESH &&
                    sample.terminal_result->state ==
                        TP_SESSION_JOB_SUCCEEDED &&
                    sample.terminal_result->refresh.projection) {
                    NT_ASSERT(
                        prepared_refresh_snapshot != NULL);
                    tp_source_runtime_projection *retired_projection =
                        session->source_projection;
                    session->source_projection =
                        sample.terminal_result->refresh.projection;
                    sample.terminal_result->refresh.projection =
                        NULL;
                    session->admission_sequence++;
                    session->source_generation++;
                    const int64_t revision =
                        tp_model_revision(session->model);
                    publish_event(
                        session,
                        TP_SESSION_EVENT_SOURCE_RUNTIME_CHANGED,
                        NULL, revision, revision, NULL, NULL);
                    history_record_refresh(session);
                    session->view.sources =
                        session->source_projection;
                    tp_session_view__adopt_prepared_source_refresh(
                        session, prepared_refresh_snapshot);
                    prepared_refresh_snapshot = NULL;
                    tp_source_runtime_destroy(
                        retired_projection);
                }
                NT_ASSERT(
                    prepared_refresh_snapshot == NULL);
                NT_ASSERT(session->active_job == job);
                session->active_job = NULL;
                tp_session_job_release_internal(job);
                if (sample.terminal_result &&
                    optional_owned_completion) {
                    *optional_owned_completion =
                        *sample.terminal_result;
                    optional_owned_completion->rejection =
                        rejection;
                    optional_owned_completion->_owner =
                        (tp_session_job_result_handle *)job;
                    job = NULL;
                }
            }
        }
        tp_session_job_release_internal(job);
    }

    const tp_status refreshed_snapshot_status =
        tp_session_view__refresh_snapshot(session, err);
    if (refreshed_snapshot_status != TP_STATUS_OK) {
        return refreshed_snapshot_status;
    }
    session->view.sources =
        session->source_projection;

    session->view.task =
        session->observed_job_state;
    session->view.recovery_health =
        tp_session_recovery_health_query(session);
    if ((!session_job_state_equal(
             &prior_task, &session->view.task) ||
         !session_recovery_health_equal(
             &prior_recovery,
             &session->view.recovery_health)) &&
        session->view.generation < UINT64_MAX) {
        ++session->view.generation;
    }
    return TP_STATUS_OK;
}

const struct tp_session_view *tp_session_view(
    const tp_session *session) {
    if (!session) {
        return NULL;
    }
    tp_session__assert_owner_thread(session);
    return &session->view;
}

void tp_session_owned_job_init(tp_session_owned_job *job,
                               void (*cancel)(tp_session_owned_job *job),
                               void (*destroy)(tp_session_owned_job *job)) {
    if (!job) {
        return;
    }
    atomic_init(&job->refs, 1U);
    job->cancel = cancel;
    job->pump = NULL;
    job->destroy = destroy;
    memset(&job->observation_descriptor, 0,
           sizeof job->observation_descriptor);
    job->observe = NULL;
}

void tp_session_owned_job_configure_observation(
    tp_session_owned_job *job,
    const tp_session_job_descriptor *descriptor,
    tp_session_job_observe_fn observe) {
    NT_ASSERT(job != NULL);
    NT_ASSERT(descriptor != NULL);
    NT_ASSERT(descriptor->kind != TP_SESSION_JOB_NONE);
    NT_ASSERT(
        descriptor->target_count == 0U ||
        descriptor->targets != NULL);
    NT_ASSERT(observe != NULL);
    job->observation_descriptor = *descriptor;
    job->observe = observe;
}

void tp_session_job_retain_internal(tp_session_owned_job *job) {
    if (!job) {
        return;
    }
    const unsigned previous = atomic_fetch_add_explicit(
        &job->refs, 1U, memory_order_relaxed);
    NT_ASSERT(previous > 0U);
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

tp_status tp_session_job_start_internal(
    tp_session *session, tp_session_owned_job *job,
    tp_session_job_start_fn start, void *start_context,
    tp_error *err) {
    if (!session || !job || !start) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "job start requires session, owner, and start callback");
    }
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session was discarded");
    }
    if (session->active_job) {
        return tp_error_set(err, TP_STATUS_BUSY,
                            "a session task is already active");
    }
    if (!job->observe ||
        job->observation_descriptor.kind ==
            TP_SESSION_JOB_NONE ||
        (job->observation_descriptor.target_count > 0U &&
         !job->observation_descriptor.targets)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "job start requires descriptor and observe callback");
    }
    /* Reserve the request identity before process creation so the exact
     * admitted identity is encoded into the child request. A failed spawn may
     * consume an id, but publishes no job/result state. Cannot fail twice:
     * __begin below re-enters the same reservation helper and finds the id
     * already non-zero. */
    if (job->observation_descriptor.request_id == 0U) {
        if (session->next_job_request_id == UINT64_MAX) {
            return tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "session job request id space is exhausted");
        }
        job->observation_descriptor.request_id =
            ++session->next_job_request_id;
    }
    /* Process creation is fail-atomic with observable publication. The start
     * callback only spawns/encodes; it never pumps or calls the session. */
    const tp_status start_status = start(start_context, err);
    if (start_status != TP_STATUS_OK) {
        return start_status;
    }
    session->active_job = job;
    session->observed_job_state =
        (tp_session_job_observed_state){
            .present = true,
            .session_instance_generation =
                job->observation_descriptor
                    .session_instance_generation,
            .request_id =
                job->observation_descriptor.request_id,
            .kind = job->observation_descriptor.kind,
            .state = TP_SESSION_JOB_RUNNING,
            .base_input_token =
                job->observation_descriptor.base_input_token,
        };
    return TP_STATUS_OK;
}

#ifdef TP_ENABLE_TEST_SEAMS
/* See tp_job_owner_internal.h: test-only lease adoption, no shipping caller. */
tp_status tp_session_job_attach_internal(tp_session *session,
                                         tp_session_owned_job *job,
                                         tp_error *err) {
    if (!session || !job || !job->cancel || !job->destroy) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session job attach requires a concrete job handle");
    }
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session was discarded");
    }
    if (session->active_job) {
        return tp_error_set(err, TP_STATUS_BUSY,
                            "a session task is already active");
    }
    session->active_job = job;
    return TP_STATUS_OK;
}
#endif /* TP_ENABLE_TEST_SEAMS */

tp_session_owned_job *tp_session_job_acquire_internal(
    const tp_session *session) {
    if (!session) {
        return NULL;
    }
    tp_session__assert_owner_thread(session);
    tp_session_owned_job *job = session->active_job;
    if (job) {
        tp_session_job_retain_internal(job);
    }
    return job;
}

tp_status tp_session_job_detach_internal(tp_session *session,
                                         tp_session_owned_job *expected,
                                         tp_error *err) {
    if (!session || !expected) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session job detach requires session and handle");
    }
    tp_session__assert_owner_thread(session);
    if (session->active_job != expected) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "session no longer owns that job handle");
    }
    session->active_job = NULL;
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
    tp_session__assert_owner_thread(session);
    tp_status status = tp_model_attach_journal(session->model, journal, err);
    if (status == TP_STATUS_OK) {
        session->recovery_healthy = true;
        bump_recovery_owner_generation(session);
    }
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
    tp_session__assert_owner_thread(session);
    if (session->recovery_live || tp_model_has_journal(session->model)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session already has recovery attached");
    }
    session->recovery_live = live;
    tp_status status = tp_recovery_live_attach(live, session->model, metadata, err);
    session->recovery_healthy = status == TP_STATUS_OK;
    if (status != TP_STATUS_OK) {
        tp_model__degrade_recovery(session->model, status);
    }
    bump_recovery_owner_generation(session);
    return status;
}

tp_status tp_session_require_recovery(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery requirement needs a session");
    }
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session was discarded");
    }
    if (!session->recovery_required) {
        session->recovery_required = true;
        bump_recovery_owner_generation(session);
    }
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
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    const int history_pos_before = tp_model_history_position(session->model);
    tp_txn_result local_result;
    tp_txn_result *published_result = result ? result : &local_result;
    tp_status status = tp_model_apply(session->model, request,
                                      published_result, err);
    observe_model_recovery(session);
    if (status == TP_STATUS_OK && published_result->committed) {
        session->model_generation++;
        publish_event(session, TP_SESSION_EVENT_MODEL_COMMITTED, request->id_hex,
                      revision_before, tp_model_revision(session->model),
                      request->label, request->author);
        /* A committed edit may have discarded a redo branch and/or FIFO-evicted
         * old edit records; drop the markers those rows carried so visible
         * History mirrors the edit stack. */
        history_markers_after_commit(session, history_pos_before,
                                     tp_model_history_position(session->model));
    }
    if (!result) {
        tp_txn_result_free(&local_result);
    }
    return status;
}

tp_status tp_session_undo(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "undo requires session");
    }
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    tp_status status = tp_model_undo(session->model, err);
    observe_model_recovery(session);
    if (status == TP_STATUS_OK) {
        session->model_generation++;
        publish_event(session, TP_SESSION_EVENT_UNDONE, NULL, revision_before,
                      tp_model_revision(session->model), NULL, NULL);
    }
    return status;
}

tp_status tp_session_redo(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "redo requires session");
    }
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    const int64_t revision_before = tp_model_revision(session->model);
    tp_status status = tp_model_redo(session->model, err);
    observe_model_recovery(session);
    if (status == TP_STATUS_OK) {
        session->model_generation++;
        publish_event(session, TP_SESSION_EVENT_REDONE, NULL, revision_before,
                      tp_model_revision(session->model), NULL, NULL);
    }
    return status;
}
// #endregion

// #region save & identity
static void remap_save_error_path(tp_status status, const char *public_path,
                                  tp_error *err) {
    if (status == TP_STATUS_FILE_IO_FAILED && err) {
        (void)snprintf(err->file_io.path, sizeof err->file_io.path, "%s",
                       public_path ? public_path : "");
        /* snprintf truncates on bytes, so an over-long path can be cut mid
         * codepoint and leave the field invalid UTF-8 -- the same reason
         * tp_error_set_file_io trims. This rewrite must obey the same rule. */
        tp_error_trim_partial_utf8(err->file_io.path);
    }
}

/* Shared publication body for Save, Save As, and Save New: canonicalizes the
 * destination, publishes the file, then rebinds identity/lease/fingerprint,
 * recovery, and the visible checkpoint. The three entry points differ only in
 * how they choose `path` and `create_only`. */
static tp_status save_to_path(tp_session *session, const char *path,
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
    session->file_durability_uncertain = file_durability_degraded;
    const bool model_was_degraded =
        tp_model__recovery_degraded(session->model);
    bool recovery_degraded = !recovery_is_healthy(session);
    bool recovery_rebind_required = false;
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
        tp_recovery_live *retired_live = session->recovery_live;
        tp_error cleanup_error = {{0}};
        const tp_status cleanup_status =
            tp_recovery_live_retire(retired_live, &cleanup_error);
        /* Retire always finishes the old-identity owner and detaches its
         * journal, including when physical cleanup reports a failure. Do not
         * leave that terminal handle installed: a frontend must be able to
         * attach a fresh slot for the newly published identity. */
        tp_recovery_live_destroy(retired_live);
        session->recovery_live = NULL;
        session->recovery_healthy = false;
        recovery_rebind_required = true;
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
                    tp_model__degrade_recovery(session->model,
                                               metadata_status);
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
        result->recovery_rebind_required = recovery_rebind_required;
        (void)snprintf(result->target_path, sizeof result->target_path, "%s", canonical);
        result->file_fingerprint = fingerprint;
        result->recovery_token = session->recovery_token;
        result->has_recovery_token = session->has_recovery_token;
    }
    const int64_t revision = tp_model_revision(session->model);
    publish_event(session, TP_SESSION_EVENT_SAVED, NULL, revision, revision,
                  NULL, NULL);
    /* Only a successful publication reaches this choke point (a failed Save returned
     * earlier), so the visible checkpoint (§9.2) is never recorded for a failed Save.
     * Non-undoable: it does not touch revision, the cursor, or the edit budget. */
    history_record_checkpoint(session, canonical);
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
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    session->admission_sequence++;
    return save_to_path(session, path, create_only, result, err);
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
    tp_session__assert_owner_thread(session);
    if (!session->has_recovery_token || session->discarded) {
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
            /* The adopted project + mark_saved + model_generation bump are only
             * VISIBLE to observers once an event moves the observation token --
             * mirror tp_session_save. Without it every observer keeps serving the
             * pre-save dirty/freshness state until some unrelated event lands. */
            const int64_t revision = tp_model_revision(session->model);
            publish_event(session, TP_SESSION_EVENT_SAVED, NULL, revision,
                          revision, NULL, NULL);
        }
        tp_project_destroy(candidate);
    }
    return status;
}

tp_status tp_session_save(tp_session *session, tp_session_save_result *result,
                          tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "Save requires session");
    }
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "session was discarded");
    }
    if (session->identity.kind != TP_IDENTITY_SAVED ||
        session->identity.canonical_path[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "unsaved session requires Save As");
    }
    session->admission_sequence++;
    return save_to_path(session, session->identity.canonical_path, false,
                        result, err);
}
// #endregion

// #region session commands
tp_status tp_session_discard(tp_session *session, tp_error *err) {
    if (!session) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "Discard requires session");
    }
    tp_session__assert_owner_thread(session);
    if (session->discarded) {
        return TP_STATUS_OK;
    }
    session->admission_sequence++;
    session->discarded = true;
    const int64_t revision = tp_model_revision(session->model);
    publish_event(session, TP_SESSION_EVENT_DISCARDED, NULL, revision,
                  revision, NULL, NULL);
    return TP_STATUS_OK;
}

// #endregion

// #region queries
int64_t tp_session_revision(const tp_session *session) {
    if (!session) {
        return 0;
    }
    tp_session__assert_owner_thread(session);
    return tp_model_revision(session->model);
}

bool tp_session_recovery_available(const tp_session *session) {
    if (!session) {
        return false;
    }
    tp_session__assert_owner_thread(session);
    return recovery_is_healthy(session);
}

bool tp_session_can_undo(const tp_session *session) {
    if (!session) {
        return false;
    }
    tp_session__assert_owner_thread(session);
    return !session->discarded && tp_model_can_undo(session->model);
}

bool tp_session_can_redo(const tp_session *session) {
    if (!session) {
        return false;
    }
    tp_session__assert_owner_thread(session);
    return !session->discarded && tp_model_can_redo(session->model);
}

int tp_session_undo_depth(const tp_session *session) {
    if (!session) {
        return 0;
    }
    tp_session__assert_owner_thread(session);
    return tp_model_undo_depth(session->model);
}

int tp_session_redo_depth(const tp_session *session) {
    if (!session) {
        return 0;
    }
    tp_session__assert_owner_thread(session);
    return tp_model_redo_depth(session->model);
}

int tp_session_history_count(const tp_session *session) {
    if (!session) {
        return 0;
    }
    tp_session__assert_owner_thread(session);
    return tp_model_history_count(session->model) + (int)session->marker_count;
}

tp_status tp_session_history_at(const tp_session *session, int index,
                                tp_session_history_entry *out, tp_error *err) {
    if (!session || !out || index < 0) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "history query requires a session, output, and a non-negative index");
    }
    tp_session__assert_owner_thread(session);
    const int pos = tp_model_history_position(session->model);
    const int edit_count = tp_model_history_count(session->model);
    /* Chronological spine: markers anchored at cursor depth `a` render just before
     * edit record `a`; markers anchored at the tip render after the last edit. */
    int cursor = 0;
    for (int a = 0; a <= edit_count; a++) {
        for (size_t mi = 0U; mi < session->marker_count; mi++) {
            if (session->markers[mi].anchor_pos != a) {
                continue;
            }
            if (cursor == index) {
                history_fill_marker(session, mi, pos, out);
                return TP_STATUS_OK;
            }
            cursor++;
        }
        if (a < edit_count) {
            if (cursor == index) {
                history_fill_edit(session, a, pos, out);
                return TP_STATUS_OK;
            }
            cursor++;
        }
    }
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "history index %d is out of range (%d rows)", index,
                        cursor);
}

uint64_t tp_session_event_sequence(const tp_session *session) {
    if (!session) {
        return 0U;
    }
    tp_session__assert_owner_thread(session);
    return session->event_sequence;
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
    tp_session__assert_owner_thread(session);
    if (after_sequence > session->event_sequence) {
        *out_resync_required = true;
        return TP_STATUS_OK;
    }
    if (after_sequence == session->event_sequence) {
        return TP_STATUS_OK;
    }
    const uint64_t oldest = session->event_count > 0U
                                ? session->events[session->event_start].sequence
                                : session->event_sequence + 1U;
    if (after_sequence + 1U < oldest) {
        *out_resync_required = true;
        return TP_STATUS_OK;
    }
    for (size_t i = 0U; i < session->event_count && *out_count < capacity; ++i) {
        const size_t slot = (session->event_start + i) % TP_SESSION_EVENT_CAPACITY;
        if (session->events[slot].sequence > after_sequence) {
            out[*out_count] = session->events[slot];
            (*out_count)++;
        }
    }
    return TP_STATUS_OK;
}
// #endregion
