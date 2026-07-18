/* Model/state mutation layer for the ntpacker GUI (see gui_actions.h). This TU is Clay-free AND
 * nt_ui-free: it reads/mutates the model + shared state only. */

#include "gui_actions.h"
#include "gui_actions_internal.h"

#include "gui_defs.h" /* S() -- the compact-strip stop that folds the preview selector away */
#include "gui_state.h"
#include "gui_rows.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_paths.h"
#include "gui_shell.h" /* reset the canvas borrow across pack/history transitions */
#include "tinyfiledialogs.h"

#include "tp_core/tp_export.h" /* tp_exporter_at -> the preview selector's exporter list */
#include "tp_core/tp_names.h"  /* tp_names_common_prefix (anim id from selection) */

#include "app/nt_app.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* deferred side effects (dialogs + model mutations), applied at the top of the next frame */
bool s_pending_open, s_pending_save, s_pending_save_as, s_pending_add_files, s_pending_add_folder, s_pending_add_atlas, s_pending_refresh;
bool s_pending_pack, s_pending_export;
bool s_pending_commit_edit; /* a press landed outside the active inline-edit field -> commit it */
bool s_pending_commit_edit_enter; /* Enter in the inline editor -> commit it (deferred, non-force) */

/* Presentation-only mapping from the active stable animation to one Pack result. */

bool s_pending_remove_atlas;
tp_id128 s_pending_remove_atlas_id;
int64_t s_pending_remove_atlas_revision;
bool s_pending_remove_source;
tp_id128 s_pending_remove_source_atlas_id;
tp_id128 s_pending_remove_source_id;
int64_t s_pending_remove_source_revision;
int s_pending_preview_target = -1; /* boundary-ok: exporter option, not a target entity index */
int s_after_confirm;
bool s_confirm_open;
int s_modal_action;
/* R6b: startup crash-recovery modal glue. The orphan list lives here; the modal reads it via the
 * count/at accessors and requests a per-row action, deferred to apply_pending() (below) so the
 * Save-As dialog + disk-mutating gui_recovery_resolve run outside nt_ui_begin/end, like s_pending_save_as. */
bool s_recovery_open;
double s_last_pack_ms;      /* wall-clock ms of the last successful pack (for the stats line) */
int s_last_pack_atlas = -1; /* which atlas that timing belongs to */

gui_actions_state s_actions = {.recovery_pending_row = -1};




_Static_assert(sizeof s_actions.edit_sprite_source_key == TP_SRCKEY_MAX,
               "editor source-key buffer must match the canonical bound");

/* True (and raises a status) when an async pack/export is running: the destructive ops (new/open/exit/
 * undo/redo) refuse while busy. Centralizes the guard the request_* fns had copy-pasted, and closes the
 * gap where undo/redo skipped it (P2 -- undo mid-pack then a pre-undo result landing was confusing). */
bool gui_actions__busy_block(void) {
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Wait for the pack/export to finish (or Cancel) first.");
        return true;
    }
    return false;
}

// #region undo/redo + refresh actions
void do_undo(void) {
    if (gui_actions__busy_block()) {
        return; /* same async-busy guard as new/open/exit -- undo mid-pack then a pre-undo land is confusing */
    }
    if (gui_actions__flush_failed()) {
        return; /* fix4 [2]: a buffered gesture could not be journaled -> abort (error surfaced), never
                 * undo a DIFFERENT step. After this, gui_project_undo()'s false means empty-history only
                 * (undo does NOT journal), so "Nothing to undo." below is correct again. */
    }
    if (gui_project_undo()) {
        gui_shell_reset_shown_result();
        cancel_edit();
        clamp_selection();
        reset_selection();
        gui_canvas_invalidate(&s_canvas);
        set_statusf("Undo (undo:%d redo:%d)", gui_project_undo_depth(), gui_project_redo_depth());
    } else {
        set_status("Nothing to undo.");
    }
}
void do_redo(void) {
    if (gui_actions__busy_block()) {
        return;
    }
    if (gui_actions__flush_failed()) {
        return; /* fix4 [2]: a journal-failed flush is not "Nothing to redo" -- abort (error surfaced) */
    }
    if (gui_project_redo()) {
        gui_shell_reset_shown_result();
        cancel_edit();
        clamp_selection();
        reset_selection();
        gui_canvas_invalidate(&s_canvas);
        set_statusf("Redo (undo:%d redo:%d)", gui_project_undo_depth(), gui_project_redo_depth());
    } else {
        set_status("Nothing to redo.");
    }
}

