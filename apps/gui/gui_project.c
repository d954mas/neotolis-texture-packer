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

static void reduce_project_observation(
    void *context, const tp_session_observation *observation,
    uint64_t instance_generation) {
    gui_project_state *project = context;
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

tp_status gui_project__client_init(tp_error *err) {
    if (s_project.client_initialized) {
        return TP_STATUS_OK;
    }
    gui_session_client_init(&s_project.client);
    const tp_status status =
        gui_session_client_register_reducer(
            &s_project.client,
            reduce_project_observation, &s_project, err);
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

tp_session *gui_project_session_for_jobs(void) { return s_project.session; }

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

void gui_project__next_transaction_id(char out[33]) {
    (void)snprintf(out, 33U, "%032llx", (unsigned long long)(s_project.txn_seq++));
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
    const tp_session_recovery_health health =
        tp_session_recovery_health_query(s_project.session);
    if (!health.degraded) {
        /* A failed cross-identity retire can temporarily leave no owner while
         * Save As asks the frontend to rebind. Preserve the exact result notice
         * until a healthy owner is actually available. */
        if (!health.available && s_project.recovery_notice_active) {
            return;
        }
        s_project.recovery_notice_active = false;
        s_project.recovery_notice.notice_id = health.notice_id;
        s_project.recovery_notice.generation = health.generation;
        s_project.recovery_notice.status = TP_STATUS_OK;
        s_project.recovery_notice.message[0] = '\0';
        return;
    }
    if (s_project.recovery_notice_active &&
        strcmp(health.notice_id, s_project.recovery_notice.notice_id) == 0 &&
        health.generation == s_project.recovery_notice.generation &&
        health.first_cause == s_project.recovery_notice.status) {
        return;
    }
    gui_project__note_recovery_degraded(health.first_cause);
}

bool gui_project__refresh_after_session_commit(void) {
    gui_project__sync_recovery_notice();
    /* Core is the sole semantic no-op owner. A no-change admission leaves the
     * revision unchanged, so keep the current projection and preview state. */
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    if (snapshot &&
        tp_session_snapshot_revision(snapshot) ==
            tp_session_revision(s_project.session)) {
        return false;
    }
    s_project.preview_stale = true;
    gui_project__snapshot_drop();
    return true;
}

// #endregion

// #region lifecycle dev seams (selftest only)
#ifdef NTPACKER_GUI_SELFTEST
tp_session *gui_project__test_session(void) { return s_project.session; }

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
