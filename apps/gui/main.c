/* ntpacker-gui -- native GUI for the Neotolis Texture Packer.
 *
 * PROJECT EDITOR + LIVE PACKER (this iteration): create/open/save .ntpacker_project files,
 * manage atlases, add image files/folders as sources, browse+select sprites, PACK atlases
 * through typed session jobs, render the real packed page on the center canvas with
 * zoom/pan + region overlays + selection sync, and export target files through the same boundary.
 * The session is the single mutable source of truth; the GUI reads owned snapshots as a thin editor
 * (AGENTS tool-parity). The Pack button surfaces the "preview stale" state per ux.md §3.3b.
 *
 * Module split: gui_project (state + dirty bits + load/save), gui_scan (display-only folder
 * enumeration), gui_pack (typed job adapter + presentation slots), gui_canvas (dual-mode
 * source-image / atlas-page custom nt_ui element). main.c is init/frame/shutdown + layout +
 * the OS file dialogs (tinyfiledialogs).
 *
 * Wiring template: external/neotolis-engine/examples/ui_showcase/main.c. */

// #region includes
#include "app/nt_app.h"
#include "atlas/nt_atlas.h"
#include "core/nt_assert.h"
#include "core/nt_core.h"
#include "font/nt_font.h"
#include "fpng/nt_fpng.h"
#include "fs/nt_fs.h"
#include "graphics/nt_gfx.h"
#include "hash/nt_hash.h"
#include "http/nt_http.h"
#include "input/nt_input.h"
#include "log/nt_log.h"
#include "material/nt_material.h"
#include "math/nt_math.h"
#include "memory/nt_mem_scratch.h"
#include "nt_pack_format.h"
#include "render/nt_render_defs.h"
#include "renderers/nt_shape_renderer.h"
#include "renderers/nt_sprite_renderer.h"
#include "renderers/nt_text_renderer.h"
#include "resource/nt_resource.h"
#include "ui/nt_ui.h"
#include "ui/nt_ui_button.h"
#include "ui/nt_ui_checkbox.h"
#include "ui/nt_ui_dropdown.h"
#include "ui/nt_ui_image.h"
#include "ui/nt_ui_input.h"
#include "ui/nt_ui_label.h"
#include "ui/nt_ui_menu.h"
#include "ui/nt_ui_modal.h"
#include "ui/nt_ui_scale.h"
#include "ui/nt_ui_scroll.h"
#include "ui/nt_ui_slider.h"
#include "ui/nt_ui_tooltip.h"
#include "ui/nt_ui_vlist.h"
#include "window/nt_window.h"

#include "ntpacker_ui_assets.h"
#if defined(_WIN32)
#include "nt_utf8_argv.h"
#endif

#include "clay.h"

#include "tp_core/tp_build_worker.h" /* private build-worker re-exec dispatch (decision 0018) */
#include "tp_core/tp_export.h"
#include "tp_core/tp_names.h" /* tp_sprite_export_key (slice9 frame-sync key) */

#include "gui_canvas.h"
#include "gui_bootstrap.h"
#include "gui_defs.h"
#include "gui_state.h"
#include "gui_widgets.h"
#include "gui_actions.h"
#include "gui_rows.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_shell.h"    /* shell-owned surface the dev seams read (UI pool caps) */
#include "gui_paths.h"    /* app-data root + exe-dir resolver (canonical home for s_exe_dir) */
#include "gui_log_file.h" /* rotating app-side log file (nt_log sink); no-op under headless */
#include "gui_crash.h"    /* crash handler + dump + marker (installed first thing in main) */
#include "gui_startup.h"  /* pure startup open/defer guard (gui_startup_decide) */
#include "gui_selftest.h" /* dev seam: headless self-test (compiled out unless flag on) */
#include "gui_shot.h"     /* dev seam: --shot screenshot capture */
#include "gui_bench.h"    /* dev seam: --bench-perf headless perf probe */
#include "gui_view_canvas.h"   /* center canvas view (declare_canvas) */
#include "gui_view_chrome.h"   /* menubar/menus/context menu/tooltips/modals (frame() entry points) */
#include "gui_view_lists.h"    /* left panel view (declare_left_panel) */
#include "gui_view_settings.h" /* right settings panel view (declare_right_panel) */
#include "tinyfiledialogs.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h> /* POSIX shell APIs (exe-dir getcwd now lives in gui_paths_exe_dir) */
#endif
// #endregion

// #region engine state
/* UI context capacities. UI_STATE_SLOTS / UI_STATE_PROBE_MAX / UI_ROW_ID_RING live in gui_shell.h
 * (shared with the selftest dev-seam) and exceed the engine defaults because nt_ui_state has no
 * per-frame eviction: a heavy session scrolling a large folder must retain every distinct row id
 * or placement overflows the pool. UI_MAX_ELEMENTS is the Clay layout-element cap. */
#define UI_MAX_ELEMENTS ((uint32_t)8192U)
#define UI_ARENA_SIZE ((size_t)24U * 1024U * 1024U)
#define SCRATCH_ARENA_SIZE ((size_t)4U * 1024U * 1024U)

static NT_UI_DECLARE_ARENA(s_ui_arena, UI_ARENA_SIZE);
static nt_buffer_t s_frame_ubo;

// #endregion

// #region ui ids gate
/* s_id_mb_ file/edit/view/help and s_id_menu_ file/edit/view/help (menubar + menu-panel ids) live in
 * gui_state (same precedent as s_id_ctx_menu); gui_bootstrap seeds them with the other UI ids.
 * The menu-state buffers (s_file/edit/view/help_state + the four
 * nt_ui_menu_ctx_t working buffers), the MK_ item-key enum, and s_ctx_menu (the context-menu
 * declare-machinery working buffer) are chrome-local statics in gui_view_chrome.c -- nothing outside
 * chrome ever reads them. */
// #endregion

// #region editor state
static const tp_result *s_shown_result; /* pack result currently bound to the canvas (sync guard) */
/* Canvas mouse model: a left press arms a potential click; if the pointer moves past a small
 * threshold while held it becomes a PAN (no selection on release); otherwise release = click-select.
 * Middle-drag always pans; wheel always zooms. Selection never captures later pan/zoom. */
static bool s_lmb_armed;      /* left button pressed on the canvas, click vs drag undecided */
static bool s_lmb_panning;    /* left drag crossed the threshold -> panning */
static bool s_mmb_panning;    /* middle-drag pan */
static bool s_lmb_zoomed;     /* second press zoomed; suppress its release hit-test */
static float s_press_x, s_press_y; /* left-press origin (threshold test) */
static float s_pan_last_x, s_pan_last_y;
static gui_canvas_double_click_ref s_canvas_double_click;
static const nt_ui_events_cfg_t s_canvas_dbl_cfg = {
    .long_press_secs = 0.0F,
    .double_click = true,
};
#define CANVAS_DRAG_THRESHOLD 4.0F

/* The deferred side-effect queue (s_pending_*), the new/open/exit confirm-flow flags (s_after_confirm/
 * s_confirm_open/s_modal_action) and the last-pack timing (s_last_pack_*) live in gui_actions. The
 * modal open flags (s_about_open / s_export_open) live in gui_state (shared with the selftest); so
 * does s_blur_inputs -- set here by frame(), read by the settings-panel field widgets in the view TU. */

