#include "gui_actions_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_state.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_pack.h"
#include "gui_paths.h"
#include "gui_shell.h"
#include "tinyfiledialogs.h"

#include "app/nt_app.h"
// #region file dialogs (tinyfiledialogs)
static void do_open(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *path = tinyfd_openFileDialog("Open Project", "", 1, filt, "ntpacker project", 0);
    if (!path) {
        return;
    }
    if (!gui_scan_exists(path)) {
        set_statusf_ex(STATUS_WARNING, "project not found: %s", path); /* never fatal (F6b) */
        return;
    }
    char err[256];
    if (gui_project_open(path, err, sizeof err) == TP_STATUS_OK) {
        gui_pack_clear(-1);
        gui_shell_reset_shown_result();
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_statusf("Opened %s", gui_project_display_name());
    } else {
        set_statusf_ex(STATUS_ERROR, "Open failed: %s", err);
    }
}

static void do_save_as(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *def = gui_project_has_path() ? gui_project_path() : "untitled.ntpacker_project";
    const char *path = tinyfd_saveFileDialog("Save Project As", def, 1, filt, "ntpacker project");
    if (!path) {
        return;
    }
    char full[TP_IDENTITY_PATH_MAX];
    if (!gui_paths_project_file(path, full, sizeof full)) {
        set_status_ex(STATUS_ERROR,
                      "Save path is invalid or exceeds the supported path limit.");
        return;
    }
    char err[256];
    if (gui_project_save_as(full, err, sizeof err) == TP_STATUS_OK) {
        char notice[256];
        if (gui_project_take_save_notice(notice, sizeof notice)) {
            set_statusf_ex(STATUS_WARNING, "%s", notice);
        } else {
            set_statusf("Saved %s", gui_project_display_name());
        }
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
    }
}

static void do_save(void) {
    if (!gui_project_has_path()) {
        do_save_as();
        return;
    }
    char err[256];
    if (gui_project_save(err, sizeof err) == TP_STATUS_OK) {
        char notice[256];
        if (gui_project_take_save_notice(notice, sizeof notice)) {
            set_statusf_ex(STATUS_WARNING, "%s", notice);
        } else {
            set_statusf("Saved %s", gui_project_display_name());
        }
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
    }
}

void gui_request_remove_animation(int animation_index) {
    gui_animation_ref animation;
    if (gui_project_animation_ref_at(s_sel_atlas, animation_index, &animation)) {
        gui_request_remove_animation_ref(&animation);
    }
}

void gui_request_remove_animation_ref(const gui_animation_ref *animation) {
    if (animation && !tp_id128_is_nil(animation->atlas_id) &&
        !tp_id128_is_nil(animation->animation_id)) {
        s_actions.pending_remove_anim = true;
        s_actions.pending_remove_anim_ref = *animation;
    }
}

void gui_request_remove_target(int target_index) {
    gui_target_ref target;
    if (gui_project_target_ref_at(s_sel_atlas, target_index, &target)) {
        gui_request_remove_target_ref(&target);
    }
}


void gui_request_remove_target_ref(const gui_target_ref *target) {
    if (target && !tp_id128_is_nil(target->atlas_id) &&
        !tp_id128_is_nil(target->target_id)) {
        s_actions.pending_remove_target = true;
        s_actions.pending_remove_target_ref = *target;
    }
}

static bool selected_atlas_intent(tp_id128 *atlas_id, int64_t *revision) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                                         : NULL;
    if (!atlas) {
        return false;
    }
    *atlas_id = atlas->id;
    *revision = tp_session_snapshot_revision(snapshot);
    return true;
}

