/* Model/state mutation layer for the ntpacker GUI (see gui_actions.h). This TU is Clay-free AND
 * nt_ui-free: it reads/mutates the model + shared state only. */

#include "gui_actions.h"
#include "gui_actions_internal.h"
#include "gui_project_driver.h"

#include "gui_defs.h" /* S() -- the compact-strip stop that folds the preview selector away */
#include "gui_state.h"
#include "gui_rows.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_paths.h"
#include "gui_shell.h" /* reset the canvas borrow across pack/history transitions */
#include "tinyfiledialogs.h"

#include "clipboard/nt_clipboard.h"
#if defined(NTPACKER_GUI_DEV_SEAMS) || defined(TP_ENABLE_TEST_SEAMS)
#include "time/nt_time.h"
#endif
#include "tp_core/tp_export.h" /* tp_exporter_at -> the preview selector's exporter list */
#include "tp_core/tp_id.h"
#include "tp_core/tp_names.h"  /* tp_names_common_prefix (anim id from selection) */

#include "app/nt_app.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum gui_pending_history_action {
    GUI_PENDING_HISTORY_NONE = 0,
    GUI_PENDING_HISTORY_UNDO,
    GUI_PENDING_HISTORY_REDO,
} gui_pending_history_action;

static gui_pending_history_action s_pending_history_action;

/* Presentation-only mapping from the active stable animation to one Pack result. */

gui_lifecycle_request s_after_confirm;
bool s_confirm_open;
bool s_confirm_draft;
int s_modal_action;
/* R6b: startup crash-recovery modal glue. The orphan list lives here; the modal reads it via the
 * count/at accessors and requests a per-row action, deferred to gui_actions_step so the
 * Save-As dialog + disk-mutating gui_recovery_resolve run outside nt_ui_begin/end, like s_pending_save_as. */
bool s_recovery_open;
double s_last_pack_ms;      /* wall-clock ms of the last successful pack (for the stats line) */
tp_id128 s_last_pack_atlas_id;

gui_actions_state s_actions = {.recovery_pending_row = -1};

bool gui_actions_copy_text_available(void) {
    return nt_clipboard_available();
}

void gui_actions_copy_text(const char *text) {
    if (text) {
        nt_clipboard_set_text(text);
    }
}

// #region undo/redo + refresh actions
static tp_id128 selected_animation_id(void) {
    return gui_view_animation_id();
}

/* After undo/redo, drop transient editor and preview state but retain canonical
 * selection refs for revalidation against rebuilt rows. */
static void undo_redo_settle(tp_id128 animation_id) {
    gui_shell_reset_shown_result();
    cancel_edit();
    gui_view_reconcile_observation(gui_project_snapshot());
    if (tp_id128_is_nil(animation_id) ||
        gui_view_animation_index(
            gui_project_snapshot()) < 0) {
        gui_view_select_animation(tp_id128_nil());
    }
    if (s_preview_active) {
        preview_stop();
    }
    preview_target_reset();
    gui_canvas_invalidate(&s_canvas);
}
void do_undo(void) {
    if (gui_draft_phase() != GUI_EDIT_IDLE) {
        set_status_ex(
            STATUS_WARNING,
            "Apply or discard the active edit before Undo.");
        return;
    }
    const tp_id128 animation_id = selected_animation_id();
    if (gui_project_undo()) {
        undo_redo_settle(animation_id);
        set_statusf("Undo (undo:%d redo:%d)", gui_project_undo_depth(), gui_project_redo_depth());
    } else {
        set_status("Nothing to undo.");
    }
}
void do_redo(void) {
    if (gui_draft_phase() != GUI_EDIT_IDLE) {
        set_status_ex(
            STATUS_WARNING,
            "Apply or discard the active edit before Redo.");
        return;
    }
    const tp_id128 animation_id = selected_animation_id();
    if (gui_project_redo()) {
        undo_redo_settle(animation_id);
        set_statusf("Redo (undo:%d redo:%d)", gui_project_undo_depth(), gui_project_redo_depth());
    } else {
        set_status("Nothing to redo.");
    }
}

void gui_request_undo(void) {
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_ACTIVE) {
        return;
    }
    s_pending_history_action = GUI_PENDING_HISTORY_UNDO;
}

void gui_request_redo(void) {
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_ACTIVE) {
        return;
    }
    s_pending_history_action = GUI_PENDING_HISTORY_REDO;
}

