/* ntpacker-gui -- native GUI for the Neotolis Texture Packer.
 *
 * PROJECT EDITOR + LIVE PACKER (this iteration): create/open/save .ntpacker_project files,
 * manage atlases, add image files/folders as sources, browse+select sprites, PACK atlases
 * in-process (gui_pack -> tp_pack), render the real packed page on the center canvas with
 * zoom/pan + region overlays + selection sync, and EXPORT target files (gui_pack -> tp_export_run).
 * The single source of truth is the linked tp_project (tp_core) -- the GUI is a thin editor
 * (AGENTS tool-parity). The Pack button surfaces the "preview stale" state per ux.md §3.3b.
 *
 * Module split: gui_project (state + dirty bits + load/save), gui_scan (display-only folder
 * enumeration), gui_pack (in-process pack + export orchestration), gui_canvas (dual-mode
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

#include "tp_core/tp_export.h" /* exporter registry -> target dropdown */

#include "ntpacker_ui_assets.h"

#include "clay.h"

#include "gui_canvas.h"
#include "gui_defs.h"
#include "gui_state.h"
#include "gui_widgets.h"
#include "gui_actions.h"
#include "gui_rows.h"
#include "gui_history.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_shell.h"    /* shell-owned surface the dev seams read (UI pool caps) */
#include "gui_selftest.h" /* dev seam: headless self-test (compiled out unless flag on) */
#include "gui_shot.h"     /* dev seam: --shot screenshot capture */
#include "gui_view_settings.h" /* right settings panel view (declare_right_panel) */
#include "gui_version.h"
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
#endif
// #endregion

// #region engine state
/* UI context capacities. UI_STATE_SLOTS / UI_STATE_PROBE_MAX / UI_ROW_ID_RING moved to gui_shell.h
 * in step 3 (shared with the selftest dev-seam); their rationale + values live there. The notes
 * below still describe them. Provisioned with generous headroom for a heavy desktop session
 * (thousands of project rows). The sprite list is virtualized (nt_ui_vlist): only the visible
 * window materializes Clay elements + widget state each frame, and per-row ids RECYCLE through
 * the vlist id_ring, so live widgets are bounded by the viewport, not by project size.
 * nt_ui_state has NO per-frame eviction -- a state cell persists until its id is reused -- so the
 * pool must hold every DISTINCT id that appears over a session. With recycling that is
 * id_ring x per-row stateful children + fixed chrome (menus, overlay cluster, page bar, modals).
 *   UI_STATE_SLOTS     : power-of-2 retained-state cells. Was 512 -> overflowed once a 500-file
 *                        folder scrolled through every ring slot (256 ring x [hit + x] ~= 512).
 *   UI_STATE_PROBE_MAX : linear-probe window. The engine default (4) is far too small at this
 *                        load -- placement failed well below capacity and fired the "pool
 *                        overflow" assert (nt_ui_state.c:38). THIS was the real crash trigger.
 *   UI_MAX_ELEMENTS    : Clay layout-element cap (also Clay's persistent per-id hashmap). */
#define UI_MAX_ELEMENTS ((uint32_t)8192U)
#define UI_ARENA_SIZE ((size_t)24U * 1024U * 1024U)
#define SCRATCH_ARENA_SIZE ((size_t)4U * 1024U * 1024U)

static NT_UI_DECLARE_ARENA(s_ui_arena, UI_ARENA_SIZE);
static nt_buffer_t s_frame_ubo;

static nt_hash32_t s_pack_id;
static nt_resource_t s_sprite_vs_handle;
static nt_resource_t s_sprite_fs_handle;
static nt_resource_t s_text_vs_handle;
static nt_resource_t s_text_fs_handle;
static nt_resource_t s_atlas_handle;
static nt_resource_t s_atlas_tex_handle;
static nt_resource_t s_font_resource;
static nt_material_t s_sprite_material;
static nt_material_t s_text_material;
static bool s_atlas_bound;
// #endregion

// #region ui ids + menu state
static bool s_ids_ready;

static uint32_t s_id_mb_file, s_id_mb_edit, s_id_mb_view, s_id_mb_help;
static uint32_t s_id_menu_file, s_id_menu_edit, s_id_menu_view, s_id_menu_help;
static nt_ui_menu_state_t s_file_state, s_edit_state, s_view_state, s_help_state;
static nt_ui_menu_ctx_t s_file_menu, s_edit_menu, s_view_menu, s_help_menu;
enum {
    MK_NEW = 1, MK_OPEN, MK_SAVE, MK_SAVEAS, MK_EXPORT, MK_REFRESH, MK_EXIT,
    MK_UNDO, MK_REDO,
    MK_ZIN, MK_ZOUT, MK_FIT, MK_ABOUT, MK_S100, MK_S125, MK_S150, MK_S200,
    MK_OV_OUTLINE, MK_OV_FRAME, MK_OV_TRIM, MK_OV_PIVOT, MK_CTX_FIT, MK_CTX_100,
    MK_CTX_RENAME, MK_CTX_REMOVE, MK_CTX_TOGGLE, MK_CTX_CREATE_ANIM, MK_CTX_PREVIEW
};

/* Right-click context menu: one cursor-anchored menu whose items depend on the row a
 * right-click armed it over (§3.3e mouse-complete access). Its actions call the same code
 * paths as the [x] buttons / inline editors. The trigger/payload state (s_id_ctx_menu, s_ctx_state,
 * s_ctx_kind, the CTX_ enum, s_ctx_atlas, s_ctx_anim, s_ctx_target, s_ctx_src, s_ctx_sprite,
 * s_ctx_leaf, s_ctx_removable) moved to gui_state (step 4 -- written by three different views);
 * s_ctx_menu is the declare-machinery working buffer for declare_context_menu below, which stays
 * here until chrome (step 6b). */
static nt_ui_menu_ctx_t s_ctx_menu;
// #endregion

// #region editor state
static const tp_result *s_shown_result; /* pack result currently bound to the canvas (sync guard) */
/* Canvas mouse model: a left press arms a potential click; if the pointer moves past a small
 * threshold while held it becomes a PAN (no selection on release); otherwise release = click-select.
 * Middle-drag always pans; wheel always zooms. Selection never captures later pan/zoom. */
static bool s_lmb_armed;      /* left button pressed on the canvas, click vs drag undecided */
static bool s_lmb_panning;    /* left drag crossed the threshold -> panning */
static bool s_mmb_panning;    /* middle-drag pan */
static float s_press_x, s_press_y; /* left-press origin (threshold test) */
static float s_pan_last_x, s_pan_last_y;
#define CANVAS_DRAG_THRESHOLD 4.0F

/* The deferred side-effect queue (s_pending_*), the new/open/exit confirm-flow flags (s_after_confirm/
 * s_confirm_open/s_modal_action) and the last-pack timing (s_last_pack_*) moved to gui_actions (step 2);
 * the modal open flags (s_about_open / s_export_open) moved to gui_state (step 3 -- shared with the
 * selftest); s_blur_inputs (set here by frame(), read by the settings-panel field widgets) moved to
 * gui_state too (step 4 -- the view TU needs it). */

/* Pack-button state cached for the tooltip pass (declared at root). */
static bool s_pack_has_sources, s_pack_stale;
/* The right settings panel's disclosure/dropdown-open bits and numeric-field edit buffers moved to
 * gui_view_settings.c (step 4 -- panel-local). GUI_MAX_TARGETS and k_playback_names live in
 * gui_defs.h (shared with declare_export_modal / declare_canvas_preview here). */

// #endregion

// #region small helpers
/* Opens `url` in the OS default browser. Reusable helper (About link now; future notices/docs
 * links reuse it). Windows: ShellExecuteA (shell32 -- already linked via tinyfiledialogs).
 * POSIX: xdg-open/open, best-effort. Returns true if the open was dispatched. */
static bool gui_open_url(const char *url) {
    if (!url || url[0] == '\0') {
        return false;
    }
#ifdef _WIN32
    HINSTANCE rc = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    return (INT_PTR)rc > 32; /* ShellExecute returns a value > 32 on success */
#elif defined(__APPLE__)
    char cmd[1200];
    (void)snprintf(cmd, sizeof cmd, "open '%s' >/dev/null 2>&1 &", url);
    return system(cmd) == 0;
#else
    char cmd[1200];
    (void)snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", url);
    return system(cmd) == 0;
#endif
}

// #endregion

// #region init helpers
static void resolve_exe_dir(void) {
#ifdef _WIN32
    char exe[1024];
    DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (n > 0U && n < (DWORD)sizeof exe) {
        char *slash = strrchr(exe, '\\');
        if (slash != NULL) {
            *slash = '\0';
            (void)snprintf(s_exe_dir, sizeof s_exe_dir, "%s", exe);
            return;
        }
    }
#endif
    (void)snprintf(s_exe_dir, sizeof s_exe_dir, ".");
}

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