/* s_pack_has_sources/s_pack_stale (pack-button state cached for the tooltip pass) live in
 * gui_state (written by gui_view_canvas's declare_canvas_strip, read by gui_view_chrome's
 * declare_tooltips). The right settings panel's disclosure/dropdown-open bits and numeric-field edit
 * buffers are panel-local statics in gui_view_settings.c. GUI_MAX_TARGETS and k_playback_names
 * live in gui_defs.h (shared with gui_view_chrome's declare_export_modal / gui_view_canvas's
 * declare_canvas_preview). */

// #endregion

// #region small helpers
/* gui_open_url (opens a URL in the OS default browser -- the About link) lives in
 * gui_view_chrome.c: it is chrome-only, the About modal is its sole caller. */

/* Shell-owned release of the canvas borrow and its comparison cache. Callers
 * either free pack slots or retain a stale slot across history navigation; the
 * next frame binds the then-current result without reusing the old borrow. */
void gui_shell_reset_shown_result(void) {
    gui_canvas_rebind_result(&s_canvas, &s_canvas_double_click, NULL);
    s_shown_result = NULL;
}
// #endregion

// #region init helpers
/* Canonical resolver now lives in gui_paths (D1 -- reused by the app-data root + the log file). Kept
 * as a thin wrapper so the call site + the absolute-CWD-on-CI lesson stay put; behavior is identical. */
static void resolve_exe_dir(void) { gui_paths_exe_dir(s_exe_dir, sizeof s_exe_dir); }

/* Seed the global UI scale from the system DPI (96 dpi = 100%). GLFW makes the process
 * per-monitor DPI aware, so the framebuffer is physical pixels -- without this the fixed
 * metrics render physically tiny on a high-DPI display. Overridable via View > UI Scale. */
static float detect_dpi_scale(void) {
#ifdef _WIN32
    const UINT dpi = GetDpiForSystem();
    float s = (dpi >= 96U) ? ((float)dpi / 96.0F) : 1.0F;
    if (s < 1.0F) {
        s = 1.0F;
    }
    if (s > 3.0F) {
        s = 3.0F;
    }
    return s;
#else
    return 1.0F; /* POSIX: engine exposes no GLFW window handle for glfwGetWindowContentScale */
#endif
}

// #endregion

// #region menu bar
/* close_menubar_menus/close_all_menus, the File/Edit/View/Help menu item builders (file_items/
 * edit_items/scale_item/overlay_item/view_items/help_items), menubar_entry, and declare_menubar
 * live in gui_view_chrome.c. close_menubar_menus is declared here via gui_shell.h (the sanctioned
 * cross-view surface the three other views' context-menu triggers read) and in gui_view_chrome.h
 * (for frame() below); its definition lives in gui_view_chrome.c. */
// #endregion

// #region left panel (atlases + sprites)
/* declare_left_panel + its atlas/sprite/animation row helpers live in gui_view_lists.c.
 * declare_row_tooltips lives in gui_view_chrome.c (chrome-owned per the plan's §3 fan-out table). */
// #endregion

// #region canvas
/* Atlas-canvas mouse (standard canvas model): wheel = zoom-at-cursor always; middle-drag = pan
 * always; left = press-move-release -> a drag past CANVAS_DRAG_THRESHOLD pans (no selection), a
 * release within the threshold click-selects the region under the cursor (or clears on empty).
 * Selection never blocks pan/zoom. Runs after build_rows so region->row mapping sees current rows. */
static void handle_canvas_input(void) {
    s_canvas.hover_sprite = -1;
    /* NOTE: intentionally NOT gated on nt_ui_input_any_focused -- pan/zoom/select over the atlas must
     * stay live while a text field holds focus. A press outside the panels also blurs that field (see
     * the blur-request block before nt_ui_begin). */
    if (gui_canvas_get_mode(&s_canvas) != GUI_CANVAS_ATLAS || !gui_canvas_has_atlas(&s_canvas) ||
        s_confirm_open || s_about_open || s_export_open || s_recovery_open || s_edit_kind != EDIT_NONE) {
        s_lmb_armed = s_lmb_panning = s_mmb_panning = false;
        s_lmb_zoomed = false;
        gui_canvas_double_click_reset(&s_canvas_double_click);
        return;
    }
    /* Use the box the handler actually drew the page into (captured last frame). Layout px ==
     * framebuffer px in this app (STRETCH ref = fb). */
    const float *box = s_canvas.last_bb;
    if (box[2] <= 1.0F) {
        s_lmb_armed = s_lmb_panning = s_mmb_panning = false;
        s_lmb_zoomed = false;
        gui_canvas_double_click_reset(&s_canvas_double_click);
        return;
    }
    const nt_pointer_t *p = &g_nt_input.pointers[0];
    /* The floating message pill sits INSIDE the canvas box; a click on it (its × dismiss especially) must
     * not also click-select/pan the atlas. Exclude last frame's pill bbox from the canvas hit region. */
    bool over_pill = false;
    if (s_status[0] != '\0' && !s_status_dismissed) {
        const nt_ui_bbox_t pb = nt_ui_get_bbox(s_ctx, s_id_status_pill);
        over_pill = pb.found && pb.width > 0.0F && p->x >= pb.x && p->x < (pb.x + pb.width) &&
                    p->y >= pb.y && p->y < (pb.y + pb.height);
    }
    const bool inside = !over_pill && p->x >= box[0] && p->x < (box[0] + box[2]) && p->y >= box[1] &&
                        p->y < (box[1] + box[3]);

    /* wheel: always zoom around the cursor, regardless of selection/hover/what was clicked */
    if (inside && p->wheel_dy != 0.0F) {
        gui_canvas_zoom_at(&s_canvas, box, p->x, p->y, p->wheel_dy);
    }

    /* middle-drag: always pan */
    if (inside && p->buttons[NT_BUTTON_MIDDLE].is_pressed) {
        s_mmb_panning = true;
        s_pan_last_x = p->x;
        s_pan_last_y = p->y;
    }
    if (s_mmb_panning && p->buttons[NT_BUTTON_MIDDLE].is_down) {
        gui_canvas_pan(&s_canvas, box, p->x - s_pan_last_x, p->y - s_pan_last_y);
        s_pan_last_x = p->x;
        s_pan_last_y = p->y;
    } else {
        s_mmb_panning = false;
    }

    /* left: arm on press, decide click-vs-pan by movement, resolve on release */
    if (inside && p->buttons[NT_BUTTON_LEFT].is_pressed) {
        s_lmb_armed = true;
        s_lmb_panning = false;
        s_press_x = p->x;
        s_press_y = p->y;
        s_pan_last_x = p->x;
        s_pan_last_y = p->y;
    }
    if (s_lmb_armed && p->buttons[NT_BUTTON_LEFT].is_down) {
        if (!s_lmb_panning &&
            (fabsf(p->x - s_press_x) > CANVAS_DRAG_THRESHOLD || fabsf(p->y - s_press_y) > CANVAS_DRAG_THRESHOLD)) {
            s_lmb_panning = true;
        }
        if (s_lmb_panning) {
            gui_canvas_pan(&s_canvas, box, p->x - s_pan_last_x, p->y - s_pan_last_y);
            s_pan_last_x = p->x;
            s_pan_last_y = p->y;
        }
    }
    if (s_lmb_armed && p->buttons[NT_BUTTON_LEFT].is_released) {
        if (s_lmb_panning) {
            gui_canvas_double_click_reset(&s_canvas_double_click);
        } else if (!s_lmb_zoomed) { /* click: select region under cursor, or clear on empty */
            const int hit = gui_canvas_hit(&s_canvas, p->x, p->y);
            gui_canvas_select(&s_canvas, hit);
            if (hit >= 0) {
                select_row_for_result_region(s_canvas.result, hit);
            }
        }
        s_lmb_armed = false;
        s_lmb_panning = false;
        s_lmb_zoomed = false;
    }

    if (inside && !s_lmb_panning && !s_mmb_panning) {
        s_canvas.hover_sprite = gui_canvas_hit(&s_canvas, p->x, p->y);
    }
}

