#include "gui_project.h"
#include "gui_project_internal.h"

#include <stdio.h>
#include <string.h>

#include "gui_scan.h"

#include "core/nt_assert.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_session.h"
#ifdef NTPACKER_GUI_SELFTEST
#include "tp_session_internal.h"
#endif

/* GUI mutation and Undo/Redo run through tp_session; reads use one cached owned
 * snapshot. Field edits coalesce by exact target until the gesture boundary, so
 * one gesture produces one transaction and one Undo step. */

gui_project_state s_project;

// #region helpers
static void reduce_project_display_name(
    gui_project_state *project,
    const tp_session_snapshot *snapshot) {
    NT_ASSERT(project != NULL);
    NT_ASSERT(snapshot != NULL);
    const tp_session_identity identity =
        tp_session_snapshot_identity(snapshot);
    const char *path =
        identity.kind == TP_IDENTITY_SAVED
            ? identity.canonical_path
            : "";
    if (path[0] == '\0') {
        (void)snprintf(
            project->name, sizeof project->name,
            "untitled");
        return;
    }
    const char *base = path;
    for (const char *cursor = path; *cursor;
         ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    (void)snprintf(
        project->name, sizeof project->name,
        "%s", base);
}

static void reduce_recovery_health(
    gui_project_state *project,
    tp_session_recovery_health health) {
    NT_ASSERT(project != NULL);
    if (!health.degraded) {
        /* A failed cross-identity retire can temporarily leave no owner while
         * Save As asks the frontend to rebind. Preserve the exact result notice
         * until a healthy owner is actually available. */
        if (!health.available &&
            project->recovery_notice_active) {
            return;
        }
        project->recovery_notice_active = false;
        project->recovery_notice.notice_id =
            health.notice_id;
        project->recovery_notice.generation =
            health.generation;
        project->recovery_notice.status =
            TP_STATUS_OK;
        project->recovery_notice.message[0] =
            '\0';
        return;
    }
    if (project->recovery_notice_active &&
        strcmp(
            health.notice_id,
            project->recovery_notice.notice_id) == 0 &&
        health.generation ==
            project->recovery_notice.generation &&
        health.first_cause ==
            project->recovery_notice.status) {
        return;
    }
    project->recovery_notice_active = true;
    project->recovery_notice.notice_id =
        health.notice_id;
    project->recovery_notice.generation =
        health.generation;
    project->recovery_notice.status =
        health.first_cause == TP_STATUS_OK
            ? TP_STATUS_JOURNAL_FAILED
            : health.first_cause;
    project->recovery_notice
        .has_last_durable_revision =
        health.has_last_durable_revision;
    project->recovery_notice
        .last_durable_revision =
        health.last_durable_revision;
    (void)snprintf(
        project->recovery_notice.message,
        sizeof project->recovery_notice.message,
        "Crash recovery is degraded (%s). Editing and Undo remain available, but recent unsaved changes may not survive an app or system crash.",
        tp_status_str(
            project->recovery_notice.status));
}

static void reduce_project_observation(
    void *context, const tp_session_observation *observation,
    uint64_t instance_generation) {
    gui_project_state *project = context;
    reduce_recovery_health(
        project,
        tp_session_observation_recovery_health(
            observation));
    const tp_session_snapshot *snapshot =
        tp_session_observation_snapshot(observation);
    if (project->observed_instance_generation !=
        instance_generation) {
        reduce_project_display_name(
            project, snapshot);
        project->observed_instance_generation =
            instance_generation;
        project->observed_revision =
            snapshot
                ? tp_session_snapshot_revision(snapshot)
                : 0;
        return;
    }
    if (tp_session_observation_resync_required(observation)) {
        if (snapshot &&
            project->observed_revision !=
                tp_session_snapshot_revision(snapshot)) {
            project->preview_stale = true;
        }
    }
    const size_t event_count =
        tp_session_observation_event_count(observation);
    for (size_t index = 0U; index < event_count; ++index) {
        const tp_session_event *event =
            tp_session_observation_event_at(
                observation, index);
        if (event &&
            event->kind != TP_SESSION_EVENT_SAVED) {
            project->preview_stale = true;
        }
    }
    if (snapshot) {
        reduce_project_display_name(
            project, snapshot);
        project->observed_revision =
            tp_session_snapshot_revision(snapshot);
    }
}

static void reduce_host_observation(
    void *context,
    const tp_session_observation *observation,
    uint64_t instance_generation) {
    gui_project_state *project = context;
    NT_ASSERT(project != NULL);
    gui_host_queue_reduce_observation(
        &project->host_queue, observation,
        instance_generation);
}

tp_status gui_project__client_init(tp_error *err) {
    if (s_project.client_initialized) {
        return TP_STATUS_OK;
    }
    gui_session_client_init(&s_project.client);
    gui_host_queue_init(&s_project.host_queue);
    tp_status status =
        gui_session_client_register_reducer(
            &s_project.client,
            reduce_project_observation, &s_project, err);
    if (status == TP_STATUS_OK) {
        status = gui_session_client_register_reducer(
            &s_project.client,
            reduce_host_observation, &s_project, err);
    }
    if (status == TP_STATUS_OK) {
        s_project.client_initialized = true;
    }
    return status;
}

void gui_project__snapshot_drop(void) {
    if (!s_project.client_initialized ||
        !s_project.session) {
        return;
    }
    tp_error err = {{0}};
    const tp_status status =
        gui_session_client_request_observe(
            &s_project.client, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
    }
}

const tp_session_snapshot *gui_project_snapshot(void) {
    return gui_session_client_snapshot(
        &s_project.client);
}

uint64_t gui_project_snapshot_lifetime_generation(void) {
    return gui_session_client_snapshot_lifetime_generation(
        &s_project.client);
}

uint64_t gui_project_source_runtime_generation(void) {
    return gui_session_client_source_runtime_generation(
        &s_project.client);
}

tp_status gui_project_frame_begin(tp_error *err) {
    return gui_session_client_frame_begin(
        &s_project.client, err);
}

void gui_project_frame_end(void) {
    gui_session_client_frame_end(
        &s_project.client);
}

bool gui_project_frame_is_pinned(void) {
    return gui_session_client_frame_is_pinned(
        &s_project.client);
}

tp_status gui_project_job_enqueue_pack(
    tp_id128 atlas_id, const char *work_dir,
    const char *preview_exporter_id, tp_error *err) {
    return gui_host_queue_enqueue_pack(
        &s_project.host_queue, atlas_id, work_dir,
        preview_exporter_id, err);
}

tp_status gui_project_job_enqueue_export(
    tp_id128 atlas_id, const char *work_dir,
    tp_error *err) {
    return gui_host_queue_enqueue_export(
        &s_project.host_queue, atlas_id, work_dir,
        err);
}

tp_status gui_project_job_enqueue_cancel(
    tp_error *err) {
    return gui_host_queue_enqueue_cancel(
        &s_project.host_queue, err);
}

tp_status gui_project_host_drain(tp_error *err) {
    if (!s_project.session) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "GUI host has no live session");
    }
    NT_ASSERT(
        s_project.host_queue
                .session_instance_generation ==
        gui_session_client_instance_generation(
            &s_project.client));
    return gui_host_queue_drain(
        &s_project.host_queue, s_project.session, err);
}

