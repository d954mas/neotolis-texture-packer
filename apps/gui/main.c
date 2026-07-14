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

#include "ntpacker_ui_assets.h"

#include "clay.h"

#include "tp_core/tp_names.h" /* tp_sprite_export_key (slice9 frame-sync key) */

#include "gui_canvas.h"
#include "gui_defs.h"
#include "gui_state.h"
#include "gui_widgets.h"
#include "gui_actions.h"
#include "gui_rows.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_shell.h"    /* shell-owned surface the dev seams read (UI pool caps) */
#include "gui_paths.h"    /* D1: app-data root + exe-dir resolver (canonical home for s_exe_dir) */
#include "gui_log_file.h" /* D1: rotating app-side log file (nt_log sink); no-op under headless */
#include "gui_startup.h"  /* H/P1-8: pure startup open/defer guard (gui_startup_decide) */
#include "gui_selftest.h" /* dev seam: headless self-test (compiled out unless flag on) */
#include "gui_shot.h"     /* dev seam: --shot screenshot capture */
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

// #region ui ids gate
static bool s_ids_ready;

/* s_id_mb_ file/edit/view/help and s_id_menu_ file/edit/view/help (menubar + menu-panel ids), the
 * menu-state buffers (s_file/edit/view/help_state + the four nt_ui_menu_ctx_t working buffers), the
 * MK_ item-key enum, and s_ctx_menu (the context-menu declare-machinery working buffer) moved to
 * gui_view_chrome.c (GUI decomposition step 6b) -- nothing outside chrome ever read them. The ids
 * themselves moved into gui_state (same precedent as s_id_ctx_menu in step 4) rather than staying
 * chrome-local, so ensure_ids below still seeds them directly with no extra hook function; the
 * menu-state buffers and the item-key enum, which nothing outside chrome touches, became
 * chrome-local statics instead. */
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

/* s_pack_has_sources/s_pack_stale (pack-button state cached for the tooltip pass) moved to
 * gui_state (step 6a -- written by gui_view_canvas's declare_canvas_strip, read by gui_view_chrome's
 * declare_tooltips). The right settings panel's disclosure/dropdown-open bits and numeric-field edit
 * buffers moved to gui_view_settings.c (step 4 -- panel-local). GUI_MAX_TARGETS and k_playback_names
 * live in gui_defs.h (shared with gui_view_chrome's declare_export_modal / gui_view_canvas's
 * declare_canvas_preview). */

// #endregion

// #region small helpers
/* gui_open_url (opens a URL in the OS default browser -- the About link) moved to
 * gui_view_chrome.c (GUI decomposition step 6b) as a pure move: it is chrome-only, the About modal
 * is its sole caller. */

/* Shell-owned reset of the canvas-bound-result cache (gui_shell.h): the destructive flows call this
 * right after gui_pack_clear(-1) so s_shown_result never holds a freed slot pointer at the next
 * frame's `want != s_shown_result` compare (P2 hardening). */
void gui_shell_reset_shown_result(void) { s_shown_result = NULL; }
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
    /* Floating surfaces must SEPARATE from the panels (owner: open menus blended in). The engine
     * default is a semi-transparent near-panel gray + a warm hover foreign to the palette. Elevated
     * surface = opaque header tone (lighter than panel, same as section headers, §2.1); hover = the
     * app's selection blue; separators = the border tone. */
    s_menu_style.bg_color = RGBA8(40, 45, 57);         /* C_HEADER: opaque elevated surface */
    s_menu_style.item_hover_color = RGBA8(48, 74, 120); /* C_SEL: selection blue, not the default amber */
    s_menu_style.separator_color = RGBA8(52, 58, 72);   /* C_BORDER */
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
    /* The OPEN list is a floating surface, not a well: elevated header tone (was RGBA8(30,33,41),
     * 2 units off the panel fill -- it blended in; owner report). Matches s_menu_style.bg_color. */
    s_dd_style.panel_fill = RGBA8(40, 45, 57);
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
/* close_menubar_menus/close_all_menus, the File/Edit/View/Help menu item builders (file_items/
 * edit_items/scale_item/overlay_item/view_items/help_items), menubar_entry, and declare_menubar
 * moved to gui_view_chrome.c (GUI decomposition step 6b) as a pure move. close_menubar_menus keeps a
 * prototype here via gui_shell.h (the sanctioned cross-view surface the three other views' context-
 * menu triggers read) and in gui_view_chrome.h (for frame() below); its definition lives in
 * gui_view_chrome.c now. */