static void ensure_ids(void) {
    if (s_ids_ready) {
        return;
    }
    s_id_btn_pack = nt_ui_id("ntpacker/btn_pack");
    s_id_btn_export = nt_ui_id("ntpacker/btn_export");
    s_id_btn_refresh = nt_ui_id("ntpacker/btn_refresh");
    s_id_vlist = nt_ui_id("ntpacker/sprite_vlist");
    s_id_canvas = nt_ui_id("ntpacker/canvas");
    s_id_modal = nt_ui_id("ntpacker/confirm_modal");
    s_id_about = nt_ui_id("ntpacker/about_modal");
    s_id_rename = nt_ui_id("ntpacker/rename_input");
    s_id_right_panel = nt_ui_id("ntpacker/right_panel");
    s_id_left_panel = nt_ui_id("ntpacker/left_panel");
    s_id_strip = nt_ui_id("ntpacker/canvas_strip");
    s_id_status_pill = nt_ui_id("ntpacker/status_pill");
    s_id_right_content = nt_ui_id("ntpacker/right_content");
    s_id_export_modal = nt_ui_id("ntpacker/export_modal");
    s_id_mb_file = nt_ui_id("ntpacker/mb_file");
    s_id_mb_edit = nt_ui_id("ntpacker/mb_edit");
    s_id_mb_view = nt_ui_id("ntpacker/mb_view");
    s_id_mb_help = nt_ui_id("ntpacker/mb_help");
    s_id_menu_file = nt_ui_id("ntpacker/menu_file");
    s_id_menu_edit = nt_ui_id("ntpacker/menu_edit");
    s_id_menu_view = nt_ui_id("ntpacker/menu_view");
    s_id_menu_help = nt_ui_id("ntpacker/menu_help");
    s_id_ctx_menu = nt_ui_id("ntpacker/ctx_menu");

    s_menu_style = nt_ui_menu_style_defaults();
    s_menu_style.icon_size = 0U;
    s_modal_style = nt_ui_modal_style_defaults();
    s_tip_style = nt_ui_tooltip_style_defaults();

    s_rename_input = nt_ui_input_style_defaults();
    s_rename_input.text.font_id = 0;
    s_rename_input.text.color = (Clay_Color){225.0F, 228.0F, 235.0F, 255.0F};
    s_rename_input.placeholder.font_id = 0;
    s_rename_input.placeholder.color = (Clay_Color){120.0F, 126.0F, 138.0F, 255.0F};
    /* Field well (§2.7 item 7): recessed `input` fill + `border`; focus ring = `border-strong` blue. */
    s_rename_input.skin[NT_UI_INPUT_IDLE].bg_color = RGBA8(21, 23, 30);
    s_rename_input.skin[NT_UI_INPUT_IDLE].border_color = RGBA8(52, 58, 72);
    s_rename_input.skin[NT_UI_INPUT_FOCUSED].bg_color = RGBA8(21, 23, 30);
    s_rename_input.skin[NT_UI_INPUT_FOCUSED].border_color = RGBA8(86, 132, 204);
    s_rename_input.border_width = 1.0F;

    /* Settings-panel widget styles. The atlas WHITE region (s_white_ref, bound by now
     * since can_render gates ensure_ids) is the art for checkbox/slider parts, tinted
     * per state; the dropdown is flat-color. */
    s_dd_style = nt_ui_dropdown_style_defaults();
    s_dd_style.font_id = 0;
    s_dd_style.trigger_text = RGBA8(214, 220, 230);
    s_dd_style.row_text = RGBA8(214, 220, 230);
    /* Well look (§2.7 item 7): recessed `input` fill so combos read as fields, not buttons. The engine
     * dropdown trigger has no border field, so the recessed fill (darker than panel) carries the well
     * read; the open state uses the lighter `pressed` tint as its active signal. */
    s_dd_style.trigger_idle.fill = RGBA8(21, 23, 30);
    s_dd_style.trigger_hover.fill = RGBA8(30, 34, 44);
    s_dd_style.trigger_pressed.fill = RGBA8(40, 46, 58);
    s_dd_style.panel_fill = RGBA8(30, 33, 41);
    s_dd_style.row_idle.fill = 0U;
    s_dd_style.row_hover.fill = RGBA8(54, 60, 74);
    s_dd_style.row_pressed.fill = RGBA8(36, 40, 50);
    s_dd_style.row_selected.fill = RGBA8(52, 78, 120);
    s_dd_style.panel_corner_radius = 4U;
    s_dd_style.max_visible_rows = 8U;

    s_slider_style = nt_ui_slider_style_defaults();
    s_slider_style.states[NT_UI_SLIDER_IDLE].track = s_white_ref;
    s_slider_style.states[NT_UI_SLIDER_IDLE].track_tint = RGBA8(46, 50, 60);
    s_slider_style.states[NT_UI_SLIDER_IDLE].fill = s_white_ref;
    s_slider_style.states[NT_UI_SLIDER_IDLE].fill_tint = RGBA8(78, 126, 192);
    s_slider_style.states[NT_UI_SLIDER_IDLE].thumb = s_white_ref;
    s_slider_style.states[NT_UI_SLIDER_IDLE].thumb_tint = RGBA8(220, 228, 238);

    s_num_input = nt_ui_input_style_defaults();
    s_num_input.text.font_id = 0;
    s_num_input.text.color = (Clay_Color){225.0F, 228.0F, 235.0F, 255.0F};
    s_num_input.placeholder.font_id = 0;
    s_num_input.placeholder.color = (Clay_Color){120.0F, 126.0F, 138.0F, 255.0F};
    /* Field well (§2.7 item 7): recessed `input` fill + always-on `border`; focus/active = `border-strong`. */
    s_num_input.skin[NT_UI_INPUT_IDLE].bg_color = RGBA8(21, 23, 30);
    s_num_input.skin[NT_UI_INPUT_IDLE].border_color = RGBA8(52, 58, 72);
    s_num_input.skin[NT_UI_INPUT_HOVER].bg_color = RGBA8(21, 23, 30);
    s_num_input.skin[NT_UI_INPUT_HOVER].border_color = RGBA8(70, 78, 96);
    s_num_input.skin[NT_UI_INPUT_FOCUSED].bg_color = RGBA8(21, 23, 30);
    s_num_input.skin[NT_UI_INPUT_FOCUSED].border_color = RGBA8(86, 132, 204);
    s_num_input.skin[NT_UI_INPUT_DISABLED].bg_color = RGBA8(26, 28, 36);
    s_num_input.skin[NT_UI_INPUT_DISABLED].border_color = RGBA8(40, 44, 54);
    s_num_input.border_width = 1.0F;

    s_panel_scroll = nt_ui_scroll_style_defaults();
    s_panel_scroll.scroll_x = false;
    s_panel_scroll.scroll_y = true;
    s_panel_scroll.bar_visibility = NT_UI_SCROLLBAR_AUTO_HIDE;
    s_panel_scroll.track_ref = s_white_ref;
    s_panel_scroll.track_tint = RGBA8(30, 33, 41);
    s_panel_scroll.thumb_ref = s_white_ref;
    s_panel_scroll.thumb_tint = RGBA8(80, 86, 100);

    s_ids_ready = true;
}

/* Resolve one baked icon region into a memoized ref, exactly like the white region. The icon MUST be
 * present (build_packs bakes it) -- a miss is a bake/codegen mismatch, so crash early. */
static nt_atlas_region_ref_t bind_icon_ref(nt_hash64_t region_id) {
    const uint32_t idx = nt_atlas_find_region(s_atlas_handle, region_id.value);
    NT_ASSERT(idx != NT_ATLAS_INVALID_REGION && "ntpacker-gui: baked UI icon region missing");
    return nt_atlas_ref_idx(s_atlas_handle, region_id.value, idx);
}

static void try_bind_resources(void) {
    if (s_atlas_bound && s_font_bound) {
        return;
    }
    if (!s_atlas_bound && nt_resource_is_ready(s_atlas_handle)) {
        const uint32_t white = nt_atlas_find_region(s_atlas_handle, ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS__WHITE.value);
        NT_ASSERT(white != NT_ATLAS_INVALID_REGION);
        nt_ui_set_atlas_white_region(s_ctx, s_atlas_handle, white);
        s_white_ref = nt_atlas_ref_idx(s_atlas_handle, ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS__WHITE.value, white);
        s_ic_layout_grid = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_LAYOUT_GRID);
        s_ic_triangle_alert = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_TRIANGLE_ALERT);
        s_ic_download = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_DOWNLOAD);
        s_ic_refresh = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_REFRESH_CW);
        s_ic_chevron_left = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_CHEVRON_LEFT);
        s_ic_chevron_right = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_CHEVRON_RIGHT);
        s_ic_minus = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_MINUS);
        s_ic_plus = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_PLUS);
        s_ic_scan = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_SCAN);
        s_ic_maximize = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_MAXIMIZE_2);
        s_ic_chevron_down = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_CHEVRON_DOWN);
        s_ic_layers = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_LAYERS);
        s_ic_folder = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FOLDER);
        s_ic_image = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_IMAGE);
        s_ic_film = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FILM);
        s_ic_file_plus = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FILE_PLUS);
        s_ic_folder_plus = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FOLDER_PLUS);
        s_ic_x = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_X);
        s_ic_info = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_INFO);
        s_ic_circle_check = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_CIRCLE_CHECK);
        s_ic_octagon_alert = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_OCTAGON_ALERT);
        s_ic_folder_plus_hero = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FOLDER_PLUS_HERO);
        s_atlas_bound = true;
        nt_log_info("ntpacker-gui: atlas white + icon regions bound");
    }
    if (!s_font_bound && nt_resource_is_ready(s_font_resource)) {
        nt_font_add(s_font, s_font_resource);
        nt_ui_set_font(s_ctx, 0U, s_font);
        s_font_bound = true;
        nt_log_info("ntpacker-gui: font bound at slot 0");
    }
}
// #endregion

// #region menu bar
/* Prototype in gui_shell.h (step 4 -- gui_view_settings' target-row context-menu trigger needs it,
 * same as every other right-click trigger; interim until it moves to gui_view_chrome in step 6b). */
void close_menubar_menus(void) {
    s_file_state.open = false;
    s_edit_state.open = false;
    s_view_state.open = false;
    s_help_state.open = false;
}
static void close_all_menus(void) {
    close_menubar_menus();
    s_ctx_state.open = false;
}
static void file_items(nt_ui_menu_ctx_t *m) {
    if (nt_ui_menu_item_ex(m, MK_NEW, "New", (nt_ui_menu_item_opts_t){.shortcut = "Ctrl+N"})) {
        request_new();
    }
    if (nt_ui_menu_item_ex(m, MK_OPEN, "Open...", (nt_ui_menu_item_opts_t){.shortcut = "Ctrl+O"})) {
        request_open();
    }
    if (nt_ui_menu_item_ex(m, MK_SAVE, "Save", (nt_ui_menu_item_opts_t){.shortcut = "Ctrl+S"})) {
        s_pending_save = true;
    }
    if (nt_ui_menu_item_ex(m, MK_SAVEAS, "Save As...", (nt_ui_menu_item_opts_t){.shortcut = "Ctrl+Shift+S"})) {
        s_pending_save_as = true;
    }
    nt_ui_menu_separator(m);
    if (nt_ui_menu_item_ex(m, MK_EXPORT, "Export\xE2\x80\xA6", (nt_ui_menu_item_opts_t){.shortcut = "Ctrl+E"})) {
        s_export_open = true;
    }
    nt_ui_menu_separator(m);
    if (nt_ui_menu_item_ex(m, MK_REFRESH, "Refresh", (nt_ui_menu_item_opts_t){.shortcut = "F5"})) {
        s_pending_refresh = true;
    }
    nt_ui_menu_separator(m);
    if (nt_ui_menu_item(m, MK_EXIT, "Exit")) {
        request_exit();
    }
}
static void edit_items(nt_ui_menu_ctx_t *m) {
    nt_ui_menu_item_opts_t u = {.shortcut = "Ctrl+Z", .disabled = !gui_project_can_undo()};
    if (nt_ui_menu_item_ex(m, MK_UNDO, "Undo", u)) {
        do_undo();
    }
    nt_ui_menu_item_opts_t r = {.shortcut = "Ctrl+Y", .disabled = !gui_project_can_redo()};
    if (nt_ui_menu_item_ex(m, MK_REDO, "Redo", r)) {
        do_redo();
    }
}
/* Radio-style UI-scale item; the active one is marked with a check glyph (baked in the DejaVu font). */
static void scale_item(nt_ui_menu_ctx_t *m, uint32_t key, const char *pct, float value) {
    const bool active = (g_ui_scale > (value - 0.01F)) && (g_ui_scale < (value + 0.01F));
    char buf[32];
    (void)snprintf(buf, sizeof buf, active ? "%s  \xE2\x9C\x93" : "%s", pct); /* U+2713 check */
    if (nt_ui_menu_item(m, key, buf)) {
        g_ui_scale = value; /* TODO: persist in an app-settings file (not the project) later */
        set_statusf("UI scale %s", pct);
    }
}
/* Overlay-toggle menu item: shows a check when active; click flips the game-owned bool. */
static void overlay_item(nt_ui_menu_ctx_t *m, uint32_t key, const char *name, bool *flag) {
    char buf[56];
    (void)snprintf(buf, sizeof buf, *flag ? "%s  \xE2\x9C\x93" : "%s", name); /* U+2713 check mark */
    if (nt_ui_menu_item(m, key, buf)) {
        *flag = !*flag;
    }
}
static void view_items(nt_ui_menu_ctx_t *m) {
    if (nt_ui_menu_item(m, MK_ZIN, "Zoom In")) {
        gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, gui_canvas_zoom_pct(&s_canvas) * 1.25F);
    }
    if (nt_ui_menu_item(m, MK_ZOUT, "Zoom Out")) {
        gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, gui_canvas_zoom_pct(&s_canvas) * 0.8F);
    }
    if (nt_ui_menu_item(m, MK_FIT, "Fit")) {
        gui_canvas_fit(&s_canvas);
    }
    nt_ui_menu_separator(m);
    overlay_item(m, MK_OV_OUTLINE, "Region outlines (hull)", &s_canvas.show_outline);
    overlay_item(m, MK_OV_FRAME, "Frame rects", &s_canvas.show_frame);
    overlay_item(m, MK_OV_TRIM, "Trim bounds", &s_canvas.show_trim);
    overlay_item(m, MK_OV_PIVOT, "Pivots", &s_canvas.show_pivot);
    nt_ui_menu_separator(m);
    scale_item(m, MK_S100, "UI Scale 100%", 1.0F);
    scale_item(m, MK_S125, "UI Scale 125%", 1.25F);
    scale_item(m, MK_S150, "UI Scale 150%", 1.5F);
    scale_item(m, MK_S200, "UI Scale 200%", 2.0F);
}
static void help_items(nt_ui_menu_ctx_t *m) {
    if (nt_ui_menu_item(m, MK_ABOUT, "About")) {
        s_about_open = true; /* opens the real modal (F6a) */
    }
}
static void menubar_entry(nt_ui_context_t *ctx, uint32_t btn_id, const char *label, nt_ui_menu_state_t *st) {
    nt_ui_button_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), btn_id, &g_menubtn,
                       &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0)},
                                                             .padding = {Su(10), Su(10), Su(2), Su(2)},
                                                             .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                                  .cornerRadius = CLAY_CORNER_RADIUS(S(4))},
                       true, NULL);
    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), label, &g_body);
    if (nt_ui_button_end(ctx)) {
        const bool was_open = st->open;
        close_all_menus();
        if (!was_open) {
            const nt_ui_bbox_t bb = nt_ui_get_bbox(ctx, btn_id);
            st->anchor_x = bb.x;
            st->anchor_y = bb.y + bb.height;
            st->open = true;
        }
    }
}
static void declare_menubar(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_MENUBAR_H))},
                     .padding = {Su(4), Su(8), 0, 0},
                     .childGap = Su(2),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_STATUS}) { /* docked: flush to the top edge, no rounded corners */
        menubar_entry(ctx, s_id_mb_file, "File", &s_file_state);
        menubar_entry(ctx, s_id_mb_edit, "Edit", &s_edit_state);
        menubar_entry(ctx, s_id_mb_view, "View", &s_view_state);
        menubar_entry(ctx, s_id_mb_help, "Help", &s_help_state);
        /* right side: project name + dirty dot */
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        if (gui_project_is_dirty()) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "*", &g_tag);
        }
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), gui_project_display_name(), &g_body);
    }
}
// #endregion

