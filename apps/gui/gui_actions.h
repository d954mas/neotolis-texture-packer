#ifndef NTPACKER_GUI_ACTIONS_H
#define NTPACKER_GUI_ACTIONS_H

/* Model/state mutation layer for the ntpacker GUI: the deferred side-effect queue (s_pending_*) +
 * its pump (apply_pending), the pack/export/undo/redo/refresh actions, file dialogs + add-files/
 * folder, the new/open/save/exit unsaved-changes confirm flow, inline-rename commits + the start-edit
 * entry points (start_atlas_edit/start_anim_edit/start_sprite_edit_named/start_sprite_edit -- the
 * entry side of the same edit lifecycle, moved here in step 4 so every view that starts an inline
 * edit shares one home), the animation ops + preview player, and the small selection/edit helpers.
 * Split out of main.c (GUI decomposition step 2) as a pure move -- no behavior change. This layer is
 * Clay-free AND nt_ui-free: views set the s_pending_* flags; apply_pending consumes them at the top
 * of the next frame. Include discipline: actions -> gui_state + gui_rows + model headers
 * (gui_project/gui_scan/gui_canvas/gui_pack/gui_history) + tinyfiledialogs; it must never include
 * widgets or any view header. */

#include <stdbool.h>

#include "tp_core/tp_project.h" /* tp_project_anim (current_anim return type) */

#include "gui_rows.h" /* sprite_row (start_sprite_edit parameter) */

#ifdef __cplusplus
extern "C" {
#endif

/* --- deferred side-effect queue (set by views, consumed by apply_pending) --- */
extern bool s_pending_open, s_pending_save, s_pending_save_as, s_pending_add_files, s_pending_add_folder, s_pending_add_atlas, s_pending_refresh;
extern bool s_pending_pack, s_pending_export;
extern bool s_pending_commit_edit; /* a press landed outside the active inline-edit field -> commit it */
extern bool s_pending_add_anim;    /* "+ Animation" -> append empty animation, select it */
extern bool s_pending_create_anim; /* "Create animation from selection" */
extern bool s_pending_open_preview;/* open the anim preview player on s_ctx_anim / s_sel_anim */
extern int s_pending_remove_atlas;
extern int s_pending_remove_source;
extern int s_pending_remove_anim;  /* animation index to remove */
extern bool s_pending_add_target;
extern int s_pending_remove_target;
extern int s_pending_browse_target;        /* target whose out-path "..." dialog is queued */
extern int s_pending_export_browse_atlas;  /* Export dialog: atlas of the queued out-path browse */
extern int s_pending_export_browse_target; /* Export dialog: target index of the queued browse */

/* --- new/open/exit unsaved-changes confirm flow --- */
enum { AFTER_NONE = 0, AFTER_NEW, AFTER_EXIT, AFTER_OPEN };
extern int s_after_confirm;
extern bool s_confirm_open;
enum { MODAL_NONE = 0, MODAL_SAVE, MODAL_DISCARD, MODAL_CANCEL };
extern int s_modal_action;

/* --- last successful pack timing (written by the pack actions; read by the canvas stats line) --- */
extern double s_last_pack_ms;   /* wall-clock ms of the last successful pack (for the stats line) */
extern int s_last_pack_atlas;   /* which atlas that timing belongs to */

/* --- deferred side-effect pump: lands async pack/export, commits blur edits, drains the queue --- */
void apply_pending(void);

/* --- pack / export / undo / redo / refresh --- */
void do_pack(void);          /* interactive async pack of the selected atlas */
void do_pack_blocking(void); /* deterministic blocking pack (selftest + --shot) */
void do_undo(void);
void do_redo(void);

/* --- new/open/exit confirm flow entry points --- */
void request_new(void);
void request_open(void);
void request_exit(void);

/* --- selection / edit helpers --- */
void reset_selection(void);
void clamp_selection(void);
void cancel_edit(void);
void preview_stop(void);

/* --- start-edit entry points (pair with the inline-rename commits below) --- */
void start_atlas_edit(int i);
void start_anim_edit(int i);
void start_sprite_edit_named(const char *sprite_name);
void start_sprite_edit(const sprite_row *row);

/* --- inline rename commits --- */
void commit_atlas_rename(void);
void commit_sprite_rename(void);
void commit_anim_rename(void);

/* --- animation ops + preview player (ux.md §3.7b) --- */
tp_project_anim *current_anim(void); /* selected animation of the selected atlas, or NULL */
int create_animation_from_selection(void);
void add_selection_frames_to_anim(int anim_index);
void open_preview(int anim_index);
void preview_toggle_play(void);
void preview_step(int delta);
void update_preview(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_ACTIONS_H */
