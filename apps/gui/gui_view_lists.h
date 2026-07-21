#ifndef NTPACKER_GUI_VIEW_LISTS_H
#define NTPACKER_GUI_VIEW_LISTS_H

/* Left panel view (ux.md region D): the atlases list, the sprite source tree (vlist-virtualized),
 * and the animations list, each with its add-button row, inline rename, drag/hover row visuals, and
 * right-click context-menu trigger. Declare-only: exposes the one entry point frame() calls. Include
 * discipline: this view TU may include gui_defs/gui_state/gui_widgets/gui_actions/gui_rows headers +
 * model headers (gui_canvas/gui_pack/gui_project) + gui_shell.h (interim close_menubar_menus
 * prototype) + nt_ui/Clay; it must never include another view TU's header (gui_view_settings.h). */

#include <stdbool.h>

#include "ui/nt_ui.h" /* nt_ui_context_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Declares the left panel (atlases / sprites / animations lists), docked at its clamped width. */
void declare_left_panel(nt_ui_context_t *ctx);

/* --- sprite-list keyboard focus model (U-02 T3) ---
 * Operate on the current filtered/sorted view (s_view). The caller (frame()) gates key input
 * (no field focus / modal / headless) and translates keys into these calls after build_view(). */
void gui_list_focus_step(int delta, bool extend);  /* Up/Down (+Shift extend selection) */
void gui_list_focus_edge(bool end, bool extend);   /* Home/End */
void gui_list_focus_activate(void);                /* Enter: toggle folder, else select */
void gui_list_focus_rename(void);                  /* F2: inline-rename the focused leaf */
void gui_list_focus_collapse(bool expand);         /* Right=expand folder / Left=collapse or jump to parent */

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_VIEW_LISTS_H */
