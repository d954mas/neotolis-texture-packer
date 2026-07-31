#ifndef NTPACKER_GUI_ACTIONS_H
#define NTPACKER_GUI_ACTIONS_H

/* Model/state mutation layer for the ntpacker GUI: the single value-draft
 * owner, between-frame structural requests, pack/export/undo/redo/refresh,
 * file dialogs, lifecycle confirmation, animation operations, and preview.
 * This layer is Clay-free AND nt_ui-free. Views declare values into typed
 * drafts or enqueue structural requests with stable identities;
 * gui_actions_step consumes them at the next between-frame boundary. Include
 * discipline:
 * actions -> gui_state + gui_rows + model headers
 * (gui_project/gui_canvas/gui_pack) + tinyfiledialogs; it must never include
 * widgets or any view header. */

#include <stdbool.h>

#include "gui_edit_state.h"
#include "tp_core/tp_pack_result.h"   /* tp_result (preview_target_result return type) */

#include "gui_project_view.h" /* passive identities + deferred-edit enqueue types */
#include "gui_rows.h"    /* sprite_row (start_sprite_edit parameter) */

#ifdef __cplusplus
extern "C" {
#endif

/* --- deferred side-effect queue (enqueued by views, consumed by gui_actions_step) ---
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
void gui_request_cancel(void);
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
typedef enum gui_lifecycle_phase {
    GUI_LIFECYCLE_IDLE = 0,
    GUI_LIFECYCLE_RESOLVE_DRAFT,
    GUI_LIFECYCLE_RESOLVE_DIRTY,
    GUI_LIFECYCLE_OPEN_DIALOG
} gui_lifecycle_phase;
typedef enum gui_lifecycle_choice {
    GUI_LIFECYCLE_CHOICE_NONE = 0,
    GUI_LIFECYCLE_CHOICE_ACCEPT,
    GUI_LIFECYCLE_CHOICE_DISCARD,
    GUI_LIFECYCLE_CHOICE_CANCEL
} gui_lifecycle_choice;
typedef struct gui_lifecycle_view {
    gui_lifecycle_phase phase;
    gui_lifecycle_request request;
} gui_lifecycle_view;
/* Passive state + typed input are the whole lifecycle UI contract. The
 * controller owns all transition ordering inside gui_actions_step. */
gui_lifecycle_view gui_actions_lifecycle_view(void);
bool gui_actions_lifecycle_active(void);
void gui_actions_lifecycle_choose(gui_lifecycle_choice choice);
void gui_actions_lifecycle_dismiss(void);

/* --- startup crash-recovery flow --- */
typedef enum gui_recovery_phase {
    GUI_RECOVERY_IDLE = 0,
    GUI_RECOVERY_CHOOSE,
    GUI_RECOVERY_RESOLVING
} gui_recovery_phase;
typedef struct gui_recovery_view {
    gui_recovery_phase phase;
    int count;
    bool has_more;
} gui_recovery_view;
void gui_actions_open_recovery(
    const gui_recovery_list *list);
gui_recovery_view gui_actions_recovery_view(void);
bool gui_actions_recovery_active(void);
const gui_recovery_entry *gui_actions_recovery_at(int i);
bool gui_actions_recovery_request(
    int row, gui_recovery_action action);
void gui_actions_recovery_dismiss(void);

/* --- passive last-successful-Pack presentation state --- */
typedef struct gui_last_pack_view {
    double duration_ms;
    tp_id128 atlas_id;
} gui_last_pack_view;
gui_last_pack_view gui_actions_last_pack_view(void);

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

/* Raised by a view widget when an edit gesture ends. gui_actions_step submits the
 * active draft once, producing one transaction and one Undo step. */
void gui_request_gesture_commit(void);

/* --- pack / export / undo / redo / refresh --- */
/* View-facing semantic ingress. The request is consumed by gui_actions_step
 * between frames; neither menu declaration nor keyboard handling mutates the
 * session directly. */
void gui_request_undo(void);
void gui_request_redo(void);
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

/* --- animation ops + preview player --- */
const tp_snapshot_animation *preview_animation(void); /* active stable-ID target, or NULL */
#ifdef NTPACKER_GUI_SELFTEST
int create_animation_from_selection(void);
#endif
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
