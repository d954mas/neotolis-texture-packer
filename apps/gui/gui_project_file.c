#include "gui_project_internal.h"

#include <stdio.h>
#include <string.h>

#include "core/nt_assert.h"
#include "gui_scan.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_session.h"

#ifdef TP_ENABLE_TEST_SEAMS
static uint64_t s_test_open_call_count;
#endif

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
    if (gui_project_lifecycle_state_query() !=
            GUI_PROJECT_LIFECYCLE_OPEN_IDLE ||
        s_project.lifecycle_kind !=
            GUI_PROJECT_LIFECYCLE_NONE) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI project lifecycle transition is already in progress");
    }
    return TP_STATUS_OK;
}

static tp_status begin_candidate(
    gui_project_lifecycle_kind kind,
    tp_session *candidate, tp_error *err) {
    NT_ASSERT(candidate != NULL);
    const tp_status recovery_status =
        gui_project__prepare_candidate_recovery(
            candidate, err);
    if (recovery_status != TP_STATUS_OK) {
        tp_session_destroy(candidate);
        return recovery_status;
    }
    const tp_status status =
        gui_host_binding_begin_replace(
            &s_project.binding, candidate,
            true, err);
    if (status == TP_STATUS_OK) {
        s_project.lifecycle_kind = kind;
    }
    return status;
}

static tp_status create_fresh_candidate(
    tp_session **out, tp_error *err) {
    tp_rng rng = tp_rng_os();
    return tp_session_create_default_project(
        &rng, out, err);
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

static tp_status observe_current_identity(
    tp_error *err) {
    if (!gui_session_client_is_attached(
            &s_project.binding.client)) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "GUI session identity is unavailable");
    }
    return gui_session_client_observe(
        &s_project.binding.client, err);
}

// #region lifecycle
void gui_project_init(void) {
    if (gui_session_client_is_attached(
            &s_project.binding.client)) {
        return;
    }
    tp_error err = {{0}};
    const tp_status client_status =
        gui_project__client_init(&err);
    if (client_status != TP_STATUS_OK) {
        gui_project__note_session_reject(
            client_status, &err);
        return;
    }
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
        gui_host_binding_attach_initial(
            &s_project.binding, initial, &err);
    if (attach_status != TP_STATUS_OK) {
        tp_session_destroy(initial);
        gui_project__note_session_reject(
            attach_status, &err);
        return;
    }
    s_project.preview_stale = false;
}

