#include "gui_project_internal.h"

#include <stdio.h>
#include <string.h>

#include "core/nt_assert.h"
#include "time/nt_time.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_session.h"

#define GUI_PROJECT_DRAIN_GRACE_SECONDS 1.0

#ifdef TP_ENABLE_TEST_SEAMS
static uint64_t s_test_open_call_count;
static int s_test_drain_grace_ms = -1;
#endif

static bool lifecycle_is_new(
    gui_project_lifecycle_state state) {
    return state == GUI_PROJECT_LIFECYCLE_NEW_DRAINING ||
           state == GUI_PROJECT_LIFECYCLE_NEW_READY;
}

static bool lifecycle_is_open(
    gui_project_lifecycle_state state) {
    return state == GUI_PROJECT_LIFECYCLE_OPEN_DRAINING ||
           state == GUI_PROJECT_LIFECYCLE_OPEN_READY;
}

static bool lifecycle_is_shutdown(
    gui_project_lifecycle_state state) {
    return state == GUI_PROJECT_LIFECYCLE_SHUTDOWN_DRAINING ||
           state == GUI_PROJECT_LIFECYCLE_SHUTDOWN_READY;
}

static bool lifecycle_is_ready(
    gui_project_lifecycle_state state) {
    return state == GUI_PROJECT_LIFECYCLE_NEW_READY ||
           state == GUI_PROJECT_LIFECYCLE_OPEN_READY ||
           state == GUI_PROJECT_LIFECYCLE_SHUTDOWN_READY;
}

static bool lifecycle_transition_allowed(
    gui_project_lifecycle_state from,
    gui_project_lifecycle_state to) {
    switch (from) {
        case GUI_PROJECT_LIFECYCLE_CLOSED:
            return to == GUI_PROJECT_LIFECYCLE_ACTIVE;
        case GUI_PROJECT_LIFECYCLE_ACTIVE:
            return to == GUI_PROJECT_LIFECYCLE_NEW_DRAINING ||
                   to == GUI_PROJECT_LIFECYCLE_OPEN_DRAINING ||
                   to == GUI_PROJECT_LIFECYCLE_SHUTDOWN_DRAINING;
        case GUI_PROJECT_LIFECYCLE_NEW_DRAINING:
            return to == GUI_PROJECT_LIFECYCLE_NEW_READY;
        case GUI_PROJECT_LIFECYCLE_NEW_READY:
            return to == GUI_PROJECT_LIFECYCLE_ACTIVE;
        case GUI_PROJECT_LIFECYCLE_OPEN_DRAINING:
            return to == GUI_PROJECT_LIFECYCLE_OPEN_READY;
        case GUI_PROJECT_LIFECYCLE_OPEN_READY:
            return to == GUI_PROJECT_LIFECYCLE_ACTIVE;
        case GUI_PROJECT_LIFECYCLE_SHUTDOWN_DRAINING:
            return to == GUI_PROJECT_LIFECYCLE_SHUTDOWN_READY;
        case GUI_PROJECT_LIFECYCLE_SHUTDOWN_READY:
            return to == GUI_PROJECT_LIFECYCLE_CLOSED;
    }
    return false;
}

void gui_project__assert_lifecycle_invariants(void) {
    const gui_project_lifecycle_state state =
        s_project.lifecycle_state;
    const bool has_live_session =
        state != GUI_PROJECT_LIFECYCLE_CLOSED;
    NT_ASSERT(
        has_live_session ==
        (s_project.session != NULL));
    NT_ASSERT(!has_live_session ||
              s_project.view != NULL);
    if (state == GUI_PROJECT_LIFECYCLE_CLOSED) {
        NT_ASSERT(s_project.candidate == NULL);
        NT_ASSERT(!s_project.refresh_pending);
        return;
    }
    if (state == GUI_PROJECT_LIFECYCLE_ACTIVE ||
        lifecycle_is_shutdown(state)) {
        NT_ASSERT(s_project.candidate == NULL);
    } else {
        NT_ASSERT(
            lifecycle_is_new(state) ||
            lifecycle_is_open(state));
        NT_ASSERT(s_project.candidate != NULL);
    }
    if (lifecycle_is_ready(state)) {
        NT_ASSERT(
            !tp_session_job_active(
                s_project.session) ||
            s_project.drain_deadline_expired);
    }
    if (state == GUI_PROJECT_LIFECYCLE_ACTIVE ||
        state == GUI_PROJECT_LIFECYCLE_CLOSED) {
        NT_ASSERT(!s_project.drain_deadline_expired);
        NT_ASSERT(s_project.drain_started_at == 0.0);
    }
}