/* Fingerprint every source (folders expand to their scanned children, files stat
 * directly) so a Refresh can diff added/removed/changed. Missing entries carry
 * size==-1 so a vanish/restore reads as removed/added. */
typedef struct fp_entry {
    char *abs;
    long long size;
    long long mtime;
} fp_entry;

static void fp_free(fp_entry *entries, int count) {
    for (int i = 0; i < count; ++i) {
        free(entries[i].abs);
    }
    free(entries);
}

static tp_status fp_push(fp_entry **arr, int *count, int *cap,
                         const char *abs, long long size, long long mtime,
                         tp_error *error) {
    if (*count == *cap) {
        if (*cap > INT_MAX / 2) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "refresh fingerprint has too many entries");
        }
        int nc = *cap ? *cap * 2 : 64;
        if ((size_t)nc > SIZE_MAX / sizeof **arr) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "refresh fingerprint table overflows size_t");
        }
        fp_entry *ne =
            (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
        if (!ne) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "refresh fingerprint allocation failed");
        }
        *arr = ne;
        *cap = nc;
    }
    char *path = gui_actions__strdup(abs);
    if (!path) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "refresh fingerprint path allocation failed");
    }
    (*arr)[*count] = (fp_entry){path, size, mtime};
    (*count)++;
    return TP_STATUS_OK;
}

static tp_status fp_collect(fp_entry **arr, int *count, int *cap,
                            tp_error *error) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int ai = 0; ai < atlas_count; ai++) {
        const tp_snapshot_atlas *a = tp_session_snapshot_atlas_at(snapshot, ai);
        for (int si = 0; si < a->source_count; si++) {
            const tp_snapshot_source *source = tp_session_snapshot_source_at(snapshot, a->id, si);
            char abs[TP_IDENTITY_PATH_MAX];
            if (!source || tp_session_snapshot_resolve_path(snapshot, a->id, source->id,
                                                            abs, sizeof abs, error) != TP_STATUS_OK) {
                continue;
            }
            if (gui_scan_is_dir(abs)) {
                const gui_scan_result *sc = NULL;
                tp_status scan_status = gui_scan_get(abs, &sc, error);
                if (scan_status != TP_STATUS_OK) {
                    return scan_status;
                }
                for (int ci = 0; ci < sc->count; ci++) {
                    tp_status push_status = fp_push(
                        arr, count, cap, sc->entries[ci].abs,
                        sc->entries[ci].size, sc->entries[ci].mtime, error);
                    if (push_status != TP_STATUS_OK) {
                        return push_status;
                    }
                }
            } else {
                long long sz = -1;
                long long mt = -1;
                (void)gui_scan_stat(abs, &sz, &mt);
                tp_status push_status = fp_push(arr, count, cap, abs, sz, mt,
                                                error);
                if (push_status != TP_STATUS_OK) {
                    return push_status;
                }
            }
        }
    }
    return TP_STATUS_OK;
}

static const fp_entry *fp_find(const fp_entry *arr, int n, const char *abs) {
    for (int i = 0; i < n; i++) {
        if (strcmp(arr[i].abs, abs) == 0) {
            return &arr[i];
        }
    }
    return NULL;
}

/* F4: rescan all sources, diff, evict the canvas cache, mark preview stale (NOT dirty). */
static void do_refresh(void) {
    fp_entry *before = NULL;
    int bn = 0;
    int bc = 0;
    tp_error error = {0};
    tp_status status = fp_collect(&before, &bn, &bc, &error);
    if (status != TP_STATUS_OK) {
        fp_free(before, bn);
        set_statusf_ex(STATUS_ERROR, "Refresh failed: %s", error.msg);
        return;
    }

    gui_project_invalidate_sources(); /* publish the external runtime refresh */

    fp_entry *after = NULL;
    int an = 0;
    int ac = 0;
    status = fp_collect(&after, &an, &ac, &error);
    if (status != TP_STATUS_OK) {
        fp_free(before, bn);
        fp_free(after, an);
        set_statusf_ex(STATUS_ERROR, "Refresh failed: %s", error.msg);
        return;
    }

    int added = 0;
    int removed = 0;
    int changed = 0;
    for (int i = 0; i < an; i++) {
        const fp_entry *b = fp_find(before, bn, after[i].abs);
        if (!b) {
            added++;
        } else if (b->size != after[i].size || b->mtime != after[i].mtime) {
            changed++;
        }
    }
    for (int i = 0; i < bn; i++) {
        if (!fp_find(after, an, before[i].abs)) {
            removed++;
        }
    }
    fp_free(before, bn);
    fp_free(after, an);

    gui_canvas_invalidate(&s_canvas); /* force the shown image to reload (or show missing) */
    gui_project_mark_stale();         /* disk changed -> preview stale, project NOT dirtied */
    set_statusf("Refresh: +%d new, %d removed, %d changed", added, removed, changed);
}