tp_status gui_project_host_begin_drain(
    tp_error *err) {
    return gui_host_queue_begin_drain(
        &s_project.host_queue, err);
}

bool gui_project_host_take_completion(
    gui_project_job_completion *out) {
    if (!out) {
        return false;
    }
    gui_host_completion completion = {0};
    if (!gui_host_queue_take_completion(
            &s_project.host_queue, &completion)) {
        return false;
    }
    *out = (gui_project_job_completion){
        .present = completion.present,
        .publish_result = completion.publish_result,
        .session_instance_generation =
            completion.envelope
                .session_instance_generation,
        .request_id = completion.envelope.request_id,
        .kind = completion.envelope.kind,
        .state = completion.state,
        .rejection = completion.rejection,
        .status = completion.status,
        .error = completion.error,
        .result = completion.result,
    };
    completion.result = (tp_session_job_result){0};
    gui_host_completion_destroy(&completion);
    return true;
}

void gui_project_job_completion_destroy(
    gui_project_job_completion *completion) {
    if (!completion) {
        return;
    }
    tp_session_job_result_destroy(
        &completion->result);
    *completion =
        (gui_project_job_completion){0};
}

bool gui_project_job_busy(void) {
    const tp_session_job_observed_state state =
        gui_session_client_job_state(
            &s_project.client);
    if (state.present &&
        state.session_instance_generation ==
            gui_session_client_instance_generation(
                &s_project.client) &&
        !state.terminal) {
        return true;
    }
    /* Before the next atomic observation, ingress and staged receipts are
     * host lifecycle facts rather than a second runtime-state projection.
     * `active` also keeps admission fail-safe if observation refresh failed. */
    return s_project.host_queue.command_count > 0U ||
           s_project.host_queue.staged.present ||
           s_project.host_queue.active;
}

bool gui_project_job_cancelling(void) {
    return gui_host_queue_cancelling(
        &s_project.host_queue);
}

tp_session_job_kind
gui_project_job_active_kind(void) {
    return gui_host_queue_active_kind(
        &s_project.host_queue);
}