static void lifecycle_transition(
    gui_project_lifecycle_state next) {
    NT_ASSERT(lifecycle_transition_allowed(
        s_project.lifecycle_state, next));
    s_project.lifecycle_state = next;
    gui_project__assert_lifecycle_invariants();
}

static void lifecycle_force_closed(void) {
    s_project.refresh_pending = false;
    s_project.drain_started_at = 0.0;
    s_project.drain_deadline_expired = false;
    s_project.lifecycle_state =
        GUI_PROJECT_LIFECYCLE_CLOSED;
    gui_project__assert_lifecycle_invariants();
}

static void lifecycle_begin_drain(
    gui_project_lifecycle_state draining_state) {
    s_project.drain_started_at = nt_time_now();
    s_project.drain_deadline_expired = false;
    lifecycle_transition(draining_state);
}

static double lifecycle_drain_grace_seconds(void) {
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_test_drain_grace_ms >= 0) {
        return (double)s_test_drain_grace_ms / 1000.0;
    }
#endif
    return GUI_PROJECT_DRAIN_GRACE_SECONDS;
}

static bool lifecycle_task_is_drained(void) {
    if (!tp_session_job_active(s_project.session)) {
        return true;
    }
    if (s_project.drain_deadline_expired) {
        return true;
    }
    const double elapsed =
        nt_time_now() - s_project.drain_started_at;
    if (elapsed < lifecycle_drain_grace_seconds()) {
        return false;
    }
    s_project.drain_deadline_expired = true;
    return true;
}

static gui_project_lifecycle_kind lifecycle_kind(
    gui_project_lifecycle_state state) {
    if (lifecycle_is_new(state)) {
        return GUI_PROJECT_LIFECYCLE_NEW;
    }
    if (lifecycle_is_open(state)) {
        return GUI_PROJECT_LIFECYCLE_OPEN;
    }
    if (lifecycle_is_shutdown(state)) {
        return GUI_PROJECT_LIFECYCLE_SHUTDOWN;
    }
    return GUI_PROJECT_LIFECYCLE_NONE;
}

static gui_project_lifecycle_state lifecycle_ready_state(
    gui_project_lifecycle_state state) {
    switch (state) {
        case GUI_PROJECT_LIFECYCLE_NEW_DRAINING:
            return GUI_PROJECT_LIFECYCLE_NEW_READY;
        case GUI_PROJECT_LIFECYCLE_OPEN_DRAINING:
            return GUI_PROJECT_LIFECYCLE_OPEN_READY;
        case GUI_PROJECT_LIFECYCLE_SHUTDOWN_DRAINING:
            return GUI_PROJECT_LIFECYCLE_SHUTDOWN_READY;
        default:
            break;
    }
    NT_ASSERT(false);
    return state;
}

static void reset_cutover_state(
    gui_project_lifecycle_kind kind) {
    s_project.op_error = false;
    s_project.op_error_status = TP_STATUS_OK;
    s_project.op_error_msg[0] = '\0';
    s_project.save_notice_pending = false;
    s_project.save_notice[0] = '\0';
    s_project.preview_stale =
        kind == GUI_PROJECT_LIFECYCLE_OPEN;
}

static tp_status require_idle_lifecycle(
    tp_error *err) {
    if (!gui_project__ingress_is_open()) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI project lifecycle transition is already in progress");
    }
    return TP_STATUS_OK;
}

static tp_status begin_candidate(
    gui_project_lifecycle_state draining_state,
    tp_session *candidate, tp_error *err) {
    NT_ASSERT(candidate != NULL);
    const tp_status recovery_status =
        gui_project__prepare_candidate_recovery(
            candidate, err);
    if (recovery_status != TP_STATUS_OK) {
        tp_session_destroy(candidate);
        return recovery_status;
    }
    const tp_status update_status =
        tp_session_update(candidate, NULL, err);
    if (update_status != TP_STATUS_OK) {
        tp_session_destroy(candidate);
        return update_status;
    }
    if (!gui_project__ingress_is_open() ||
        s_project.candidate) {
        tp_session_destroy(candidate);
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI replacement requires one open idle session");
    }
    s_project.candidate = candidate;
    s_project.discard_retired_session = true;
    lifecycle_begin_drain(draining_state);
    if (tp_session_job_active(s_project.session)) {
        tp_error cancel_error = {{0}};
        gui_project__invalidate_observation();
        (void)tp_session_job_cancel(
            s_project.session, &cancel_error);
    }
    return TP_STATUS_OK;
}