static void apply_pending_history_action(void) {
    const gui_pending_history_action action =
        s_pending_history_action;
    s_pending_history_action = GUI_PENDING_HISTORY_NONE;
    if (action == GUI_PENDING_HISTORY_UNDO) {
        do_undo();
    } else if (action == GUI_PENDING_HISTORY_REDO) {
        do_redo();
    }
}

void gui_actions__clear_history_request(void) {
    s_pending_history_action = GUI_PENDING_HISTORY_NONE;
}

bool gui_actions_refresh_should_mark_stale(tp_status status,
                                           bool sources_invalidated) {
    return status == TP_STATUS_OK || sources_invalidated;
}

/* Admit source I/O as the third kind of the session's one task slot. */
void gui_actions__refresh(void) {
    if (gui_pack_async_busy()) {
        gui_actions__record_job_request(
            GUI_JOB_REQUEST_REFRESH, false,
            "a task is already running");
        set_status_ex(
            STATUS_WARNING,
            "Busy -- a Pack, Export, or Refresh task is already running.");
        return;
    }
    char error[256] = {0};
    const bool admitted =
        gui_refresh_async_start(
            error, sizeof error);
    gui_actions__record_job_request(
        GUI_JOB_REQUEST_REFRESH, admitted,
        admitted ? "" : error);
    if (admitted) {
        set_status_ex(STATUS_INFO, "Refreshing sources...");
    } else {
        set_statusf_ex(
            STATUS_ERROR, "Refresh failed: %s", error);
    }
}

/* Dev/test-only blocking driver. The admitted Refresh still advances solely
 * through the production frame/action pump. Test builds read the typed
 * completion copied by gui_actions__poll_pack after its real poll path. */
#if defined(NTPACKER_GUI_DEV_SEAMS) || defined(TP_ENABLE_TEST_SEAMS)
bool gui_actions_refresh_diff_headless(int *out_added, int *out_removed,
                                       int *out_changed,
                                       int *out_unavailable) {
#ifdef TP_ENABLE_TEST_SEAMS
    gui_actions__test_reset_refresh_completion();
#else
    const uint64_t before = gui_project_source_runtime_generation();
#endif
    char start_error[256] = {0};
    if (!gui_refresh_async_start(
            start_error, sizeof start_error)) {
        return false;
    }
#ifdef TP_ENABLE_TEST_SEAMS
    gui_pack_done done = GUI_PACK_DONE_NONE;
    gui_pack_result_info info = {0};
    for (int attempt = 0;
         attempt < 5000 && done == GUI_PACK_DONE_NONE;
         ++attempt) {
        tp_error update_error = {{0}};
        if (gui_actions_step(
                NULL, &update_error) != TP_STATUS_OK) {
            return false;
        }
        if (!gui_actions__test_take_refresh_completion(
                &done, &info) &&
            gui_project_job_busy()) {
            nt_time_sleep(0.001);
        }
    }
    if (done == GUI_PACK_DONE_NONE) {
        return false;
    }
    if (out_added) *out_added = info.added;
    if (out_removed) *out_removed = info.removed;
    if (out_changed) *out_changed = info.changed;
    if (out_unavailable) *out_unavailable = info.unavailable;
    return done == GUI_PACK_DONE_REFRESH_OK;
#else
    while (gui_project_job_busy()) {
        tp_error update_error = {{0}};
        if (gui_actions_step(
                NULL, &update_error) != TP_STATUS_OK) {
            return false;
        }
    }
    (void)out_added;
    (void)out_removed;
    (void)out_changed;
    (void)out_unavailable;
    return gui_project_source_runtime_generation() > before;
#endif
}
#endif

// #endregion

