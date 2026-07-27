#include "gui_actions_internal.h"
#include "gui_project.h"

#include <string.h>

#include "gui_state.h"

/* Between-frame structural intents: create/remove of atlases, sources, targets,
 * animations, plus the one-shot pending reset. These are discrete commands, not
 * editable values -- the draft owner in gui_actions_edits.c owns the latter. */

void gui_actions__apply_structural_edits(void) {
    if (s_pending_add_atlas) {
        const gui_project_create_result created =
            gui_project_add_atlas();
        if (created.committed) {
            gui_view_select_atlas(created.created_id);
            const tp_snapshot_atlas *added =
                !tp_id128_is_nil(created.created_id)
                    ? tp_session_snapshot_atlas_by_id(
                          gui_project_snapshot(),
                          created.created_id)
                    : NULL;
            set_statusf("Added atlas '%s'", added ? added->name : "?");
        }
    }
    if (s_pending_remove_source) {
        /* Side effects and success text run only after a real removal. */
        if (gui_project_remove_source(s_pending_remove_source_atlas_id,
                                      s_pending_remove_source_id,
                                      s_pending_remove_source_revision)) {
            reset_selection();
            set_status("Removed source (Ctrl+Z to undo).");
        }
    }
    if (s_pending_remove_atlas) {
        if (gui_project_remove_atlas(s_pending_remove_atlas_id,
                                     s_pending_remove_atlas_revision)) {
            gui_view_reconcile_observation(
                gui_project_snapshot());
            set_status("Removed atlas (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_add_target) {
        const gui_project_create_result created =
            gui_project_add_target(
                s_actions.pending_add_target_atlas_id,
                s_actions.pending_add_target_revision);
        if (created.committed) {
            set_status("Added export target (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_remove_target) {
        if (gui_project_remove_target(&s_actions.pending_remove_target_ref)) {
            set_status("Removed export target (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_browse_target) {
        gui_actions__browse_target(&s_actions.pending_browse_target_ref);
    }
    if (s_actions.pending_add_anim) {
        const gui_project_create_result created =
            gui_project_create_animation(
            s_actions.pending_add_anim_atlas_id, s_actions.pending_add_anim_revision,
            NULL, NULL, 0);
        if (created.committed) {
            const tp_session_snapshot *after_snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *after_atlas = after_snapshot
                ? tp_session_snapshot_atlas_by_id(
                      after_snapshot, s_actions.pending_add_anim_atlas_id)
                : NULL;
            const tp_snapshot_animation *animation =
                after_atlas
                    ? tp_session_snapshot_animation_by_id(
                          after_snapshot, after_atlas->id,
                          created.created_id)
                    : NULL;
            if (tp_id128_eq(
                    gui_view_atlas_id(),
                    s_actions.pending_add_anim_atlas_id)) {
                gui_view_select_animation(created.created_id);
            }
            set_statusf("Added animation '%s' (Ctrl+Z to undo).", animation ? animation->name : "?");
        }
    }
    if (s_actions.pending_create_anim.active) {
        const gui_project_create_result created =
            gui_project_create_animation(
            s_actions.pending_create_anim.atlas_id,
            s_actions.pending_create_anim.expected_revision,
            s_actions.pending_create_anim.name[0] ? s_actions.pending_create_anim.name : NULL,
            s_actions.pending_create_anim.frames,
            s_actions.pending_create_anim.frame_count);
        if (created.committed) {
            const tp_session_snapshot *after_snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *after_atlas = after_snapshot
                ? tp_session_snapshot_atlas_by_id(
                      after_snapshot, s_actions.pending_create_anim.atlas_id)
                : NULL;
            const tp_snapshot_animation *animation =
                after_atlas
                    ? tp_session_snapshot_animation_by_id(
                          after_snapshot, after_atlas->id,
                          created.created_id)
                    : NULL;
            if (tp_id128_eq(
                    gui_view_atlas_id(),
                    s_actions.pending_create_anim.atlas_id)) {
                gui_view_select_animation(created.created_id);
            }
            set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).",
                        animation ? animation->name : "?",
                        s_actions.pending_create_anim.frame_count);
        }
    }
    gui_actions__pending_create_animation_dispose(
        &s_actions.pending_create_anim);
    if (s_actions.pending_remove_anim) {
            /* preview_stop + selection reset + success text run only after a real removal.
             * On any operation rejection the animation remains, so we must not clear its UI state.
             * (preview_stop only resets flags,
             * so running it AFTER the removal is safe -- no project deref.) */
            const bool was_previewing =
                s_preview_active &&
                tp_id128_eq(s_actions.preview_animation_ref.atlas_id,
                            s_actions.pending_remove_anim_ref.atlas_id) &&
                tp_id128_eq(s_actions.preview_animation_ref.animation_id,
                            s_actions.pending_remove_anim_ref.animation_id);
            if (gui_project_remove_animation(&s_actions.pending_remove_anim_ref)) {
                if (was_previewing) {
                    preview_stop();
                }
                if (tp_id128_eq(
                        gui_view_animation_id(),
                        s_actions.pending_remove_anim_ref.animation_id)) {
                    gui_view_select_animation(
                        tp_id128_nil());
                }
                set_status("Removed animation (Ctrl+Z to undo).");
            }
    }
    if (s_actions.pending_open_preview) {
        open_preview_ref(
            &s_actions.pending_open_preview_ref);
    }
}

void gui_actions__clear_pending(void) {
    gui_actions__clear_history_request();
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_NONE;
    s_pending_open = s_pending_save = s_pending_save_as = false;
    s_pending_add_files = s_pending_add_folder = s_pending_add_atlas = false;
    s_pending_refresh = s_pending_pack = s_pending_export = false;
    s_actions.pending_add_target = false;
    s_actions.pending_add_target_atlas_id = tp_id128_nil();
    s_actions.pending_add_target_revision = 0;
    s_actions.pending_add_anim = false;
    s_actions.pending_open_preview = false;
    memset(&s_actions.pending_open_preview_ref, 0,
           sizeof s_actions.pending_open_preview_ref);
    s_actions.pending_add_anim_atlas_id = tp_id128_nil();
    s_actions.pending_add_anim_revision = 0;
    s_pending_remove_source = false;
    s_pending_remove_source_atlas_id = tp_id128_nil();
    s_pending_remove_source_id = tp_id128_nil();
    s_pending_remove_source_revision = 0;
    s_pending_remove_atlas = false;
    s_pending_remove_atlas_id = tp_id128_nil();
    s_pending_remove_atlas_revision = 0;
    s_actions.pending_remove_target = false;
    s_actions.pending_remove_anim = false;
    gui_actions__pending_create_animation_dispose(
        &s_actions.pending_create_anim);
    s_actions.pending_browse_target = false;
    memset(&s_actions.pending_browse_target_ref, 0,
           sizeof s_actions.pending_browse_target_ref);
    s_pending_preview_target = -1;
}