/* Engine-owned timing/radius decides whether this press is a double-click;
 * the canonical {result, region} gate prevents A->B presses on the shared
 * canvas id from zooming the wrong sprite. Called after nt_ui_begin so the
 * event cell is current, while hit geometry still comes from the last draw. */
static void handle_canvas_double_click(void) {
    if (gui_canvas_get_mode(&s_canvas) != GUI_CANVAS_ATLAS ||
        !gui_canvas_has_atlas(&s_canvas) || s_confirm_open || s_about_open ||
        s_export_open || s_recovery_open || s_edit_kind != EDIT_NONE) {
        gui_canvas_double_click_reset(&s_canvas_double_click);
        return;
    }
    const nt_ui_events_t ev =
        nt_ui_events(s_ctx, s_id_canvas, &s_canvas_dbl_cfg);
    if (!ev.pressed_now) {
        return;
    }
    const nt_pointer_t *pointer = &g_nt_input.pointers[0];
    const int hit = gui_canvas_hit(&s_canvas, pointer->x, pointer->y);
    if (gui_canvas_double_click_press(&s_canvas_double_click, s_canvas.result,
                                      hit, ev.double_clicked)) {
        gui_canvas_select(&s_canvas, hit);
        select_row_for_result_region(s_canvas.result, hit);
        if (gui_canvas_zoom_to_sprite(&s_canvas, s_canvas.last_bb, hit)) {
            /* A zero-hold injected click can press+release in this same frame;
             * its release was already processed before nt_ui_begin, so only
             * arm suppression while a future release is still pending. */
            s_lmb_zoomed =
                !pointer->buttons[NT_BUTTON_LEFT].is_released;
        }
    }
}

/* atlas_fill_pct, strip_group_actions/pages/zoom, declare_canvas_strip, declare_canvas_preview,
 * declare_canvas, status_sev_color/status_sev_icon, and declare_status_pill live in
 * gui_view_canvas.c. handle_canvas_input above stays here per the P-2 lead ruling
 * (docs/plans/gui-decomposition.md §2). */
// #endregion

// #region status bar + menus + tooltips
/* status_sev_color, status_sev_icon, declare_status_pill live in gui_view_canvas.c -- see the note
 * in the canvas region above.
 *
 * declare_menus, declare_context_menu, declare_tooltips, declare_export_modal, declare_confirm_modal,
 * and declare_about_modal live in gui_view_chrome.c. */
// #endregion

/* The right settings panel (regions F/G + per-region packing overrides) lives in
 * gui_view_settings.c/h -- declare_right_panel is called from frame() below; the header exposes
 * only that entry point. */

// #region keyboard shortcuts (ux.md §3.3d)
/* Global shortcuts routed through the SAME actions as the menus. Text-input focus swallows
 * them first (no accidental global actions while typing); an open modal blocks them too. */
static void handle_shortcuts(void) {
    if (gui_shot_active() || gui_bench_active()) {
        return; /* headless capture/probe: the user's live typing must not trigger hotkeys mid-run */
    }
    if (nt_ui_input_any_focused(s_ctx) || gui_view_chrome_any_menu_open() ||
        s_confirm_open || s_about_open || s_export_open || s_recovery_open) {
        return;
    }
    /* Preview + editor accelerators (each also a button; §3.3e). */
    if (s_preview_active && nt_input_key_is_pressed(NT_KEY_SPACE)) {
        preview_toggle_play();
    }
    if (s_edit_kind == EDIT_NONE && s_sel_anim >= 0 && s_sel_anim_frame >= 0 &&
        nt_input_key_is_pressed(NT_KEY_DELETE)) {
        /* Clear the frame selection only after a real removal. On rejection the
         * frame remains, so its selection must remain too. */
        gui_animation_ref animation;
        if (gui_project_animation_ref_at(s_sel_atlas, s_sel_anim, &animation) &&
            gui_project_anim_remove_frame(&animation, s_sel_anim_frame)) {
            s_sel_anim_frame = -1;
        }
    }
    if (nt_input_key_is_pressed(NT_KEY_F5)) {
        s_pending_refresh = true;
    }
    const bool ctrl = nt_input_key_is_down(NT_KEY_LCTRL) || nt_input_key_is_down(NT_KEY_RCTRL);
    if (!ctrl) {
        return;
    }
    const bool shift = nt_input_key_is_down(NT_KEY_LSHIFT) || nt_input_key_is_down(NT_KEY_RSHIFT);
    if (nt_input_key_is_pressed(NT_KEY_N)) {
        request_new();
    } else if (nt_input_key_is_pressed(NT_KEY_O)) {
        request_open();
    } else if (nt_input_key_is_pressed(NT_KEY_S)) {
        if (shift) {
            s_pending_save_as = true;
        } else {
            s_pending_save = true;
        }
    } else if (nt_input_key_is_pressed(NT_KEY_Z)) {
        if (shift) {
            do_redo(); /* Ctrl+Shift+Z alias */
        } else {
            do_undo();
        }
    } else if (nt_input_key_is_pressed(NT_KEY_Y)) {
        do_redo();
    } else if (nt_input_key_is_pressed(NT_KEY_P)) {
        s_pending_pack = true;
    } else if (nt_input_key_is_pressed(NT_KEY_E)) {
        s_export_open = true;
    } else if (nt_input_key_is_pressed(NT_KEY_F)) {
        s_filter_active = true; /* Ctrl+F arms the sprite-tree speed-search filter (U-02 T1) */
    }
}

/* Sprite-list keyboard navigation (ux.md §3.3d, U-02 T3). Runs AFTER build_view() so it acts on the
 * fresh filtered/sorted view. Same gating as handle_shortcuts: no field focus, no modal, not headless,
 * and Ctrl is reserved for the global shortcuts above. Arrows/Home/End/Enter/F2 drive the list focus. */
