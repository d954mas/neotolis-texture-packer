/* Model/state mutation layer for the ntpacker GUI (see gui_actions.h). This TU is Clay-free AND
 * nt_ui-free: it reads/mutates the model + shared state only. */

#include "gui_actions.h"
#include "gui_actions_internal.h"
#include "gui_project.h"

#include "gui_defs.h" /* S() -- the compact-strip stop that folds the preview selector away */
#include "gui_state.h"
#include "gui_rows.h"
#include "gui_project.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_paths.h"
#include "gui_shell.h" /* reset the canvas borrow across pack/history transitions */
#include "tinyfiledialogs.h"

#include "clipboard/nt_clipboard.h"
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
 * count/at accessors and requests a per-row action, deferred to apply_pending() (below) so the
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
        set_status_ex(
            STATUS_WARNING,
            "Busy -- a Pack, Export, or Refresh task is already running.");
        return;
    }
    char error[256] = {0};
    if (gui_refresh_async_start(error, sizeof error)) {
        set_status_ex(STATUS_INFO, "Refreshing sources...");
#ifdef TP_ENABLE_TEST_SEAMS
        while (gui_pack_async_busy()) {
            tp_error update_error = {{0}};
            if (gui_project_frame_begin(&update_error) !=
                TP_STATUS_OK) {
                break;
            }
            gui_actions__poll_pack();
            gui_project_frame_end();
        }
#endif
    } else {
        set_statusf_ex(
            STATUS_ERROR, "Refresh failed: %s", error);
    }
}

/* Headless seam drains the same session Refresh task. */
bool gui_actions_refresh_diff_headless(int *out_added, int *out_removed,
                                       int *out_changed) {
    const uint64_t before = gui_project_source_runtime_generation();
    char start_error[256] = {0};
    if (!gui_refresh_async_start(
            start_error, sizeof start_error)) {
        return false;
    }
#ifdef TP_ENABLE_TEST_SEAMS
    while (gui_project_job_busy()) {
        tp_error update_error = {{0}};
        if (gui_project_frame_begin(
                &update_error) != TP_STATUS_OK) {
            break;
        }
        gui_actions__poll_pack();
        gui_project_frame_end();
    }
#endif
    if (out_added) *out_added = 0;
    if (out_removed) *out_removed = 0;
    if (out_changed) *out_changed = 0;
    return gui_project_source_runtime_generation() > before;
}

// #endregion

// #region deferred side-effects (run at the top of the frame, between frames)
void apply_pending(void) {
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
         * frame_begin below. Preserve the lifecycle request for the next
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

void gui_actions_poll_host_completion(void) {
    gui_actions__poll_pack();
    gui_actions__reconcile_observation();
}
// #endregion
