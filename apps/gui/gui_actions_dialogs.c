#include "gui_actions_internal.h"
#include "gui_project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_state.h"
#include "gui_project.h"
#include "tp_core/tp_scan.h"
#include "gui_pack.h"
#include "gui_paths.h"
#include "gui_shell.h"
#include "tinyfiledialogs.h"

#include "app/nt_app.h"
// #region file dialogs (tinyfiledialogs)
bool gui_actions__open_dialog(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *path = tinyfd_openFileDialog("Open Project", "", 1, filt, "ntpacker project", 0);
    if (!path) {
        return false;
    }
    if (!tp_scan_exists(path)) {
        set_statusf_ex(STATUS_WARNING, "project not found: %s", path); /* never fatal (F6b) */
        return false;
    }
    tp_error error = {{0}};
    const tp_status status =
        gui_project_lifecycle_begin_open(
            path, &error);
    if (status != TP_STATUS_OK) {
        set_statusf_ex(
            STATUS_ERROR, "Open failed: %s",
            error.msg[0]
                ? error.msg
                : tp_status_str(status));
        return false;
    }
    return true;
}

bool gui_actions__save_as(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *def = gui_project_has_path() ? gui_project_path() : "untitled.ntpacker_project";
    const char *path = tinyfd_saveFileDialog("Save Project As", def, 1, filt, "ntpacker project");
    if (!path) {
        return false;
    }
    char full[TP_IDENTITY_PATH_MAX];
    if (!gui_paths_project_file(path, full, sizeof full)) {
        set_status_ex(STATUS_ERROR,
                      "Save path is invalid or exceeds the supported path limit.");
        return false;
    }
    char err[256];
    const tp_status preflight =
        gui_project_save_as_preflight(
            full, err, sizeof err);
    if (preflight != TP_STATUS_OK) {
        set_statusf_ex(
            STATUS_ERROR, "Save failed: %s",
            err[0] ? err : tp_status_str(preflight));
        return false;
    }

    const tp_session_snapshot *before =
        gui_project_snapshot();
    const int64_t revision_before =
        before ? tp_session_snapshot_revision(before)
               : -1;
    if (gui_draft_phase() != GUI_EDIT_IDLE &&
        !gui_actions__submit_draft()) {
        return false;
    }
    const tp_session_snapshot *after =
        gui_project_snapshot();
    const int64_t revision_after =
        after ? tp_session_snapshot_revision(after)
              : -1;
    if (revision_before >= 0 &&
        revision_after >= 0) {
        gui_actions__rebase_deferred_edits(
            revision_before, revision_after);
    }
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_EDIT);
    if (gui_project_save_as(full, err, sizeof err) == TP_STATUS_OK) {
        char notice[256];
        if (gui_project_take_save_notice(notice, sizeof notice)) {
            set_statusf_ex(STATUS_WARNING, "%s", notice);
        } else {
            set_statusf("Saved %s", gui_project_display_name());
        }
        return true;
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
        return false;
    }
}

bool gui_actions__save(void) {
    if (!gui_project_has_path()) {
        return gui_actions__save_as();
    }
    if (!gui_actions__submit_draft()) {
        return false;
    }
    char err[256];
    if (gui_project_save(err, sizeof err) == TP_STATUS_OK) {
        char notice[256];
        if (gui_project_take_save_notice(notice, sizeof notice)) {
            set_statusf_ex(STATUS_WARNING, "%s", notice);
        } else {
            set_statusf("Saved %s", gui_project_display_name());
        }
        return true;
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
        return false;
    }
}

void gui_request_remove_animation_ref(const gui_animation_ref *animation) {
    if (animation && !tp_id128_is_nil(animation->atlas_id) &&
        !tp_id128_is_nil(animation->animation_id)) {
        const gui_intent intent = {.kind = GUI_INTENT_REMOVE_ANIMATION,
                                   .payload.animation = *animation};
        (void)gui_actions__intent_push(&intent);
    }
}

void gui_request_remove_target_ref(const gui_target_ref *target) {
    if (target && !tp_id128_is_nil(target->atlas_id) &&
        !tp_id128_is_nil(target->target_id)) {
        const gui_intent intent = {.kind = GUI_INTENT_REMOVE_TARGET,
                                   .payload.target = *target};
        (void)gui_actions__intent_push(&intent);
    }
}

