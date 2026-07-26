#ifndef NTPACKER_GUI_ACTIONS_H
#define NTPACKER_GUI_ACTIONS_H

/* Model/state mutation layer for the ntpacker GUI: the deferred side-effect queue (s_pending_*) +
 * its pump (apply_pending), the pack/export/undo/redo/refresh actions, file dialogs + add-files/
 * folder, the new/open/save/exit unsaved-changes confirm flow, inline-rename commits + the start-edit
 * entry points (start_atlas_edit/start_anim_edit/start_sprite_edit_ref/start_sprite_edit -- the
 * entry side of the same edit lifecycle, so every view that starts an inline
 * edit shares one home), the animation ops + preview player, and the small selection/edit helpers.
 * This layer is Clay-free AND nt_ui-free: views enqueue typed requests that capture stable structural IDs,
 * expected revision, typed arguments, and owned copied strings; apply_pending consumes them at the
 * top of the next frame. Include discipline: actions -> gui_state + gui_rows + model headers
 * (gui_project/gui_scan/gui_canvas/gui_pack) + tinyfiledialogs; it must never include
 * widgets or any view header. */

#include <stdbool.h>

#include "tp_core/tp_model.h"   /* tp_result (preview_target_result return type) */

#include "gui_project.h" /* gui_atlas_field / gui_sprite_ov (deferred-edit enqueue types) */
#include "gui_rows.h"    /* sprite_row (start_sprite_edit parameter) */

