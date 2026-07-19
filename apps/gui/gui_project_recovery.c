#include "gui_project_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_recovery.h"
static int64_t recovery_now(void) {
    return (int64_t)time(NULL);
}

/* Stable application key for recovery sidecars. Bump when the journal contract
 * changes incompatibly so old slots cannot be misapplied. */
static tp_id128 recovery_key(void) {
    tp_id128 k;
    static const uint8_t b[16] = {'n', 't', 'p', 'k', '_', 'r', 'e', 'c', 'o', 'v', 'e', 'r', 'y', '_', '0', '1'};
    memcpy(k.bytes, b, sizeof b);
    return k;
}

/* Raise the status-bar channel for a degraded recovery notice. Editing and
 * history remain available; only crash-recovery coverage is degraded. */
void gui_project__note_recovery_degraded(const char *msg) {
    s_project.op_error = true;
    (void)snprintf(
        s_project.op_error_msg, sizeof s_project.op_error_msg,
        "Crash recovery is unavailable (%s). Changes can continue, but unsaved work may not survive an app or system crash.",
        msg ? msg : "unknown");
}

static bool recovery_configured(void) {
    return s_project.recovery_root[0] != '\0';
}

/* Attach one shared live recovery owner after the session identity is final.
 * The session owns the handle on every accepted attach path, including degraded
 * filesystem outcomes; GUI retains only configuration and presentation state. */
void gui_project__attach_recovery_live(tp_session *session) {
    if (!session) {
        return;
    }
    tp_error err = {0};
    if (s_project.recovery_required &&
        tp_session_require_recovery(session, &err) != TP_STATUS_OK) {
        gui_project_note_recovery_setup_failure(
            err.msg[0] ? err.msg : "recovery could not be required");
        return;
    }
    if (!recovery_configured()) {
        if (s_project.recovery_required) {
            gui_project_note_recovery_setup_failure(
                "the recovery directory is not configured");
        }
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_session_identity identity = tp_session_snapshot_identity(snapshot);
    tp_id128 saved_fingerprint = tp_id128_nil();
    const bool has_saved_fingerprint =
        tp_session_snapshot_saved_file_fingerprint(snapshot, &saved_fingerprint);
    const tp_recovery_metadata metadata = {
        .timestamp = recovery_now(),
        .project_path = identity.kind == TP_IDENTITY_SAVED
                            ? identity.canonical_path
                            : "",
        .project_name = s_project.name,
        .file_fingerprint = has_saved_fingerprint ? &saved_fingerprint : NULL,
    };
    tp_rng rng = tp_rng_os();
    const tp_status status = tp_recovery_session_attach(
        s_project.recovery_root, recovery_key(), &rng, session, &metadata, &err);
    if (status != TP_STATUS_OK && !tp_session_recovery_available(session)) {
        gui_project_note_recovery_setup_failure(
            status == TP_STATUS_RECOVERY_BUSY
                ? "another ntpacker window owns this recovery slot"
                : "the recovery storage lock could not be acquired");
        gui_project__note_recovery_degraded(err.msg[0] ? err.msg
                                         : "could not checkpoint the recovery journal");
    }
}


void gui_project_require_recovery(void) { s_project.recovery_required = true; }

/* Configure crash recovery from the host app-data root. Core generates the
 * per-process slot identity and owns all liveness and exclusion mechanics. */
void gui_project_enable_recovery(const char *root) {
    s_project.recovery_root[0] = '\0';
    if (!root || root[0] == '\0') {
        return;
    }
    tp_error err = {0};
    const tp_status status = tp_recovery_root_validate(
        root, recovery_key(), &err);
    if (status == TP_STATUS_OK) {
        (void)snprintf(s_project.recovery_root, sizeof s_project.recovery_root, "%s", root);
    }
    if (status != TP_STATUS_OK) {
        gui_project_note_recovery_setup_failure(
            err.msg[0] ? err.msg : "the recovery root is unavailable");
    }
}

void gui_project_note_recovery_setup_failure(const char *reason) {
    s_project.recovery_setup_notice_pending = true;
    (void)snprintf(s_project.recovery_setup_notice, sizeof s_project.recovery_setup_notice,
                   "Crash recovery is unavailable (%s). Changes can continue, but unsaved work may not survive an app or system crash.",
                   (reason && reason[0] != '\0') ? reason : "startup setup failed");
}

static void note_recovery_scan_limited(void) {
    s_project.recovery_setup_notice_pending = true;
    (void)snprintf(s_project.recovery_setup_notice, sizeof s_project.recovery_setup_notice,
                   "Some previous recovery sessions were not scanned because the startup safety budget was reached.");
}

/* Drains the one-shot recovery-unavailable notice. */
bool gui_project_take_recovery_setup_notice(char *out, size_t cap) {
    if (!s_project.recovery_setup_notice_pending) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_project.recovery_setup_notice[0] != '\0'
                                             ? s_project.recovery_setup_notice
                                             : "Crash recovery is unavailable. Changes can continue, but unsaved work may not survive an app or system crash.");
    }
    s_project.recovery_setup_notice_pending = false;
    s_project.recovery_setup_notice[0] = '\0';
    return true;
}

