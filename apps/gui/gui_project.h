#ifndef NTPACKER_GUI_PROJECT_H
#define NTPACKER_GUI_PROJECT_H

/* GUI-owned project state (ux.md §3.3b/§3.3c, Task 2): exactly ONE tp_project + its
 * absolute file path (empty while unsaved) + two independent dirty bits:
 *   - project_dirty : unsaved changes on disk. RECOMPUTED by comparing the current
 *                     model snapshot to the last-SAVED snapshot (so undoing back to the
 *                     saved state clears it). Cleared by Save/Open/New. Menu-bar dot.
 *   - preview_stale : model changed since the last successful pack. Since in-process
 *                     packing is blocked (engine #282), nothing clears it this round --
 *                     once set it stays set, driving the Pack button's stale state.
 *
 * Every model mutation goes through a wrapper here that funnels through one choke point
 * (gui_project_touch): it serializes the model, memcmp-dedups against the previous
 * snapshot, pushes the PRE-mutation snapshot onto the undo history (gui_history), and
 * recomputes project_dirty. Undo/redo swap a snapshot back into the live model.
 *
 * Refresh (F4) is deliberately NOT a model mutation: rescanning disk sources changes what
 * is DISPLAYED/packed, not the PROJECT MODEL (sources are paths). So Refresh calls
 * gui_project_mark_stale (preview_stale only) and never touches project_dirty.
 *
 * File operations take explicit paths (the OS dialogs live in the UI layer) so they can be
 * driven headless by the startup self-test. */

#include <stddef.h>

#include "tp_core/tp_project.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an add-source attempt (the GUI surfaces "already added" distinctly). */
typedef enum { GUI_ADD_FAILED = 0, GUI_ADD_ADDED, GUI_ADD_DUPLICATE } gui_add_status;

/* Action tag for undo coalescing: same-tag mutations within one gesture window fold
 * into a single undo entry (ux.md §3.3c). */
typedef enum {
    GUI_ACT_NONE = 0,
    GUI_ACT_ADD_SOURCE,
    GUI_ACT_REMOVE_SOURCE,
    GUI_ACT_ADD_ATLAS,
    GUI_ACT_REMOVE_ATLAS,
    GUI_ACT_RENAME_ATLAS,
    GUI_ACT_RENAME_SPRITE,
    GUI_ACT_SET_SETTING, /* atlas knob or per-sprite override edit (coalesces a gesture) */
    GUI_ACT_SET_TARGET,
    GUI_ACT_ADD_TARGET,
    GUI_ACT_REMOVE_TARGET,
    GUI_ACT_ADD_ANIM,
    GUI_ACT_REMOVE_ANIM,
    GUI_ACT_RENAME_ANIM,
    GUI_ACT_SET_ANIM,   /* fps / playback / flips edit (coalesces a gesture) */
    GUI_ACT_ANIM_FRAMES /* add / remove / reorder frames */
} gui_action;

/* Per-sprite packing-override field selector (region-panel "Packing overrides"). */
typedef enum {
    GUI_SPRITE_OV_SHAPE = 0,
    GUI_SPRITE_OV_ROTATE,
    GUI_SPRITE_OV_MAXVERT,
    GUI_SPRITE_OV_MARGIN,
    GUI_SPRITE_OV_EXTRUDE
} gui_sprite_ov;

/* Creates the initial in-memory project (one default atlas, no path, clean). */
void gui_project_init(void);
void gui_project_shutdown(void);

/* --- accessors --- */
tp_project *gui_project_get(void);
const char *gui_project_path(void);         /* absolute file path, or "" while unsaved */
const char *gui_project_display_name(void); /* file basename, or "untitled" */
bool gui_project_has_path(void);
bool gui_project_is_dirty(void);
bool gui_project_is_stale(void);

/* --- dirty/stale choke point --- */
/* Serializes + snapshots the model, pushes the pre-mutation snapshot to undo history
 * (coalesced by `act`), sets preview_stale, and recomputes project_dirty. Every
 * mutation wrapper calls it AFTER mutating. */
void gui_project_touch(gui_action act);
/* Clears preview_stale after a successful pack (unused this round; #282). */
void gui_project_mark_packed(void);
/* Marks the preview stale WITHOUT dirtying the project (Refresh: disk changed, model
 * did not). */
void gui_project_mark_stale(void);
/* Advances the history clock (seconds) each frame -- drives undo coalescing windows. */
void gui_project_tick(double now_seconds);