static tp_status create_fresh_candidate(
    tp_session **out, tp_error *err) {
    tp_rng rng = tp_rng_os();
    return tp_session_create_default_project_with_catalog(
        s_project.format_catalog, &rng, out, err);
}

static tp_format_catalog *create_startup_format_catalog(void) {
    char root[TP_IDENTITY_PATH_MAX];
    tp_error error = {{0}};
    if (tp_format_root_from_executable(root, sizeof root, &error) !=
        TP_STATUS_OK) {
        return tp_format_catalog_retain(tp_format_catalog_native());
    }
    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure_diagnostics = NULL;
    tp_status status = tp_format_catalog_scan_root(
        root, &scan, &failure_diagnostics, &error);
    tp_format_diagnostic_report_destroy(failure_diagnostics);
    if (status != TP_STATUS_OK || !scan ||
        tp_format_catalog_scan_compile_count(scan) != 0U) {
        tp_format_catalog_scan_destroy(scan);
        return tp_format_catalog_retain(tp_format_catalog_native());
    }
    tp_format_catalog *catalog = NULL;
    status = tp_format_catalog_scan_finish_without_compile(
        &scan, &catalog, &error);
    tp_format_catalog_scan_destroy(scan);
    return status == TP_STATUS_OK && catalog
               ? catalog
               : tp_format_catalog_retain(tp_format_catalog_native());
}

static bool current_identity_is(
    const char *canonical_path) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    if (!snapshot || !canonical_path) {
        return false;
    }
    const tp_session_identity identity =
        tp_session_snapshot_identity(snapshot);
    return identity.kind == TP_IDENTITY_SAVED &&
           tp_identity_path_equal(
               identity.canonical_path,
               canonical_path);
}

static bool controller_attached(void) {
    return s_project.controller_status.attached &&
           s_project.controller_status.attached(
               s_project.controller_status.context);
}

// #region lifecycle
void gui_project_init(void) {
    if (s_project.session) {
        return;
    }
    if (!s_project.format_catalog) {
        s_project.format_catalog = create_startup_format_catalog();
    }
    tp_error err = {{0}};
    tp_session *initial = NULL;
    const tp_status create_status =
        create_fresh_candidate(
            &initial, &err);
    if (create_status != TP_STATUS_OK) {
        gui_project__note_session_reject(
            create_status, &err);
        return;
    }
    const tp_status recovery_status =
        gui_project__prepare_candidate_recovery(
            initial, &err);
    if (recovery_status != TP_STATUS_OK) {
        tp_session_destroy(initial);
        gui_project__note_session_reject(
            recovery_status, &err);
        return;
    }
    const tp_status attach_status =
        tp_session_update(initial, NULL, &err);
    if (attach_status != TP_STATUS_OK) {
        tp_session_destroy(initial);
        gui_project__note_session_reject(
            attach_status, &err);
        return;
    }
    s_project.instance_generation = 1U;
    s_project.session = initial;
    gui_project__publish_view(
        tp_session_view(initial));
    s_project.preview_stale = false;
    lifecycle_transition(
        GUI_PROJECT_LIFECYCLE_ACTIVE);
}

void gui_project_shutdown(void) {
    NT_ASSERT(
        gui_project_lifecycle_state_query() ==
        GUI_PROJECT_LIFECYCLE_CLOSED);
    s_project.recovery_root[0] = '\0';
    s_project.save_notice_pending = false;
    s_project.save_notice[0] = '\0';
    s_project.refresh_pending = false;
    tp_format_catalog_release(s_project.format_catalog);
    s_project.format_catalog = NULL;
}

tp_status gui_project_lifecycle_begin_new(
    tp_error *err) {
    const tp_status lifecycle_status =
        require_idle_lifecycle(err);
    if (lifecycle_status != TP_STATUS_OK) {
        return lifecycle_status;
    }
    tp_session *candidate = NULL;
    const tp_status create_status =
        create_fresh_candidate(&candidate, err);
    if (create_status != TP_STATUS_OK) {
        return create_status;
    }
    return begin_candidate(
        GUI_PROJECT_LIFECYCLE_NEW_DRAINING,
        candidate, err);
}