void gui_project_shutdown(void) {
    NT_ASSERT(
        gui_project_lifecycle_state_query() ==
        GUI_PROJECT_LIFECYCLE_CLOSED);
    s_project.recovery_root[0] = '\0';
    s_project.save_notice_pending = false;
    s_project.save_notice[0] = '\0';
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
        GUI_PROJECT_LIFECYCLE_NEW,
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
    const tp_status observe_status =
        observe_current_identity(err);
    if (observe_status != TP_STATUS_OK) {
        return observe_status;
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
        tp_session_open(
            canonical_path, &rng,
            &candidate, err);
    if (open_status != TP_STATUS_OK) {
        return open_status;
    }
    return begin_candidate(
        GUI_PROJECT_LIFECYCLE_OPEN,
        candidate, err);
}

tp_status gui_project_lifecycle_begin_shutdown(
    bool discard_recovery, tp_error *err) {
    const tp_status lifecycle_status =
        require_idle_lifecycle(err);
    if (lifecycle_status != TP_STATUS_OK) {
        return lifecycle_status;
    }
    const tp_status status =
        gui_host_binding_begin_shutdown(
            &s_project.binding,
            discard_recovery, err);
    if (status == TP_STATUS_OK) {
        s_project.lifecycle_kind =
            GUI_PROJECT_LIFECYCLE_SHUTDOWN;
    }
    return status;
}

void gui_project_lifecycle_force_close(void) {
    gui_host_binding_force_close(
        &s_project.binding);
    s_project.lifecycle_kind =
        GUI_PROJECT_LIFECYCLE_NONE;
}

tp_status gui_project_lifecycle_pump(
    gui_project_lifecycle_kind *completed,
    tp_error *err) {
    if (completed) {
        *completed = GUI_PROJECT_LIFECYCLE_NONE;
    }
    if (!gui_session_client_is_attached(
            &s_project.binding.client)) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "GUI host has no live session");
    }
    gui_host_transition_kind binding_completion =
        GUI_HOST_TRANSITION_NONE;
    const tp_status status =
        gui_host_binding_pump(
            &s_project.binding,
            &binding_completion, err);
    if (status != TP_STATUS_OK ||
        binding_completion ==
            GUI_HOST_TRANSITION_NONE) {
        return status;
    }
    const gui_project_lifecycle_kind kind =
        s_project.lifecycle_kind;
    if (binding_completion ==
        GUI_HOST_TRANSITION_REPLACE) {
        NT_ASSERT(
            kind == GUI_PROJECT_LIFECYCLE_NEW ||
            kind == GUI_PROJECT_LIFECYCLE_OPEN);
        reset_cutover_state(kind);
        gui_project_invalidate_sources();
    } else {
        NT_ASSERT(
            binding_completion ==
            GUI_HOST_TRANSITION_SHUTDOWN);
        NT_ASSERT(
            kind ==
            GUI_PROJECT_LIFECYCLE_SHUTDOWN);
    }
    s_project.lifecycle_kind =
        GUI_PROJECT_LIFECYCLE_NONE;
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
           gui_host_binding_can_undo(
               &s_project.binding);
}
bool gui_project_can_redo(void) {
    return gui_project__ingress_is_open() &&
           gui_host_binding_can_redo(
               &s_project.binding);
}
int gui_project_undo_depth(void) {
    return gui_host_binding_undo_depth(
        &s_project.binding);
}
int gui_project_redo_depth(void) {
    return gui_host_binding_redo_depth(
        &s_project.binding);
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
    tp_status st = gui_host_binding_undo(
        &s_project.binding, &e);
    if (st != TP_STATUS_OK) {
        if (st != TP_STATUS_NOT_FOUND) {
            note_history_reject("undo", st, &e);
        }
        return false;
    }
    s_project.preview_stale = true; /* restored model no longer matches the last pack */
    gui_project_invalidate_sources();
    return true;
}

bool gui_project_redo(void) {
    if (!gui_project__ingress_is_open()) {
        return false;
    }
    tp_error e = {0};
    tp_status st = gui_host_binding_redo(
        &s_project.binding, &e);
    if (st != TP_STATUS_OK) {
        if (st != TP_STATUS_NOT_FOUND) {
            note_history_reject("redo", st, &e);
        }
        return false;
    }
    s_project.preview_stale = true;
    gui_project_invalidate_sources();
    return true;
}
// #endregion

// #region file operations
#ifdef TP_ENABLE_TEST_SEAMS
uint64_t gui_project__test_open_call_count(
    void) {
    return s_test_open_call_count;
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
    if (!gui_project_has_path()) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no path (use Save As)");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_error err = {0};
    tp_session_save_result result;
    const tp_status st = gui_host_binding_save(
        &s_project.binding, &result, &err);
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

/* Master spec 14.2: tp_session exclusively owns the exact-byte Open/Save
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
    const tp_status observe_status =
        observe_current_identity(&err);
    if (observe_status != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(
                err_out, err_cap, "%s",
                err.msg[0]
                    ? err.msg
                    : tp_status_str(
                          observe_status));
        }
        return observe_status;
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

tp_status gui_project_save_as_preflight(
    const char *path, char *err_out, size_t err_cap) {
    char canonical_path[TP_IDENTITY_PATH_MAX];
    return save_as_preflight(
        path, canonical_path, err_out, err_cap);
}

tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap) {
    char canonical_path[TP_IDENTITY_PATH_MAX];
    const tp_status preflight_status =
        save_as_preflight(
            path, canonical_path, err_out, err_cap);
    if (preflight_status != TP_STATUS_OK) {
        return preflight_status;
    }
    tp_error err = {0};
    tp_session_save_result result;
    const tp_status st = gui_host_binding_save_as(
        &s_project.binding, canonical_path,
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
// #endregion
