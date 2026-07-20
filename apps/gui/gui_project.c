#include "gui_project.h"
#include "gui_project_internal.h"

#include <stdio.h>
#include <string.h>

#include "gui_scan.h"

#include "tp_core/tp_identity.h"
#ifdef NTPACKER_GUI_SELFTEST
#include "tp_session_internal.h"
#endif

/* GUI mutation and Undo/Redo run through tp_session; reads use one cached owned
 * snapshot. Field edits coalesce by exact target until the gesture boundary, so
 * one gesture produces one transaction and one Undo step. */

gui_project_state s_project;

// #region helpers
void gui_project__snapshot_drop(void) {
    if (s_project.snapshot) {
        s_project.snapshot_lifetime_generation++;
    }
    tp_session_snapshot_destroy(s_project.snapshot);
    s_project.snapshot = NULL;
}

const tp_session_snapshot *gui_project_snapshot(void) {
    if (!s_project.snapshot && s_project.session) {
        tp_error err = {0};
        if (tp_session_snapshot_create(s_project.session, &s_project.snapshot, &err) != TP_STATUS_OK) {
            s_project.snapshot = NULL;
        }
    }
    return s_project.snapshot;
}

uint64_t gui_project_snapshot_lifetime_generation(void) {
    return s_project.snapshot_lifetime_generation;
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
    if (s_project.snapshot &&
        tp_session_snapshot_revision(s_project.snapshot) == tp_session_revision(s_project.session)) {
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