// #region deferred side-effects (run at the top of the frame, between frames)
static void gui_actions__drain_intents(void) {
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_ACTIVE) {
        return;
    }
    const bool save_as_requires_preflight =
        gui_draft_phase() != GUI_EDIT_IDLE &&
        (gui_actions__intent_queued(GUI_INTENT_SAVE_AS) ||
         (gui_actions__intent_queued(GUI_INTENT_SAVE) &&
          !gui_project_has_path()));
    if (save_as_requires_preflight) {
        s_actions.gesture_commit = false;
        (void)gui_actions__intent_drain(GUI_INTENT_PHASE_DIALOG);
        gui_actions__clear_pending();
        return;
    }
    const tp_session_snapshot *before_atlas =
        gui_project_snapshot();
    const int64_t revision_before_atlas =
        before_atlas
            ? tp_session_snapshot_revision(before_atlas)
            : -1;
    if (s_actions.draft_apply_mine) {
        const bool applied =
            gui_actions__apply_draft_mine();
        s_actions.draft_apply_mine = false;
        if (!applied) {
            s_actions.gesture_commit = false;
            gui_actions__discard_deferred_edits();
            gui_actions__clear_pending();
            return;
        }
    } else if (s_actions.gesture_commit &&
               gui_draft_phase() !=
                   GUI_EDIT_IDLE) {
        if (!gui_actions__submit_draft()) {
            s_actions.gesture_commit = false;
            gui_actions__discard_deferred_edits();
            gui_actions__clear_pending();
            return;
        }
    }
    const tp_session_snapshot *after_atlas =
        gui_project_snapshot();
    const int64_t revision_after_atlas =
        after_atlas
            ? gui_project_committed_revision()
            : -1;
    if (revision_before_atlas >= 0 &&
        revision_after_atlas >= 0) {
        gui_actions__rebase_deferred_edits(
            revision_before_atlas,
            revision_after_atlas);
    }

    /* The atlas draft is the prerequisite for the remaining edits in this
     * frame. Only after it reaches a terminal success may dependent edit
     * queues mutate the session. */
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_EDIT);

    s_actions.gesture_commit = false;

    if (gui_actions__apply_lifecycle_request() &&
        gui_project_lifecycle_state_query() !=
            GUI_PROJECT_LIFECYCLE_ACTIVE) {
        return;
    }

    apply_pending_history_action();

    gui_actions__apply_confirm();
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_ACTIVE) {
        return;
    }
    if (s_actions.pending_lifecycle_request !=
        GUI_LIFECYCLE_REQUEST_NONE) {
        /* A draft terminal is already committed, but its view echo belongs to
         * project step below. Preserve the lifecycle request for the next
         * between-frame gate instead of clearing or executing it on stale
         * dirty state. */
        return;
    }

    gui_actions__apply_recovery();

    if (gui_actions__intent_drain(GUI_INTENT_PHASE_DIALOG)) {
        gui_actions__clear_pending();
        return;
    }
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_STRUCTURAL);
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_REFRESH);
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_PACK);

    gui_actions__clear_pending();
}

void gui_actions_shutdown(void) {
    gui_actions__intent_shutdown();
    gui_actions__preview_shutdown();
}

void gui_actions__record_job_request(
    gui_job_request_kind kind, bool admitted,
    const char *detail) {
    gui_actions_step_result *result =
        s_actions.active_step_result;
    if (!result ||
        result->job_receipt_count >=
            GUI_ACTIONS_STEP_MAX_JOB_RECEIPTS) {
        return;
    }
    gui_job_request_receipt *receipt =
        &result->job_receipts[
            result->job_receipt_count++];
    receipt->kind = kind;
    receipt->admitted = admitted;
    (void)snprintf(
        receipt->detail,
        sizeof receipt->detail,
        "%s", detail ? detail : "");
}

tp_status gui_actions_step(
    gui_actions_step_result *out, tp_error *err) {
    gui_actions_step_result result = {0};
    s_actions.active_step_result = &result;
    gui_actions__drain_intents();
    s_actions.active_step_result = NULL;
    gui_project_step_result project_result = {0};
    const tp_status status =
        gui_project_step(&project_result, err);
    if (status != TP_STATUS_OK) {
        tp_session_job_result_destroy(
            &project_result.completion);
        if (out) {
            *out = result;
        }
        return status;
    }
    gui_actions__consume_completion(
        &project_result.completion);
    gui_actions__complete_lifecycle(
        project_result.lifecycle_completed);
    gui_actions__reconcile_observation();
    if (out) {
        *out = result;
    }
    return TP_STATUS_OK;
}

#ifdef TP_ENABLE_TEST_SEAMS
void gui_actions__test_drain_intents(void) {
    gui_actions__drain_intents();
}
#endif
#ifdef NTPACKER_GUI_SELFTEST
void gui_actions__selftest_drain_intents(void) {
    gui_actions__drain_intents();
}
#endif

// #endregion