static void do_add_files(void) {
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

bool gui_actions__flush_failed(void); /* defined below; a discrete browse is a flush-first entry point */

/* Save dialog for a target's output path, relativized to the project like sources. Atlas-explicit so
 * the Export dialog (which spans all atlases) can browse any target, not just the selected atlas's. */
void gui_actions__browse_target(const gui_target_ref *queued) {
    /* H/G3: commit any BUFFERED out-path gesture FIRST so the Save dialog seeds from the just-typed path,
     * not a stale committed one (clicking the "..." button is in-panel, so no blur gesture-commit fired).
     * Route through gui_actions__flush_failed() like every other flush-first entry: an operation
     * rejection surfaces the error and aborts. Re-fetch a/t AFTER the flush
     * (a committed flush clone-swaps the project). */
    if (gui_actions__flush_failed()) {
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
    /* Browse changes one field only. Commit the masked out-path operation as
     * this dialog's discrete gesture; never round-trip exporter/enabled through
     * frontend buffers where a long registered format id could be truncated. */
    if (gui_project_set_target_out_path(&target, rel) &&
        gui_project_flush_pending()) {
        set_statusf("Output path: %s", rel);
    }
}

void gui_request_browse_target(int atlas_index, int target_index) {
    gui_target_ref target;
    if (gui_project_target_ref_at(atlas_index, target_index, &target)) {
        s_actions.pending_browse_target = true;
        s_actions.pending_browse_target_ref = target;
    }
}

void gui_request_add_target(int atlas_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    if (atlas) {
        s_actions.pending_add_target = true;
        s_actions.pending_add_target_atlas_id = atlas->id;
        s_actions.pending_add_target_revision = tp_session_snapshot_revision(snapshot);
    }
}

void gui_request_add_animation(int atlas_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    if (atlas) {
        s_actions.pending_add_anim = true;
        s_actions.pending_add_anim_atlas_id = atlas->id;
        s_actions.pending_add_anim_revision = tp_session_snapshot_revision(snapshot);
    }
}

static void do_add_folder(void) {
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
                                 ? gui_project_add_source(atlas_id, revision, norm)
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
/* A rejected buffered gesture leaves the model older than the caller expects.
 * Surface the operation error and tell the caller to abort, so a destructive
 * gate never discards the project and Pack never runs on stale state.
 * Returns true when the flush FAILED (the caller must abort); false when it is safe to proceed. */
bool gui_actions__flush_failed(void) {
    if (gui_project_flush_pending()) {
        return false; /* nothing pending / net-zero no-op / committed OK -> proceed */
    }
    char m[256];
    gui_project_flush_error(m, sizeof m); /* fix3 [2]: shared neutral wording (save/pack/gate) */
    set_status_ex(STATUS_ERROR, m);
    return true;
}

void request_new(void) {
    if (gui_actions__busy_block()) {
        return;
    }
    if (gui_actions__flush_failed()) {
        return; /* A rejected buffered edit must not be silently discarded. */
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_NEW;
        s_confirm_open = true;
    } else if (gui_project_new()) {
        gui_pack_clear(-1);
        gui_shell_reset_shown_result();
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_status("New project.");
    } else {
        set_status_ex(STATUS_ERROR, "Out of memory: could not create a new project (current project kept)."); /* F3 */
    }
}
void request_exit(void) {
    if (gui_actions__busy_block()) {
        return;
    }
    if (gui_actions__flush_failed()) {
        return; /* A rejected buffered edit must not be silently discarded. */
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_EXIT;
        s_confirm_open = true;
    } else {
        nt_app_quit();
    }
}
/* Open routes through the same unsaved-changes confirm as New/Exit (no silent discard). The actual
 * OS open dialog runs via s_pending_open, either now (clean) or after the modal resolves. */
void request_open(void) {
    if (gui_actions__busy_block()) {
        return;
    }
    if (gui_actions__flush_failed()) {
        return; /* A rejected buffered edit must not be silently discarded. */
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_OPEN;
        s_confirm_open = true;
    } else {
        s_pending_open = true;
    }
}
static void confirm_perform(void) {
    if (s_after_confirm == AFTER_NEW) {
        if (gui_project_new()) {
            gui_pack_clear(-1);
            gui_shell_reset_shown_result();
            cancel_edit();
            clamp_selection();
            reset_selection();
            set_status("New project.");
        } else {
            set_status_ex(STATUS_ERROR, "Out of memory: could not create a new project (current project kept)."); /* F3 */
        }
    } else if (s_after_confirm == AFTER_EXIT) {
        gui_project_discard_recovery_on_shutdown();
        nt_app_quit();
    } else if (s_after_confirm == AFTER_OPEN) {
        s_pending_open = true; /* runs the open dialog next frame */
    }
    s_after_confirm = AFTER_NONE;
}
// #endregion


void gui_actions__apply_confirm(void) {
    if (s_modal_action == MODAL_SAVE) {
        do_save();
        s_confirm_open = false;
        if (!gui_project_is_dirty()) {
            confirm_perform();
        } else {
            s_after_confirm = AFTER_NONE;
        }
    } else if (s_modal_action == MODAL_DISCARD) {
        s_confirm_open = false;
        confirm_perform();
    } else if (s_modal_action == MODAL_CANCEL) {
        s_confirm_open = false;
        s_after_confirm = AFTER_NONE;
    }
    s_modal_action = MODAL_NONE;
}

void gui_actions__apply_file_dialogs(void) {
    if (s_pending_open) {
        do_open();
    }
    if (s_pending_save) {
        do_save();
    }
    if (s_pending_save_as) {
        do_save_as();
    }
    if (s_pending_add_files) {
        do_add_files();
    }
    if (s_pending_add_folder) {
        do_add_folder();
    }
}
