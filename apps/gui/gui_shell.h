#ifndef NTPACKER_GUI_SHELL_H
#define NTPACKER_GUI_SHELL_H

/* Shell-owned symbols other TUs read. main.c OWNS these symbols; this header exists only so the
 * handful of other TUs that touch them can see the declarations. It is NOT a god-header -- keep it
 * minimal, and each entry stays only as long as its owner does (several are interim: they move out
 * when the TU that will own them permanently exists). Split out of main.c as part of the GUI
 * decomposition (step 3).
 *
 * Today that surface is: the nt_ui retained-state pool capacities (main() provisions the UI context
 * with them and the selftest logs them to prove the row model stays bounded), and
 * close_menubar_menus (interim, step 4 -- gui_view_settings' target-row context-menu trigger needs
 * to close the File/Edit/View/Help menus like every other right-click trigger does). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nt_ui retained-state pool capacities. nt_ui_state has NO per-frame eviction -- a state cell persists
 * until its id is reused -- so the pool must hold every DISTINCT id seen over a session. The sprite
 * list virtualizes (nt_ui_vlist): per-row ids RECYCLE through the vlist id_ring, so live widgets are
 * bounded by the viewport (id_ring x per-row stateful children + fixed chrome), not by project size.
 *   UI_STATE_SLOTS     : power-of-2 retained-state cells. Was 512 -> overflowed once a 500-file folder
 *                        scrolled through every ring slot (256 ring x [hit + x] ~= 512).
 *   UI_STATE_PROBE_MAX : linear-probe window. The engine default (4) is far too small at this load --
 *                        placement failed well below capacity and fired the "pool overflow" assert.
 *   UI_ROW_ID_RING     : per-row id recycle modulus; must exceed max visible rows. */
#define UI_STATE_SLOTS ((uint32_t)4096U)
#define UI_STATE_PROBE_MAX ((uint32_t)64U)
#define UI_ROW_ID_RING ((uint32_t)128U)

/* Closes the File/Edit/View/Help menubar menus. Every right-click context-menu trigger calls this
 * first (a right-click while a menubar menu is open should close it, not stack menus). Definition
 * stays in main.c: its body mutates s_file/edit/view/help_state, which remain shell-owned until the
 * menu bar itself moves in step 6b (gui_view_chrome) -- interim: moves there then. */
void close_menubar_menus(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SHELL_H */