// #endregion

// #region inline rename commit
void commit_sprite_rename(void) {
    /* empty input clears the override back to the file-derived name. (Reached only via commit_active_edit,
     * which flush-firsts, so set_sprite_rename's own flush is a no-op here.) */
    const gui_sprite_ref sprite = {s_actions.edit_sprite_atlas_id, s_actions.edit_sprite_source_id,
                                   s_actions.edit_sprite_source_key, s_actions.edit_sprite_revision};
    if (gui_project_set_sprite_rename(&sprite, s_edit_buf)) {
        if (s_edit_buf[0] == '\0') {
            set_statusf("Cleared rename on '%s'", s_edit_sprite);
        } else {
            set_statusf("Renamed '%s' -> '%s'", s_edit_sprite, s_edit_buf);
        }
    }
    cancel_edit();
}

/* Commit the active inline edit as if Enter was pressed (click-outside / model-change path).
 * `force` = the editor is being dismissed involuntarily: an invalid atlas name CANCELS instead of
 * keeping a zombie editor (the validation message stays in the status bar). Sprite rename never
 * zombies (empty clears the override). No-op when nothing is being edited.
 *
 * fix4 (DEFINITIVE closure of the journal-failed-flush class): FLUSH-FIRST at the entry via
 * gui_actions__flush_failed(). Any buffered gesture is committed-or-aborted HERE; on a journal-failed flush the
 * neutral error is surfaced and we abort (keeping the editor open unless forced, so the user can retry
 * after freeing disk). After a successful flush, each rename op's OWN internal flush is a guaranteed
 * no-op, so its return is DOMAIN-ONLY. The anim branch also routes through an op
 * (set_anim_id builds TP_OP_ANIMATION_RENAME -> commit_txn_now), so a false there is a core reject whose
 * structured message rides the op-error channel -- surfaced directly, no anim_id_exists heuristic (fix3's
 * heuristic was wrong: it matched the anim's own unchanged name). set_atlas_name / set_sprite_rename /
 * set_anim_id journal their rename op; on that op's OWN append failure they return false -> no success
 * message + the op-error surfaces (not a false success, not a wrong "already exists"). */