#ifdef __cplusplus
extern "C" {
#endif

/* --- deferred side-effect queue (set by views, consumed by apply_pending) --- */
extern bool s_pending_open, s_pending_save, s_pending_save_as, s_pending_add_files, s_pending_add_folder, s_pending_add_atlas, s_pending_refresh;
extern bool s_pending_pack, s_pending_export;
extern bool s_pending_commit_edit; /* a press landed outside the active inline-edit field -> commit it */
extern bool s_pending_commit_edit_enter; /* Enter pressed in the inline editor -> commit it (deferred, non-force) */
void gui_request_add_animation(int atlas_index);
void gui_request_create_animation_from_selection(void);
void gui_request_open_preview(const gui_animation_ref *animation);
extern bool s_pending_remove_atlas;
extern tp_id128 s_pending_remove_atlas_id;
extern int64_t s_pending_remove_atlas_revision;
extern bool s_pending_remove_source;
extern tp_id128 s_pending_remove_source_atlas_id;
extern tp_id128 s_pending_remove_source_id;
extern int64_t s_pending_remove_source_revision;
void gui_request_remove_animation(int animation_index);
void gui_request_remove_animation_ref(const gui_animation_ref *animation);
void gui_request_add_target(int atlas_index);
void gui_request_remove_target(int target_index);
void gui_request_remove_target_ref(const gui_target_ref *target);
void gui_request_browse_target(int atlas_index, int target_index);
extern int s_pending_preview_target; /* boundary-ok: exporter option, not a target entity index */

/* --- new/open/exit unsaved-changes confirm flow --- */
enum { AFTER_NONE = 0, AFTER_NEW, AFTER_EXIT, AFTER_OPEN };
extern int s_after_confirm;
extern bool s_confirm_open;
enum { MODAL_NONE = 0, MODAL_SAVE, MODAL_DISCARD, MODAL_CANCEL };
extern int s_modal_action;

/* --- R6b: startup crash-recovery modal glue --- */
extern bool s_recovery_open;                       /* R6b: the startup recovery modal is visible */
/* The orphan list is stashed inside gui_actions.c; the modal (gui_view_chrome.c) reads it via count/at
 * and requests a per-row action, which is DEFERRED to the next apply_pending() (so the Save-As file dialog
 * and the disk-mutating resolve run outside nt_ui_begin/end, like s_pending_save_as). */
void gui_actions_open_recovery(const gui_recovery_list *list); /* copy list, set s_recovery_open (main() startup) */
int  gui_actions_recovery_count(void);
bool gui_actions_recovery_has_more(void);
const gui_recovery_entry *gui_actions_recovery_at(int i);      /* NULL if out of range */
void gui_actions_recovery_request(int row, int action);        /* action = gui_recovery_action; deferred */

/* --- last successful pack timing (written by the pack actions; read by the canvas stats line) --- */
extern double s_last_pack_ms;   /* wall-clock ms of the last successful pack (for the stats line) */
extern int s_last_pack_atlas;   /* which atlas that timing belongs to */

/* --- deferred MODEL-EDIT queue (decision 0015) ---
 * A commit clone-swaps the model and frees the old project, so a declare_* render fn must
 * NEVER commit while holding a live atlas/sprite/anim/target pointer (the whole
 * pointer-invalidation UAF class). Instead the declare fns ENQUEUE the edit here (capturing
 * the atlas INDEX + typed args + copied strings -- never a pointer); apply_pending drains
 * the queue at frame top, where no live declare-fn pointer is held, by calling the
 * self-contained gui_project_* setters. Coalescable setters BUFFER into gui_project's pending
 * transaction (they do not commit on drain); a gesture boundary raises
 * gui_request_gesture_commit(), which apply_pending honours by flushing the buffer -- so one
 * interaction = one committed transaction = one undo step (decision 0015). */
void gui_queue_atlas_setting(tp_id128 atlas_id, int64_t expected_revision,
                             gui_atlas_field field, int ivalue, float fvalue);
void gui_queue_sprite_origin(const gui_sprite_ref *sprite, int axis, float value); /* axis 0=X, 1=Y (#2) */
void gui_queue_sprite_slice9(const gui_sprite_ref *sprite, int lrtb_index, int value);
void gui_queue_sprite_override(const gui_sprite_ref *sprite, gui_sprite_ov which, int value);
void gui_edit_anim_fps(const gui_animation_ref *animation, float fps);
void gui_edit_anim_playback(const gui_animation_ref *animation, int playback);
void gui_edit_anim_flip(const gui_animation_ref *animation, bool flip_h, bool flip_v);
void gui_edit_anim_frame_remove(const gui_animation_ref *animation, int frame_index);
void gui_edit_anim_frame_move(const gui_animation_ref *animation, int frame_index, int delta);
/* Enqueue "Add frames": COPIES canonical refs into the edit so the drain can replay them next
 * frame -- "Add frames" must NOT commit synchronously from the anim editor's declare fn (F1 UAF). */
void gui_edit_anim_add_frames(const gui_animation_ref *animation,
                              const tp_op_sprite_ref *frames, int count);
void gui_edit_target(const gui_target_ref *target, const char *exporter_id,
                     const char *out_path, bool enabled);
/* H/G3: the out-path text field's per-keystroke enqueue -- drains to the COALESCABLE setter so an edit
 * gesture (typing then Enter/blur) collapses into ONE undo step, unlike the immediate gui_edit_target. */
void gui_edit_target_out_path(const gui_target_ref *target, const char *out_path);
/* H/G3: discrete enabled/exporter enqueues -- carry ONLY the changed field; the drain setters read the
 * un-edited fields from the committed record post-flush, so a discrete edit mid-typing never reverts the
 * just-typed out_path. Use these instead of gui_edit_target for the enabled checkbox / exporter dropdown. */
void gui_edit_target_enabled(const gui_target_ref *target, bool enabled);
void gui_edit_target_exporter(const gui_target_ref *target,
                              const char *exporter_id);

/* --- deferred side-effect pump: lands async pack/export, commits blur edits, drains the queue --- */
void apply_pending(void);

/* Raised by a view widget when an EDIT GESTURE ENDS (slider release / field Enter+blur / a discrete
 * dropdown/checkbox pick): apply_pending flushes gui_project's pending transaction AFTER the queue
 * drains, committing the whole gesture as ONE undo step (decision 0015). */
void gui_request_gesture_commit(void);

/* --- pack / export / undo / redo / refresh --- */
void do_pack(void);          /* interactive async pack of the selected atlas */
void do_pack_blocking(void); /* deterministic blocking pack (selftest + --shot) */
void do_undo(void);
void do_redo(void);
/* F15: the synchronous refresh cost (fp_collect x2 + source invalidate + diff) with NO UI/status/canvas
 * side effects -- for the --bench-perf headless probe. Preserves the refresh semantic-purity invariant
 * (no revision/dirty change). Out params may be NULL; returns false on a scan failure. */
bool gui_actions_refresh_diff_headless(int *out_added, int *out_removed,
                                       int *out_changed);
/* Drops the retained last-successful runtime fingerprint. Production refresh
 * also resets automatically when the owning session changes. */
void gui_actions_refresh_fingerprint_reset(void);
/* Refresh policy seam: after runtime invalidation, even a later diff failure
 * leaves the retained preview stale. */
bool gui_actions_refresh_should_mark_stale(tp_status status,
                                           bool sources_invalidated);

/* --- new/open/exit confirm flow entry points --- */
void request_new(void);
void request_open(void);
void request_exit(void);

/* --- selection / edit helpers --- */
void reset_selection(void);
void clamp_selection(void);
void cancel_edit(void);
void preview_stop(void);

/* --- export-target preview (packet EXP-PREVIEW) --- */
void preview_target_reset(void);              /* back to Native: drop preview state + free the preview slot */
const tp_result *preview_target_result(void); /* the result the canvas binds this frame (preview or native) */

/* --- start-edit entry points (pair with the inline-rename commits below) --- */
void start_atlas_edit(int i);
void start_atlas_edit_ref(tp_id128 atlas_id, int64_t expected_revision);
void start_anim_edit(int i);
void start_anim_edit_ref(const gui_animation_ref *animation);
void start_sprite_edit_ref(const gui_sprite_ref *sprite,
                           const char *display_name);
void start_sprite_edit(const sprite_row *row);
bool gui_sprite_edit_matches(const sprite_row *row);
bool gui_atlas_edit_matches(tp_id128 atlas_id);
bool gui_animation_edit_matches(tp_id128 atlas_id, tp_id128 animation_id);

/* --- inline rename commits --- */
/* The live Enter/blur path is commit_active_edit (static, gui_actions.c), which inlines the atlas +
 * animation rename and delegates the sprite rename to commit_sprite_rename. (fix4 deleted the dead
 * commit_atlas_rename / commit_anim_rename that had no callers.) */
void commit_sprite_rename(void);

/* --- animation ops + preview player (ux.md §3.7b) --- */
const tp_snapshot_animation *preview_animation(void); /* active stable-ID target, or NULL */
int create_animation_from_selection(void);
void add_selection_frames_to_anim(int anim_index);
void open_preview(int anim_index);
void preview_toggle_play(void);
void preview_step(int delta);
void update_preview(void);

#ifdef NTPACKER_GUI_SELFTEST
typedef struct gui_preview_frame_work {
    uint64_t rebuilds;
    uint64_t frame_span_lookups;
    uint64_t frame_iterations;
    uint64_t realloc_calls;
} gui_preview_frame_work;
void gui_preview_frame_work_reset(void);
gui_preview_frame_work gui_preview_frame_work_get(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_ACTIONS_H */