tp_status gui_project_lifecycle_begin_open(
    const char *path, tp_error *err) {
    const tp_status lifecycle_status =
        require_idle_lifecycle(err);
    if (lifecycle_status != TP_STATUS_OK) {
        return lifecycle_status;
    }
    if (!path ||
        strlen(path) >=
            TP_IDENTITY_PATH_MAX) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "project path exceeds the supported limit");
    }
    char canonical_path[TP_IDENTITY_PATH_MAX];
    const tp_status canonical_status =
        tp_identity_project_path_canonical(
            path, canonical_path,
            sizeof canonical_path, err);
    if (canonical_status != TP_STATUS_OK) {
        return canonical_status;
    }
    if (current_identity_is(canonical_path)) {
        return tp_error_set(
            err, TP_STATUS_PROJECT_LIVE,
            "project is already open in this GUI session: %s",
            canonical_path);
    }
    tp_rng rng = tp_rng_os();
    tp_session *candidate = NULL;
#ifdef TP_ENABLE_TEST_SEAMS
    ++s_test_open_call_count;
#endif
    const tp_status open_status =
        tp_session_open_with_catalog(
            canonical_path, s_project.format_catalog, &rng,
            &candidate, err);
    if (open_status != TP_STATUS_OK) {
        return open_status;
    }
    return begin_candidate(
        GUI_PROJECT_LIFECYCLE_OPEN_DRAINING,
        candidate, err);
}

tp_status gui_project_lifecycle_begin_shutdown(
    bool discard_recovery, tp_error *err) {
    const tp_status lifecycle_status =
        require_idle_lifecycle(err);
    if (lifecycle_status != TP_STATUS_OK) {
        return lifecycle_status;
    }
    s_project.discard_retired_session =
        discard_recovery;
    lifecycle_begin_drain(
        GUI_PROJECT_LIFECYCLE_SHUTDOWN_DRAINING);
    if (tp_session_job_active(s_project.session)) {
        tp_error cancel_error = {{0}};
        gui_project__invalidate_observation();
        (void)tp_session_job_cancel(
            s_project.session, &cancel_error);
    }
    return TP_STATUS_OK;
}

void gui_project_lifecycle_force_close(void) {
    tp_session_destroy(s_project.candidate);
    s_project.candidate = NULL;
    if (s_project.session &&
        s_project.discard_retired_session) {
        (void)tp_session_discard(
            s_project.session, NULL);
    }
    tp_session_destroy(s_project.session);
    s_project.session = NULL;
    gui_project__publish_view(NULL);
    s_project.discard_retired_session = false;
    s_project.refresh_pending = false;
    lifecycle_force_closed();
}

tp_status gui_project__advance_lifecycle(
    gui_project_lifecycle_kind *completed,
    tp_error *err) {
    if (completed) {
        *completed = GUI_PROJECT_LIFECYCLE_NONE;
    }
    gui_project__assert_lifecycle_invariants();
    if (s_project.lifecycle_state ==
        GUI_PROJECT_LIFECYCLE_CLOSED) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "GUI host has no live session");
    }
    if (s_project.lifecycle_state ==
        GUI_PROJECT_LIFECYCLE_ACTIVE) {
        return TP_STATUS_OK;
    }
    if (!lifecycle_is_ready(
            s_project.lifecycle_state)) {
        if (!lifecycle_task_is_drained()) {
            return TP_STATUS_OK;
        }
        lifecycle_transition(
            lifecycle_ready_state(
                s_project.lifecycle_state));
        return TP_STATUS_OK;
    }

    const gui_project_lifecycle_kind kind =
        lifecycle_kind(
            s_project.lifecycle_state);
    tp_session *retired = s_project.session;
    if (kind == GUI_PROJECT_LIFECYCLE_NEW ||
        kind == GUI_PROJECT_LIFECYCLE_OPEN) {
        NT_ASSERT(s_project.candidate != NULL);
        s_project.session = s_project.candidate;
        s_project.candidate = NULL;
        if (s_project.instance_generation <
            UINT64_MAX) {
            ++s_project.instance_generation;
        }
        gui_project__publish_view(
            tp_session_view(s_project.session));
        reset_cutover_state(kind);
    } else {
        NT_ASSERT(
            kind ==
            GUI_PROJECT_LIFECYCLE_SHUTDOWN);
        s_project.session = NULL;
        gui_project__publish_view(NULL);
        s_project.refresh_pending = false;
    }
    if (retired &&
        s_project.discard_retired_session) {
        (void)tp_session_discard(
            retired, NULL);
    }
    tp_session_destroy(retired);
    s_project.discard_retired_session = false;
    s_project.drain_started_at = 0.0;
    s_project.drain_deadline_expired = false;
    lifecycle_transition(
        kind == GUI_PROJECT_LIFECYCLE_SHUTDOWN
            ? GUI_PROJECT_LIFECYCLE_CLOSED
            : GUI_PROJECT_LIFECYCLE_ACTIVE);
    if (kind == GUI_PROJECT_LIFECYCLE_NEW ||
        kind == GUI_PROJECT_LIFECYCLE_OPEN) {
        gui_project__reduce_view();
        if (s_project.recovery_required &&
            !tp_session_recovery_available(
                s_project.session)) {
            gui_project_note_recovery_setup_failure(
                "the recovery directory is not configured");
        }
        /* Initial source I/O is admitted only after ACTIVE opens ingress. */
        gui_project_refresh_sources();
    }
    if (completed) {
        *completed = kind;
    }
    return TP_STATUS_OK;
}