// #region recovery resolution
int gui_recovery_collect(gui_recovery_list *out) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!recovery_configured() || !out) {
        return 0;
    }
    tp_error err = {0};
    tp_status status = tp_recovery_scan_root(
        s_project.recovery_root, recovery_key(), s_project.session, out, &err);
    if (status != TP_STATUS_OK) {
        gui_project_note_recovery_setup_failure("the recovery directory could not be scanned");
        return 0;
    }
    if (out->has_more) {
        note_recovery_scan_limited();
    }
    return (int)out->count;
}

static void recovery_copy_error(char *out, size_t cap, tp_status status,
                                const tp_error *err) {
    if (out && cap) {
        (void)snprintf(out, cap, "%s",
                       err && err->msg[0] ? err->msg : tp_status_str(status));
    }
}

static tp_status
gui_recovery_resolve(const char *journal_path, gui_recovery_action action,
                     const char *target_path,
                     char *err_out, size_t err_cap) {
    if (err_out && err_cap) {
        err_out[0] = '\0';
    }
    if (!journal_path || journal_path[0] == '\0') {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no recovery journal to resolve");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (action != GUI_RECOVERY_DISCARD &&
        action != GUI_RECOVERY_SAVE_ORIGINAL &&
        action != GUI_RECOVERY_SAVE_AS) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "unknown recovery action");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_error err = {0};
    if (!recovery_configured()) {
        const tp_status missing = tp_error_set(
            &err, TP_STATUS_INVALID_ARGUMENT,
            "recovery domain is not configured");
        recovery_copy_error(err_out, err_cap, missing, &err);
        return missing;
    }
    tp_recovery_action core_action = TP_RECOVERY_ACTION_DISCARD;
    if (action == GUI_RECOVERY_SAVE_ORIGINAL) {
        core_action = TP_RECOVERY_ACTION_SAVE_ORIGINAL;
    } else if (action == GUI_RECOVERY_SAVE_AS) {
        core_action = TP_RECOVERY_ACTION_SAVE_AS;
    }
    tp_rng rng = tp_rng_os();
    tp_recovery_resolve_result result;
    const tp_status status = tp_recovery_resolve_journal(
        s_project.recovery_root, recovery_key(), journal_path, s_project.session, core_action,
        target_path, &rng, &result, &err);
    if (status != TP_STATUS_OK) {
        recovery_copy_error(err_out, err_cap, status, &err);
    }
    return status;
}


tp_status gui_recovery_resolve_entry(const gui_recovery_entry *entry, gui_recovery_action action,
                                     const char *target_path, char *err_out, size_t err_cap) {
    if (!entry) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no recovery entry to resolve");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    return gui_recovery_resolve(entry->journal_path, action, target_path,
                                err_out, err_cap);
}
// #endregion