// #region left panel (atlases + sprites)
static const nt_ui_events_cfg_t s_dbl_cfg = {.long_press_secs = 0.0F, .double_click = true};

/* start_atlas_edit/start_anim_edit/start_sprite_edit_named/start_sprite_edit moved to gui_actions
 * (step 4 -- the entry side of the edit lifecycle, needed by both this panel and the settings view). */

static void declare_atlas_list(nt_ui_context_t *ctx, tp_project *proj) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(20.0F))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        section_rule_label(ctx, "ATLASES");
    }
    for (int i = 0; i < proj->atlas_count; i++) {
        char idbuf[64];
        (void)snprintf(idbuf, sizeof idbuf, "ntpacker/atlas_row_%d", i);
        const uint32_t row_id = nt_ui_id(idbuf);
        const uint32_t x_id = nt_ui_child_id(row_id, "x");
        const bool editing = (s_edit_kind == EDIT_ATLAS && s_edit_atlas == i);
        const bool selected = (i == s_sel_atlas);
        const nt_ui_events_t ev = nt_ui_events(ctx, row_id, &s_dbl_cfg);
        const nt_ui_events_t xev = nt_ui_events(ctx, x_id, NULL);
        if (xev.clicked) {
            s_pending_remove_atlas = i;
        } else if (ev.double_clicked) {
            start_atlas_edit(i);
        } else if (ev.clicked && i != s_sel_atlas) {
            s_sel_atlas = i;
            reset_selection();
            cancel_edit();
        }
        if (nt_ui_menu_open_trigger(ctx, s_id_ctx_menu, row_id, false, &s_ctx_state)) {
            close_menubar_menus();
            s_sel_atlas = i; /* right-click selects the row first */
            reset_selection();
            cancel_edit();
            s_ctx_kind = CTX_ATLAS;
            s_ctx_atlas = i;
        }
        const bool has_x = (proj->atlas_count > 1);
        const Clay_Color bg = selected ? C_SEL : (ev.hovered ? C_HOVER : C_TRANSPARENT);
        CLAY({.id = {.id = row_id},
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                         .padding = {Su(8), Su(4), 0, 0},
                         .childGap = Su(4),
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = bg,
              .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                ui_row_icon(ctx, &s_ic_layers, selected ? &g_row_strong : &g_caption);
                if (editing) {
                    if (render_rename_field(ctx)) {
                        commit_atlas_rename();
                    }
                } else {
                    ui_label_fit(ctx, proj->atlases[i].name, selected ? &g_row_strong : &g_row,
                                 fmaxf(left_row_text_w(S(8.0F), has_x) - S(ROW_ICON_RESERVE), S(16.0F)), row_id);
                }
            }
            if (has_x) {
                record_row_tip(x_id, "Remove atlas");
                (void)ui_icon_btn(ctx, x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                                  xev.hovered ? &g_danger : &g_caption);
            }
        }
    }
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_atlas"), &s_ic_plus, 16.0F, "Atlas", &g_btn_ghost, true, 0.0F, 26.0F, &g_caption)) {
        s_pending_add_atlas = true;
    }
}

/* Applies a click on sprite row `i` (Ctrl toggles, Shift range-selects from the anchor, plain replaces)
 * to the multi-selection, and updates the PRIMARY selection (region panel / canvas sync). */
static void select_sprite_row(int i, bool ctrl, bool shift) {
    if (i < 0 || i >= s_row_count) {
        return;
    }
    const sprite_row *row = &s_rows[i];
    const bool leaf = (!row->is_folder && !row->missing && row->sprite_name[0] != '\0');
    s_sel_src = row->src;
    s_sel_child = row->child;
    s_sel_missing = row->missing;
    (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", row->abs);
    if (leaf) {
        if (shift && s_sel_anchor_row >= 0 && s_sel_anchor_row < s_row_count) {
            multi_sel_clear();
            const int lo = (s_sel_anchor_row < i) ? s_sel_anchor_row : i;
            const int hi = (s_sel_anchor_row < i) ? i : s_sel_anchor_row;
            for (int k = lo; k <= hi; k++) {
                const sprite_row *rk = &s_rows[k];
                if (!rk->is_folder && !rk->missing && rk->sprite_name[0] != '\0') {
                    multi_sel_add(rk->sprite_name);
                }
            }
        } else if (ctrl) {
            if (multi_sel_contains(row->sprite_name)) {
                multi_sel_remove(row->sprite_name);
            } else {
                multi_sel_add(row->sprite_name);
            }
            s_sel_anchor_row = i;
        } else {
            multi_sel_set_single(row->sprite_name);
            s_sel_anchor_row = i;
        }
    } else if (!ctrl && !shift) {
        multi_sel_clear();
        s_sel_anchor_row = -1;
    }
    if (gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_ATLAS && leaf) {
        gui_canvas_select(&s_canvas, gui_pack_find_sprite(s_sel_atlas, row->sprite_name));
    }
}

static void declare_sprite_list(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(28))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        section_rule_label(ctx, "SPRITES");
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        /* Smart folder is the primary input (ux.md principle 3: folders, not files); it keeps its label so
         * the live-linked behaviour is explicit (owner 2026-07-11). Per-file adds are "the exception", so the
         * Files button is icon-only (tooltip in declare_tooltips) -- this also keeps the header on one line at
         * the owner's 450px panel where two labelled buttons overran and wrapped "Smart folder". */
        if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_files"), &s_ic_file_plus, 16.0F, NULL, &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            s_pending_add_files = true;
        }
        /* Drop the label to icon-only on a heavily-clamped panel (narrow window / high DPI) so it never
         * wraps or bleeds; the tooltip carries the meaning either way (mouse-complete). >= 240 design px keeps
         * the label at the owner's 1920/1366 @1.5 (450px panels) and every unclamped base-300 panel. */
        const char *folder_lbl = (s_left_panel_w >= S(240.0F)) ? "Smart folder" : NULL;
        if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_folder"), &s_ic_folder_plus, 16.0F, folder_lbl, &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            s_pending_add_folder = true;
        }
    }

    if (s_row_count == 0) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No sources. Add a smart folder or files.", &g_caption);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        return;
    }

    nt_ui_vlist_style_t vs = nt_ui_vlist_style_defaults();
    vs.overscan = 3;
    vs.id_ring = UI_ROW_ID_RING; /* bound per-row state to the viewport, not project size */
    const nt_ui_vlist_range_t r = nt_ui_vlist_begin(
        ctx, NULL, s_id_vlist, (uint32_t)s_row_count, S(BASE_ROW_H), NT_UI_AXIS_Y, &vs,
        &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}});
    if (r.first <= r.last) {
        for (uint32_t i = r.first; i <= r.last; i++) {
            const sprite_row *row = &s_rows[i];
            const uint32_t row_id = nt_ui_vlist_item_id(ctx, i);
            const uint32_t hit_id = nt_ui_child_id(row_id, "hit");
            const uint32_t x_id = nt_ui_child_id(row_id, "x");
            const bool editing = (s_edit_kind == EDIT_SPRITE && row->sprite_name[0] != '\0' &&
                                  strcmp(s_edit_sprite, row->sprite_name) == 0);
            const bool leaf_row = (!row->is_folder && !row->missing && row->sprite_name[0] != '\0');
            const bool primary = (row->is_source ? (s_sel_src == row->src && s_sel_child == -1)
                                                  : (s_sel_src == row->src && s_sel_child == row->child));
            const bool selected = primary || (leaf_row && multi_sel_contains(row->sprite_name));
            const nt_ui_events_t ev = nt_ui_events(ctx, hit_id, &s_dbl_cfg);
            bool x_clicked = false;
            nt_ui_events_t xev = {0}; /* hoisted: the render below reads xev.hovered for the danger tint */
            if (row->is_source) {
                xev = nt_ui_events(ctx, x_id, NULL);
                x_clicked = xev.clicked;
                if (x_clicked) {
                    s_pending_remove_source = row->src;
                }
            }
            if (ev.double_clicked && !row->is_folder && !row->missing) {
                select_sprite_row((int)i, false, false);
                start_sprite_edit(row);
            } else if (ev.clicked && !x_clicked) {
                const bool ctrl = nt_input_key_is_down(NT_KEY_LCTRL) || nt_input_key_is_down(NT_KEY_RCTRL);
                const bool shift = nt_input_key_is_down(NT_KEY_LSHIFT) || nt_input_key_is_down(NT_KEY_RSHIFT);
                select_sprite_row((int)i, ctrl, shift);
            }
            if (nt_ui_menu_open_trigger(ctx, s_id_ctx_menu, hit_id, false, &s_ctx_state)) {
                close_menubar_menus();
                /* right-click a row NOT in the multi-set selects just it; keep an existing set otherwise */
                if (!(leaf_row && multi_sel_contains(row->sprite_name))) {
                    select_sprite_row((int)i, false, false);
                } else {
                    s_sel_src = row->src;
                    s_sel_child = row->child;
                    s_sel_missing = row->missing;
                    (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", row->abs);
                }
                s_ctx_kind = CTX_SPRITE;
                s_ctx_src = row->src;
                (void)snprintf(s_ctx_sprite, sizeof s_ctx_sprite, "%s", row->sprite_name);
                s_ctx_leaf = leaf_row;
                s_ctx_removable = row->is_source;
            }
            const Clay_Color bg = selected ? C_SEL : (ev.hovered ? C_HOVER : C_TRANSPARENT);
            const uint16_t indent = Su(8.0F + ((float)row->indent * 16.0F));
            /* Leading type icon: folder for a directory source, image for a sprite leaf (folder child or
             * file source); missing files reuse the image mask tinted warn. Label brightens on selection.
             * Smart-folder distinction (TexturePacker convention): the folder icon is AMBER (warn) vs the
             * neutral file icons, so a live-linked folder reads as a special input at a glance. */
            const nt_ui_label_style_t *lbl = row->missing ? &g_warn : (selected ? &g_row_strong : &g_row);
            nt_atlas_region_ref_t *ic = row->is_folder ? &s_ic_folder : &s_ic_image;
            const nt_ui_label_style_t *ic_tint = (row->missing || row->is_folder) ? &g_warn : (selected ? &g_row_strong : &g_caption);
            CLAY({.id = {.id = row_id},
                  .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                             .padding = {indent, Su(4), 0, 0},
                             .childGap = Su(4),
                             .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = bg,
                  .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
                CLAY({.id = {.id = hit_id},
                      .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    ui_row_icon(ctx, ic, ic_tint);
                    if (editing) {
                        if (render_rename_field(ctx)) {
                            commit_sprite_rename();
                        }
                    } else {
                        /* Folder rows carry a fixed smart-folder tooltip on the whole row (below), so skip the
                         * truncation tip here (one tip per id); leaf/file rows keep the full-text truncation tip. */
                        ui_label_fit(ctx, row->label, lbl,
                                     fmaxf(left_row_text_w(S(8.0F + (float)row->indent * 16.0F), row->is_source) - S(ROW_ICON_RESERVE), S(16.0F)),
                                     row->is_folder ? 0U : hit_id);
                    }
                    if (row->is_folder) {
                        record_row_tip(hit_id, "Smart folder: every image inside is packed automatically, "
                                               "including files added later. Press F5 to rescan.");
                    }
                }
                if (row->is_source) {
                    record_row_tip(x_id, row->is_folder ? "Remove this smart folder and all its sprites" : "Remove source");
                    (void)ui_icon_btn(ctx, x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                                      xev.hovered ? &g_danger : &g_caption);
                }
            }
        }
    }
    nt_ui_vlist_end(ctx);
}

/* ANIMATIONS block (ux.md §2.1 region D, §3.7b): one row per animation (id + frame count), a per-row
 * [x] remove + right-click Rename/Remove/Preview, "+ Animation" to add. Double-click a row = preview. */
static void declare_animations_list(nt_ui_context_t *ctx, tp_project_atlas *a) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(28))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        section_rule_label(ctx, "ANIMATIONS");
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_anim"), &s_ic_plus, 16.0F, "Animation", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            s_pending_add_anim = true;
        }
    }
    if (!a || a->animation_count == 0) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "None. Multi-select sprites, then right-click \xE2\x86\x92 Create animation.",
                    &g_caption);
        return;
    }
    for (int i = 0; i < a->animation_count; i++) {
        char idbuf[64];
        (void)snprintf(idbuf, sizeof idbuf, "ntpacker/anim_row_%d", i);
        const uint32_t row_id = nt_ui_id(idbuf);
        const uint32_t x_id = nt_ui_child_id(row_id, "x");
        const bool editing = (s_edit_kind == EDIT_ANIM && s_edit_anim == i);
        const bool selected = (i == s_sel_anim);
        const nt_ui_events_t ev = nt_ui_events(ctx, row_id, &s_dbl_cfg);
        const nt_ui_events_t xev = nt_ui_events(ctx, x_id, NULL);
        if (xev.clicked) {
            s_pending_remove_anim = i;
        } else if (ev.double_clicked) {
            s_sel_anim = i;
            s_sel_anim_frame = -1;
            s_pending_open_preview = true;
        } else if (ev.clicked) {
            s_sel_anim = i;
            s_sel_anim_frame = -1;
            cancel_edit();
        }
        if (nt_ui_menu_open_trigger(ctx, s_id_ctx_menu, row_id, false, &s_ctx_state)) {
            close_menubar_menus();
            s_sel_anim = i; /* right-click selects the row first */
            s_sel_anim_frame = -1;
            s_ctx_kind = CTX_ANIM;
            s_ctx_anim = i;
        }
        const Clay_Color bg = selected ? C_SEL : (ev.hovered ? C_HOVER : C_TRANSPARENT);
        CLAY({.id = {.id = row_id},
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                         .padding = {Su(8), Su(4), 0, 0},
                         .childGap = Su(4),
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = bg,
              .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                ui_row_icon(ctx, &s_ic_film, selected ? &g_row_strong : &g_caption);
                if (editing) {
                    if (render_rename_field(ctx)) {
                        commit_anim_rename();
                    }
                } else {
                    ui_label_fit(ctx, a->animations[i].id, selected ? &g_row_strong : &g_row,
                                 fmaxf(left_row_text_w(S(8.0F), true) - S(28.0F) - S(ROW_ICON_RESERVE), S(16.0F)), row_id);
                }
            }
            char fc[16];
            (void)snprintf(fc, sizeof fc, "%df", a->animations[i].frame_count);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), fc, &g_caption);
            record_row_tip(x_id, "Remove animation");
            (void)ui_icon_btn(ctx, x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                              xev.hovered ? &g_danger : &g_caption);
        }
    }
}

