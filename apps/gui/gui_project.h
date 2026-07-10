#ifndef NTPACKER_GUI_PROJECT_H
#define NTPACKER_GUI_PROJECT_H

/* GUI-owned project state (ux.md §3.3b, Task 2): exactly ONE tp_project + its absolute
 * file path (empty while unsaved) + two independent dirty bits:
 *   - project_dirty : unsaved changes on disk. Cleared by Save. Shows the menu-bar dot.
 *   - preview_stale : model changed since the last successful pack. Since in-process
 *                     packing is blocked (engine #282), nothing clears it this round --
 *                     once set it stays set, driving the Pack button's stale state.
 *
 * Every model mutation goes through a wrapper here that funnels the two bits through one
 * choke point (gui_project_touch), so no caller can forget to mark dirty/stale. File
 * operations take explicit paths (the OS dialogs live in the UI layer) so they can be
 * driven headless by the startup self-test. */

#include <stddef.h>

#include "tp_core/tp_project.h"

#ifdef __cplusplus
extern "C" {
#endif

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
/* Sets BOTH project_dirty and preview_stale. Every mutation wrapper calls it. */
void gui_project_touch(void);
/* Clears preview_stale after a successful pack (unused this round; #282). */
void gui_project_mark_packed(void);

/* --- mutation wrappers (all funnel through gui_project_touch) --- */
int gui_project_add_atlas(void);                     /* returns new atlas index, or -1 */
void gui_project_remove_atlas(int index);
bool gui_project_add_source(int atlas_index, const char *path); /* stored verbatim; relativized on save */
void gui_project_remove_source(int atlas_index, int source_index);

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