static void commit_active_edit(bool force) {
    if (s_edit_kind == EDIT_NONE) {
        return; /* nothing being edited */
    }
    const tp_session_snapshot *before_flush = gui_project_snapshot();
    const int64_t revision_before_flush = before_flush
        ? tp_session_snapshot_revision(before_flush)
        : 0;
    const bool rebase_atlas = s_edit_kind == EDIT_ATLAS &&
                              s_actions.edit_atlas_revision == revision_before_flush;
    const bool rebase_sprite = s_edit_kind == EDIT_SPRITE &&
                               s_actions.edit_sprite_revision == revision_before_flush;
    const bool rebase_anim = s_edit_kind == EDIT_ANIM &&
                             s_actions.edit_anim_revision == revision_before_flush;
    if (gui_actions__flush_failed()) {
        /* a buffered gesture could not be journaled -> abort the rename commit (error already surfaced) */
        if (force) {
            cancel_edit();
        }
        return;
    }
    const tp_session_snapshot *after_flush = gui_project_snapshot();
    const int64_t revision_after_flush = after_flush
        ? tp_session_snapshot_revision(after_flush)
        : revision_before_flush;
    if (revision_after_flush != revision_before_flush) {
        if (rebase_atlas) {
            s_actions.edit_atlas_revision = revision_after_flush;
        } else if (rebase_sprite) {
            s_actions.edit_sprite_revision = revision_after_flush;
        } else if (rebase_anim) {
            s_actions.edit_anim_revision = revision_after_flush;
        }
    }
    if (s_edit_kind == EDIT_ATLAS) {
        const tp_session_snapshot *snapshot = gui_project_snapshot();
        if (!snapshot || !tp_session_snapshot_atlas_by_id(snapshot, s_actions.edit_atlas_id)) {
            set_status_ex(STATUS_WARNING, "The atlas changed before the rename could be applied.");
            cancel_edit();
            return;
        }
        if (!gui_project_set_atlas_name(s_actions.edit_atlas_id, s_actions.edit_atlas_revision,
                                        s_edit_buf)) {
            char atlas_err[256];
            if (gui_project_take_op_error(atlas_err, sizeof atlas_err)) {
                set_status_ex(STATUS_WARNING, atlas_err);
            } else {
                set_status_ex(STATUS_ERROR, "Could not rename the atlas.");
            }
            if (force) {
                cancel_edit();
            }
            return;
        }
        char committed_name[TP_SRCKEY_MAX];
        tp_error read_error = {0};
        if (gui_project_copy_atlas_name(s_actions.edit_atlas_id, committed_name,
                                        sizeof committed_name, &read_error) == TP_STATUS_OK) {
            set_statusf("Renamed atlas to '%s'", committed_name);
        } else {
            set_status_ex(STATUS_ERROR,
                          read_error.msg[0] ? read_error.msg : "Renamed atlas could not be read back.");
        }
        cancel_edit();
    } else if (s_edit_kind == EDIT_SPRITE) {
        commit_sprite_rename();
    } else if (s_edit_kind == EDIT_ANIM) {
        const gui_animation_ref animation = {
            s_actions.edit_anim_atlas_id, s_actions.edit_anim_id, s_actions.edit_anim_revision};
        if (!gui_project_set_anim_id(&animation, s_edit_buf)) {
            /* Animation rename is a first-class op now (undoable + journaled), so a false is a
             * core REJECT -- a name collision (validate enforces uniqueness) or a rare OOM/stale-index
             * failure. The structured reject rides commit_txn_now's op-error channel, so surface it
             * DIRECTLY instead of re-deriving the reason with the old anim_id_exists heuristic (which
             * could not tell the anim's own unchanged name from a real clash). The entry gui_actions__flush_failed()
             * already handled the journal-fail case, so the op-error here is the domain reject. */
            char anim_err[256];
            if (gui_project_take_op_error(anim_err, sizeof anim_err)) {
                set_status_ex(STATUS_WARNING, anim_err);
            } else {
                set_status_ex(STATUS_ERROR, "Could not rename the animation.");
            }
            if (force) {
                cancel_edit();
            }
            return;
        }
        set_statusf("Renamed animation to '%s'", s_edit_buf);
        cancel_edit();
    }
}
// #endregion