static void declare_left_panel(nt_ui_context_t *ctx) {
    tp_project *proj = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(proj, s_sel_atlas);
    s_row_tip_count = 0; /* per-frame; filled by ui_label_fit when a row truncates */
    CLAY({.id = {.id = s_id_left_panel},
          .layout = {.sizing = {CLAY_SIZING_FIXED(s_left_panel_w), CLAY_SIZING_GROW(0)},
                     .padding = {Su(8), Su(8), Su(8), Su(8)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
          .backgroundColor = C_PANEL,
          /* Docked region (pass 2): no card corner/border -- the 2px C_BG seam to the canvas is the divider.
           * Vertical clip only (X-clip is forbidden -- it makes children X-scrollable/unbounded): at short
           * window heights the animations hint wraps tall and would otherwise draw past the panel bottom.
           * The sprite vlist keeps its own inner scroll. */
          .clip = {.vertical = true}}) {
        declare_atlas_list(ctx, proj);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(1))}}, .backgroundColor = C_BORDER}) {}
        declare_sprite_list(ctx);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(1))}}, .backgroundColor = C_BORDER}) {}
        declare_animations_list(ctx, a);
    }
}

/* Emit hover tooltips (full text) for the truncated rows collected this frame. */
static void declare_row_tooltips(nt_ui_context_t *ctx) {
    for (int i = 0; i < s_row_tip_count; i++) {
        (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_row_tips[i].id, s_row_tips[i].full, &s_tip_style);
    }
}
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
        s_confirm_open || s_about_open || s_export_open || s_edit_kind != EDIT_NONE) {
        s_lmb_armed = s_lmb_panning = s_mmb_panning = false;
        return;
    }
    /* Use the box the handler actually drew the page into (captured last frame). Layout px ==
     * framebuffer px in this app (STRETCH ref = fb). */
    const float *box = s_canvas.last_bb;
    if (box[2] <= 1.0F) {
        s_lmb_armed = s_lmb_panning = s_mmb_panning = false;
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
        if (!s_lmb_panning) { /* click: select region under cursor, or clear on empty */
            const int hit = gui_canvas_hit(&s_canvas, p->x, p->y);
            gui_canvas_select(&s_canvas, hit);
            if (hit >= 0) {
                select_row_for_region(hit);
            }
        }
        s_lmb_armed = false;
        s_lmb_panning = false;
    }

    if (inside && !s_lmb_panning && !s_mmb_panning) {
        s_canvas.hover_sprite = gui_canvas_hit(&s_canvas, p->x, p->y);
    }
}

/* Approximate page fill: sum of placed AABB areas (transform-swapped where diagonal) over sum of
 * page areas. Labeled "filled" -- an approximation (ignores padding/margin), good enough for the E'
 * stats line (ux.md §2.1). */
static float atlas_fill_pct(const tp_result *r) {
    if (!r || r->page_count == 0) {
        return 0.0F;
    }
    double placed = 0.0;
    double total = 0.0;
    for (int i = 0; i < r->page_count; i++) {
        total += (double)r->pages[i].w * (double)r->pages[i].h;
    }
    for (int i = 0; i < r->sprite_count; i++) {
        const tp_sprite *s = &r->sprites[i];
        const int ow = (s->transform & 4u) ? s->frame.h : s->frame.w;
        const int oh = (s->transform & 4u) ? s->frame.w : s->frame.h;
        placed += (double)ow * (double)oh;
    }
    return (total > 0.0) ? (float)(placed * 100.0 / total) : 0.0F;
}

/* transform_decode_str moved to gui_widgets (step 4 -- shared by this canvas readout and the
 * settings-panel "Packed" row). */

/* --- Canvas strip control groups (icons; shared by the single-row strip and the two-row compact). Every
 * icon-only button gets a tooltip in declare_tooltips (mouse-complete). `h` = the row height. --- */
static void strip_group_actions(nt_ui_context_t *ctx, bool accent, bool labels, float h) {
    if (gui_pack_async_busy()) {
        /* Busy: a disabled elapsed/progress label + a Cancel affordance (ux.md §3 worker thread). The
         * status label always carries text (even in the icon-only tier) so it stays honest. */
        char busy[48];
        if (gui_pack_async_active_kind() == GUI_PACK_ASYNC_EXPORT) {
            int cur = 0;
            int total = 0;
            gui_pack_export_progress(&cur, &total);
            (void)snprintf(busy, sizeof busy, "Exporting %d/%d\xE2\x80\xA6", cur > 0 ? cur : 1, total > 0 ? total : 1);
        } else if (gui_pack_async_cancelling()) {
            (void)snprintf(busy, sizeof busy, "Cancelling\xE2\x80\xA6");
        } else if (labels) {
            (void)snprintf(busy, sizeof busy, "Packing\xE2\x80\xA6 %.1fs", gui_pack_async_elapsed_sec());
        } else {
            (void)snprintf(busy, sizeof busy, "Packing\xE2\x80\xA6"); /* narrow tier: drop the seconds */
        }
        (void)ui_icon_btn(ctx, s_id_btn_pack, &s_ic_refresh, 16.0F, busy, &g_btn_primary, false, 0.0F, h, &g_onaccent);
        if (ui_icon_btn(ctx, s_id_btn_export, &s_ic_x, 16.0F, labels ? "Cancel" : NULL, &g_btn,
                        !gui_pack_async_cancelling(), 0.0F, h, &g_body)) {
            gui_pack_async_cancel();
        }
        if (ui_icon_btn(ctx, s_id_btn_refresh, &s_ic_refresh, 16.0F, NULL, &g_btn_ghost, true, 0.0F, h, &g_caption)) {
            s_pending_refresh = true;
        }
        return;
    }
    /* Pack: PRIMARY blue + layout-grid when up to date; amber + alert-triangle when stale (§2.9). Icon +
     * label tier switch together so the amber button carries dark content, the blue one bright. */
    nt_atlas_region_ref_t *pack_ic = accent ? &s_ic_triangle_alert : &s_ic_layout_grid;
    nt_ui_button_style_t *pack_st = accent ? &g_btn_accent : &g_btn_primary;
    const nt_ui_label_style_t *pack_lbl = accent ? &g_onwarn : &g_onaccent;
    if (ui_icon_btn(ctx, s_id_btn_pack, pack_ic, 16.0F, labels ? "Pack" : NULL, pack_st, s_pack_has_sources,
                    0.0F, h, pack_lbl)) {
        s_pending_pack = true;
    }
    if (ui_icon_btn(ctx, s_id_btn_export, &s_ic_download, 16.0F, labels ? "Export" : NULL, &g_btn,
                    s_pack_has_sources, 0.0F, h, &g_body)) {
        s_export_open = true;
    }
    if (ui_icon_btn(ctx, s_id_btn_refresh, &s_ic_refresh, 16.0F, NULL, &g_btn_ghost, true, 0.0F, h, &g_caption)) {
        s_pending_refresh = true;
    }
}

