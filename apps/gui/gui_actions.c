/* Model/state mutation layer for the ntpacker GUI (see gui_actions.h). This TU is Clay-free AND
 * nt_ui-free: it reads/mutates the model + shared state only. */

#include "gui_actions.h"
#include "gui_actions_internal.h"

#include "gui_defs.h" /* S() -- the compact-strip stop that folds the preview selector away */
#include "gui_state.h"
#include "gui_rows.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_paths.h"
#include "gui_shell.h" /* reset the canvas borrow across pack/history transitions */
#include "tinyfiledialogs.h"

#include "clipboard/nt_clipboard.h"
#include "log/nt_log.h"
#include "time/nt_time.h"
#include "tp_core/tp_export.h" /* export capability vocabulary */
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

gui_actions_state s_actions = {
    .recovery = {.pending_row = -1},
};

gui_last_pack_view gui_actions_last_pack_view(void) {
    return (gui_last_pack_view){
        .duration_ms = s_actions.last_pack_ms,
        .atlas_id = s_actions.last_pack_atlas_id,
    };
}

gui_lifecycle_view gui_actions_lifecycle_view(void) {
    return (gui_lifecycle_view){
        .phase = s_actions.lifecycle.phase,
        .request = s_actions.lifecycle.request,
    };
}

bool gui_actions_lifecycle_active(void) {
    return s_actions.lifecycle.phase !=
           GUI_LIFECYCLE_IDLE;
}

void gui_actions_lifecycle_choose(
    gui_lifecycle_choice choice) {
    if ((s_actions.lifecycle.phase !=
             GUI_LIFECYCLE_RESOLVE_DRAFT &&
         s_actions.lifecycle.phase !=
             GUI_LIFECYCLE_RESOLVE_DIRTY) ||
        choice == GUI_LIFECYCLE_CHOICE_NONE) {
        return;
    }
    s_actions.lifecycle.choice = choice;
}

void gui_actions_lifecycle_dismiss(void) {
    gui_actions_lifecycle_choose(
        GUI_LIFECYCLE_CHOICE_CANCEL);
}

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
    s_actions.pending_history_reconcile.animation_id =
        animation_id;
    s_actions.pending_history_reconcile.present = true;
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

