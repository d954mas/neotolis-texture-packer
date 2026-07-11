#ifndef NTPACKER_GUI_VIEW_SETTINGS_H
#define NTPACKER_GUI_VIEW_SETTINGS_H

/* Right settings panel view (regions F/G + per-region packing overrides): atlas settings, the
 * selected region's fields + per-region packing overrides, export targets, and the animation editor.
 * Split out of main.c (GUI decomposition step 4) as a pure move -- no behavior change. Declare-only:
 * exposes the one entry point frame() calls. Include discipline: this view TU may include gui_defs/
 * gui_state/gui_widgets/gui_actions/gui_rows headers + model headers (gui_project/gui_pack) +
 * gui_shell.h (interim close_menubar_menus prototype) + nt_ui/Clay; it must never include another
 * view TU's header (none exist yet). */

#include "ui/nt_ui.h" /* nt_ui_context_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Declares the right settings panel (atlas settings / region + overrides / animation editor /
 * export targets), docked at its fixed (already-clamped) width. */
void declare_right_panel(nt_ui_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_VIEW_SETTINGS_H */