static void strip_group_pages(nt_ui_context_t *ctx, int pc, int cur, float h) {
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/pg_prev"), &s_ic_chevron_left, 12.0F, NULL, &g_btn_ghost, cur > 0,
                    0.0F, h, &g_caption)) {
        gui_canvas_set_page(&s_canvas, cur - 1);
    }
    char pl[24];
    (void)snprintf(pl, sizeof pl, "%d/%d", cur + 1, pc);
    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), pl, &g_caption);
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/pg_next"), &s_ic_chevron_right, 12.0F, NULL, &g_btn_ghost,
                    cur < pc - 1, 0.0F, h, &g_caption)) {
        gui_canvas_set_page(&s_canvas, cur + 1);
    }
}

static void strip_group_zoom(nt_ui_context_t *ctx, float h, bool scan) {
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/zoom_out"), &s_ic_minus, 16.0F, NULL, &g_btn_ghost, true, 0.0F, h, &g_caption)) {
        gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, gui_canvas_zoom_pct(&s_canvas) * 0.8F);
    }
    char zl[16];
    (void)snprintf(zl, sizeof zl, "%.0f%%", (double)gui_canvas_zoom_pct(&s_canvas));
    /* scan icon wraps the % readout (§3): one button = zoom-to-100% + the current zoom. The two-row
     * compact fallback drops the icon (scan == false) to keep its min-content within MIN_CANVAS_W. */
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/zoom_100"), scan ? &s_ic_scan : NULL, 16.0F, zl, &g_btn_ghost, true, 0.0F, h, &g_caption)) {
        gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, 100.0F);
    }
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/zoom_in"), &s_ic_plus, 16.0F, NULL, &g_btn_ghost, true, 0.0F, h, &g_caption)) {
        gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, gui_canvas_zoom_pct(&s_canvas) * 1.25F);
    }
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/zoom_fit"), &s_ic_maximize, 16.0F, NULL, &g_btn_ghost, true, 0.0F, h, &g_caption)) {
        gui_canvas_fit(&s_canvas);
    }
}

/* Canvas action strip (E'): [Pack][Export][Refresh] | pages | zoom | stale chip. Semi-transparent bar with
 * a bottom rule (§2.2) at the TOP of the canvas. Single row that DROPS LABELS as it narrows (§4): Pack/
 * Export icon+label >= LABELS, icon-only below; the stale chip shows only >= CHIP so a trailing chip can
 * never push the row past the canvas. When even the icon-only single row can't fit (s_canvas_w < SINGLE)
 * it falls to the overflow-safe two-row compact rather than wrapping a control. Every control is also in
 * the menus (§3.3e); every icon-only button has a tooltip. */
static void declare_status_pill(nt_ui_context_t *ctx); /* floating message pill, defined below (canvas child) */

static void declare_canvas_strip(nt_ui_context_t *ctx, bool atlas) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    s_pack_has_sources = a && a->source_count > 0;
    s_pack_stale = gui_project_is_stale();
    const bool accent = s_pack_has_sources && s_pack_stale;
    const int pc = gui_canvas_page_count(&s_canvas);
    const int cur = gui_canvas_cur_page(&s_canvas);
    const Clay_Color strip_bg = {30.0F, 34.0F, 42.0F, 205.0F}; /* semi-transparent */
    const bool compact = s_canvas_w < S(STRIP_SINGLE_MIN_W);
    const bool labels = s_canvas_w >= S(STRIP_LABELS_MIN_W);

    if (compact) {
        /* Two rows: [Pack Export Refresh] over [pages | zoom], all icon-only. The canvas reservation
         * (MIN_CANVAS_W) is >= this layout's min-content, so neither row forces the column past the window.
         * The amber Pack + alert-triangle carries the stale state; the wide chip is dropped. */
        CLAY({.id = {.id = s_id_strip},
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(62))},
                         .padding = {Su(8), Su(8), Su(4), Su(4)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(4)},
              .backgroundColor = strip_bg,
              .cornerRadius = CLAY_CORNER_RADIUS(S(6)),
              .border = {.color = C_BORDER, .width = {0, 0, 0, Su(1), 0}}}) {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(26))}, .childGap = Su(4), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                strip_group_actions(ctx, accent, false, 26.0F);
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
            }
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(24))}, .childGap = Su(4), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (atlas && pc > 1) {
                    strip_group_pages(ctx, pc, cur, 24.0F);
                }
                if (atlas) {
                    strip_group_zoom(ctx, 24.0F, false); /* compact: plain % (no scan icon) keeps min-content small */
                }
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
            }
        }
        return;
    }

    CLAY({.id = {.id = s_id_strip},
          .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(34))},
                     .padding = {Su(8), Su(8), Su(4), Su(4)},
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = strip_bg,
          .cornerRadius = CLAY_CORNER_RADIUS(S(6)),
          .border = {.color = C_BORDER, .width = {0, 0, 0, Su(1), 0}}}) {
        strip_group_actions(ctx, accent, labels, 26.0F);
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(1)), CLAY_SIZING_FIXED(S(20))}}, .backgroundColor = C_BORDER}) {}
        if (atlas && pc > 1) {
            strip_group_pages(ctx, pc, cur, 26.0F);
        }
        if (atlas) {
            strip_group_zoom(ctx, 26.0F, true);
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        /* Clickable stale chip -> Pack (§2.9; owner rule: actionable hints are buttons). Gated on a width
         * stop so the trailing chip never pushes the row off the canvas; below it the amber Pack carries it. */
        if (accent && s_canvas_w >= S(STRIP_CHIP_MIN_W)) {
            if (ui_icon_btn(ctx, nt_ui_id("ntpacker/stale_chip"), &s_ic_triangle_alert, 14.0F, "outdated",
                            &g_btn_stale, true, 0.0F, 24.0F, &g_onwarn)) {
                s_pending_pack = true;
            }
        }
    }
}

/* Animation preview player in the canvas area (ux.md §3.7b): a control strip (play/pause, frame step,
 * "cur/total", Close) over the ANIM-mode custom element, or a "Pack to preview" hint without a result. */
static void declare_canvas_preview(nt_ui_context_t *ctx) {
    const tp_project_anim *an = current_anim();
    const bool have = (an != NULL && gui_pack_result(s_sel_atlas) != NULL && s_preview_frame_count > 0);
    const float cap_w = s_content_w - s_left_panel_w - s_right_panel_w - S(70.0F);
    char caption[192];
    if (an) {
        const char *pb = (an->playback >= 0 && an->playback < 7) ? k_playback_names[an->playback] : "?";
        (void)snprintf(caption, sizeof caption, "%s  \xC2\xB7  %s  \xC2\xB7  %g fps%s%s", an->id, pb, (double)an->fps,
                       an->flip_h ? "  \xC2\xB7  flip H" : "", an->flip_v ? "  \xC2\xB7  flip V" : "");
    } else {
        (void)snprintf(caption, sizeof caption, "No animation");
    }

    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                     .padding = {Su(6), Su(6), Su(6), Su(6)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_CANVAS,
          /* Docked (pass 2): square corners; keep the 1px border for canvas-vs-panel contrast. */
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        const Clay_Color strip_bg = {30.0F, 34.0F, 42.0F, 205.0F};
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(34))},
                         .padding = {Su(8), Su(8), Su(4), Su(4)},
                         .childGap = Su(6),
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = strip_bg,
              .cornerRadius = CLAY_CORNER_RADIUS(S(6))}) {
            if (ui_btn(ctx, nt_ui_id("prev/play"), s_preview_playing ? "\xE2\x9D\x9A\xE2\x9D\x9A" : "\xE2\x96\xB6",
                       &g_btn, have, 40.0F, 26.0F, &g_body)) { /* U+275A pause / U+25B6 play */
                preview_toggle_play();
            }
            if (ui_btn(ctx, nt_ui_id("prev/back"), "\xE2\x97\x80", &g_btn_ghost, have, 30.0F, 24.0F, &g_caption)) {
                preview_step(-1);
            }
            char fnum[24];
            (void)snprintf(fnum, sizeof fnum, "%d/%d", have ? (s_preview_cur + 1) : 0, s_preview_frame_count);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), fnum, &g_caption);
            if (ui_btn(ctx, nt_ui_id("prev/fwd"), "\xE2\x96\xB6", &g_btn_ghost, have, 30.0F, 24.0F, &g_caption)) {
                preview_step(+1);
            }
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
            if (ui_btn(ctx, nt_ui_id("prev/close"), "Close", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
                preview_stop();
            }
        }
        CLAY({.id = {.id = s_id_canvas},
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(8),
                         .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
              .clip = {.horizontal = true, .vertical = true}}) {
            if (have) {
                nt_ui_custom(ctx, NT_UI_DATA_LAYER(LAYER_IMG), &s_canvas);
            } else if (an && an->frame_count == 0) {
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "This animation has no frames yet.", &g_canvas_hint);
            } else if (an && gui_pack_result(s_sel_atlas) == NULL) {
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Pack to preview \xE2\x80\x94 press Pack (Ctrl+P).", &g_canvas_hint);
            } else {
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No frames resolve to packed regions \xE2\x80\x94 repack (Ctrl+P).", &g_canvas_hint);
            }
            declare_status_pill(ctx); /* floating message pill (bottom-left of the canvas) */
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(22))}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
            ui_label_fit(ctx, caption, &g_caption, cap_w, 0U);
        }
    }
}