bool gui_actions_dev_settle_task(tp_error *err) {
    const double deadline =
        nt_time_now() + 120.0;
    while (nt_time_now() < deadline) {
        const bool busy_before =
            gui_project_job_busy();
        gui_actions_step_result step = {0};
        const tp_status status =
            gui_actions_step(&step, err);
        if (status != TP_STATUS_OK) {
            return false;
        }
        if (step.job_completion_present &&
            (step.job_completion.kind ==
                 GUI_PACK_DONE_REFRESH_FAIL ||
             step.job_completion.kind ==
                 GUI_PACK_DONE_REFRESH_CANCELLED)) {
            const tp_status refresh_status =
                step.job_completion.status !=
                        TP_STATUS_OK
                    ? step.job_completion.status
                    : (step.job_completion.kind ==
                               GUI_PACK_DONE_REFRESH_CANCELLED
                           ? TP_STATUS_CANCELLED
                           : TP_STATUS_BUILDER_FAILED);
            (void)tp_error_set(
                err, refresh_status, "%s",
                step.job_completion.err[0]
                    ? step.job_completion.err
                    : "pending source Refresh did not complete");
            return false;
        }
        const bool busy_after =
            gui_project_job_busy();
        if (!busy_before && !busy_after) {
            /* This step ENTERED with a free slot. Therefore any coalesced
             * automatic Refresh was either admitted and completed here or
             * there was no controller-owned task left to admit. A busy->idle
             * terminal is not enough: Refresh admission happens before the
             * old task is updated to terminal. */
            return true;
        }
        if (busy_after) {
            nt_time_sleep(0.001);
        }
    }
    (void)tp_error_set(
        err, TP_STATUS_BUSY,
        "controller tasks did not reach a confirmed idle boundary");
    return false;
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
    bool reuse_active_refresh =
        gui_project_job_busy() &&
        gui_pack_async_active_kind() ==
            GUI_PACK_ASYNC_REFRESH;
    for (int run = 0; run < 2; ++run) {
        if (!reuse_active_refresh) {
            tp_error settle_error = {{0}};
            if (gui_project_job_busy() &&
                !gui_actions_dev_settle_task(
                    &settle_error)) {
                nt_log_error(
                    "gui_actions_refresh_diff_headless: existing task failed to settle: %s",
                    settle_error.msg[0]
                        ? settle_error.msg
                        : "unknown error");
                return false;
            }
            char start_error[256] = {0};
            if (!gui_refresh_async_start(
                    start_error,
                    sizeof start_error)) {
                nt_log_error(
                    "gui_actions_refresh_diff_headless: start failed: %s",
                    start_error[0]
                        ? start_error
                        : "unknown error");
                return false;
            }
        }
        const double deadline =
            nt_time_now() + 120.0;
        while (nt_time_now() < deadline) {
            tp_error update_error = {{0}};
            gui_actions_step_result step = {0};
            if (gui_actions_step(
                    &step, &update_error) !=
                TP_STATUS_OK) {
                nt_log_error(
                    "gui_actions_refresh_diff_headless: actions step failed: %s",
                    update_error.msg[0]
                        ? update_error.msg
                        : "unknown error");
                return false;
            }
            if (step.job_completion_present) {
                const gui_pack_result_info *info =
                    &step.job_completion;
                if (info->kind ==
                    GUI_PACK_DONE_REFRESH_OK) {
                    if (out_added) {
                        *out_added = info->added;
                    }
                    if (out_removed) {
                        *out_removed = info->removed;
                    }
                    if (out_changed) {
                        *out_changed = info->changed;
                    }
                    if (out_unavailable) {
                        *out_unavailable =
                            info->unavailable;
                    }
                    return true;
                }
                if (reuse_active_refresh) {
                    /* A stale/cancelled automatic Refresh cannot satisfy this
                     * explicit blocking request. Its terminal is consumed;
                     * run one fresh Refresh without exposing the retry
                     * sequence to the caller. */
                    reuse_active_refresh = false;
                    break;
                }
                return false;
            }
            if (gui_project_job_busy()) {
                nt_time_sleep(0.001);
            } else {
                nt_log_error(
                    "gui_actions_refresh_diff_headless: task ended without a classified terminal");
                return false;
            }
        }
        if (gui_project_job_busy()) {
            nt_log_error(
                "gui_actions_refresh_diff_headless: timed out while the task remained active");
            return false;
        }
    }
    nt_log_error(
        "gui_actions_refresh_diff_headless: refresh ended without a usable terminal");
    return false;
}
#endif

// #endregion

