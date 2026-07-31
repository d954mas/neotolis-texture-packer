#include "gui_project.h"
#include "gui_project_internal.h"

#include <stdio.h>
#include <string.h>


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

static void reduce_project_view(
    gui_project_state *project,
    const struct tp_session_view *view) {
    NT_ASSERT(project != NULL);
    NT_ASSERT(view != NULL);
    const tp_session_recovery_health recovery =
        view->recovery_health;
    const bool new_instance =
        project->reduced_instance_generation !=
        project->instance_generation;
    if (new_instance &&
        project->recovery_required &&
        !recovery.available) {
        gui_project_note_recovery_setup_failure(
            "required recovery is unavailable for the active project");
    }
    reduce_recovery_health(project, recovery);
    const tp_session_snapshot *snapshot = view->snapshot;
    const tp_session_input_token input_token =
        snapshot
            ? tp_session_snapshot_input_token(snapshot)
            : (tp_session_input_token){0};
    if (new_instance) {
        if (snapshot) {
            reduce_project_display_name(
                project, snapshot);
        }
        project->reduced_instance_generation =
            project->instance_generation;
        project->observed_revision =
            snapshot
                ? tp_session_snapshot_revision(snapshot)
                : 0;
        project->observed_source_generation =
            input_token.source_generation;
        return;
    }
    if (snapshot) {
        const int64_t revision =
            tp_session_snapshot_revision(snapshot);
        if (project->observed_revision != revision ||
            project->observed_source_generation !=
                input_token.source_generation) {
            project->preview_stale = true;
        }
        reduce_project_display_name(
            project, snapshot);
        project->observed_revision =
            revision;
        project->observed_source_generation =
            input_token.source_generation;
    }
}

void gui_project__reduce_view(void) {
    if (s_project.view) {
        reduce_project_view(
            &s_project, s_project.view);
    }
}

tp_session *gui_project__borrow_active_session(void) {
    return s_project.session;
}

bool gui_project__ingress_is_open(void) {
    return s_project.session &&
           s_project.lifecycle_state ==
               GUI_PROJECT_LIFECYCLE_ACTIVE &&
           !s_project.frame_pinned;
}

tp_session *gui_project__mutation_session(void) {
    return gui_project__ingress_is_open()
               ? s_project.session
               : NULL;
}

const tp_session_snapshot *gui_project_snapshot(void) {
    return s_project.view
               ? s_project.view->snapshot
               : NULL;
}

const tp_source_runtime_projection *gui_project_sources(void) {
    return s_project.view ? s_project.view->sources : NULL;
}

uint64_t gui_project_snapshot_lifetime_generation(void) {
    return s_project.snapshot_lifetime_generation;
}

uint64_t gui_project_source_runtime_generation(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    return snapshot
               ? tp_session_snapshot_input_token(
                     snapshot).source_generation
               : 0U;
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
    gui_project__assert_lifecycle_invariants();
    if (!s_project.session ||
        s_project.frame_pinned) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI frame begin requires one unpinned live session");
    }
    if (s_project.completion_pending) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI host completion was not consumed before the next frame");
    }
    tp_session_job_result completion = {0};
    const tp_status status =
        tp_session_update(
            s_project.session, &completion, err);
    if (completion.kind !=
        TP_SESSION_JOB_NONE) {
        s_project.completion = completion;
        s_project.completion_pending = true;
    }
    if (status != TP_STATUS_OK) {
        return status;
    }
    s_project.view =
        tp_session_view(s_project.session);
    if (!s_project.view ||
        !s_project.view->snapshot) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "GUI session has no current view");
    }
    s_project.snapshot_lifetime_generation =
        s_project.view->snapshot_generation;
    gui_project__reduce_view();
    s_project.frame_pinned = true;
    return TP_STATUS_OK;
}

void gui_project_frame_end(void) {
    s_project.frame_pinned = false;
    gui_project__assert_lifecycle_invariants();
}

bool gui_project_frame_is_pinned(void) {
    return s_project.frame_pinned;
}