static void declare_canvas(nt_ui_context_t *ctx) {
    if (s_preview_active) {
        declare_canvas_preview(ctx);
        return;
    }
    const bool atlas = gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_ATLAS && gui_canvas_has_atlas(&s_canvas);
    const bool has_img = gui_canvas_has_image(&s_canvas);
    const float cap_w = s_content_w - s_left_panel_w - s_right_panel_w - S(70.0F);

    /* caption / hover readout */
    char label[256];
    if (atlas) {
        const tp_result *r = gui_pack_result(s_sel_atlas);
        const int h = (s_canvas.hover_sprite >= 0) ? s_canvas.hover_sprite : gui_canvas_selected(&s_canvas);
        if (r && h >= 0 && h < r->sprite_count) {
            const tp_sprite *s = &r->sprites[h];
            char geom[24];
            if (s->vert_count > 4) {
                (void)snprintf(geom, sizeof geom, "%d verts", s->vert_count);
            } else {
                (void)snprintf(geom, sizeof geom, "rect");
            }
            (void)snprintf(label, sizeof label, "%s  \xC2\xB7  %dx%d  \xC2\xB7  %s  \xC2\xB7  %s", s->name, s->frame.w,
                           s->frame.h, transform_decode_str(s->transform), geom);
        } else if (r) {
            /* E' stats line: N sprites, P pages, WxH (current page), F% filled, total verts, packed M ms. */
            const int pg = gui_canvas_cur_page(&s_canvas);
            const int pw = (pg >= 0 && pg < r->page_count) ? r->pages[pg].w : 0;
            const int ph = (pg >= 0 && pg < r->page_count) ? r->pages[pg].h : 0;
            int tv = 0;
            for (int i = 0; i < r->sprite_count; i++) {
                tv += r->sprites[i].vert_count;
            }
            const char *sep = "  \xC2\xB7  "; /* U+00B7 middle dot (now baked) */
            if (s_last_pack_atlas == s_sel_atlas && s_last_pack_ms > 0.0) {
                (void)snprintf(label, sizeof label,
                               "%d sprites%s%d pages%s%dx%d%s%.0f%% filled%s%d verts%spacked %.0f ms", r->sprite_count,
                               sep, r->page_count, sep, pw, ph, sep, (double)atlas_fill_pct(r), sep, tv, sep,
                               s_last_pack_ms);
            } else {
                (void)snprintf(label, sizeof label, "%d sprites%s%d pages%s%dx%d%s%.0f%% filled%s%d verts",
                               r->sprite_count, sep, r->page_count, sep, pw, ph, sep, (double)atlas_fill_pct(r), sep,
                               tv);
            }
        } else {
            (void)snprintf(label, sizeof label, "No atlas");
        }
    } else if (has_img) {
        (void)snprintf(label, sizeof label, "%s  --  %d x %d", path_last(s_sel_abs), gui_canvas_img_w(&s_canvas),
                       gui_canvas_img_h(&s_canvas));
    } else if (s_sel_missing) {
        (void)snprintf(label, sizeof label, "file missing: %s", s_sel_abs);
    } else {
        (void)snprintf(label, sizeof label, "No image selected");
    }

    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                     .padding = {Su(6), Su(6), Su(6), Su(6)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_CANVAS,
          /* Docked (pass 2): square corners like the panels; keep the 1px border -- it is the one place a
           * border earns its keep (the dark canvas well vs the mid panels). */
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        declare_canvas_strip(ctx, atlas); /* action strip at the top of the canvas */
        CLAY({.id = {.id = s_id_canvas},
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(8),
                         .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
              /* clip so the custom handler's page quad + overlay lines are GL-scissored to this box
               * (the walker keeps the scissor active through the CUSTOM command) -- the atlas can be
               * panned/zoomed half-off every edge without bleeding onto neighboring panels. */
              .clip = {.horizontal = true, .vertical = true}}) {
            if (atlas || has_img) {
                nt_ui_custom(ctx, NT_UI_DATA_LAYER(LAYER_IMG), &s_canvas);
            } else if (s_sel_missing) {
                ui_label_fit(ctx, label, &g_warn, cap_w, 0U);
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Restore the file and press Refresh (F5) to bring it back.", &g_caption);
            } else {
                const tp_project_atlas *ea = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
                const bool no_sources = (ea == NULL || ea->source_count == 0);
                if (no_sources) {
                    /* Empty state (§2.7): hero folder-plus + "Add a folder to start" + a PRIMARY Add-folder
                     * button wired to the SAME pending action as the +Folder button (no duplicated logic). */
                    CLAY({.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                     .childGap = Su(12),
                                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}}}) {
                        nt_ui_image_style_t hero = nt_ui_image_style_defaults();
                        hero.color_packed = label_tint(&g_canvas_hint); /* text-faint (region baked at 96px) */
                        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(48.0F)), CLAY_SIZING_FIXED(S(48.0F))}}}) {
                            nt_ui_image(ctx, NT_UI_DATA_LAYER(LAYER_IMG), &s_ic_folder_plus_hero, &hero, NULL);
                        }
                        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Add a smart folder to start", &g_canvas_hint);
                        if (ui_icon_btn(ctx, nt_ui_id("ntpacker/empty_add_folder"), &s_ic_folder_plus, 16.0F,
                                        "Add smart folder", &g_btn_primary, true, 0.0F, 28.0F, &g_onaccent)) {
                            s_pending_add_folder = true;
                        }
                    }
                } else {
                    /* Sources present but not packed yet: keep the "press Pack" hint. Bounded, centered
                     * column so long hints WRAP within the canvas instead of clipping both edges. */
                    const float hint_w = fmaxf(S(80.0F), fminf(cap_w, S(460.0F)));
                    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(hint_w), CLAY_SIZING_FIT(0)},
                                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                     .childGap = Su(6),
                                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}}}) {
                        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No atlas preview yet -- press Pack (Ctrl+P) to build it.", &g_canvas_hint);
                        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Or select a sprite on the left to preview its source image.", &g_canvas_hint);
                    }
                }
            }
            /* Stale visuals (§2.9): a floating overlay OVER the custom-drawn page -- ~12% dim + a corner
             * amber "outdated" tag. Floating -> higher zIndex -> the walker draws it in a later segment,
             * above the CUSTOM page (zIndex 0); PASSTHROUGH keeps canvas pan/zoom/right-click live (canvas
             * input reads the raw pointer vs last_bb, not Clay hover). Pure draw (no widget state). */
            if (atlas && s_pack_stale) {
                nt_ui_label_style_t tag_lbl = g_tag; /* tag size, dark-on-amber like the strip chip */
                tag_lbl.color = g_onwarn.color;
                nt_ui_image_style_t tagi = nt_ui_image_style_defaults();
                tagi.color_packed = label_tint(&tag_lbl);
                CLAY({.id = {.id = nt_ui_id("ntpacker/stale_overlay")},
                      .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                 .padding = {Su(8), Su(8), Su(8), Su(8)},
                                 .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
                      .backgroundColor = C_STALE_DIM,
                      .floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                                   .zIndex = 8,
                                   .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH}}) {
                    CLAY({.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                     .padding = {Su(6), Su(6), Su(3), Su(3)},
                                     .childGap = Su(4),
                                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
                          .backgroundColor = C_WARN,
                          .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
                        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(12.0F)), CLAY_SIZING_FIXED(S(12.0F))}}}) {
                            nt_ui_image(ctx, NT_UI_DATA_LAYER(LAYER_IMG), &s_ic_triangle_alert, &tagi, NULL);
                        }
                        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "outdated", &tag_lbl);
                    }
                }
            }
            declare_status_pill(ctx); /* floating message pill (bottom-left of the canvas) */
        }
        /* stats/readout line; dimmed when stale (it describes the LAST pack, not current settings) */
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(22))}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
            ui_label_fit(ctx, label, (atlas && s_pack_stale) ? &g_dim : &g_caption, cap_w, 0U);
        }
    }
    /* canvas right-click: overlay toggles + zoom (mouse-complete access, §3.3e) */
    if (atlas) {
        if (nt_ui_menu_open_trigger(ctx, s_id_ctx_menu, s_id_canvas, false, &s_ctx_state)) {
            close_menubar_menus();
            s_ctx_kind = CTX_CANVAS;
        }
    }
}
// #endregion

// #region status bar + menus + tooltips
/* Severity language (§2.8): leading icon + text tint. Info = text-dim (g_caption), success green,
 * warning amber, error red. One baked white icon tinted to the tier -> icon + text speak one color. */
static Clay_Color status_sev_color(status_sev_t sev) {
    switch (sev) {
    case STATUS_SUCCESS:
        return C_SUCCESS;
    case STATUS_WARNING:
        return C_WARN;
    case STATUS_ERROR:
        return C_DANGER;
    case STATUS_INFO:
    default:
        return g_caption.color; /* text-dim */
    }
}
static nt_atlas_region_ref_t *status_sev_icon(status_sev_t sev) {
    switch (sev) {
    case STATUS_SUCCESS:
        return &s_ic_circle_check;
    case STATUS_WARNING:
        return &s_ic_triangle_alert;
    case STATUS_ERROR:
        return &s_ic_octagon_alert;
    case STATUS_INFO:
    default:
        return &s_ic_info;
    }
}
/* The single-message feedback surface (owner 2026-07-11 pass 2): a compact pill FLOATING over the
 * bottom-left of the canvas, replacing the permanent status-bar row. A new message replaces the old
 * (set_status* clears the dismiss bit); the pill exists only while there is a message and it has not been
 * clicked away. No timers -- errors/warnings and success/info alike persist until replaced or dismissed
 * (render-pure; immediate mode). Keeps declare_statusbar's severity language (icon + text tint). ux.md
 * region H (the future notices PANEL) is not built here -- this is the interim single-message surface.
 * Declared as a child of the canvas clip box (s_id_canvas); floating -> escapes the clip and does not
 * disturb sibling layout. */
static void declare_status_pill(nt_ui_context_t *ctx) {
    if (s_status[0] == '\0' || s_status_dismissed) {
        return;
    }
    nt_ui_label_style_t st = g_caption; /* caption size; recolor per severity (already scaled this frame) */
    st.color = status_sev_color(s_status_sev);
    nt_ui_image_style_t sicon = nt_ui_image_style_defaults();
    sicon.color_packed = label_tint(&st);
    const uint32_t x_id = nt_ui_child_id(s_id_status_pill, "x");
    /* Cap the text so a long message can never grow the pill past the canvas right edge. */
    const float max_txt = fmaxf(s_canvas_w - S(96.0F), S(80.0F));
    CLAY({.id = {.id = s_id_status_pill},
          .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                     .padding = {Su(10), Su(4), Su(5), Su(5)},
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_STATUS,
          .cornerRadius = CLAY_CORNER_RADIUS(S(6)),
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}},
          .floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM},
                       .offset = {S(8.0F), -S(8.0F)},
                       .zIndex = 12}}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(14.0F)), CLAY_SIZING_FIXED(S(14.0F))}}}) {
            nt_ui_image(ctx, NT_UI_DATA_LAYER(LAYER_IMG), status_sev_icon(s_status_sev), &sicon, NULL);
        }
        ui_label_fit(ctx, s_status, &st, max_txt, 0U); /* clip, never wrap/overflow */
        record_row_tip(x_id, "Dismiss");
        if (ui_icon_btn(ctx, x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 22.0F, 20.0F, &g_caption)) {
            s_status_dismissed = true;
        }
    }
}

static void declare_menus(nt_ui_context_t *ctx) {
    nt_ui_menu_begin(&s_file_menu, ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_menu_file, &s_file_state, &s_menu_style);
    file_items(&s_file_menu);
    nt_ui_menu_end(&s_file_menu);
    nt_ui_menu_begin(&s_edit_menu, ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_menu_edit, &s_edit_state, &s_menu_style);
    edit_items(&s_edit_menu);
    nt_ui_menu_end(&s_edit_menu);
    nt_ui_menu_begin(&s_view_menu, ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_menu_view, &s_view_state, &s_menu_style);
    view_items(&s_view_menu);
    nt_ui_menu_end(&s_view_menu);
    nt_ui_menu_begin(&s_help_menu, ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_menu_help, &s_help_state, &s_menu_style);
    help_items(&s_help_menu);
    nt_ui_menu_end(&s_help_menu);
}

/* Row right-click menu: same code paths as the [x] buttons / inline editors (§3.3e). Declared
 * every frame (open or not); items no-op while closed. */
