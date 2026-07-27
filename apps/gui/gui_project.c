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

/* GUI mutation and Undo/Redo run through tp_session; reads use one atomically observed
 * immutable snapshot. Feature-local draft owners submit one typed transaction per gesture. */

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
    const tp_session_recovery_health recovery =
        tp_session_observation_recovery_health(
            observation);
    const bool new_instance =
        project->observed_instance_generation !=
        instance_generation;
    if (new_instance &&
        project->recovery_required &&
        !recovery.available) {
        gui_project_note_recovery_setup_failure(
            "required recovery is unavailable for the active project");
    }
    reduce_recovery_health(project, recovery);
    const tp_session_snapshot *snapshot =
        tp_session_observation_snapshot(observation);
    if (new_instance) {
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

tp_status gui_project__client_init(tp_error *err) {
    if (s_project.binding_initialized) {
        return TP_STATUS_OK;
    }
    gui_host_binding_init(&s_project.binding);
    tp_status status =
        gui_session_client_register_reducer(
            &s_project.binding.client,
            reduce_project_observation, &s_project, err);
    if (status == TP_STATUS_OK) {
        s_project.binding_initialized = true;
    }
    return status;
}

tp_session *gui_project__borrow_active_session(void) {
    return gui_session_client_attached_session(
        &s_project.binding.client);
}

bool gui_project__ingress_is_open(void) {
    return s_project.binding_initialized &&
           gui_host_binding_lifecycle(
               &s_project.binding) ==
               GUI_HOST_OPEN;
}

static void request_observation(void) {
    if (!s_project.binding_initialized ||
        !gui_project__borrow_active_session()) {
        return;
    }
    tp_error err = {{0}};
    const tp_status status =
        gui_session_client_request_observe(
            &s_project.binding.client, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
    }
}

const tp_session_snapshot *gui_project_snapshot(void) {
    return gui_session_client_snapshot(
        &s_project.binding.client);
}

uint64_t gui_project_snapshot_lifetime_generation(void) {
    return gui_session_client_snapshot_lifetime_generation(
        &s_project.binding.client);
}

uint64_t gui_project_source_runtime_generation(void) {
    return gui_session_client_source_runtime_generation(
        &s_project.binding.client);
}

bool gui_project_observed_input_token(
    tp_session_input_token *out) {
    if (!out) {
        return false;
    }
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    if (!snapshot) {
        *out = (tp_session_input_token){0};
        return false;
    }
    *out =
        tp_session_snapshot_input_token(snapshot);
    out->source_generation =
        gui_project_source_runtime_generation();
    return true;
}

tp_status gui_project_frame_begin(tp_error *err) {
    return gui_session_client_frame_begin(
        &s_project.binding.client, err);
}

void gui_project_frame_end(void) {
    gui_session_client_frame_end(
        &s_project.binding.client);
}

bool gui_project_frame_is_pinned(void) {
    return gui_session_client_frame_is_pinned(
        &s_project.binding.client);
}

tp_status gui_project_register_observation_reducer(
    gui_session_client_reducer_fn reduce, void *context, tp_error *err) {
    return gui_session_client_register_reducer(
        &s_project.binding.client, reduce, context, err);
}

bool gui_project_submit_receipt_query(
    const char transaction_id[33],
    gui_session_submit_identity identity,
    gui_session_submit_terminal *out) {
    return gui_session_client_pending_submit_query(
        &s_project.binding.client, transaction_id, identity, out);
}

tp_status gui_project_job_enqueue_pack(
    tp_id128 atlas_id, const char *work_dir,
    const char *preview_exporter_id, tp_error *err) {
    if (!gui_project__ingress_is_open()) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session ingress is closed during lifecycle transition");
    }
    return gui_host_queue_enqueue_pack(
        &s_project.binding.queue, atlas_id, work_dir,
        preview_exporter_id, err);
}

tp_status gui_project_job_enqueue_export(
    tp_id128 atlas_id, const char *work_dir,
    tp_error *err) {
    if (!gui_project__ingress_is_open()) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI session ingress is closed during lifecycle transition");
    }
    return gui_host_queue_enqueue_export(
        &s_project.binding.queue, atlas_id, work_dir,
        err);
}

tp_status gui_project_job_enqueue_cancel(
    tp_error *err) {
    return gui_host_queue_enqueue_cancel(
        &s_project.binding.queue, err);
}

bool gui_project_host_take_completion(
    gui_host_completion *out) {
    return out && gui_host_queue_take_completion(
                      &s_project.binding.queue, out);
}

bool gui_project_job_busy(void) {
    const tp_session_job_observed_state state =
        gui_session_client_job_state(
            &s_project.binding.client);
    if (state.present &&
        state.session_instance_generation ==
            gui_session_client_instance_generation(
                &s_project.binding.client) &&
        !state.terminal) {
        return true;
    }
    /* Before the next atomic observation, ingress and staged receipts are
     * host lifecycle facts rather than a second runtime-state projection.
     * `active` also keeps admission fail-safe if observation refresh failed. */
    return gui_host_queue_busy(
        &s_project.binding.queue);
}

tp_session_job_kind
gui_project_job_active_kind(void) {
    return gui_host_queue_active_kind(
        &s_project.binding.queue);
}

tp_session_job_observed_state
gui_project_job_observed_state(void) {
    return gui_session_client_job_state(
        &s_project.binding.client);
}

gui_project_lifecycle_state
gui_project_lifecycle_state_query(void) {
    switch (gui_host_binding_lifecycle(
        &s_project.binding)) {
    case GUI_HOST_CLOSED:
        return GUI_PROJECT_LIFECYCLE_CLOSED;
    case GUI_HOST_OPEN:
        return GUI_PROJECT_LIFECYCLE_OPEN_IDLE;
    case GUI_HOST_DRAINING:
    case GUI_HOST_READY_TO_CUTOVER:
        return GUI_PROJECT_LIFECYCLE_DRAINING;
    default:
        NT_ASSERT(false);
        return GUI_PROJECT_LIFECYCLE_CLOSED;
    }
}

void gui_project_set_controller_status_port(
    gui_project_controller_status_port port) {
    s_project.controller_status = port;
}

uint64_t
gui_project_session_instance_generation(void) {
    return gui_session_client_instance_generation(
        &s_project.binding.client);
}

void gui_project_invalidate_sources(void) {
    gui_scan_invalidate_all();
    if (!gui_project__ingress_is_open()) {
        return;
    }
    tp_session *session =
        gui_project__borrow_active_session();
    if (!session) {
        return;
    }
    tp_error err = {0};
    const tp_status status =
        tp_session_invalidate_sources(session, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return;
    }
    request_observation();
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

void gui_project__note_session_reject(tp_status status, const tp_error *err) {
    const char *message = (err && err->msg[0]) ? err->msg : tp_status_str(status);
    s_project.op_error = true;
    s_project.op_error_status = status;
    (void)snprintf(s_project.op_error_msg, sizeof s_project.op_error_msg, "%s", message);
}

void gui_project__sync_recovery_notice(void) {
    request_observation();
}

// #endregion

// #region lifecycle dev seams (selftest only)
#if defined(NTPACKER_GUI_SELFTEST) || defined(TP_ENABLE_TEST_SEAMS)
tp_session *gui_project__test_session(void) {
    return gui_project__borrow_active_session();
}

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
        &s_project.binding.queue);
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
/* Preview freshness is presentation state; model/history/dirty remain session-owned. */
void gui_project_mark_packed(void) { s_project.preview_stale = false; }
void gui_project_mark_stale(void) { s_project.preview_stale = true; }
// #endregion