static bool selected_atlas_intent(tp_id128 *atlas_id, int64_t *revision) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_index = gui_view_atlas_index(snapshot);
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, atlas_index)
                                         : NULL;
    if (!atlas) {
        return false;
    }
    *atlas_id = atlas->id;
    *revision = tp_session_snapshot_revision(snapshot);
    return true;
}

void gui_actions__add_files(void) {
    static const char *filt[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga"};
    const char *res = tinyfd_openFileDialog("Add Image Files", "", 5, filt, "image files", 1);
    if (!res) {
        return;
    }
    /* tinyfd returns a '|'-joined path list in its OWN fixed static buffer, capped at 32 paths of
     * <= 1024 bytes each (MAX_MULTIPLE_FILES x MAX_PATH_OR_CMD). Parse it IN PLACE, one segment at a
     * time into a single-path buffer, so nothing truncates: the old `char buf[8192]` silently dropped
     * the tail of a large multi-select mid-path (P2). Any per-path overflow (a path longer than the OS
     * max -- pathological) is dropped LOUDLY with a count. */
    int added = 0;
    int dup = 0;
    int too_long = 0; /* a single path longer than the 1024-byte cap */
    int overflow = 0; /* more paths than the 32-path cap were selected */
    /* Collect the selected paths, then commit them as ONE transaction (H/P2-13): the old per-path
     * gui_project_add_source_kind loop made an N-file multi-select N undo steps + non-atomic (a mid-batch
     * failure left a partial add). tinyfd caps the list at 32 paths of <= 1024 bytes each. */
    char paths[32][1024];
    const char *ptrs[32];
    int n = 0;
    const char *start = res;
    for (;;) {
        const char *bar = strchr(start, '|');
        const size_t seg = bar ? (size_t)(bar - start) : strlen(start);
        if (seg > 0) {
            if (n >= (int)(sizeof paths / sizeof paths[0])) {
                overflow++; /* > 32 selected -> the tail is dropped (distinct from a too-long path) */
            } else if (seg >= sizeof paths[0]) {
                too_long++; /* one pathological over-long path */
            } else {
                memcpy(paths[n], start, seg);
                paths[n][seg] = '\0';
                normalize_slashes(paths[n]);
                ptrs[n] = paths[n];
                n++;
            }
        }
        if (!bar) {
            break;
        }
        start = bar + 1;
    }
    /* These come from the file-picker dialog: record the true kind. One atomic transaction, one undo step. */
    tp_id128 atlas_id;
    int64_t revision = 0;
    const bool ok = selected_atlas_intent(&atlas_id, &revision) &&
                    gui_project_add_sources(atlas_id, revision, ptrs, n,
                                            TP_SOURCE_KIND_FILE, &added, &dup);
    if (!ok) {
        /* The whole atomic batch was rejected (OOM, validation, or core) -- the reason is already on
         * the op-error channel; make THIS line an ERROR too, not a benign info-toned "Added 0". */
        set_status_ex(STATUS_ERROR, "Could not add the selected files.");
    } else if (too_long > 0 || overflow > 0) {
        set_statusf_ex(STATUS_WARNING,
                       "Added %d file source(s); %d skipped (%d too long, %d over the 32-file limit), %d already added",
                       added, too_long + overflow, too_long, overflow, dup);
    } else if (dup > 0) {
        set_statusf("Added %d file source(s); %d already added", added, dup);
    } else {
        set_statusf("Added %d file source(s)", added);
    }
}

/* Save dialog for a target's output path, relativized to the project like sources. Atlas-explicit so
 * the Export dialog (which spans all atlases) can browse any target, not just the selected atlas's. */
void gui_actions__browse_target(const gui_target_ref *queued) {
    if (!gui_actions__submit_draft()) {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot && queued
                                     ? tp_session_snapshot_atlas_by_id(snapshot,
                                                                        queued->atlas_id)
                                     : NULL;
    const tp_snapshot_target *t = a
                                      ? tp_session_snapshot_target_by_id(
                                            snapshot, a->id, queued->target_id)
                                      : NULL;
    if (!t) {
        return;
    }
    const gui_target_ref target = {
        .atlas_id = a->id,
        .target_id = t->id,
        .expected_revision = tp_session_snapshot_revision(snapshot),
    };
    const char *path = tinyfd_saveFileDialog("Export output path", t->out_path, 0, NULL, NULL);
    if (!path) {
        return;
    }
    char rel[TP_IDENTITY_PATH_MAX];
    if (!gui_paths_relativize_to_project(path, gui_project_path(), rel,
                                         sizeof rel)) {
        set_status_ex(STATUS_ERROR,
                      "Output path is invalid or exceeds the supported path limit.");
        return;
    }
    /* The dialog is one complete text gesture. It enters the same draft
     * lifecycle as typing, then submits the exact masked operation. */
    if (gui_text_edit_begin_target_out_path(
            &target, t->out_path) &&
        gui_text_edit_update(rel) &&
        gui_actions__submit_draft()) {
        set_statusf("Output path: %s", rel);
    }
}

void gui_request_browse_target_ref(const gui_target_ref *target) {
    if (target && !tp_id128_is_nil(target->atlas_id) &&
        !tp_id128_is_nil(target->target_id)) {
        const gui_intent intent = {.kind = GUI_INTENT_BROWSE_TARGET,
                                   .payload.target = *target};
        (void)gui_actions__intent_push(&intent);
    }
}

void gui_request_add_target(tp_id128 atlas_id, int64_t expected_revision) {
    if (!tp_id128_is_nil(atlas_id)) {
        const gui_intent intent = {
            .kind = GUI_INTENT_ADD_TARGET,
            .payload.scope = {atlas_id, expected_revision}};
        (void)gui_actions__intent_push(&intent);
    }
}

void gui_request_add_animation(tp_id128 atlas_id,
                               int64_t expected_revision) {
    if (!tp_id128_is_nil(atlas_id)) {
        const gui_intent intent = {
            .kind = GUI_INTENT_ADD_ANIMATION,
            .payload.scope = {atlas_id, expected_revision}};
        (void)gui_actions__intent_push(&intent);
    }
}

void gui_actions__add_folder(void) {
    const char *dir = tinyfd_selectFolderDialog("Add Folder", "");
    if (!dir) {
        return;
    }
    char norm[TP_IDENTITY_PATH_MAX];
    if (!gui_paths_copy_normalized(dir, norm, sizeof norm)) {
        set_status_ex(STATUS_ERROR,
                      "Folder path is invalid or exceeds the supported path limit.");
        return;
    }
    tp_id128 atlas_id;
    int64_t revision = 0;
    const gui_add_status r = selected_atlas_intent(&atlas_id, &revision)
                                 ? gui_project_add_source_kind(
                                       atlas_id, revision, norm,
                                       TP_SOURCE_KIND_FOLDER)
                                 : GUI_ADD_FAILED;
    if (r == GUI_ADD_ADDED) {
        set_statusf("Added folder %s", path_last(norm));
    } else if (r == GUI_ADD_DUPLICATE) {
        set_statusf_ex(STATUS_WARNING, "already added: %s", path_last(norm));
    } else {
        set_status_ex(STATUS_ERROR, "Add folder failed.");
    }
}
// #endregion

// #region new/exit confirm flow
void request_new(void) {
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_NEW;
}
void request_exit(void) {
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_EXIT;
}
/* Open routes through the same unsaved-changes confirm as New/Exit (no silent discard). The actual
 * OS open dialog runs as a GUI_INTENT_OPEN, either now (clean) or after the modal resolves. */
void request_open(void) {
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_OPEN;
}

static bool begin_new(void) {
    tp_error error = {{0}};
    const tp_status status =
        gui_project_lifecycle_begin_new(
            &error);
    if (status == TP_STATUS_OK) {
        return true;
    }
    set_statusf_ex(
        STATUS_ERROR,
        "New project failed: %s",
        error.msg[0]
            ? error.msg
            : tp_status_str(status));
    return false;
}

static bool begin_exit(void) {
    tp_error error = {{0}};
    const tp_status status =
        gui_project_lifecycle_begin_shutdown(
            true, &error);
    if (status == TP_STATUS_OK) {
        return true;
    }
    set_statusf_ex(
        STATUS_ERROR,
        "Exit failed: %s",
        error.msg[0]
            ? error.msg
            : tp_status_str(status));
    return false;
}

bool gui_actions__apply_lifecycle_request(void) {
    const gui_lifecycle_request request =
        s_actions.pending_lifecycle_request;
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_NONE;
    if (request ==
        GUI_LIFECYCLE_REQUEST_NONE) {
        return false;
    }
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE) {
        set_status_ex(
            STATUS_WARNING,
            "A project lifecycle transition is already in progress.");
        return false;
    }
    if (gui_draft_phase() != GUI_EDIT_IDLE) {
        s_after_confirm = request;
        s_confirm_draft = true;
        s_confirm_open = true;
        return false;
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = request;
        s_confirm_draft = false;
        s_confirm_open = true;
        return false;
    }
    if (request ==
        GUI_LIFECYCLE_REQUEST_NEW) {
        return begin_new();
    }
    if (request ==
        GUI_LIFECYCLE_REQUEST_EXIT) {
        return begin_exit();
    }
    gui_actions__request_open_dialog();
    return false;
}
static void confirm_perform(void) {
    if (s_after_confirm == GUI_LIFECYCLE_REQUEST_NEW) {
        (void)begin_new();
    } else if (s_after_confirm == GUI_LIFECYCLE_REQUEST_EXIT) {
        (void)begin_exit();
    } else if (s_after_confirm == GUI_LIFECYCLE_REQUEST_OPEN) {
        gui_actions__request_open_dialog(); /* runs the open dialog next frame */
    }
    s_after_confirm = GUI_LIFECYCLE_REQUEST_NONE;
}

static void confirm_continue(void) {
    s_actions.pending_lifecycle_request =
        s_after_confirm;
    s_after_confirm =
        GUI_LIFECYCLE_REQUEST_NONE;
    (void)gui_actions__apply_lifecycle_request();
}
// #endregion


void gui_actions__apply_confirm(void) {
    if (s_confirm_draft) {
        if (s_modal_action == MODAL_SAVE) {
            const bool applied =
                gui_draft_phase() ==
                        GUI_EDIT_CONFLICTED
                    ? gui_actions__apply_draft_mine()
                    : gui_actions__submit_draft();
            if (applied &&
                gui_draft_phase() ==
                    GUI_EDIT_IDLE) {
                s_confirm_open = false;
                s_confirm_draft = false;
                confirm_continue();
            } else {
                /* Keep the explicit choice open after validation, OOM, a
                 * deleted target, or another conflict. The draft and outer
                 * lifecycle request remain intact for retry, Discard, or
                 * Cancel. */
                s_confirm_open = true;
                s_confirm_draft = true;
            }
        } else if (s_modal_action == MODAL_DISCARD) {
            gui_draft_discard();
            if (gui_draft_phase() ==
                GUI_EDIT_IDLE) {
                s_confirm_open = false;
                s_confirm_draft = false;
                confirm_continue();
            }
        } else if (s_modal_action == MODAL_CANCEL) {
            s_confirm_open = false;
            s_confirm_draft = false;
            s_after_confirm =
                GUI_LIFECYCLE_REQUEST_NONE;
        }
        s_modal_action = MODAL_NONE;
        return;
    }
    if (s_modal_action == MODAL_SAVE) {
        const bool saved = gui_actions__save();
        s_confirm_open = false;
        if (saved) {
            confirm_perform();
        } else {
            s_after_confirm = GUI_LIFECYCLE_REQUEST_NONE;
        }
    } else if (s_modal_action == MODAL_DISCARD) {
        s_confirm_open = false;
        confirm_perform();
    } else if (s_modal_action == MODAL_CANCEL) {
        s_confirm_open = false;
        s_after_confirm = GUI_LIFECYCLE_REQUEST_NONE;
    }
    s_modal_action = MODAL_NONE;
}

void gui_actions_pump_lifecycle(void) {
    tp_error error = {{0}};
    gui_project_lifecycle_kind completed =
        GUI_PROJECT_LIFECYCLE_NONE;
    const tp_status status =
        gui_project_lifecycle_pump(
            &completed, &error);
    if (status != TP_STATUS_OK) {
        set_statusf_ex(
            STATUS_ERROR,
            "Session lifecycle failed: %s",
            error.msg[0]
                ? error.msg
                : tp_status_str(status));
        return;
    }
    if (completed ==
        GUI_PROJECT_LIFECYCLE_NONE) {
        return;
    }
    gui_actions__discard_edits();
    gui_actions__clear_pending();
    if (completed ==
        GUI_PROJECT_LIFECYCLE_SHUTDOWN) {
        nt_app_quit();
        return;
    }
    gui_pack_clear(tp_id128_nil());
    gui_shell_reset_shown_result();
    cancel_edit();
    gui_view_reconcile_observation(gui_project_snapshot());
    reset_selection();
    /* A replacement project is "newly current": adopt its first atlas so the
     * panels open populated (reconciliation itself only clears). */
    gui_view_adopt_default_atlas(gui_project_snapshot());
    if (completed ==
        GUI_PROJECT_LIFECYCLE_NEW) {
        set_status("New project.");
    } else {
        set_statusf(
            "Opened %s",
            gui_project_display_name());
    }
}