static void declare_context_menu(nt_ui_context_t *ctx) {
    nt_ui_menu_begin(&s_ctx_menu, ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_ctx_menu, &s_ctx_state, &s_menu_style);
    if (s_ctx_kind == CTX_ATLAS) {
        if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_RENAME, "Rename")) {
            start_atlas_edit(s_ctx_atlas);
        }
        const tp_project *cp = gui_project_get();
        nt_ui_menu_item_opts_t rm = {.disabled = (cp == NULL || cp->atlas_count <= 1)};
        if (nt_ui_menu_item_ex(&s_ctx_menu, MK_CTX_REMOVE, "Remove", rm)) {
            s_pending_remove_atlas = s_ctx_atlas;
        }
    } else if (s_ctx_kind == CTX_SPRITE) {
        if (s_ctx_leaf) {
            if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_RENAME, "Rename")) {
                start_sprite_edit_named(s_ctx_sprite);
            }
        }
        if (s_multi_sel_count > 0) {
            char lbl[48];
            (void)snprintf(lbl, sizeof lbl, "Create animation from selection (%d)", s_multi_sel_count);
            if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_CREATE_ANIM, lbl)) {
                s_pending_create_anim = true;
            }
        }
        if (s_ctx_removable) {
            if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_REMOVE, "Remove")) {
                s_pending_remove_source = s_ctx_src;
            }
        }
    } else if (s_ctx_kind == CTX_ANIM) {
        if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_PREVIEW, "Preview")) {
            s_sel_anim = s_ctx_anim;
            s_pending_open_preview = true;
        }
        if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_RENAME, "Rename")) {
            start_anim_edit(s_ctx_anim);
        }
        if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_REMOVE, "Remove")) {
            s_pending_remove_anim = s_ctx_anim;
        }
    } else if (s_ctx_kind == CTX_TARGET) {
        tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
        if (a && s_ctx_target >= 0 && s_ctx_target < a->target_count) {
            tp_project_target *t = &a->targets[s_ctx_target];
            if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_TOGGLE, t->enabled ? "Disable" : "Enable")) {
                gui_project_set_target(s_sel_atlas, s_ctx_target, t->exporter_id, t->out_path, !t->enabled);
            }
            if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_REMOVE, "Remove")) {
                s_pending_remove_target = s_ctx_target;
            }
        }
    } else if (s_ctx_kind == CTX_CANVAS) {
        overlay_item(&s_ctx_menu, MK_OV_OUTLINE, "Region outlines (hull)", &s_canvas.show_outline);
        overlay_item(&s_ctx_menu, MK_OV_FRAME, "Frame rects", &s_canvas.show_frame);
        overlay_item(&s_ctx_menu, MK_OV_TRIM, "Trim bounds", &s_canvas.show_trim);
        overlay_item(&s_ctx_menu, MK_OV_PIVOT, "Pivots", &s_canvas.show_pivot);
        nt_ui_menu_separator(&s_ctx_menu);
        if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_100, "Zoom 100%")) {
            gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, 100.0F);
        }
        if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_FIT, "Fit")) {
            gui_canvas_fit(&s_canvas);
        }
    }
    nt_ui_menu_end(&s_ctx_menu);
}

static void declare_tooltips(nt_ui_context_t *ctx) {
    const char *pack_tip = s_pack_stale
        ? "Pack (Ctrl+P): sources or settings changed -- press to repack now and refresh the atlas preview (session only, no files exported)."
        : "Pack (Ctrl+P): atlas is up to date. Repacks with current settings and refreshes the preview (session only, no files exported).";
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_btn_pack, pack_tip, &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_btn_export,
                        "Export All (Ctrl+E): for every enabled target, pack (project settings INTERSECT the target's capabilities) and write its files to the target's output path. Repacks stale atlases first.",
                        &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_btn_refresh,
                        "Refresh (F5): rescan all source folders/files from disk; updates the sprite list and marks the preview stale.",
                        &s_tip_style);
    /* Smart-folder + Files add buttons (make the live-linked-folder behaviour explicit -- owner 2026-07-11). */
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/add_folder"),
                        "Add a smart folder: every image inside it is packed -- including files added later. Press F5 to rescan.",
                        &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/add_files"),
                        "Add individual image files (the exception -- prefer a smart folder so new art is picked up automatically).",
                        &s_tip_style);
    /* Icon-only strip ghosts (mouse-complete). A tooltip for a control not laid out this frame (single
     * page, non-atlas mode) is a safe no-op -- it only opens on hover of an existing target. */
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/pg_prev"), "Previous page", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/pg_next"), "Next page", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/zoom_out"), "Zoom out", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/zoom_100"), "Zoom to 100% (actual size)", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/zoom_in"), "Zoom in", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/zoom_fit"), "Fit atlas to view", &s_tip_style);
}

/* Export dialog (mouse-complete): every atlas's targets, toggle/browse per target, then Export runs the
 * same do_export path. All edits go through gui_project_set_target (dirty + undo, no parallel state). */
static void declare_export_modal(nt_ui_context_t *ctx) {
    if (!nt_ui_modal_visible(ctx, s_id_export_modal, &s_modal_style, &s_export_open)) {
        return;
    }
    tp_project *p = gui_project_get();
    int enabled_targets = 0;
    int atlases_with = 0;
    int total_targets = 0;
    int line_count = 0;
    for (int ai = 0; p && ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        if (a->target_count > 0) {
            atlases_with++;
            line_count += 1 + a->target_count;
        }
        for (int ti = 0; ti < a->target_count; ti++) {
            total_targets++;
            enabled_targets += a->targets[ti].enabled ? 1 : 0;
        }
    }
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(560)), CLAY_SIZING_FIT(0)},
                     .padding = {Su(22), Su(22), Su(20), Su(20)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(14)},
          .backgroundColor = C_PANEL,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Export", &g_body);
        if (total_targets == 0) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "This project has no export targets yet.", &g_caption);
            CLAY({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = Su(12), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (ui_btn(ctx, nt_ui_id("export/goto"), "Add targets in the panel", &g_btn_primary, true, 0.0F, 34.0F, &g_onaccent)) {
                    s_export_open = false;
                    s_sec_export_open = true;
                }
                if (ui_btn(ctx, nt_ui_id("export/close0"), "Close", &g_btn, true, 100.0F, 34.0F, &g_body)) {
                    s_export_open = false;
                }
            }
            nt_ui_modal_end(ctx);
            return;
        }
        const int ne = tp_exporter_count();
        float list_h = (float)line_count * S(30.0F) + S(6.0F);
        const float list_cap = S(330.0F);
        if (list_h > list_cap) {
            list_h = list_cap;
        }
        nt_ui_scroll_begin(ctx, NULL, nt_ui_id("export/scroll"), &s_panel_scroll,
                           &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(list_h)}}});
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = Su(4), .padding = {0, Su(8), 0, 0}}}) {
            for (int ai = 0; ai < p->atlas_count; ai++) {
                tp_project_atlas *a = &p->atlases[ai];
                if (a->target_count == 0) {
                    continue;
                }
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), a->name, &g_row);
                for (int ti = 0; ti < a->target_count && ti < GUI_MAX_TARGETS; ti++) {
                    tp_project_target *t = &a->targets[ti];
                    char idb[48];
                    (void)snprintf(idb, sizeof idb, "export/a%d_t%d", ai, ti);
                    const uint32_t rid = nt_ui_id(idb);
                    const char *exp_name = t->exporter_id;
                    for (int i = 0; i < ne; i++) {
                        const tp_exporter *e = tp_exporter_at(i);
                        if (e && strcmp(e->id, t->exporter_id) == 0) {
                            exp_name = (e->display_name && e->display_name[0]) ? e->display_name : e->id;
                            break;
                        }
                    }
                    const bool has_path = (t->out_path && t->out_path[0] != '\0');
                    const nt_ui_events_t pev = nt_ui_events(ctx, nt_ui_child_id(rid, "path"), NULL);
                    if (pev.clicked) {
                        s_pending_export_browse_atlas = ai;
                        s_pending_export_browse_target = ti;
                    }
                    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                                     .padding = {Su(8), Su(6), 0, 0},
                                     .childGap = Su(8),
                                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                        if (tp_checkbox(ctx, nt_ui_child_id(rid, "en"), t->enabled, true)) {
                            gui_project_set_target(ai, ti, t->exporter_id, t->out_path, !t->enabled);
                        }
                        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(96)), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                            ui_label_fit(ctx, exp_name, &g_caption, S(96), 0U);
                        }
                        CLAY({.id = {.id = nt_ui_child_id(rid, "path")},
                              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = {Su(6), Su(6), 0, 0}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
                              .backgroundColor = pev.hovered ? C_HOVER : C_BG,
                              .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
                            ui_label_fit(ctx, has_path ? t->out_path : "(click to set output path)", has_path ? &g_row : &g_dim, S(300), 0U);
                        }
                        if (ui_btn(ctx, nt_ui_child_id(rid, "br"), "\xE2\x80\xA6", &g_btn_ghost, true, 28.0F, 22.0F, &g_caption)) {
                            s_pending_export_browse_atlas = ai;
                            s_pending_export_browse_target = ti;
                        }
                    }
                }
            }
        }
        nt_ui_scroll_end(ctx);
        char summary[96];
        (void)snprintf(summary, sizeof summary, "%d target%s enabled across %d atlas%s", enabled_targets, enabled_targets == 1 ? "" : "s", atlases_with,
                       atlases_with == 1 ? "" : "es");
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), summary, &g_caption);
        CLAY({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = Su(12), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
            if (ui_btn(ctx, nt_ui_id("export/run"), gui_pack_async_busy() ? "Exporting\xE2\x80\xA6" : "Export", &g_btn_primary,
                       enabled_targets > 0 && !gui_pack_async_busy(), 120.0F, 34.0F, &g_onaccent)) {
                s_pending_export = true;
                s_export_open = false;
            }
            if (ui_btn(ctx, nt_ui_id("export/cancel"), "Cancel", &g_btn, true, 100.0F, 34.0F, &g_body)) {
                s_export_open = false;
            }
        }
    }
    nt_ui_modal_end(ctx);
}

static void declare_confirm_modal(nt_ui_context_t *ctx) {
    if (nt_ui_modal_visible(ctx, s_id_modal, &s_modal_style, &s_confirm_open)) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(460)), CLAY_SIZING_FIT(0)},
                         .padding = {Su(22), Su(22), Su(22), Su(22)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(16)},
              .backgroundColor = C_PANEL,
              .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
              .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Unsaved changes", &g_body);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Save changes before continuing?", &g_caption);
            CLAY({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = Su(12), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (ui_btn(ctx, nt_ui_id("ntpacker/modal_save"), "Save", &g_btn_primary, true, 100.0F, 34.0F, &g_onaccent)) {
                    s_modal_action = MODAL_SAVE;
                }
                if (ui_btn(ctx, nt_ui_id("ntpacker/modal_discard"), "Discard", &g_btn, true, 100.0F, 34.0F, &g_body)) {
                    s_modal_action = MODAL_DISCARD;
                }
                if (ui_btn(ctx, nt_ui_id("ntpacker/modal_cancel"), "Cancel", &g_btn, true, 100.0F, 34.0F, &g_body)) {
                    s_modal_action = MODAL_CANCEL;
                }
            }
        }
        nt_ui_modal_end(ctx);
    }
}