static void handle_list_nav(void) {
    if (gui_shot_active() || gui_bench_active()) {
        return;
    }
    if (nt_ui_input_any_focused(s_ctx) || gui_view_chrome_any_menu_open() ||
        s_confirm_open || s_about_open || s_export_open || s_recovery_open ||
        s_edit_kind != EDIT_NONE) {
        return;
    }
    if (nt_input_key_is_down(NT_KEY_LCTRL) || nt_input_key_is_down(NT_KEY_RCTRL)) {
        return;
    }
    const bool shift = nt_input_key_is_down(NT_KEY_LSHIFT) || nt_input_key_is_down(NT_KEY_RSHIFT);
    if (nt_input_key_is_pressed(NT_KEY_ARROW_DOWN)) {
        gui_list_focus_step(+1, shift);
    } else if (nt_input_key_is_pressed(NT_KEY_ARROW_UP)) {
        gui_list_focus_step(-1, shift);
    } else if (nt_input_key_is_pressed(NT_KEY_HOME)) {
        gui_list_focus_edge(false, shift);
    } else if (nt_input_key_is_pressed(NT_KEY_END)) {
        gui_list_focus_edge(true, shift);
    } else if (nt_input_key_is_pressed(NT_KEY_ARROW_RIGHT)) {
        gui_list_focus_collapse(true);
    } else if (nt_input_key_is_pressed(NT_KEY_ARROW_LEFT)) {
        gui_list_focus_collapse(false);
    } else if (nt_input_key_is_pressed(NT_KEY_ENTER)) {
        gui_list_focus_activate();
    } else if (nt_input_key_is_pressed(NT_KEY_F2)) {
        gui_list_focus_rename();
    }
}
// #endregion

// #region frame
/* DEV (--auto-pack): after resources bind, start ONE async pack of atlas 0 and quit when it lands.
 * Headless driver for the heartbeat proof (an interactive Pack is otherwise human-driven). */
static bool s_auto_pack;
static int s_auto_pack_frame;
static bool s_auto_pack_started;
static void auto_pack_tick(void) {
    if (!s_auto_pack) {
        return;
    }
    s_auto_pack_frame++;
    if (!s_auto_pack_started && s_auto_pack_frame == 8) {
        s_sel_atlas = 0;
        do_pack(); /* async */
        s_auto_pack_started = true;
    } else if (s_auto_pack_started && s_auto_pack_frame > 8 && !gui_pack_async_busy()) {
        nt_log_info("AUTO-PACK: async pack landed, quitting");
        nt_app_quit();
    }
}

