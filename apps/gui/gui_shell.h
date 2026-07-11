#ifndef NTPACKER_GUI_SHELL_H
#define NTPACKER_GUI_SHELL_H

/* Shell-owned symbols other TUs read, PLUS the one sanctioned chrome-owned/view-consumed surface
 * (close_menubar_menus). main.c owns the pool capacities below; gui_view_chrome.c owns
 * close_menubar_menus. This header exists only so the handful of other TUs that touch them can see
 * the declarations -- it is NOT a god-header, keep it minimal. Split out of main.c as part of the GUI
 * decomposition (step 3).
 *
 * Today that surface is: the nt_ui retained-state pool capacities (main() provisions the UI context
 * with them and the selftest logs them to prove the row model stays bounded), and
 * close_menubar_menus (chrome-owned, view-consumed -- gui_view_lists/gui_view_settings/gui_view_canvas
 * each call it on their row/canvas right-click trigger, closing the File/Edit/View/Help menus like
 * every other right-click trigger does; view<->view includes stay banned, so those three views keep
 * reading the prototype from here rather than from gui_view_chrome.h). */

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
 * first (a right-click while a menubar menu is open should close it, not stack menus). Chrome-owned,
 * view-consumed: defined in gui_view_chrome.c (GUI decomposition step 6b) alongside the menu bar
 * itself; gui_view_chrome.h re-declares it too (for main.c's frame()) -- this is the sanctioned
 * cross-view surface so gui_view_lists.c/gui_view_settings.c/gui_view_canvas.c don't need to include
 * a sibling view header. */
void close_menubar_menus(void);

/* Clears the shell's cached "result currently bound to the canvas" pointer (s_shown_result). Called by
 * the destructive flows (undo/redo/new/open) right after gui_pack_clear(-1) frees the slot arenas: the
 * cache would otherwise hold a freed address that next frame's `want != s_shown_result` bind compares
 * (indeterminate-pointer read, C11 6.2.4 -- P2 hardening). NULL is always a valid comparand. */
void gui_shell_reset_shown_result(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SHELL_H */