// #endregion

// #region left panel (atlases + sprites)
/* declare_left_panel + its atlas/sprite/animation row helpers moved to gui_view_lists.c (GUI
 * decomposition step 5). declare_row_tooltips moved to gui_view_chrome.c (step 6b -- chrome-owned
 * per the plan's §3 fan-out table). */
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

/* atlas_fill_pct, strip_group_actions/pages/zoom, declare_canvas_strip, declare_canvas_preview,
 * declare_canvas, status_sev_color/status_sev_icon, and declare_status_pill moved to
 * gui_view_canvas.c (GUI decomposition step 6a) as a pure move. handle_canvas_input above stays
 * here per the P-2 lead ruling (docs/plans/gui-decomposition.md §2). */
// #endregion

// #region status bar + menus + tooltips
/* status_sev_color, status_sev_icon, declare_status_pill moved to gui_view_canvas.c (GUI
 * decomposition step 6a) as a pure move -- see the note in the canvas region above.
 *
 * declare_menus, declare_context_menu, declare_tooltips, declare_export_modal, declare_confirm_modal,
 * and declare_about_modal moved to gui_view_chrome.c (GUI decomposition step 6b) as a pure move. */
// #endregion

/* The right settings panel (regions F/G + per-region packing overrides) moved to
 * gui_view_settings.c/h (step 4) as a pure move -- declare_right_panel is called from frame() below;
 * the header exposes only that entry point. */

// #region keyboard shortcuts (ux.md §3.3d)
/* Global shortcuts routed through the SAME actions as the menus. Text-input focus swallows
 * them first (no accidental global actions while typing); an open modal blocks them too. */
