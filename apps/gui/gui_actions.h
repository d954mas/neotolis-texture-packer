#ifndef NTPACKER_GUI_ACTIONS_H
#define NTPACKER_GUI_ACTIONS_H

/* Model/state mutation layer for the ntpacker GUI: the single value-draft
 * owner, between-frame structural requests, pack/export/undo/redo/refresh,
 * file dialogs, lifecycle confirmation, animation operations, and preview.
 * This layer is Clay-free AND nt_ui-free. Views declare values into typed
 * drafts or enqueue structural requests with stable identities; apply_pending
 * consumes them at the top of the next frame. Include discipline:
 * actions -> gui_state + gui_rows + model headers
 * (gui_project/gui_scan/gui_canvas/gui_pack) + tinyfiledialogs; it must never include
 * widgets or any view header. */

#include <stdbool.h>

#include "gui_edit_state.h"
#include "tp_core/tp_model.h"   /* tp_result (preview_target_result return type) */

#include "gui_project_view.h" /* passive identities + deferred-edit enqueue types */
#include "gui_rows.h"    /* sprite_row (start_sprite_edit parameter) */

#ifdef __cplusplus
extern "C" {
#endif

/* --- deferred side-effect queue (enqueued by views, consumed by apply_pending) ---
 * There is ONE queue and it is private to the actions layer. A view declares an
 * intent through the functions below and can neither read nor reorder what is
 * queued; the drain order is the actions layer's contract, not a view's. */
void gui_request_save(void);
void gui_request_save_as(void);
void gui_request_add_files(void);
void gui_request_add_folder(void);
void gui_request_add_atlas(void);
void gui_request_refresh(void);
void gui_request_pack(void);
void gui_request_export(void);
void gui_request_add_animation(tp_id128 atlas_id, int64_t expected_revision);
void gui_request_create_animation_from_selection(void);
void gui_request_open_preview(const gui_animation_ref *animation);
void gui_request_remove_atlas(tp_id128 atlas_id, int64_t expected_revision);
void gui_request_remove_source(tp_id128 atlas_id, tp_id128 source_id,
                               int64_t expected_revision);
void gui_request_remove_animation_ref(const gui_animation_ref *animation);
void gui_request_add_target(tp_id128 atlas_id, int64_t expected_revision);
void gui_request_remove_target_ref(const gui_target_ref *target);
void gui_request_browse_target_ref(const gui_target_ref *target);
/* boundary-ok: an exporter option slot, not a target entity index */
void gui_request_preview_target(int exporter_slot);

/* --- new/open/exit draft resolution + unsaved-changes confirm flow --- */
typedef enum gui_lifecycle_request {
    GUI_LIFECYCLE_REQUEST_NONE = 0,
    GUI_LIFECYCLE_REQUEST_NEW,
    GUI_LIFECYCLE_REQUEST_OPEN,
    GUI_LIFECYCLE_REQUEST_EXIT
} gui_lifecycle_request;
extern gui_lifecycle_request s_after_confirm;
extern bool s_confirm_open;
extern bool s_confirm_draft;
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
extern tp_id128 s_last_pack_atlas_id;

/* --- draft-owned semantic ingress ---
 * A value edit stores stable identity, exact component, typed value, and
 * captured revision. The gesture boundary submits one typed operation. */
void gui_edit_atlas_setting(tp_id128 atlas_id, int64_t expected_revision,
                            gui_atlas_field field, int ivalue, float fvalue);
bool gui_atlas_edit_value(tp_id128 atlas_id, gui_atlas_field field,
                          int *ivalue, float *fvalue);
gui_edit_phase gui_draft_phase(void);
bool gui_draft_can_apply(void);
void gui_draft_apply_mine(void);
void gui_draft_discard(void);
bool gui_actions_copy_text_available(void);
void gui_actions_copy_text(const char *text);
bool gui_text_edit_begin_atlas_name(tp_id128 atlas_id,
                                    int64_t expected_revision,
                                    const char *initial_value);
bool gui_text_edit_begin_animation_name(const gui_animation_ref *animation,
                                        const char *initial_value);
bool gui_text_edit_begin_sprite_rename(const gui_sprite_ref *sprite,
                                       const char *initial_value);
bool gui_text_edit_begin_target_out_path(const gui_target_ref *target,
                                         const char *initial_value);
bool gui_text_edit_update(const char *value);
const char *gui_text_edit_value(void);
bool gui_target_path_edit_matches(const gui_target_ref *target);
bool gui_inline_text_edit_active(void);
typedef enum gui_sprite_edit_kind {
    GUI_SPRITE_EDIT_ORIGIN = 0,
    GUI_SPRITE_EDIT_SLICE9,
    GUI_SPRITE_EDIT_OVERRIDE
} gui_sprite_edit_kind;
typedef enum gui_animation_edit_kind {
    GUI_ANIMATION_EDIT_FPS = 0,
    GUI_ANIMATION_EDIT_PLAYBACK,
    GUI_ANIMATION_EDIT_FLIP
} gui_animation_edit_kind;
void gui_edit_sprite_origin(const gui_sprite_ref *sprite, int axis, float value);
void gui_edit_sprite_slice9(const gui_sprite_ref *sprite, int component, int value);
void gui_edit_sprite_override(const gui_sprite_ref *sprite, gui_sprite_ov component, int value);
bool gui_sprite_edit_value(const gui_sprite_ref *sprite,
                           gui_sprite_edit_kind kind, int component,
                           int *integer, float *real);
void gui_edit_anim_fps(const gui_animation_ref *animation, float fps);
void gui_edit_anim_playback(const gui_animation_ref *animation, int playback);
void gui_edit_anim_flip(const gui_animation_ref *animation, int axis, bool value);
bool gui_animation_edit_value(const gui_animation_ref *animation,
                              gui_animation_edit_kind kind, int component,
                              int *integer, float *real);
void gui_edit_anim_frame_remove(const gui_animation_ref *animation, int frame_index);
void gui_edit_anim_frame_move(const gui_animation_ref *animation, int frame_index, int delta);
/* Enqueue "Add frames": COPIES canonical refs into the edit so the drain can replay them next
 * frame -- "Add frames" must NOT commit synchronously from the anim editor's declare fn (F1 UAF). */
void gui_edit_anim_add_frames(const gui_animation_ref *animation,
                              const tp_op_sprite_ref *frames, int count);
void gui_edit_target_enabled(const gui_target_ref *target, bool enabled);
void gui_edit_target_exporter(const gui_target_ref *target,
                              const char *exporter_id);

/* --- deferred semantic ingress: commits blur edits and drains action queues --- */
void apply_pending(void);
/* Releases the action layer's own heap at exit: every intent still queued (with
 * the frame refs / names it owns) and the preview frame map. Session, pack and
 * row state are NOT touched -- each of those owners has its own shutdown, and
 * this one holds no reference to any of them, so it runs first. */
void gui_actions_shutdown(void);
/* Consumes only host-classified Pack/Export completions after the frame's
 * atomic observation has reduced them. */
void gui_actions_poll_host_completion(void);
/* Drives the sole aggregate lifecycle owner between frames and consumes typed
 * cutover/shutdown receipts before the next observation is pinned. */
void gui_actions_pump_lifecycle(void);

/* Raised by a view widget when an edit gesture ends. apply_pending submits the
 * active draft once, producing one transaction and one Undo step. */
void gui_request_gesture_commit(void);

/* --- pack / export / undo / redo / refresh --- */
void do_pack(void);          /* interactive async pack of the selected atlas */
void do_pack_blocking(void); /* deterministic blocking pack (selftest + --shot) */
/* View-facing semantic ingress. The request is consumed by apply_pending()
 * between frames; neither menu declaration nor keyboard handling mutates the
 * session directly. */
void gui_request_undo(void);
void gui_request_redo(void);
/* Host-side executors retained for focused action/self-tests. Shipping views
 * must use gui_request_undo/gui_request_redo. */
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
void cancel_edit(void);
void preview_stop(void);

/* --- export-target preview (packet EXP-PREVIEW) --- */
void preview_target_reset(void);              /* back to Native: drop preview state + free the preview slot */
const tp_result *preview_target_result(void); /* the result the canvas binds this frame (preview or native) */
uint64_t preview_target_result_version(void);
bool preview_target_result_is_export(void);

/* --- start-edit entry points (pair with the inline-rename commits below) --- */
void start_atlas_edit_ref(tp_id128 atlas_id, int64_t expected_revision);
void start_anim_edit_ref(const gui_animation_ref *animation);
void start_sprite_edit_ref(const gui_sprite_ref *sprite,
                           const char *display_name);
void start_sprite_edit(const sprite_row *row);
bool gui_sprite_edit_matches(const sprite_row *row);
bool gui_atlas_edit_matches(tp_id128 atlas_id);
bool gui_animation_edit_matches(tp_id128 atlas_id, tp_id128 animation_id);

/* --- animation ops + preview player (ux.md §3.7b) --- */
const tp_snapshot_animation *preview_animation(void); /* active stable-ID target, or NULL */
int create_animation_from_selection(void);
void add_selection_frames_to_animation(
    const gui_animation_ref *animation);
void open_preview_ref(const gui_animation_ref *animation);
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