/* --- mutation wrappers (all funnel through gui_project_touch) --- */
int gui_project_add_atlas(void);                          /* returns new atlas index, or -1 */
void gui_project_remove_atlas(int index);
gui_add_status gui_project_add_source(int atlas_index, const char *path); /* stored verbatim; relativized on save */
void gui_project_remove_source(int atlas_index, int source_index);

/* Renames atlas `index` (caller validates non-empty/unique/normalization-safe). */
bool gui_project_set_atlas_name(int atlas_index, const char *name);
/* Sets/clears a sprite's rename export-name override (empty/NULL clears it). */
bool gui_project_set_sprite_rename(int atlas_index, const char *sprite_name, const char *rename);

/* Records an atlas-knob edit that the caller made in place on the tp_project_atlas:
 * funnels through the touch choke point (dirty + preview stale + coalesced undo). */
void gui_project_touch_setting(void);

/* --- region-panel per-sprite overrides (sparse: a clear that leaves only defaults
 * drops the override entry, keeping byte-identical saves) --- */
bool gui_project_set_sprite_origin(int atlas_index, const char *sprite_name, float ox, float oy);
bool gui_project_set_sprite_slice9(int atlas_index, const char *sprite_name, int lrtb_index, int value);
/* Per-sprite packing override; `value` == TP_PROJECT_OV_INHERIT clears it. */
bool gui_project_set_sprite_override(int atlas_index, const char *sprite_name, gui_sprite_ov which, int value);

/* --- animations (ux.md §3.7b: explicit manual assembly only) --- */
/* Appends an animation and fills it with `frames` (in the given order) as ONE undo entry. The id is
 * the first free of {base, base"2", base"3", ...}; a NULL/empty base auto-names "anim1"/"anim2"/...
 * `frames` may be NULL/0 for an empty animation. Returns the new animation index, or -1. */
int gui_project_create_animation(int atlas_index, const char *base, const char *const *frames, int frame_count);
/* Removes the animation with `id`. */
void gui_project_remove_animation(int atlas_index, const char *id);
/* True if the atlas already has an animation named `id`. */
bool gui_project_anim_id_exists(int atlas_index, const char *id);
/* Renames animation `anim_index`; fails on empty or a name already used by another animation. */
bool gui_project_set_anim_id(int atlas_index, int anim_index, const char *new_id);
bool gui_project_set_anim_fps(int atlas_index, int anim_index, float fps);         /* clamps >= 1 */
bool gui_project_set_anim_playback(int atlas_index, int anim_index, int playback); /* clamps 0..6 */
bool gui_project_set_anim_flip(int atlas_index, int anim_index, bool flip_h, bool flip_v);
/* Appends `frames` (in order) to animation `anim_index` as ONE undo entry. */
bool gui_project_anim_add_frames(int atlas_index, int anim_index, const char *const *frames, int count);
bool gui_project_anim_remove_frame(int atlas_index, int anim_index, int frame_index);
bool gui_project_anim_move_frame(int atlas_index, int anim_index, int frame_index, int delta);

/* --- export targets (region G, audit I1) --- */
/* Appends a default json-neotolis target "out/<atlas>.<ext>"; returns its index or -1. */
int gui_project_add_target(int atlas_index);
void gui_project_remove_target(int atlas_index, int index);
bool gui_project_set_target(int atlas_index, int index, const char *exporter_id, const char *out_path, bool enabled);

/* --- undo / redo (ux.md §3.3c) --- */
bool gui_project_can_undo(void);
bool gui_project_can_redo(void);
/* Swap a history snapshot into the live model; recomputes dirty, sets stale, drops the
 * display caches. Selection re-clamp is the caller's job. Returns false when the stack
 * is empty (or on a restore error). */
bool gui_project_undo(void);
bool gui_project_redo(void);

/* --- file operations (paths explicit; dialogs live in the UI layer) --- */
/* Fresh empty project: replaces the current one, clears path + both bits. */
void gui_project_new(void);
/* Loads `path`; on failure fills err_out (from tp_error) and leaves the current project
 * intact. On success replaces it, sets path, clears dirty, marks preview stale. */
tp_status gui_project_open(const char *path, char *err_out, size_t err_cap);
/* Saves to the current path (must exist). Clears project_dirty. */
tp_status gui_project_save(char *err_out, size_t err_cap);
/* Saves to `path`, remembers it, clears project_dirty. */
tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PROJECT_H */