// #region deferred side-effects (run at the top of the frame, between frames)
void apply_pending(void) {
    gui_actions__poll_pack();

    /* A press landed outside the active inline editor last frame -> commit it (desktop rename UX).
     * Also fires before any pending model change (remove/refresh/open/new) so no orphaned editor
     * survives a mutation. */
    if (s_edit_kind != EDIT_NONE && s_pending_commit_edit) {
        commit_active_edit(true);
    }
    s_pending_commit_edit = false;
    /* Enter pressed in an inline editor last frame -> commit it here (deferred, non-force: an
     * invalid atlas/anim name keeps the editor open). Deferring the commit off the declare pass is
     * what keeps declare_left_panel / the anim editor from committing while holding proj/a/an
     * (UAF fix). */
    if (s_edit_kind != EDIT_NONE && s_pending_commit_edit_enter) {
        commit_active_edit(false);
    }
    s_pending_commit_edit_enter = false;

    /* Drain the deferred model-edit queue (settings / overrides / anim knobs / target edits the
     * declare fns enqueued last frame). Runs here, at frame top, with no live declare-fn pointer
     * held -- so the per-edit clone-swap can never dangle a panel's cached atlas/sprite/anim/target
     * pointer (decision 0015). */
    gui_actions__drain_edits();

    /* A gesture ended last frame (slider release / field Enter+blur / discrete pick): commit the
     * buffered transaction NOW that gui_actions__drain_edits has folded in this frame's final value, so one
     * interaction == one undo step (decision 0015). */
    if (s_actions.gesture_commit) {
        /* fix2: the bool is intentionally IGNORED -- this is the gesture-BOUNDARY commit (one interaction
         * = one undo step). A journal-failed flush here drops the gesture WITH the op-error surfaced
         * (poll_async shows it); there is no persist/discard "proceed as clean" decision after it to
         * abort (unlike save/new/pack). Audited fix2 [0]/[1]. */
        (void)gui_project_flush_pending();
        s_actions.gesture_commit = false;
    }

    gui_actions__apply_confirm();

    gui_actions__apply_recovery();

    gui_actions__apply_file_dialogs();
    if (s_pending_add_atlas) {
        int idx = gui_project_add_atlas();
        if (idx >= 0) {
            s_sel_atlas = idx;
            reset_selection();
            const tp_snapshot_atlas *added = tp_session_snapshot_atlas_at(gui_project_snapshot(), idx);
            set_statusf("Added atlas '%s'", added ? added->name : "?");
        }
    }
    if (s_pending_remove_source) {
        /* fix3 [0]: side-effects + "Removed" run ONLY on a real removal -- a journal-failed flush
         * aborts the wrapper (returns false), so no false "Removed" / bad Ctrl+Z (op-error surfaced). */
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
            clamp_selection();
            reset_selection();
            set_status("Removed atlas (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_add_target) {
        const int ti = gui_project_add_target(s_actions.pending_add_target_atlas_id,
                                              s_actions.pending_add_target_revision);
        if (ti >= 0) {
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
        const int idx = gui_project_create_animation(
            s_actions.pending_add_anim_atlas_id, s_actions.pending_add_anim_revision,
            NULL, NULL, 0);
        if (idx >= 0) {
            const tp_session_snapshot *after_snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *after_atlas = after_snapshot
                ? tp_session_snapshot_atlas_by_id(
                      after_snapshot, s_actions.pending_add_anim_atlas_id)
                : NULL;
            const tp_snapshot_animation *animation = after_atlas
                                                         ? tp_session_snapshot_animation_at(after_snapshot, after_atlas->id, idx)
                                                         : NULL;
            const tp_snapshot_atlas *selected = after_snapshot
                ? tp_session_snapshot_atlas_at(after_snapshot, s_sel_atlas)
                : NULL;
            if (selected && tp_id128_eq(selected->id, s_actions.pending_add_anim_atlas_id)) {
                s_sel_anim = idx;
                s_sel_anim_frame = -1;
            }
            set_statusf("Added animation '%s' (Ctrl+Z to undo).", animation ? animation->name : "?");
        }
    }
    if (s_actions.pending_create_anim.active) {
        const int idx = gui_project_create_animation(
            s_actions.pending_create_anim.atlas_id,
            s_actions.pending_create_anim.expected_revision,
            s_actions.pending_create_anim.name[0] ? s_actions.pending_create_anim.name : NULL,
            s_actions.pending_create_anim.frames,
            s_actions.pending_create_anim.frame_count);
        if (idx >= 0) {
            const tp_session_snapshot *after_snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *after_atlas = after_snapshot
                ? tp_session_snapshot_atlas_by_id(
                      after_snapshot, s_actions.pending_create_anim.atlas_id)
                : NULL;
            const tp_snapshot_animation *animation = after_atlas
                ? tp_session_snapshot_animation_at(after_snapshot,
                                                   after_atlas->id, idx)
                : NULL;
            const tp_snapshot_atlas *selected = after_snapshot
                ? tp_session_snapshot_atlas_at(after_snapshot, s_sel_atlas)
                : NULL;
            if (selected &&
                tp_id128_eq(selected->id, s_actions.pending_create_anim.atlas_id)) {
                s_sel_anim = idx;
                s_sel_anim_frame = -1;
            }
            set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).",
                        animation ? animation->name : "?",
                        s_actions.pending_create_anim.frame_count);
        }
    }
    gui_actions__pending_create_animation_dispose(
        &s_actions.pending_create_anim);
    if (s_actions.pending_remove_anim) {
            /* fix3 [0]: preview_stop + the selection reset + "Removed" run ONLY on a real removal. A
             * journal-failed flush aborts the wrapper (returns false) -> the animation is still there,
             * so we must NOT stop its preview or clear the selection. (preview_stop only resets flags,
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
                s_sel_anim = -1;
                s_sel_anim_frame = -1;
                set_status("Removed animation (Ctrl+Z to undo).");
            }
    }
    if (s_actions.pending_open_preview) {
        int atlas_index = -1;
        int animation_index = -1;
        if (gui_actions__resolve_animation_ref(
                &s_actions.pending_open_preview_ref, &atlas_index,
                &animation_index)) {
            s_sel_atlas = atlas_index;
            open_preview(animation_index);
        }
    }
    if (s_pending_refresh) {
        do_refresh();
    }
    gui_actions__apply_pack_requests();

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
    s_actions.pending_browse_target = false;
    memset(&s_actions.pending_browse_target_ref, 0,
           sizeof s_actions.pending_browse_target_ref);
    s_pending_preview_target = -1;
}
// #endregion