static void handle_shortcuts(void) {
    if (gui_shot_active()) {
        return; /* headless capture: the user's live typing must not trigger hotkeys mid-shot */
    }
    if (nt_ui_input_any_focused(s_ctx) || s_confirm_open || s_about_open || s_export_open) {
        return;
    }
    /* Preview + editor accelerators (each also a button; §3.3e). */
    if (s_preview_active && nt_input_key_is_pressed(NT_KEY_SPACE)) {
        preview_toggle_play();
    }
    if (s_edit_kind == EDIT_NONE && s_sel_anim >= 0 && s_sel_anim_frame >= 0 &&
        nt_input_key_is_pressed(NT_KEY_DELETE)) {
        /* fix3 completeness: clear the frame selection ONLY on a real removal -- a journal-failed flush
         * aborts the remove (frame still present), so don't deselect it (op-error surfaces via poll_async). */
        if (gui_project_anim_remove_frame(s_sel_atlas, s_sel_anim, s_sel_anim_frame)) {
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
     * (F2-05b-ii-A, decision 0015). GATED on no active gesture -- no held pointer and no focused
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
        /* A blur (press outside the panel while a field held focus) is a field's gesture boundary:
         * flush its buffered edit as ONE undo step (F2-05b-ii-A, decision 0015). The value is already
         * in gui_project's pending buffer from the last keystroke; apply_pending commits it next frame. */
        if (s_blur_inputs) {
            gui_request_gesture_commit();
        }

        /* Bind the result the canvas shows (repack / atlas switch / clear): the export-target preview
         * while one is active + visible, else the native session pack (preview_target_result). */
        const tp_result *want = preview_target_result();
        if (want != s_shown_result) {
            gui_canvas_set_result(&s_canvas, want);
            s_shown_result = want;
        }
        /* Feed the selected region's LIVE slice9 override to the canvas guides: the project is the
         * source of truth, so typing in the Region panel moves the lines this same frame (no repack;
         * owner: "добавил и не вижу"). Result names are the original atlas-relative key + ext. */
        s_canvas.sel_slice9[0] = s_canvas.sel_slice9[1] = s_canvas.sel_slice9[2] = s_canvas.sel_slice9[3] = 0;
        if (want && s_canvas.sel_sprite >= 0 && s_canvas.sel_sprite < want->sprite_count) {
            tp_project_atlas *sel_a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
            if (sel_a) {
                char s9key[192];
                tp_sprite_export_key(want->sprites[s_canvas.sel_sprite].name, s9key, sizeof s9key);
                /* EFFECTIVE value (#5): a slice9 edit BUFFERS until the gesture boundary, so the committed
                 * record freezes mid-typing. Prefer the buffered slice9 (peek) when one is in flight for
                 * this atlas+sprite, so the guides move THIS frame; else read the committed record. */
                int eff[4];
                if (gui_project_peek_pending_slice9(s_sel_atlas, s9key, eff)) {
                    for (int k = 0; k < 4; k++) {
                        s_canvas.sel_slice9[k] = eff[k];
                    }
                } else {
                    const tp_project_sprite *s9ov = tp_project_atlas_find_sprite(sel_a, s9key);
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

// #region dev seam: --parity (headless saved-bytes byte-parity check)
/* Applies a FIXED non-creating edit sequence to `in` through the OP-based gui_project_*
 * setters (the F2-05b-i cutover) and saves it to `out`. Non-creating (rename/settings/
 * override/target on an already-fixed-id project) => deterministic bytes: no random ids
 * are minted, so `out` is byte-comparable to the same logical edits applied by the
 * byte-identity-proven CLI (scripts/gui_parity_check.sh). Runs before any window/GL init
 * (pure model layer). Returns 0 on success, 2 on open/save failure. */
static int gui_run_parity(const char *in, const char *out) {
    char err[256] = {0};
    gui_project_init();
    if (gui_project_open(in, err, sizeof err) != TP_STATUS_OK) {
        (void)fprintf(stderr, "parity: open '%s' failed: %s\n", in, err);
        return 2;
    }
    /* atlas rename + all 10 knobs (each a single-knob atlas.settings.set transaction; the
     * final atlas is identical to the CLI's one multi-knob `set`). */
    (void)gui_project_set_atlas_name(0, "hero_atlas");
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, 7, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_MARGIN, 3, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_EXTRUDE, 2, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_SHAPE, 0, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_ALPHA_THRESHOLD, 42, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_MAX_VERTICES, 5, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_MAX_SIZE, 2048, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_ALLOW_TRANSFORM, 0, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_POWER_OF_TWO, 1, 0.0F);
    (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PIXELS_PER_UNIT, 0, 64.0F);
    /* a pending (name-keyed) sprite override -- shape + origin + rename on an arbitrary key
     * (matches the CLI `sprite set <atlas> psp shape=0 origin=0.25,0.75 rename=psp_final`). */
    (void)gui_project_set_sprite_rename(0, "psp", "psp_final");
    (void)gui_project_set_sprite_override(0, "psp", GUI_SPRITE_OV_SHAPE, 0);
    /* origin is now COMPONENT-keyed (#2): set X then Y (matches the CLI `origin=0.25,0.75`). The final
     * override record is byte-identical -- editing Y flushes the buffered X (X committed), then Y seeds
     * the committed X, so the saved sprite carries origin=(0.25,0.75) exactly as the single-op form did. */
    (void)gui_project_set_sprite_origin(0, "psp", 0 /* X */, 0.25F);
    (void)gui_project_set_sprite_origin(0, "psp", 1 /* Y */, 0.75F);
    (void)gui_project_set_sprite_slice9(0, "psp", 0, 4);
    /* target 0: keep its exporter (read from the model -- no exporter-id literal), set path. #4 UAF fix:
     * FLUSH the buffered slice9 FIRST (it clone-swaps + frees the current project), THEN re-get `a` from
     * the now-stable project and COPY exporter_id into a local before set_target (whose own flush is now a
     * no-op) -- the pre-fix code read a->targets[0].exporter_id AFTER set_target's flush had freed it. */
    {
        gui_project_flush_pending();
        tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), 0);
        if (a && a->target_count > 0) {
            char exporter[64];
            (void)snprintf(exporter, sizeof exporter, "%s", a->targets[0].exporter_id);
            (void)gui_project_set_target(0, 0, exporter, "out/hero", true);
        }
    }
    if (gui_project_save_as(out, err, sizeof err) != TP_STATUS_OK) {
        (void)fprintf(stderr, "parity: save '%s' failed: %s\n", out, err);
        return 2;
    }
    (void)fprintf(stdout, "parity: wrote %s\n", out);
    gui_project_shutdown();
    return 0;
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

    /* dev seam: --parity <in> <out> runs the headless saved-bytes byte-parity check and exits
     * BEFORE any window/GL init (pure model layer). Returns before file logging installs -> a
     * byte-parity run stays side-effect-free (no stray app-data log). */
    for (int i = 1; i + 2 < argc; i++) {
        if (strcmp(argv[i], "--parity") == 0) {
            return gui_run_parity(argv[i + 1], argv[i + 2]);
        }
    }

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

    /* D1: mirror nt_log to a rotating file, but ONLY for a real windowed run -- the --shot capture
     * seam (and --parity, already returned) must stay side-effect-free (no stray app-data log/sink/
     * FILE). Installed before the build line so that line is captured too; also a no-op under
     * NTPACKER_GUI_HEADLESS and if the app-data dir can't be created. */
    if (!gui_shot_active()) {
        gui_log_file_install();
    }
    nt_log_info("ntpacker-gui: %s build (%s)", nt_engine_build_string(), nt_engine_preset_string());

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
    /* F2-05b-ii-B: crash recovery. The recovery-journal SIDECAR lives at a DETERMINISTIC path under
     * the exe dir (stable across launches, like pack_session), so a crashed session is recovered on
     * the next launch WITHOUT a random session id. Disabled in the headless selftest build (which
     * drives the journal itself, in isolation, and must stay deterministic + file-free). */
#ifndef NTPACKER_GUI_SELFTEST
    char recovery_slot[1152];
    (void)snprintf(recovery_slot, sizeof recovery_slot, "%s/ntpacker_recovery.ntpjournal", s_exe_dir);
    gui_project_enable_recovery(recovery_slot);
#endif
    gui_project_init();
    /* H/P1-8 fix: TWO distinct startup facets, no longer conflated in one bool.
     *  - recovery_warn_shown: did EITHER startup recovery warning get shown this launch -- the
     *    adopted-unsaved-work notice OR the "another window open -> crash recovery is off for this one"
     *    BUSY notice? Both are STATUS_WARNING that a later terminal default ("Ready...", "Opened %s",
     *    "project not found") must NOT clobber before the first frame. The busy notice warns that recovery
     *    is OFF -- silently losing it (finding 1) means a later crash discards work with no prior warning.
     *  - the actual DATA-SAFETY deferral keys off the DURABLE model predicate
     *    gui_project_has_recovered_unsaved() (the single source of truth for the condition), read at the
     *    decision point below -- NOT this drained one-shot notice. The notice is drained here only to
     *    obtain the warning TEXT. Both stay quiet in the selftest build (no interactive recovery there). */
    bool recovery_warn_shown = false;
#ifndef NTPACKER_GUI_SELFTEST
    {
        char rnotice[256];
        if (gui_project_take_recovery_notice(rnotice, sizeof rnotice)) {
            recovery_warn_shown = true;
            set_status_ex(STATUS_WARNING, rnotice); /* "Recovered unsaved changes ... Save to keep them." */
        } else if (gui_project_take_recovery_busy_notice(rnotice, sizeof rnotice)) {
            recovery_warn_shown = true;             /* fix [1] + finding 1: the BUSY notice must survive too */
            set_status_ex(STATUS_WARNING, rnotice); /* "Another window open -- crash recovery off." */
        }
    }
#endif

    /* in-process packing: session .ntpack goes under the exe dir (existing convention) */
    char pack_session[1152];
    (void)snprintf(pack_session, sizeof pack_session, "%s/pack_session", s_exe_dir);
    gui_pack_init(pack_session);

    /* open a project passed on the command line (errors go to the status bar).
     * H/P1-8 fix: route the open/defer choice through the PURE gui_startup_decide (single source of truth;
     * J14 truth-table). `recovered` is the DURABLE model predicate, so a recovered launch DEFERS even when
     * the arg is stale (DEFER > MISSING -- finding 2). No terminal default status clobbers a recovery
     * warning that is already up (recovery_warn_shown): "Opened %s" / "project not found" / "Ready..." are
     * suppressed then. A genuine open ERROR still shows -- see the OPEN case. */
    if (proj_arg != NULL) {
        char err[256];
        switch (gui_startup_decide(true, gui_scan_exists(proj_arg), gui_project_has_recovered_unsaved())) {
        case GUI_STARTUP_DEFER:
            /* Crash-recovery adopted unsaved work at init. Opening the CLI file now would SILENTLY DISCARD it
             * -- gui_project_open has no dirty prompt (pending_discard + wrap_model + re-checkpoint of the
             * recovery slot). Defer: keep the recovered model + its slot intact and tell the user to resolve
             * it first, then open via File>Open (which IS dirty-gated). This arg-specific warning deliberately
             * replaces the generic recovery notice (both STATUS_WARNING; this one is more actionable). */
            set_statusf_ex(STATUS_WARNING, "Recovered unsaved changes -- save or discard before opening %s", proj_arg);
            break;
        case GUI_STARTUP_MISSING:
            if (!recovery_warn_shown) { /* stale argv -> continue with untitled (F6b); keep any recovery warning */
                set_statusf_ex(STATUS_WARNING, "project not found: %s", proj_arg);
            }
            break;
        case GUI_STARTUP_OPEN:
            if (gui_project_open(proj_arg, err, sizeof err) == TP_STATUS_OK) {
                if (!recovery_warn_shown) { /* routine confirmation -> must not clobber the busy "recovery off" warning */
                    set_statusf("Opened %s", gui_project_display_name());
                }
            } else {
                /* A genuine open FAILURE is surfaced even over a recovery warning: it is a concrete,
                 * user-initiated failure the user is actively waiting on (they asked to open THIS file), it
                 * is higher severity (STATUS_ERROR > STATUS_WARNING), and it is rare. Present actionable
                 * failure wins over the latent "recovery off" warning. */
                set_statusf_ex(STATUS_ERROR, "Open '%s' failed: %s", proj_arg, err);
            }
            break;
        case GUI_STARTUP_IDLE:
            break; /* unreachable with proj_arg != NULL; listed for switch exhaustiveness */
        }
    } else if (!recovery_warn_shown) {
        set_status("Ready. New project -- add files or a folder to start.");
    }
    /* proj_arg == NULL && recovery_warn_shown: keep the recovery STATUS_WARNING as the first-frame status
     * (do NOT clobber it with "Ready..." before the first frame is drawn). */

#ifdef NTPACKER_GUI_SELFTEST
    run_selftest();
#endif

    clamp_selection();
    nt_log_info("ntpacker-gui: starting (live in-process packing + atlas-page canvas)");

    nt_app_run(frame);

    /* The window closed (X / Alt+F4) while a pack/export was still on the worker thread. tp_pack is
     * non-interruptible (engine limitation), so we cannot abort it -- but instead of blocking the UI
     * thread in gui_pack_shutdown's bare join (a frozen "not responding" ghost window), keep the OS
     * message pump alive and poll until the worker lands. Cancel first (export stops between atlases);
     * the wait is bounded by the pack's remaining time, and gui_pack_shutdown below is then instant. */
    if (gui_pack_worker_active()) {
        gui_pack_async_cancel();
        while (gui_pack_worker_active()) {
            nt_window_poll();
            (void)gui_pack_poll(NULL); /* joins + frees the job the frame it signals done */
        }
    }

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
    gui_log_file_shutdown(); /* D1: unregister the sink + close the file before the engine goes down */
    nt_engine_shutdown();
    return 0;
}
// #endregion
