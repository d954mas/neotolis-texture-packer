#ifndef NTPACKER_GUI_VIEW_CANVAS_H
#define NTPACKER_GUI_VIEW_CANVAS_H

/* Center canvas view (ux.md region E): the dual-mode atlas/source canvas card, its action strip
 * (Pack/Export/Refresh + pages + zoom + stale chip), the animation preview player, the floating
 * message pill, the stale-page overlay, and the empty/no-sources hero state. Declare-only: exposes
 * the one entry point frame() calls.
 *
 * P-2 lead ruling (recorded in docs/plans/gui-decomposition.md §2): handle_canvas_input (the
 * wheel/pan/click-select mouse state machine) + its mouse statics (s_lmb_armed, s_lmb_panning,
 * s_mmb_panning, s_press_x/y, s_pan_last_x/y) stay in the SHELL (main.c) permanently -- it reads
 * nt_ui_get_bbox(s_id_status_pill) so a click on the pill doesn't also drive canvas select/pan,
 * which is shell-territory input detection by the same precedent as the click-outside-commit /
 * blur-inputs carve-outs. This TU owns only the DECLARE side of the canvas.
 *
 * Include discipline: this view TU may include gui_defs/gui_state/gui_widgets/gui_actions/gui_rows
 * headers + model headers (gui_canvas/gui_pack/gui_project) + gui_shell.h (interim
 * close_menubar_menus prototype) + nt_ui/Clay; it must never include another view TU's header
 * (gui_view_settings.h / gui_view_lists.h). */

#include "ui/nt_ui.h" /* nt_ui_context_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Declares the center canvas card (or the animation preview player, when active), filling the
 * middle column between the left and right panels. */
void declare_canvas(nt_ui_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_VIEW_CANVAS_H */