bool gui_project_take_save_notice(char *out, size_t cap) {
    if (!s_project.save_notice_pending) {
        return false;
    }
    if (out && cap > 0U) {
        (void)snprintf(out, cap, "%s", s_project.save_notice);
    }
    s_project.save_notice_pending = false;
    s_project.save_notice[0] = '\0';
    return true;
}
// #endregion

// #region undo / redo
bool gui_project_can_undo(void) {
    return gui_project__ingress_is_open() &&
           tp_session_can_undo(
               s_project.session);
}
bool gui_project_can_redo(void) {
    return gui_project__ingress_is_open() &&
           tp_session_can_redo(
               s_project.session);
}
int gui_project_undo_depth(void) {
    return tp_session_undo_depth(
        s_project.session);
}
int gui_project_redo_depth(void) {
    return tp_session_redo_depth(
        s_project.session);
}

/* Record an actual history rejection on the same structured soft-error channel as
 * a rejected transaction. Recovery degradation is only a warning and does not turn
 * a successful Undo/Redo into a rejection. */
static void note_history_reject(const char *verb, tp_status st, const tp_error *err) {
    const char *detail = (err && err->msg[0]) ? err->msg : tp_status_str(st);
    s_project.op_error = true;
    (void)snprintf(s_project.op_error_msg, sizeof s_project.op_error_msg,
                   "%s rejected: %s", verb, detail);
}

/* Undo reverses the most recent committed transaction via its semantic diff. */
bool gui_project_undo(void) {
    if (!gui_project__ingress_is_open()) {
        return false;
    }
    tp_error e = {0};
    gui_project__invalidate_observation();
    const tp_status st = tp_session_undo(
        s_project.session, &e);
    if (st != TP_STATUS_OK) {
        if (st != TP_STATUS_NOT_FOUND) {
            note_history_reject("undo", st, &e);
        }
        return false;
    }
    s_project.preview_stale = true; /* restored model no longer matches the last pack */
    gui_project_refresh_sources();
    return true;
}

bool gui_project_redo(void) {
    if (!gui_project__ingress_is_open()) {
        return false;
    }
    tp_error e = {0};
    gui_project__invalidate_observation();
    const tp_status st = tp_session_redo(
        s_project.session, &e);
    if (st != TP_STATUS_OK) {
        if (st != TP_STATUS_NOT_FOUND) {
            note_history_reject("redo", st, &e);
        }
        return false;
    }
    s_project.preview_stale = true;
    gui_project_refresh_sources();
    return true;
}
// #endregion

// #region file operations
#ifdef TP_ENABLE_TEST_SEAMS
uint64_t gui_project__test_open_call_count(
    void) {
    return s_test_open_call_count;
}

void gui_project__test_set_drain_grace_ms(
    int grace_ms) {
    s_test_drain_grace_ms = grace_ms;
}
#endif

