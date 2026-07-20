#ifndef NTPACKER_GUI_VIEW_LISTS_H
#define NTPACKER_GUI_VIEW_LISTS_H

/* Left panel view (ux.md region D): the atlases list, the sprite source tree (vlist-virtualized),
 * and the animations list, each with its add-button row, inline rename, drag/hover row visuals, and
 * right-click context-menu trigger. Declare-only: exposes the one entry point frame() calls. Include
 * discipline: this view TU may include gui_defs/gui_state/gui_widgets/gui_actions/gui_rows headers +
 * model headers (gui_canvas/gui_pack/gui_project) + gui_shell.h (interim close_menubar_menus
 * prototype) + nt_ui/Clay; it must never include another view TU's header (gui_view_settings.h). */

#include "ui/nt_ui.h" /* nt_ui_context_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Declares the left panel (atlases / sprites / animations lists), docked at its clamped width. */
void declare_left_panel(nt_ui_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_VIEW_LISTS_H */