// #region deferred side-effects (run at the top of the frame, between frames)
static void gui_actions__drain_intents(void) {
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_ACTIVE) {
        return;
    }
    if (gui_actions_lifecycle_active()) {
        /* A tagged lifecycle flow exclusively owns this controller tick. A
         * resolve phase consumes only its typed choice; open-dialog consumes
         * only the OS dialog terminal. No edit, gesture, history action, other
         * dialog, or job request can mutate the session behind either owner.
         * Deferred inputs resume on a later published observation cut. */
        if (s_actions.lifecycle.phase ==
            GUI_LIFECYCLE_OPEN_DIALOG) {
            gui_actions__run_open_lifecycle_dialog();
        } else {
            gui_actions__apply_confirm();
        }
        return;
    }
    if (s_actions.pending_lifecycle_request !=
        GUI_LIFECYCLE_REQUEST_NONE) {
        if (!gui_project_observation_is_valid()) {
            /* A prerequisite draft/history terminal already closed the cut.
             * Publish its dirty/identity echo before deciding which lifecycle
             * question the pending request requires. */
            return;
        }
        /* Establish the lifecycle question before considering any request
         * queued beside or after it. The caller does not need to order
         * request_new/open/exit against ordinary semantic ingress. */
        (void)gui_actions__apply_lifecycle_request();
        return;
    }
    const bool save_as_requires_preflight =
        gui_draft_phase() != GUI_EDIT_IDLE &&
        (gui_actions__intent_queued(GUI_INTENT_SAVE_AS) ||
         (gui_actions__intent_queued(GUI_INTENT_SAVE) &&
          !gui_project_has_path()));
    if (save_as_requires_preflight) {
        s_actions.gesture_commit = false;
        gui_actions__intent_drain(GUI_INTENT_PHASE_DIALOG);
        if (gui_project_observation_is_valid()) {
            gui_actions__clear_pending();
        }
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
    const int64_t revision_after_atlas =
        gui_project_committed_revision();
    if (revision_before_atlas >= 0 &&
        revision_after_atlas >= 0) {
        gui_actions__rebase_deferred_edits(
            revision_before_atlas,
            revision_after_atlas);
    }
    if (!gui_project_observation_is_valid()) {
        s_actions.gesture_commit = false;
        return;
    }

    /* The atlas draft is the prerequisite for the remaining edits in this
     * frame. Only after it reaches a terminal success may dependent edit
     * queues mutate the session. */
    gui_actions__intent_drain(GUI_INTENT_PHASE_EDIT);

    s_actions.gesture_commit = false;
    if (!gui_project_observation_is_valid()) {
        return;
    }

    apply_pending_history_action();
    if (!gui_project_observation_is_valid()) {
        return;
    }

    gui_actions__apply_recovery();
    if (!gui_project_observation_is_valid()) {
        return;
    }

    gui_actions__intent_drain(GUI_INTENT_PHASE_DIALOG);
    if (!gui_project_observation_is_valid()) {
        return;
    }
    gui_actions__intent_drain(GUI_INTENT_PHASE_STRUCTURAL);
    if (!gui_project_observation_is_valid()) {
        return;
    }
    gui_actions__intent_drain(GUI_INTENT_PHASE_REFRESH);
    if (!gui_project_observation_is_valid()) {
        return;
    }
    gui_actions__intent_drain(GUI_INTENT_PHASE_PACK);
    if (!gui_project_observation_is_valid()) {
        return;
    }

    gui_actions__clear_pending();
}

void gui_actions_shutdown(void) {
    gui_actions__intent_shutdown();
    gui_actions__preview_shutdown();
    s_actions.lifecycle =
        (gui_lifecycle_flow){0};
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_NONE;
    gui_actions_recovery_dismiss();
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
    const gui_pack_done completion =
        gui_actions__consume_completion(
            &project_result.completion,
            &result.job_completion);
    result.job_completion_present =
        completion != GUI_PACK_DONE_NONE;
    gui_actions__complete_lifecycle(
        project_result.lifecycle_completed);
    gui_actions__reconcile_observation();
    if (out) {
        *out = result;
    }
    return TP_STATUS_OK;
}

tp_status gui_actions_host_open(
    const char *path, tp_error *err) {
    const tp_status begin =
        gui_project_lifecycle_begin_open(
            path, err);
    return begin == TP_STATUS_OK
               ? gui_actions_step(NULL, err)
               : begin;
}

tp_status gui_actions_host_shutdown_step(
    bool *out_closed, tp_error *err) {
    if (out_closed) {
        *out_closed = false;
    }
    if (gui_project_lifecycle_state_query() ==
        GUI_PROJECT_LIFECYCLE_ACTIVE) {
        const tp_status begin =
            gui_project_lifecycle_begin_shutdown(
                false, err);
        if (begin != TP_STATUS_OK) {
            return begin;
        }
    }
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_CLOSED) {
        const tp_status step =
            gui_actions_step(NULL, err);
        if (step != TP_STATUS_OK) {
            return step;
        }
    }
    if (out_closed) {
        *out_closed =
            gui_project_lifecycle_state_query() ==
            GUI_PROJECT_LIFECYCLE_CLOSED;
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