tp_session_job_observed_state
gui_project_job_observed_state(void) {
    return gui_session_client_job_state(
        &s_project.client);
}

gui_project_host_lifecycle
gui_project_host_lifecycle_query(void) {
    return (gui_project_host_lifecycle)
        gui_host_queue_lifecycle(
            &s_project.host_queue);
}

uint64_t
gui_project_session_instance_generation(void) {
    return gui_session_client_instance_generation(
        &s_project.client);
}

void gui_project_invalidate_sources(void) {
    gui_scan_invalidate_all();
    if (!s_project.session) {
        return;
    }
    tp_error err = {0};
    const tp_status status = tp_session_invalidate_sources(s_project.session, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return;
    }
    gui_project__snapshot_drop();
}

uint64_t gui_project_snapshot_model_generation(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return snapshot ? tp_session_snapshot_model_generation(snapshot) : 0U;
}

tp_status gui_project_snapshot_serialize(char **out, size_t *out_len,
                                         tp_error *err) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return snapshot ? tp_session_snapshot_serialize(snapshot, out, out_len, err)
                    : tp_error_set(err, TP_STATUS_NOT_FOUND,
                                   "GUI session snapshot is unavailable");
}

/* Record a void-context id-promotion failure so the UI can surface it. */
bool gui_project_take_op_error(char *out, size_t cap) {
    if (!s_project.op_error) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_project.op_error_msg);
    }
    s_project.op_error = false;
    s_project.op_error_status = TP_STATUS_OK;
    return true;
}

/* fix3 [2]: fill `out` with the reason a flush's commit failed -- the drained op-error, else a NEUTRAL
 * fallback that fits save AND pack AND the dirty gate (the flush-failure abort paths share one wording,
 * no "saved"-specific verb). Consumes the op-error like gui_project_take_op_error. NULL-safe. */
void gui_project_flush_error(char *out, size_t cap) {
    if (!out || !cap) {
        return;
    }
    char m[256] = {0};
    if (!gui_project_take_op_error(m, sizeof m)) {
        (void)snprintf(m, sizeof m,
                       "Your last edit could not be committed -- correct it and try again.");
    }
    (void)snprintf(out, cap, "%s", m);
}

void gui_project__note_session_reject(tp_status status, const tp_error *err) {
    const char *message = (err && err->msg[0]) ? err->msg : tp_status_str(status);
    s_project.op_error = true;
    s_project.op_error_status = status;
    (void)snprintf(s_project.op_error_msg, sizeof s_project.op_error_msg, "%s", message);
}

void gui_project__sync_recovery_notice(void) {
    if (!s_project.client_initialized ||
        !s_project.session) {
        return;
    }
    tp_error err = {{0}};
    const tp_status status =
        gui_session_client_request_observe(
            &s_project.client, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(
            status, &err);
    }
}

// #endregion

// #region lifecycle dev seams (selftest only)
#if defined(NTPACKER_GUI_SELFTEST) || defined(TP_ENABLE_TEST_SEAMS)
tp_session *gui_project__test_session(void) { return s_project.session; }

#endif
#ifdef TP_ENABLE_TEST_SEAMS
void gui_project__test_fail_next_observe(void) {
    gui_session_client__test_fail_next_observe();
}

void gui_project__test_fail_observes(
    unsigned int count) {
    gui_session_client__test_fail_observes(count);
}

bool
gui_project__test_host_has_staged_completion(void) {
    return gui_host_queue__test_has_staged(
        &s_project.host_queue);
}
#endif
// #endregion

// #region accessors
const char *gui_project_path(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return tp_session_snapshot_canonical_path(snapshot);
}
const char *gui_project_display_name(void) { return s_project.name; }
bool gui_project_has_path(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return tp_session_snapshot_identity(snapshot).kind == TP_IDENTITY_SAVED;
}
/* Dirty is a scalar captured in the cached immutable snapshot. The first read
 * after a commit refreshes the snapshot; unchanged frames only read the scalar. */
bool gui_project_is_dirty(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return snapshot && tp_session_snapshot_dirty(snapshot);
}
bool gui_project_is_stale(void) { return s_project.preview_stale; }
// #endregion

// #region dirty/stale choke point
/* Post-commit choke point: a REAL committed mutation makes the preview stale and bumps the
 * session generation. Undo history + dirty are core-owned. `act` is vestigial (coalescing moved to the
 * transaction buffer) but kept for call-site clarity + the dev-seam signature. */
void gui_project_mark_packed(void) { s_project.preview_stale = false; }
void gui_project_mark_stale(void) { s_project.preview_stale = true; }
void gui_project_tick(double now_seconds) { s_project.now = now_seconds; }
// #endregion
