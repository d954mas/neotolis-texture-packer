#ifndef NTPACKER_GUI_VIEW_CHROME_H
#define NTPACKER_GUI_VIEW_CHROME_H

/* Chrome view: the docked File/Edit/View/Help menu bar (+ its four drop-down menus), the row/canvas
 * right-click context menu, the hover-tooltip passes (per-frame truncated-row tooltips + the fixed
 * toolbar/strip icon-button tooltips), and the three modals (unsaved-changes confirm, About, Export).
 * Exposes the frame declaration entry points, menu close operations, and one
 * read-only menu-open query used by shell keyboard routing.
 *
 * close_menubar_menus is chrome-owned but cross-view-consumed: gui_view_lists.c/gui_view_settings.c/
 * gui_view_canvas.c each call it on their row/canvas right-click trigger (a right-click while a
 * menubar menu is open should close it, not stack menus) -- same as every other right-click trigger.
 * Those three views keep including gui_shell.h for it (view<->view includes stay banned, so they
 * cannot include this header); gui_shell.h keeps a matching one-line extern documented as the
 * sanctioned cross-view surface. This header declares it too, for main.c's frame() (which calls
 * close_all_menus, defined here, whose body calls close_menubar_menus).
 *
 * Include discipline: this view TU may include gui_defs/gui_state/gui_widgets/gui_actions/gui_rows
 * headers + model headers (gui_canvas/gui_pack/gui_project) + gui_shell.h + nt_ui/Clay; it must never
 * include another view TU's header (gui_view_settings.h / gui_view_lists.h / gui_view_canvas.h). */

#include "ui/nt_ui.h" /* nt_ui_context_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Docked top menu bar: File/Edit/View/Help buttons + the project name/dirty-dot on the right. */
void declare_menubar(nt_ui_context_t *ctx);

/* The File/Edit/Atlas/View/Help drop-down menus (declared every frame; items no-op while closed). */
void declare_menus(nt_ui_context_t *ctx);

/* Row/canvas right-click context menu (ux.md §3.3e): items depend on which row armed it. */
void declare_context_menu(nt_ui_context_t *ctx);

/* Hover tooltips (full text) for this frame's truncated rows, recorded via record_row_tip. */
void declare_row_tooltips(nt_ui_context_t *ctx);

/* Fixed toolbar/strip icon-button tooltips (Pack/Export/Refresh, add-folder/add-files, the canvas
 * strip's page/zoom icon-only ghosts). */
void declare_tooltips(nt_ui_context_t *ctx);

/* Unsaved-changes confirm modal (Save / Discard / Cancel). */
void declare_confirm_modal(nt_ui_context_t *ctx);

/* R6b startup crash-recovery modal: lists every recovered crash-orphan and resolves each one via the
 * R6a layer (Discard / Save to original / Save As). Dormant while s_recovery_open is false. */
void declare_recovery_modal(nt_ui_context_t *ctx);

/* About modal (version / engine / repo link). */
void declare_about_modal(nt_ui_context_t *ctx);

/* Export dialog: every atlas's targets, toggle/browse per target, then Export. */
void declare_export_modal(nt_ui_context_t *ctx);

/* Closes the File/Edit/View/Help menubar menus AND the context menu. Called by frame()'s Esc handler
 * and by menubar_entry's open-another-menu path (both close everything before opening the next). */
void close_all_menus(void);

/* Closes only the File/Edit/View/Help menubar menus (not the context menu). The sanctioned
 * cross-view surface -- see the header comment above; gui_shell.h re-declares this one. */
void close_menubar_menus(void);

/* True while either the shared context menu or any menubar dropdown owns
 * keyboard navigation. Global/list handlers must return while this is true. */
bool gui_view_chrome_any_menu_open(void);

/* Closes and consumes Escape when any menu is open. Call before non-menu
 * Escape fallbacks so one key press cannot mutate unrelated view state. */
bool gui_view_chrome_consume_escape(void);

#ifdef NTPACKER_GUI_SELFTEST
/* Test-only state driver: index order is File/Edit/Atlas/View/Help. */
void gui_view_chrome_selftest_set_menubar_open(int index, bool open);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_VIEW_CHROME_H */