static void declare_about_modal(nt_ui_context_t *ctx) {
    if (nt_ui_modal_visible(ctx, s_id_about, &s_modal_style, &s_about_open)) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(460)), CLAY_SIZING_FIT(0)},
                         .padding = {Su(24), Su(24), Su(22), Su(22)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(10)},
              .backgroundColor = C_PANEL,
              .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
              .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "ntpacker-gui", &g_body);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Neotolis Texture Packer", &g_caption);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Version " NTPACKER_VERSION, &g_caption);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Engine: " NTPACKER_ENGINE_NAME, &g_caption);
            /* Clickable repo link: looks like a hyperlink (link-blue + hover tint), opens the browser. */
            if (ui_btn(ctx, nt_ui_id("ntpacker/about_link"), NTPACKER_REPO_URL, &g_btn_link, true, 0.0F, 24.0F, &g_link)) {
                if (gui_open_url(NTPACKER_REPO_URL)) {
                    set_statusf("Opening %s", NTPACKER_REPO_URL);
                } else {
                    set_status_ex(STATUS_WARNING, "Could not open browser -- " NTPACKER_REPO_URL);
                }
            }
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(4))}}}) {}
            CLAY({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (ui_btn(ctx, nt_ui_id("ntpacker/about_ok"), "OK", &g_btn_primary, true, 100.0F, 34.0F, &g_onaccent)) {
                    s_about_open = false;
                }
            }
        }
        nt_ui_modal_end(ctx);
    }
}
// #endregion

/* The right settings panel (regions F/G + per-region packing overrides) moved to
 * gui_view_settings.c/h (step 4) as a pure move -- declare_right_panel is called from frame() below;
 * the header exposes only that entry point. */

// #region keyboard shortcuts (ux.md §3.3d)
/* Global shortcuts routed through the SAME actions as the menus. Text-input focus swallows
 * them first (no accidental global actions while typing); an open modal blocks them too. */
static void handle_shortcuts(void) {
    if (nt_ui_input_any_focused(s_ctx) || s_confirm_open || s_about_open || s_export_open) {
        return;
    }
    /* Preview + editor accelerators (each also a button; §3.3e). */
    if (s_preview_active && nt_input_key_is_pressed(NT_KEY_SPACE)) {
        preview_toggle_play();
    }
    if (s_edit_kind == EDIT_NONE && s_sel_anim >= 0 && s_sel_anim_frame >= 0 &&
        nt_input_key_is_pressed(NT_KEY_DELETE)) {
        gui_project_anim_remove_frame(s_sel_atlas, s_sel_anim, s_sel_anim_frame);
        s_sel_anim_frame = -1;
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

    if (nt_input_key_is_pressed(NT_KEY_ESCAPE)) {
        if (s_edit_kind != EDIT_NONE) {
            cancel_edit();
            set_status("Rename cancelled.");
        } else if (s_export_open) {
            s_export_open = false;
        } else if (s_about_open) {
            s_about_open = false;
        } else if (s_confirm_open) {
            s_confirm_open = false;
            s_after_confirm = AFTER_NONE;
        } else if (s_preview_active && !s_ctx_state.open) {
            preview_stop();
            set_status("Closed animation preview.");
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

    nt_resource_step();
    nt_material_step();
    try_bind_resources();

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
        nt_resource_invalidate(NT_ASSET_SHADER_CODE);
        nt_resource_invalidate(NT_ASSET_TEXTURE);
        nt_resource_invalidate(NT_ASSET_FONT);
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
        s_atlas_bound = false;
        s_font_bound = false;
    }

    nt_gfx_begin_pass(&(nt_pass_desc_t){
        .clear_color = {C_BG.r / 255.0F, C_BG.g / 255.0F, C_BG.b / 255.0F, 1.0F},
        .clear_depth = 1.0F,
    });

    nt_font_step();

    const nt_material_info_t *sprite_info = nt_material_get_info(s_sprite_material);
    const nt_material_info_t *text_info = nt_material_get_info(s_text_material);
    const bool can_render = s_atlas_bound && s_font_bound && sprite_info && sprite_info->ready && text_info && text_info->ready;

    if (can_render) {
        nt_gfx_update_buffer(s_frame_ubo, &uniforms, sizeof(uniforms));
        nt_gfx_bind_uniform_buffer(s_frame_ubo, 0);

        ensure_ids();
        apply_ui_scale();
        gui_canvas_ensure_pipeline(&s_canvas, sprite_info);
        gui_canvas_set_frame_ubo(&s_canvas, s_frame_ubo);
        gui_canvas_set_ui_scale(&s_canvas, g_ui_scale); /* overlay line widths scale with DPI */

        clamp_selection();
        /* keep the animation selection valid after undo/redo/atlas changes */
        {
            tp_project_atlas *sel_a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
            if (!sel_a || s_sel_anim >= sel_a->animation_count) {
                s_sel_anim = -1;
                s_sel_anim_frame = -1;
                if (s_preview_active) {
                    preview_stop();
                }
            }
        }
        build_rows(gui_project_get(), tp_project_get_atlas(gui_project_get(), s_sel_atlas));
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

        /* Bind the selected atlas's pack result to the canvas (repack / atlas switch / clear). */
        const tp_result *want = gui_pack_result(s_sel_atlas);
        if (want != s_shown_result) {
            gui_canvas_set_result(&s_canvas, want);
            s_shown_result = want;
        }
        update_preview();      /* resolve the current preview frame + set ANIM mode before input/handler */
        handle_canvas_input(); /* wheel/pan/click over the atlas page (uses last frame's draw box) */

        nt_ui_begin(s_ctx, scale.logical_w, scale.logical_h, g_nt_app.dt, &g_nt_input.pointers[0], 1);
        nt_ui_set_viewport(s_ctx, nt_ui_viewport_from_scale(&scale));

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

    nt_window_swap_buffers();
}
// #endregion

// #region main + init/shutdown
int main(int argc, char *argv[]) {
    nt_engine_config_t config = {0};
    config.app_name = "ntpacker-gui";
    config.version = 1;
    if (nt_engine_init(&config) != NT_OK) {
        return 1;
    }
    nt_log_info("ntpacker-gui: %s build (%s)", nt_engine_build_string(), nt_engine_preset_string());

    /* dev screenshot flags + optional project path (first non-flag arg; see gui_shot.c) */
    const char *proj_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (gui_shot_parse_arg(argv[i])) {
            /* consumed by the dev screenshot seam (--shot/--size/--scale/--shot-stale/--shot-packing) */
        } else if (strcmp(argv[i], "--auto-pack") == 0) {
            s_auto_pack = true; /* dev: headless async pack of atlas 0 for the heartbeat proof */
        } else if (proj_arg == NULL) {
            proj_arg = argv[i];
        }
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
    char ui_pack_path[1280];
    (void)snprintf(ui_pack_path, sizeof ui_pack_path, "%s/assets/ntpacker_ui.ntpack", s_exe_dir);
    s_pack_id = nt_hash32_str("ntpacker_ui");
    nt_resource_mount(s_pack_id, 100);
    nt_resource_load_auto(s_pack_id, ui_pack_path);

    s_sprite_vs_handle = nt_resource_request(ASSET_SHADER_ASSETS_SHADERS_SPRITE_VERT, NT_ASSET_SHADER_CODE);
    s_sprite_fs_handle = nt_resource_request(ASSET_SHADER_ASSETS_SHADERS_SPRITE_FRAG, NT_ASSET_SHADER_CODE);
    s_text_vs_handle = nt_resource_request(ASSET_SHADER_ASSETS_SHADERS_SLUG_TEXT_VERT, NT_ASSET_SHADER_CODE);
    s_text_fs_handle = nt_resource_request(ASSET_SHADER_ASSETS_SHADERS_SLUG_TEXT_FRAG, NT_ASSET_SHADER_CODE);
    s_atlas_handle = nt_resource_request(ASSET_ATLAS_NTPACKER_UI_ATLAS, NT_ASSET_ATLAS);
    s_atlas_tex_handle = nt_resource_request(ASSET_TEXTURE_NTPACKER_UI_ATLAS_TEX0, NT_ASSET_TEXTURE);
    s_font_resource = nt_resource_request(ASSET_FONT_NTPACKER_UI_FONT, NT_ASSET_FONT);

    s_sprite_material = nt_material_create(&(nt_material_create_desc_t){
        .vs = s_sprite_vs_handle,
        .fs = s_sprite_fs_handle,
        .textures = {{.name = "u_texture", .resource = s_atlas_tex_handle}},
        .texture_count = 1,
        .blend_mode = NT_BLEND_MODE_ALPHA,
        .depth_test = false,
        .depth_write = false,
        .cull_mode = NT_CULL_NONE,
        .label = "ntpacker_sprite",
    });
    s_text_material = nt_material_create(&(nt_material_create_desc_t){
        .vs = s_text_vs_handle,
        .fs = s_text_fs_handle,
        .blend_mode = NT_BLEND_MODE_ALPHA,
        .depth_test = false,
        .depth_write = false,
        .cull_mode = NT_CULL_NONE,
        .params[0] = {.name = "u_alpha_cutoff", .value = {NT_TEXT_ALPHA_CUTOFF_DEFAULT}},
        .param_count = 1,
        .label = "ntpacker_text",
    });

    nt_ui_set_sprite_material(s_ctx, s_sprite_material);
    nt_ui_set_text_material(s_ctx, s_text_material);

    s_font = nt_font_create(&(nt_font_create_desc_t){
        .curve_texture_width = 1024,
        .curve_texture_height = 512,
        .band_texture_height = 256,
        .band_count = 8,
        .measure_cache_size = 256,
    });

    nt_resource_set_activate_time_budget(0);

    g_ui_scale = detect_dpi_scale();
    nt_log_info("ntpacker-gui: UI scale %.2f (from system DPI)", (double)g_ui_scale);
    gui_shot_apply_scale(); /* screenshot mode (--scale): pin g_ui_scale for reproducible captures */

    /* editor state + the canvas custom-draw handler (registered outside begin/end) */
    gui_canvas_init(&s_canvas);
    nt_ui_set_custom_handler(s_ctx, gui_canvas_handler, &s_canvas);
    gui_project_init();

    /* in-process packing: session .ntpack goes under the exe dir (existing convention) */
    char pack_session[1152];
    (void)snprintf(pack_session, sizeof pack_session, "%s/pack_session", s_exe_dir);
    gui_pack_init(pack_session);

    /* open a project passed on the command line (errors go to the status bar) */
    if (proj_arg != NULL) {
        char err[256];
        if (!gui_scan_exists(proj_arg)) {
            set_statusf_ex(STATUS_WARNING, "project not found: %s", proj_arg); /* stale argv -> continue with untitled (F6b) */
        } else if (gui_project_open(proj_arg, err, sizeof err) == TP_STATUS_OK) {
            set_statusf("Opened %s", gui_project_display_name());
        } else {
            set_statusf_ex(STATUS_ERROR, "Open '%s' failed: %s", proj_arg, err);
        }
    } else {
        set_status("Ready. New project -- add files or a folder to start.");
    }

#ifdef NTPACKER_GUI_SELFTEST
    run_selftest();
#endif

    clamp_selection();
    nt_log_info("ntpacker-gui: starting (live in-process packing + atlas-page canvas)");

    nt_app_run(frame);

    gui_canvas_shutdown(&s_canvas);
    gui_pack_shutdown();
    gui_scan_shutdown();
    gui_project_shutdown();
    nt_ui_destroy_context(s_ctx);
    nt_ui_module_shutdown();
    nt_shape_renderer_shutdown();
    nt_text_renderer_shutdown();
    nt_sprite_renderer_shutdown();
    nt_font_destroy(s_font);
    nt_font_shutdown();
    nt_material_destroy(s_sprite_material);
    nt_material_destroy(s_text_material);
    nt_material_shutdown();
    nt_mem_scratch_shutdown();
    nt_resource_shutdown();
    nt_fs_shutdown();
    nt_http_shutdown();
    nt_hash_shutdown();
    nt_gfx_destroy_buffer(s_frame_ubo);
    nt_gfx_shutdown();
    nt_input_shutdown();
    nt_window_shutdown();
    nt_engine_shutdown();
    return 0;
}
// #endregion