static void frame(void) {
    nt_window_poll();
    nt_input_poll();
    nt_mem_scratch_reset();
    gui_project_tick(g_nt_app.time); /* history coalescing clock */

#ifdef NTPACKER_GUI_SELFTEST
    /* Verification build: render real frames (proves the canvas draw + walk + a pixel-level proof that
     * region outlines actually rasterize), verify no control writes the model without input, then quit. */
    selftest_pre_frame();
#endif

    /* dialogs + model mutations queued last frame run here, cleanly between frames */
    apply_pending();

    /* Fallback commit for a buffered gesture that never got a release/blur/discrete boundary
     * (decision 0015). GATED on no active gesture -- no held pointer and no focused
     * input -- so it can never split a live drag or a mid-typing field; the 0.30 s window inside
     * only fires when the edit has truly gone idle. Primary commits are gesture-scoped. */
    if (!g_nt_input.pointers[0].buttons[NT_BUTTON_LEFT].is_down && !nt_ui_input_any_focused(s_ctx)) {
        gui_project_flush_elapsed();
    }

    /* Heartbeat: the frame loop keeps ticking while a pack/export runs on the worker thread, so a slow
     * concave pack never freezes the window. Throttled to ~2 Hz; the frames-since count shows the rate. */
    if (gui_pack_async_busy()) {
        static double s_hb_last;
        static int s_hb_frames;
        s_hb_frames++;
        if (g_nt_app.time - s_hb_last >= 0.5) {
            nt_log_info("HEARTBEAT: async %s %.1fs -- %d frames in %.2fs (dt=%.1fms), UI live",
                        gui_pack_async_active_kind() == GUI_PACK_ASYNC_EXPORT ? "export" : "pack",
                        gui_pack_async_elapsed_sec(), s_hb_frames, g_nt_app.time - s_hb_last,
                        (double)g_nt_app.dt * 1000.0);
            s_hb_last = g_nt_app.time;
            s_hb_frames = 0;
        }
    }
    auto_pack_tick(); /* dev (--auto-pack): drive a headless async pack for the heartbeat proof */
    gui_bench_tick(); /* dev (--bench-perf): drive the perf-probe state machine; no-op unless active */

    if (nt_input_key_is_pressed(NT_KEY_ESCAPE)) {
        if (gui_view_chrome_consume_escape()) {
            /* Menus own Escape while open. Do not also clear filters or stop
             * either preview mode on the same key press. */
        } else if (s_edit_kind != EDIT_NONE) {
            cancel_edit();
            set_status("Rename cancelled.");
        } else if (s_export_open) {
            s_export_open = false;
        } else if (s_about_open) {
            s_about_open = false;
        } else if (s_recovery_open) {
            s_recovery_open = false; /* Esc = "Later": leave every orphan on disk, no data loss */
        } else if (s_confirm_open) {
            s_confirm_open = false;
            s_after_confirm = AFTER_NONE;
        } else if (s_filter_active || gui_rows_filter_active()) {
            gui_rows_set_filter(""); /* Esc clears the sprite-tree speed-search (U-02 T1) */
            s_filter_active = false;
            set_status("Filter cleared.");
        } else if (s_preview_active) {
            preview_stop();
            set_status("Closed animation preview.");
        } else if (s_preview_target != 0) {
            preview_target_reset(); /* export-target preview escape hatch (also works when the selector folded away) */
            set_status("Preview: back to Native.");
        } else {
            close_all_menus();
        }
    }

    handle_shortcuts();

    /* Click/right-click OUTSIDE the active inline editor commits it (Enter-equivalent desktop UX);
     * Esc still cancels. Uses last frame's field bbox vs this frame's press. The UI is configured
     * STRETCH with ref = framebuffer, so logical bbox coords and framebuffer pointer px coincide. */
    if (s_edit_kind != EDIT_NONE) {
        const nt_pointer_t *p = &g_nt_input.pointers[0];
        if (p->buttons[NT_BUTTON_LEFT].is_pressed || p->buttons[NT_BUTTON_RIGHT].is_pressed) {
            const nt_ui_bbox_t fb = nt_ui_get_bbox(s_ctx, s_id_rename);
            const bool inside = fb.width > 0.0F && p->x >= fb.x && p->x < (fb.x + fb.width) &&
                                p->y >= fb.y && p->y < (fb.y + fb.height);
            if (!inside) {
                s_pending_commit_edit = true;
            }
        }
    }

    const nt_material_info_t *sprite_info = gui_bootstrap_step();

    const float fb_w = (float)(g_nt_window.fb_width > 0 ? g_nt_window.fb_width : 800);
    const float fb_h = (float)(g_nt_window.fb_height > 0 ? g_nt_window.fb_height : 600);

    nt_ui_scale_desc_t scale_desc = {.ref_w = fb_w, .ref_h = fb_h, .mode = NT_UI_SCALE_STRETCH};
    nt_ui_scale_t scale = nt_ui_compute_scale(&scale_desc, fb_w, fb_h);
    nt_ui_scale_ortho_t ortho = nt_ui_scale_ortho(&scale);

    mat4 view_m;
    mat4 proj_m;
    mat4 vp;
    glm_mat4_identity(view_m);
    glm_ortho(ortho.left, ortho.right, ortho.bottom, ortho.top, -1.0F, 1.0F, proj_m);
    glm_mat4_mul(proj_m, view_m, vp);

    nt_frame_uniforms_t uniforms = {0};
    memcpy(uniforms.view_proj, vp, 64);
    memcpy(uniforms.view, view_m, 64);
    memcpy(uniforms.proj, proj_m, 64);
    uniforms.resolution[0] = fb_w;
    uniforms.resolution[1] = fb_h;
    uniforms.resolution[2] = (fb_w > 0.0F) ? (1.0F / fb_w) : 0.0F;
    uniforms.resolution[3] = (fb_h > 0.0F) ? (1.0F / fb_h) : 0.0F;
    uniforms.near_far[0] = -1.0F;
    uniforms.near_far[1] = 1.0F;

    nt_gfx_begin_frame();
    nt_gfx_begin_segment("frame");
    if (g_nt_gfx.context_restored) {
        gui_bootstrap_restore();
        nt_gfx_destroy_buffer(s_frame_ubo);
        s_frame_ubo = nt_gfx_make_buffer(&(nt_buffer_desc_t){
            .type = NT_BUFFER_UNIFORM,
            .usage = NT_USAGE_DYNAMIC,
            .size = sizeof(nt_frame_uniforms_t),
            .label = "frame_uniforms",
        });
        nt_sprite_renderer_restore_gpu();
        nt_text_renderer_restore_gpu();
        nt_shape_renderer_restore_gpu();
        gui_canvas_restore_gpu(&s_canvas);
    }

    nt_gfx_begin_pass(&(nt_pass_desc_t){
        .clear_color = {C_BG.r / 255.0F, C_BG.g / 255.0F, C_BG.b / 255.0F, 1.0F},
        .clear_depth = 1.0F,
    });

    nt_font_step();

    if (sprite_info) {
        nt_gfx_update_buffer(s_frame_ubo, &uniforms, sizeof(uniforms));
        nt_gfx_bind_uniform_buffer(s_frame_ubo, 0);

        apply_ui_scale();
        gui_canvas_ensure_pipeline(&s_canvas, sprite_info);
        gui_canvas_set_frame_ubo(&s_canvas, s_frame_ubo);
        gui_canvas_set_ui_scale(&s_canvas, g_ui_scale); /* overlay line widths scale with DPI */

        clamp_selection();
        /* keep the animation selection valid after undo/redo/atlas changes */
        {
            const tp_session_snapshot *snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *sel_a = snapshot
                                                 ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                                                 : NULL;
            if (!sel_a || s_sel_anim >= sel_a->animation_count) {
                s_sel_anim = -1;
                s_sel_anim_frame = -1;
                if (s_preview_active) {
                    preview_stop();
                }
            }
        }
        build_rows();
        build_view(); /* filtered/sorted/collapsible view over the row model (U-02) */
        gui_selection_revalidate(); /* re-resolve/prune the selection preserved across undo (U-02 T5) */
        filter_type_pump(); /* Ctrl+F speed-search: typed chars edit the filter (U-02 T1) */
        handle_list_nav(); /* keyboard list focus/nav on the fresh view (U-02 T3) */
        s_content_w = scale.logical_w; /* for caption/status truncation */
        compute_panel_widths(scale.logical_w); /* clamp side-panel widths so they never leave the screen */
        gui_shot_tick(); /* screenshot mode: pack + select + (post-draw) capture; no-op unless --shot */

        /* Blur focused text fields when a press/scroll lands outside the settings panel (desktop UX).
         * The engine exposes no programmatic blur, so we declare the inputs disabled for one frame,
         * which drives the engine's disabled-drop of keyboard focus. Values commit on-change every
         * keystroke, so nothing is lost. Uses last frame's panel bbox vs this frame's press. */
        s_blur_inputs = false;
        if (nt_ui_input_any_focused(s_ctx)) {
            const nt_pointer_t *bp = &g_nt_input.pointers[0];
            const bool press = bp->buttons[NT_BUTTON_LEFT].is_pressed || bp->buttons[NT_BUTTON_MIDDLE].is_pressed ||
                               bp->buttons[NT_BUTTON_RIGHT].is_pressed || bp->wheel_dy != 0.0F;
            if (press) {
                const nt_ui_bbox_t rb = nt_ui_get_bbox(s_ctx, s_id_right_panel);
                const bool in_panel = rb.width > 0.0F && bp->x >= rb.x && bp->x < (rb.x + rb.width) &&
                                      bp->y >= rb.y && bp->y < (rb.y + rb.height);
                s_blur_inputs = !in_panel;
            }
        }
        /* A blur (press outside the panel while a field held focus) is a field's gesture boundary:
         * flush its buffered edit as ONE undo step (decision 0015). The value is already
         * in gui_project's pending buffer from the last keystroke; apply_pending commits it next frame. */
        if (s_blur_inputs) {
            gui_request_gesture_commit();
        }

        /* Bind the result the canvas shows (repack / atlas switch / clear): the export-target preview
         * while one is active + visible, else the native session pack (preview_target_result). */
        const tp_result *want = preview_target_result();
        if (want != s_shown_result) {
            gui_canvas_rebind_result(&s_canvas, &s_canvas_double_click, want);
            s_shown_result = want;
            /* #4: gui_canvas_set_result just cleared the region highlight (sel_sprite -> -1). Re-derive it
             * from the tree's primary leaf so the accent outline survives ANY result rebind -- an Undo/Redo
             * settle, an ordinary repack, an atlas switch, OR a pack that lands LATER (a pack completing
             * during an undo): the rebind fires again the frame its result pointer appears, so unlike the
             * old post-undo one-shot this is never missed. ATLAS-mode only (a non-NULL want puts the canvas
             * in ATLAS mode); guarded against an absent shown pack and a leaf not present in it
             * (gui_pack_find_sprite_ref_in_result -> -1). A user atlas switch clears the selection first
             * (reset_selection), so this is a no-op there -- it never fabricates a stale highlight. Runs
             * here, BEFORE handle_canvas_input() below, so it can never fight a click. This replaces the
             * former post-undo one-shot (now retired). */
            if (want && gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_ATLAS) {
                const sprite_row *leaf = gui_rows_selected_leaf();
                if (leaf && leaf->source_key && leaf->source_key[0] != '\0') {
                    const int region = gui_pack_find_sprite_ref_in_result(
                        want, leaf->source_id, leaf->source_key);
                    if (region >= 0 && region < want->sprite_count) {
                        gui_canvas_select(&s_canvas, region);
                    }
                }
            }
        }
        /* Feed the selected region's LIVE slice9 override to the canvas guides: the project is the
         * source of truth, so typing in the Region panel moves the lines this same frame (no repack;
         * owner: "добавил и не вижу"). Result names are the original atlas-relative key + ext. */
        s_canvas.sel_slice9[0] = s_canvas.sel_slice9[1] = s_canvas.sel_slice9[2] = s_canvas.sel_slice9[3] = 0;
        if (want && s_canvas.sel_sprite >= 0 && s_canvas.sel_sprite < want->sprite_count) {
            const sprite_row *selected = gui_rows_selected_leaf();
            const tp_session_snapshot *snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *atlas = snapshot
                                                  ? tp_session_snapshot_atlas_at(snapshot,
                                                                                 s_sel_atlas)
                                                  : NULL;
            if (selected && atlas) {
                const gui_sprite_ref sprite = {
                    atlas->id, selected->source_id, selected->source_key,
                    tp_session_snapshot_revision(snapshot)};
                /* EFFECTIVE value (#5): a slice9 edit BUFFERS until the gesture boundary, so the committed
                 * record freezes mid-typing. Prefer the buffered slice9 (peek) when one is in flight for
                 * this atlas+sprite, so the guides move THIS frame; else read the committed record. */
                int eff[4];
                if (gui_project_peek_pending_slice9(&sprite, eff)) {
                    for (int k = 0; k < 4; k++) {
                        s_canvas.sel_slice9[k] = eff[k];
                    }
                } else {
                    const tp_snapshot_sprite *s9ov =
                        gui_rows_selected_override();
                    if (s9ov) {
                        for (int k = 0; k < 4; k++) {
                            s_canvas.sel_slice9[k] = (int)s9ov->slice9_lrtb[k];
                        }
                    }
                }
            }
        }
        update_preview();      /* resolve the current preview frame + set ANIM mode before input/handler */
        handle_canvas_input(); /* wheel/pan/click over the atlas page (uses last frame's draw box) */

        nt_ui_begin(s_ctx, scale.logical_w, scale.logical_h, g_nt_app.dt, &g_nt_input.pointers[0], 1);
        nt_ui_set_viewport(s_ctx, nt_ui_viewport_from_scale(&scale));
        handle_canvas_double_click();

        /* Docked chrome (owner 2026-07-11 pass 2): NO outer margin (root padding 0 -> the menubar fuses
         * flush to the top edge and the middle row fills to the window edges) + a thin 2px C_BG seam between
         * regions so panels read as docked, not floating cards. compute_panel_widths' overhead term mirrors
         * these values (root L/R padding 0 + two inter-column gaps of Su(2)). The permanent status bar row is
         * gone (owner: "странная панель"); messages now float as a pill over the canvas (declare_status_pill). */
        CLAY({.id = CLAY_ID("root"),
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                         .padding = {0, 0, 0, 0},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(2),
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
              .backgroundColor = C_BG}) {
            declare_menubar(s_ctx);
            /* Below this the columns can't lay out without collapsing a clip/input box to 0 (empty-scissor
             * assert); skip the whole middle row rather than declare a degenerate subtree. */
            const bool have_room = scale.logical_w >= S(280.0F) && scale.logical_h >= S(200.0F);
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = Su(2), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}}}) {
                if (have_room) {
                    declare_left_panel(s_ctx);
                    declare_canvas(s_ctx);
                    declare_right_panel(s_ctx);
                } else {
                    nt_ui_label(s_ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Window too small.", &g_caption);
                }
            }
        }

        declare_row_tooltips(s_ctx);
        declare_menus(s_ctx);
        declare_context_menu(s_ctx);
        declare_tooltips(s_ctx);
        declare_confirm_modal(s_ctx);
        declare_recovery_modal(s_ctx);
        declare_about_modal(s_ctx);
        declare_export_modal(s_ctx);

        nt_ui_end(s_ctx);

        /* SOURCE mode: refresh the decoded image for the selection before the walk draws it. In
         * ATLAS mode the canvas draws the packed pages, so leave the source texture alone. */
        if (gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_SOURCE) {
            if (s_sel_missing) {
                gui_canvas_clear(&s_canvas); /* placeholder is drawn by declare_canvas (§3.7) */
            } else if (s_sel_abs[0] != '\0') {
                char err[256];
                if (!gui_canvas_set_image(&s_canvas, s_sel_abs, err, sizeof err)) {
                    set_statusf_ex(STATUS_ERROR, "Decode failed: %s", err);
                    s_sel_missing = true; /* show the missing placeholder instead of a blank canvas */
                }
            } else {
                gui_canvas_clear(&s_canvas);
            }
        }

        gui_canvas_upload_pages(&s_canvas);          /* GL upload of packed pages (deferred from pack) */
        if (s_canvas.upload_failed) {
            set_status_ex(STATUS_WARNING, "Page too large for this GPU \xE2\x80\x94 lower Max page size to preview it.");
        }
        nt_shape_renderer_set_vp((const float *)vp); /* overlays share the sprite view_proj */
        nt_shape_renderer_set_depth(false);
        /* The line shader billboards width via cross(edge, cam_pos - point); overlay points sit on the
         * z=0 layout plane, so a cam ON that plane (the memset-0 default) collapses `side` onto z and the
         * lines render zero-width -> invisible. Park the camera far up +z so `side` lies in the screen plane. */
        nt_shape_renderer_set_cam_pos((const float[3]){0.0F, 0.0F, 100000.0F});

        nt_ui_target_t target = nt_ui_scale_make_target(&scale);
        nt_ui_walk(s_ctx, &target);
    }

    nt_gfx_end_pass();
    nt_gfx_end_segment();
    nt_gfx_end_frame();