tp_status gui_project_job_enqueue_pack(
    tp_id128 atlas_id, const char *work_dir,
    const char *preview_exporter_id, tp_error *err) {
    if (!gui_project__ingress_is_open()) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI Pack requires an open unpinned session");
    }
    const tp_pack_job_request request = {
        .atlas_id = atlas_id,
        .work_dir = work_dir,
        .session_instance_generation =
            s_project.instance_generation,
        .preview_exporter_id =
            preview_exporter_id,
    };
    return tp_session_pack_job_start(
        s_project.session, &request, err);
}

tp_status gui_project_job_enqueue_export(
    tp_id128 atlas_id, const char *work_dir,
    tp_error *err) {
    if (!gui_project__ingress_is_open()) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI Export requires an open unpinned session");
    }
    const tp_export_command_request request = {
        .work_dir = work_dir,
        .session_instance_generation =
            s_project.instance_generation,
        .atlas_id = atlas_id,
    };
    return tp_session_export_start(
        s_project.session, &request, err);
}

tp_status gui_project_job_enqueue_refresh(tp_error *err) {
    if (!gui_project__ingress_is_open()) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI Refresh requires an open unpinned session");
    }
    const tp_refresh_job_request request = {
        .session_instance_generation =
            s_project.instance_generation,
    };
    return tp_session_refresh_start(
        s_project.session, &request, err);
}

tp_status gui_project_job_enqueue_cancel(
    tp_error *err) {
    return s_project.session
               ? tp_session_job_cancel(
                     s_project.session, err)
               : tp_error_set(
                     err, TP_STATUS_NOT_FOUND,
                     "GUI host has no live session");
}

bool gui_project_take_completion(
    tp_session_job_result *out) {
    if (!out ||
        !s_project.completion_pending) {
        return false;
    }
    *out = s_project.completion;
    s_project.completion =
        (tp_session_job_result){0};
    s_project.completion_pending = false;
    return true;
}

bool gui_project_job_busy(void) {
    return s_project.session &&
           tp_session_job_active(
               s_project.session);
}

tp_session_job_kind
gui_project_job_active_kind(void) {
    const tp_session_job_observed_state state =
        gui_project_job_observed_state();
    return state.present && !state.terminal
               ? state.kind
               : TP_SESSION_JOB_NONE;
}

tp_session_job_observed_state
gui_project_job_observed_state(void) {
    return s_project.view
               ? s_project.view->task
               : (tp_session_job_observed_state){0};
}

gui_project_lifecycle_state
gui_project_lifecycle_state_query(void) {
    gui_project__assert_lifecycle_invariants();
    return s_project.lifecycle_state;
}

void gui_project_set_controller_status_port(
    gui_project_controller_status_port port) {
    s_project.controller_status = port;
}

uint64_t
gui_project_session_instance_generation(void) {
    return s_project.instance_generation;
}

int64_t gui_project_committed_revision(void) {
    return s_project.session
               ? tp_session_revision(
                     s_project.session)
               : -1;
}

void gui_project_refresh_sources(void) {
    tp_error err = {{0}};
    const tp_status status =
        gui_project_job_enqueue_refresh(&err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
    }
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
    if (!s_project.session) {
        return;
    }
    reduce_recovery_health(
        &s_project,
        tp_session_recovery_health_query(
            s_project.session));
}

// #endregion

// #region lifecycle dev seams (selftest only)
#if defined(NTPACKER_GUI_SELFTEST) || defined(TP_ENABLE_TEST_SEAMS)
tp_session *gui_project__test_session(void) {
    return gui_project__borrow_active_session();
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
bool gui_project_is_stale(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    if (snapshot &&
        s_project.observed_revision !=
            tp_session_snapshot_revision(snapshot)) {
        s_project.preview_stale = true;
        s_project.observed_revision =
            tp_session_snapshot_revision(snapshot);
    }
    return s_project.preview_stale;
}
// #endregion

// #region dirty/stale choke point
/* Preview freshness is presentation state; model/history/dirty remain session-owned. */
void gui_project_mark_packed(void) { s_project.preview_stale = false; }
void gui_project_mark_stale(void) { s_project.preview_stale = true; }
// #endregion