tp_status gui_project_save(char *err_out, size_t err_cap) {
    if (!gui_project__ingress_is_open()) {
        if (err_out && err_cap) {
            (void)snprintf(
                err_out, err_cap,
                "session lifecycle transition is in progress");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_error err = {0};
    tp_session_save_result result;
    gui_project__invalidate_observation();
    const tp_status st = tp_session_save(
        s_project.session, &result, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    if (result.recovery_degraded) {
        gui_project__note_recovery_degraded(result.recovery_status);
    }
    if (result.file_durability_degraded) {
        s_project.save_notice_pending = true;
        (void)snprintf(
            s_project.save_notice, sizeof s_project.save_notice,
            "Saved, but storage durability could not be confirmed");
    }
    return TP_STATUS_OK;
}

/* tp_session exclusively owns the exact-byte Open/Save
 * baseline and rejects an external replacement before publication. */

static tp_status save_as_preflight(
    const char *path,
    char canonical_path[TP_IDENTITY_PATH_MAX],
    char *err_out, size_t err_cap) {
    if (!gui_project__ingress_is_open()) {
        if (err_out && err_cap) {
            (void)snprintf(
                err_out, err_cap,
                "session lifecycle transition is in progress");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (!path || strlen(path) >= TP_IDENTITY_PATH_MAX) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "project path exceeds the supported %zu-byte limit",
                           (size_t)TP_IDENTITY_PATH_MAX - 1U);
        }
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_error err = {0};
    tp_status canonical = tp_identity_project_path_canonical(
        path, canonical_path, TP_IDENTITY_PATH_MAX, &err);
    if (canonical != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(canonical));
        }
        return canonical;
    }
    if (!current_identity_is(canonical_path) &&
        controller_attached()) {
        if (err_out && err_cap) {
            (void)snprintf(
                err_out, err_cap,
                "Save As cannot change project identity while an external controller is attached");
        }
        return TP_STATUS_UNSUPPORTED_CAPABILITY;
    }
    return TP_STATUS_OK;
}

tp_status gui_project_save_as_prepare(
    const char *path, gui_project_save_as_plan *out,
    char *err_out, size_t err_cap) {
    if (!out) {
        if (err_out && err_cap) {
            (void)snprintf(
                err_out, err_cap,
                "Save As preparation requires output");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    *out = (gui_project_save_as_plan){0};
    const tp_status status = save_as_preflight(
        path, out->canonical_path, err_out, err_cap);
    if (status == TP_STATUS_OK) {
        out->instance_generation =
            s_project.instance_generation;
    }
    return status;
}

tp_status gui_project_save_as_execute(
    const gui_project_save_as_plan *plan,
    char *err_out, size_t err_cap) {
    if (!gui_project__ingress_is_open() || !plan ||
        plan->canonical_path[0] == '\0') {
        if (err_out && err_cap) {
            (void)snprintf(
                err_out, err_cap,
                "Save As requires a prepared destination");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (plan->instance_generation !=
        s_project.instance_generation) {
        if (err_out && err_cap) {
            (void)snprintf(
                err_out, err_cap,
                "Save As plan belongs to a replaced project session");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    char revalidated_path[TP_IDENTITY_PATH_MAX];
    const tp_status preflight = save_as_preflight(
        plan->canonical_path, revalidated_path,
        err_out, err_cap);
    if (preflight != TP_STATUS_OK) {
        return preflight;
    }
    if (strcmp(revalidated_path,
               plan->canonical_path) != 0) {
        if (err_out && err_cap) {
            (void)snprintf(
                err_out, err_cap,
                "Save As destination identity changed after preparation");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_error err = {0};
    tp_session_save_result result;
    gui_project__invalidate_observation();
    const tp_status st = tp_session_save_as(
        s_project.session, plan->canonical_path,
        &result, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s",
                           err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    if (result.recovery_degraded) {
        gui_project__note_recovery_degraded(result.recovery_status);
    }
    if (result.recovery_rebind_required) {
        gui_project__attach_recovery_live();
    }
    if (result.file_durability_degraded) {
        s_project.save_notice_pending = true;
        (void)snprintf(
            s_project.save_notice, sizeof s_project.save_notice,
            "Saved, but storage durability could not be confirmed");
    }
    return TP_STATUS_OK;
}

tp_status gui_project_save_as_preflight(
    const char *path, char *err_out, size_t err_cap) {
    gui_project_save_as_plan plan;
    return gui_project_save_as_prepare(
        path, &plan, err_out, err_cap);
}

tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap) {
    gui_project_save_as_plan plan;
    const tp_status status =
        gui_project_save_as_prepare(
            path, &plan, err_out, err_cap);
    return status == TP_STATUS_OK
               ? gui_project_save_as_execute(
                     &plan, err_out, err_cap)
               : status;
}
// #endregion