#ifdef NTPACKER_GUI_SELFTEST
    selftest_post_draw(); /* read back the drawn overlay before the buffers swap */
#endif
    gui_shot_post_draw(); /* screenshot mode: full-frame PNG capture at the same pre-swap point */
    gui_bench_post_draw(); /* perf-probe mode: accrue frame-time samples, write output, then quit */

    nt_window_swap_buffers();
}
// #endregion



// #region main + init/shutdown
static int gui_main_utf8(int argc, char *argv[]) {
    /* Private build-worker re-exec (decision 0018): a pack re-execs this exe with
     * argv[1] == "__build-worker". Service it FIRST -- before engine init, the
     * window, recovery, or any UI -- and return; never open a window as a worker. */
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
#ifdef _WIN32
    /* All client/core paths are UTF-8. tinyfiledialogs defaults to the process
     * ANSI code page unless this is set before its first native dialog. */
    tinyfd_winUtf8 = 1;
#endif
    nt_engine_config_t config = {0};
    config.app_name = "ntpacker-gui";
    config.version = 1;
    if (nt_engine_init(&config) != NT_OK) {
        return 1;
    }

    /* dev screenshot flags + optional project path (first non-flag arg; see gui_shot.c) */
    const char *proj_arg = NULL;
    bool selftest_crash = false; /* D2 hidden dev arg: fault after install to exercise the handler */
    for (int i = 1; i < argc; i++) {
        if (gui_shot_parse_arg(argv[i])) {
            /* consumed by the dev screenshot seam (--shot/--size/--scale/--shot-stale/--shot-packing) */
        } else if (gui_bench_parse_arg(argv[i])) {
            /* consumed by the dev perf-probe seam (--bench-perf[=out.txt]) */
        } else if (strcmp(argv[i], "--auto-pack") == 0) {
            s_auto_pack = true; /* dev: headless async pack of atlas 0 for the heartbeat proof */
        } else if (strcmp(argv[i], "--selftest-crash") == 0) {
            selftest_crash = true; /* parse here so it's never mistaken for a project path below */
        } else if (proj_arg == NULL) {
            proj_arg = argv[i];
        }
    }
    /* dev (--bench-perf): open the owner-scale bench fixture unless a project path was passed. */
    if (gui_bench_active() && proj_arg == NULL) {
        proj_arg = gui_bench_default_project();
    }

    /* D2 crash handler + D1 file log: install for a real windowed run ONLY -- the --shot capture seam
     * stays side-effect-free (no stray <app-data>/crash dir, log/sink/
     * FILE). Crash handler goes in FIRST (before the log) so it protects the log install + the build
     * line too. NOT literally main()'s first statement: a fault before this point falls back to the OS
     * default (identical to not-installed, no ntpacker dump) -- acceptable, since the value is catching
     * interactive-session crashes, and the dev seams must stay clean. Both no-op under
     * NTPACKER_GUI_HEADLESS and if the app-data dir can't be created; crash install must NOT call nt_log
     * (see gui_crash_install). --selftest-crash is not a --shot arg, so install still runs for it. The
     * --bench-perf probe skips this block too: it must stay side-effect-free and never block on the
     * crash-report native modal. */
    if (!gui_shot_active() && !gui_bench_active()) {
        gui_crash_install();
        gui_log_file_install();
#ifndef NTPACKER_GUI_SELFTEST
        /* D3: if the PREVIOUS run crashed it left a marker -> offer to open the diagnostics root, then
         * clear it (once). Self-contained startup step: no ordering coupling with the upcoming R
         * recovery modal. No-op with no marker / headless. Native modal, so before window init is OK.
         * Disabled in the selftest build (like the recovery journal/notice below): ctest #50 runs THIS
         * exe NON-headless, so a stale/self-written marker would block ctest on the native modal.
         * Interactive-only by construction -- the shipped app never sets NTPACKER_GUI_SELFTEST. */
        gui_crash_report_prompt();
#endif
    }
    nt_log_info("ntpacker-gui: %s build (%s)", nt_engine_build_string(), nt_engine_preset_string());

    /* D2 hidden hand-verification hook: fault NOW (the handler is installed + the log sink is live, so
     * a real run produces a dump/backtrace + marker AND proves the log tail survived). Self-guards to a
     * no-op under headless, so it can never fire in CI. */
    if (selftest_crash) {
        gui_crash_selftest();
    }

    gui_shot_apply_window_size(); /* dev (--shot): request the shot window size before window init */
    nt_window_init();
    nt_input_init();

    nt_gfx_desc_t gfx_desc = nt_gfx_desc_defaults();
    nt_gfx_init(&gfx_desc);
    nt_gfx_register_global_block("Globals", 0);

    nt_http_init();
    nt_fs_init();
    nt_hash_init(&(nt_hash_desc_t){0});
    nt_resource_init(&(nt_resource_desc_t){0});
    nt_mem_scratch_init(SCRATCH_ARENA_SIZE);

    nt_resource_set_activator(NT_ASSET_TEXTURE, nt_gfx_activate_texture, nt_gfx_deactivate_texture);
    nt_resource_set_activator(NT_ASSET_SHADER_CODE, nt_gfx_activate_shader, nt_gfx_deactivate_shader);
    nt_atlas_init();

    nt_material_init(&(nt_material_desc_t){.max_materials = 2});
    nt_font_init(&(nt_font_desc_t){.max_fonts = 1});

    nt_sprite_renderer_desc_t sr_desc = nt_sprite_renderer_desc_defaults();
    nt_sprite_renderer_init(&sr_desc);
    nt_text_renderer_init();
    nt_shape_renderer_init(); /* atlas-canvas region overlays (outlines / hulls / selection) */

    nt_ui_module_init();
    nt_ui_create_desc_t ui_desc = nt_ui_create_desc_defaults();
    ui_desc.max_elements = UI_MAX_ELEMENTS;
    ui_desc.state_slots = UI_STATE_SLOTS;
    ui_desc.state_probe_max = UI_STATE_PROBE_MAX;
    s_ctx = nt_ui_create_context(s_ui_arena, sizeof s_ui_arena, &ui_desc);
    NT_ASSERT(s_ctx != NULL && "ntpacker-gui: failed to create UI context");

    g_nt_app.target_dt = 0.0F;

    s_frame_ubo = nt_gfx_make_buffer(&(nt_buffer_desc_t){
        .type = NT_BUFFER_UNIFORM,
        .usage = NT_USAGE_DYNAMIC,
        .size = sizeof(nt_frame_uniforms_t),
        .label = "frame_uniforms",
    });

    resolve_exe_dir();
    gui_bootstrap_init(s_exe_dir);

    g_ui_scale = detect_dpi_scale();
    nt_log_info("ntpacker-gui: UI scale %.2f (from system DPI)", (double)g_ui_scale);
    gui_shot_apply_scale(); /* screenshot mode (--scale): pin g_ui_scale for reproducible captures */

    /* editor state + the canvas custom-draw handler (registered outside begin/end) */
    gui_canvas_init(&s_canvas);
    nt_ui_set_custom_handler(s_ctx, gui_canvas_handler, &s_canvas);
    /* Crash recovery lives in <app-data>/recovery/. Core owns the random
     * live-slot identity, lock, and scan exclusion. At startup we collect orphaned journals and, if any,
     * open the R6b startup recovery MODAL to resolve each one (Discard / Save to original / Save As) via the R6a
     * layer -- the live editor starts FRESH untitled (recovery resolves journals to DISK, never adopts); every
     * non-resolved orphan is LEFT on disk for next launch. Every step is fail-closed + NON-FATAL: a folder/RNG/scan failure leaves the
     * editor available, with a warning that crash recovery is unavailable. GATED OUT of the headless
     * selftest build -- that build runs THIS exe non-headless and drives recovery itself on ISOLATED temp
     * folders via R6 test seams, so the production scan must never inspect test journals. */
    /* No migration code: crash recovery is unreleased, so there is no installed base with the old
     * deterministic exe-dir recovery slot to migrate into this per-session folder. */
#ifndef NTPACKER_GUI_SELFTEST
    gui_project_require_recovery();
    char rec_root[GUI_PATHS_MAX];
    char rec_folder[GUI_PATHS_MAX];
    if (!gui_paths_app_data_root(rec_root, sizeof rec_root)) {
        gui_project_note_recovery_setup_failure("the application data folder is unavailable");
    } else {
        int nf = snprintf(rec_folder, sizeof rec_folder, "%s/recovery", rec_root);
        if (nf <= 0 || (size_t)nf >= sizeof rec_folder) {
            gui_project_note_recovery_setup_failure("the recovery directory path is too long");
        } else if (!gui_paths_ensure_dir(rec_folder)) {
            gui_project_note_recovery_setup_failure("the recovery directory could not be created");
        } else {
            gui_project_enable_recovery(rec_folder);
            /* R6b: collect orphan journals and open the startup modal. The
             * live editor stays fresh; recovery resolves only to disk. */
            gui_recovery_list rlist;
            /* Screenshot automation + the perf probe cannot dismiss a modal, so they skip the
             * startup scan without changing recovery ownership. */
            if (!gui_shot_active() && !gui_bench_active() && gui_recovery_collect(&rlist) > 0) {
                gui_actions_open_recovery(&rlist);
            }
        }
    }
#endif
    gui_project_init(); /* recovery is resolved to disk by the R6 modal; the live editor always starts fresh */
    /* Preserve a recovery-unavailable warning across later terminal startup statuses. */
    bool recovery_warn_shown = false;
#ifndef NTPACKER_GUI_SELFTEST
    {
        char rnotice[256];
        if (gui_project_take_recovery_setup_notice(rnotice, sizeof rnotice)) {
            recovery_warn_shown = true;
            set_status_ex(STATUS_WARNING, rnotice);
        }
    }
#endif

    /* in-process packing: session .ntpack goes under the exe dir (existing convention) */
    char pack_session[GUI_PATHS_MAX + 128];
    (void)snprintf(pack_session, sizeof pack_session, "%s/pack_session", s_exe_dir);
    const bool pack_work_ready = gui_pack_init(pack_session);
    if (!pack_work_ready) {
        set_status_ex(STATUS_ERROR,
                      "Pack work directory is unavailable or exceeds the supported path limit.");
    }

    /* open a project passed on the command line (errors go to the status bar).
     * Route the open/defer choice through the PURE gui_startup_decide (single source of truth;
     * J14 truth-table). A pending R6 modal DEFERS the CLI open even when the arg is stale
     * (DEFER > MISSING): the modal can
     * Save-to-original the very file named on the CLI, so opening that file into the live editor behind the
     * modal would show the STALE pre-crash copy and risk a later Save clobbering the just-recovered state.
     * Deferring keeps the editor fresh-empty behind the modal (the settled R6b design). No terminal default
     * status clobbers a recovery warning already up (recovery_warn_shown). A genuine open ERROR still shows. */
    if (proj_arg != NULL) {
        char err[256];
        const bool recovery_pending = s_recovery_open;
        switch (gui_startup_decide(true, gui_scan_exists(proj_arg), recovery_pending)) {
        case GUI_STARTUP_DEFER:
            /* The startup recovery modal is up and can Save-to-original the very file named on the CLI.
             * Opening it into the live editor now would load the STALE pre-crash copy behind the modal, so a
             * later Save could clobber the state the user just recovered. Defer: leave the editor fresh-empty
             * behind the modal (settled R6b design) and tell the user to resolve recovery first, then open via
             * File>Open. (Pre-R6b this same path fired for an auto-adopted model -- same defer, new cause.) */
            set_statusf_ex(STATUS_WARNING, "Resolve recovered projects first, then open %s via File > Open", proj_arg);
            break;
        case GUI_STARTUP_MISSING:
            if (!recovery_warn_shown) { /* stale argv -> continue with untitled (F6b); keep any recovery warning */
                set_statusf_ex(STATUS_WARNING, "project not found: %s", proj_arg);
            }
            break;
        case GUI_STARTUP_OPEN:
            if (gui_project_open(proj_arg, err, sizeof err) == TP_STATUS_OK) {
                if (!recovery_warn_shown) { /* routine confirmation must not clobber the recovery warning */
                    set_statusf("Opened %s", gui_project_display_name());
                }
            } else {
                /* A genuine open FAILURE is surfaced even over a recovery warning: it is a concrete,
                 * user-initiated failure the user is actively waiting on (they asked to open THIS file), it
                 * is higher severity (STATUS_ERROR > STATUS_WARNING), and it is rare. Present actionable
                 * failure wins over the latent recovery warning. */
                set_statusf_ex(STATUS_ERROR, "Open '%s' failed: %s", proj_arg, err);
            }
            break;
        case GUI_STARTUP_IDLE:
            break; /* unreachable with proj_arg != NULL; listed for switch exhaustiveness */
        }
    } else if (!recovery_warn_shown && pack_work_ready) {
        set_status("Ready. New project -- add files or a folder to start.");
    }
    /* proj_arg == NULL && recovery_warn_shown: keep the recovery STATUS_WARNING as the first-frame status
     * (do NOT clobber it with "Ready..." before the first frame is drawn). */

#ifdef NTPACKER_GUI_SELFTEST
    run_selftest();
#endif

    clamp_selection();
    nt_log_info("ntpacker-gui: starting (typed session jobs + atlas-page canvas)");

    nt_app_run(frame);

    /* The window closed while a session job was still active. Cancellation is cooperative, so keep
     * the OS message pump alive and poll until the typed job reaches a terminal state. */
    if (gui_pack_worker_active()) {
        gui_pack_async_cancel();
        while (gui_pack_worker_active()) {
            nt_window_poll();
            (void)gui_pack_poll(NULL); /* joins + frees the job the frame it signals done */
        }
    }

    gui_canvas_shutdown(&s_canvas);
    gui_pack_shutdown();
    gui_rows_shutdown();
    gui_scan_shutdown();
    gui_project_shutdown();
    nt_ui_destroy_context(s_ctx);
    nt_ui_module_shutdown();
    nt_shape_renderer_shutdown();
    nt_text_renderer_shutdown();
    nt_sprite_renderer_shutdown();
    gui_bootstrap_shutdown();
    nt_mem_scratch_shutdown();
    nt_resource_shutdown();
    nt_fs_shutdown();
    nt_http_shutdown();
    nt_hash_shutdown();
    nt_gfx_destroy_buffer(s_frame_ubo);
    nt_gfx_shutdown();
    nt_input_shutdown();
    nt_window_shutdown();
    gui_log_file_shutdown(); /* unregister the sink + close the file before the engine goes down */
    nt_engine_shutdown();
    /* Drop the crash marker LAST -- after engine teardown, with nothing left that can fault. Clearing
     * it earlier would race a teardown fault: the handler is still live during nt_engine_shutdown, so a
     * crash there would re-create the just-cleared marker -> a false "crashed" next launch. Only a fully
     * clean run reaches here; remove() on the pre-resolved path is safe post-shutdown. */
    gui_crash_clear_marker();
    /* 0 for a normal run; --bench-perf returns non-zero iff an invariant assert or a hard fixture-load
     * failure fired (advisory timings never fail the run). */
    return gui_bench_exit_code();
}
// #endregion

#if defined(_WIN32)
int main(void) {
    nt_utf8_argv utf8 = {0};
    char error[160] = {0};
    if (!nt_utf8_argv_from_command_line(&utf8, error, sizeof error)) {
        (void)fprintf(stderr, "ntpacker-gui: invalid Windows command line: %s\n", error);
        return 2;
    }
    const int result = gui_main_utf8(utf8.argc, utf8.argv);
    nt_utf8_argv_dispose(&utf8);
    return result;
}
#else
int main(int argc, char *argv[]) { return gui_main_utf8(argc, argv); }
#endif
