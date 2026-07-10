/* ntpacker-gui -- native GUI shell for the Neotolis Texture Packer.
 *
 * SKELETON (roadmap Phase 6 grows this file): a resizable window with the
 * ntpacker UI shell -- top toolbar, left sprite panel, center preview canvas,
 * status bar -- approximating docs/design/ux.md §2. No project editing, no
 * export: the buttons post a "wired in a later phase" status line, and no packing
 * logic lives here (AGENTS tool-parity: the GUI is a thin client over tp_core).
 *
 * The live atlas preview (packing via tp_core, drawing the page) is Phase 6: it
 * can't link tp_core/nt_builder in-process with the render runtime today (the
 * builder's basisu encoder duplicates the runtime transcoder + forces a clashing
 * CRT) -- resolve upstream or pack out-of-process via the CLI. See CMakeLists.txt.
 *
 * Wiring template: external/neotolis-engine/examples/ui_showcase/main.c
 * (minimal init/frame/shutdown subset; demo tabs/widgets dropped). */

// #region includes
#include "app/nt_app.h"
#include "atlas/nt_atlas.h"
#include "core/nt_assert.h"
#include "core/nt_core.h"
#include "font/nt_font.h"
#include "fs/nt_fs.h"
#include "graphics/nt_gfx.h"
#include "hash/nt_hash.h"
#include "http/nt_http.h"
#include "input/nt_input.h"
#include "log/nt_log.h"
#include "material/nt_material.h"
#include "math/nt_math.h"
#include "memory/nt_mem_scratch.h"
#include "nt_pack_format.h" /* NT_ASSET_* kinds */
#include "render/nt_render_defs.h"
#include "renderers/nt_sprite_renderer.h"
#include "renderers/nt_text_renderer.h"
#include "resource/nt_resource.h"
#include "ui/nt_ui.h"
#include "ui/nt_ui_button.h"
#include "ui/nt_ui_label.h"
#include "ui/nt_ui_menu.h"
#include "ui/nt_ui_scale.h"
#include "window/nt_window.h"

#include "ntpacker_ui_assets.h"

#include "clay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h> /* GetModuleFileNameA: resolve asset paths from the exe dir, not cwd */
#endif
// #endregion

// #region layout constants + render layers
/* Walker sort key within a Clay zIndex: backgrounds (0) < images (1) < text (2). */
#define LAYER_IMG 1
#define LAYER_TEXT 2

/* Fixed pixel sizes -- constant across window size (desktop 1:1 layout, no UI zoom). */
#define MENUBAR_H 28
#define TOOLBAR_H 44
#define STATUSBAR_H 30
#define LEFT_PANEL_W 260
// #endregion

// #region palette (single dark theme; Clay_Color channels are 0..255)
static const Clay_Color C_BG = {18.0F, 18.0F, 22.0F, 255.0F};
static const Clay_Color C_PANEL = {30.0F, 34.0F, 42.0F, 255.0F};
static const Clay_Color C_CANVAS = {12.0F, 13.0F, 16.0F, 255.0F};
static const Clay_Color C_BORDER = {58.0F, 64.0F, 78.0F, 255.0F};
static const Clay_Color C_STATUS = {24.0F, 26.0F, 34.0F, 255.0F};

static const nt_ui_label_style_t g_title = {.font_id = 0, .font_size = 22, .color = {235.0F, 238.0F, 245.0F, 255.0F}};
static const nt_ui_label_style_t g_body = {.font_id = 0, .font_size = 17, .color = {210.0F, 214.0F, 222.0F, 255.0F}};
static const nt_ui_label_style_t g_caption = {.font_id = 0, .font_size = 14, .color = {150.0F, 156.0F, 168.0F, 255.0F}};
static const nt_ui_label_style_t g_canvas_hint = {.font_id = 0, .font_size = 18, .color = {120.0F, 126.0F, 140.0F, 255.0F}, .align = CLAY_TEXT_ALIGN_CENTER};

/* Muted flat toolbar button (reads as "not yet wired" but still clickable). */
static nt_ui_button_style_t g_btn = {
    .idle = {.bg_tint = 0xFF4A4238U, .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = 0xFF5A5040U, .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = 0xFF3A342CU, .scale = 0.97F, .offset_y = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = 0xFF4A4238U, .scale = 1.0F, .opacity = 0.4F},
    .transition_speed = 12.0F,
    .hit_padding_lrtb = {4, 4, 4, 4},
    .slice9_scale = 1.0F,
};

/* Menu-bar trigger (0xAABBGGRR). Idle tint == the bar's own color (C_STATUS) so the item
 * reads as flat until hovered -- the button always draws a bg rect, so an alpha-0 tint is
 * rejected by the engine; blend into the bar instead. */
static nt_ui_button_style_t g_menubtn = {
    .idle = {.bg_tint = 0xFF221A18U, .scale = 1.0F, .opacity = 1.0F},    /* == C_STATUS */
    .hover = {.bg_tint = 0xFF403430U, .scale = 1.0F, .opacity = 1.0F},   /* lighter slate */
    .pressed = {.bg_tint = 0xFF9E6246U, .scale = 1.0F, .opacity = 1.0F}, /* accent */
    .disabled = {.bg_tint = 0xFF221A18U, .scale = 1.0F, .opacity = 0.4F},
    .transition_speed = 16.0F,
    .hit_padding_lrtb = {0, 0, 0, 0},
    .slice9_scale = 1.0F,
};

/* Filled once (flat bg + text fallbacks, so no extra atlas art beyond the white pixel). */
static nt_ui_menu_style_t s_menu_style;
// #endregion

// #region engine state
#define UI_MAX_ELEMENTS ((uint32_t)1024U)
#define UI_ARENA_SIZE ((size_t)8U * 1024U * 1024U)
#define SCRATCH_ARENA_SIZE ((size_t)1U * 1024U * 1024U)

static NT_UI_DECLARE_ARENA(s_ui_arena, UI_ARENA_SIZE);
static nt_ui_context_t *s_ctx;
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
static nt_font_t s_font;

static bool s_atlas_bound;
static bool s_font_bound;

/* Shell UI state. */
static const char *s_project_path; /* argv[1] or NULL */
static char s_status[256];
static uint32_t s_id_btn_pack, s_id_btn_export;
static bool s_ids_ready;

/* Menu bar: one nt_ui_menu per top-level menu. State (open + anchor) is game-owned and
 * persists; the ctx is per-frame scratch reused every frame (frame_record must survive). */
static uint32_t s_id_mb_file, s_id_mb_view, s_id_mb_help;       /* trigger-button ids (for bbox anchor) */
static uint32_t s_id_menu_file, s_id_menu_view, s_id_menu_help; /* menu ids */
static nt_ui_menu_state_t s_file_state, s_view_state, s_help_state;
static nt_ui_menu_ctx_t s_file_menu, s_view_menu, s_help_menu;
/* Item keys: unique among siblings within one menu (mixed with the menu scope id). */
enum { MK_NEW = 1, MK_OPEN, MK_SAVE, MK_SAVEAS, MK_EXIT, MK_ZIN, MK_ZOUT, MK_FIT, MK_ABOUT };

/* Directory of the running executable; asset paths resolve against it so the app
 * finds its pack regardless of cwd (launch.json runs with cwd=workspaceFolder,
 * while nt_fs_native fopen()s relative to cwd). */
static char s_exe_dir[1024];
// #endregion

// #region init helpers
/* Fills s_exe_dir with the executable's directory (no trailing slash). Falls back
 * to "." (cwd) off-Windows or if the query fails -- the skeleton is Windows-first. */
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

static void ensure_ids(void) {
    if (s_ids_ready) {
        return;
    }
    s_id_btn_pack = nt_ui_id("ntpacker/btn_pack");
    s_id_btn_export = nt_ui_id("ntpacker/btn_export");
    s_id_mb_file = nt_ui_id("ntpacker/mb_file");
    s_id_mb_view = nt_ui_id("ntpacker/mb_view");
    s_id_mb_help = nt_ui_id("ntpacker/mb_help");
    s_id_menu_file = nt_ui_id("ntpacker/menu_file");
    s_id_menu_view = nt_ui_id("ntpacker/menu_view");
    s_id_menu_help = nt_ui_id("ntpacker/menu_help");

    /* Flat panel (no slice9 art) + text fallbacks for arrow/checkmark -> needs only the
     * white pixel + font already bound. Text-only rows (icon_size 0). */
    s_menu_style = nt_ui_menu_style_defaults();
    s_menu_style.font_size = 15.0F;
    s_menu_style.item_height = 26U;
    s_menu_style.min_width = 168U;
    s_menu_style.icon_size = 0U;

    s_ids_ready = true;
}

static void set_status(const char *msg) { (void)snprintf(s_status, sizeof s_status, "%s", msg); }

/* Late-bind the atlas white region + font as their pack resources become ready. */
static void try_bind_resources(void) {
    if (s_atlas_bound && s_font_bound) {
        return;
    }
    if (!s_atlas_bound && nt_resource_is_ready(s_atlas_handle)) {
        const uint32_t white = nt_atlas_find_region(s_atlas_handle, ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS__WHITE.value);
        NT_ASSERT(white != NT_ATLAS_INVALID_REGION);
        nt_ui_set_atlas_white_region(s_ctx, s_atlas_handle, white);
        s_atlas_bound = true;
        nt_log_info("ntpacker-gui: atlas white region bound");
    }
    if (!s_font_bound && nt_resource_is_ready(s_font_resource)) {
        nt_font_add(s_font, s_font_resource);
        nt_ui_set_font(s_ctx, 0U, s_font);
        s_font_bound = true;
        nt_log_info("ntpacker-gui: font bound at slot 0");
    }
}
// #endregion

// #region shell widgets
static bool toolbar_button(nt_ui_context_t *ctx, uint32_t id, const char *text) {
    nt_ui_button_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), id, &g_btn,
                       &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_FIXED(88), CLAY_SIZING_FIXED(34)},
                                                             .padding = CLAY_PADDING_ALL(6),
                                                             .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                                  .cornerRadius = CLAY_CORNER_RADIUS(6)},
                       true, NULL);
    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), text, &g_body);
    return nt_ui_button_end(ctx);
}

/* Toolbar keeps the primary action buttons only; app name lives in the OS title bar, and
 * File ops live in the menu bar -- no in-window title label, no duplication. */
static void declare_toolbar(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(TOOLBAR_H)},
                     .padding = {10, 10, 6, 6},
                     .childGap = 10,
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_PANEL,
          .cornerRadius = CLAY_CORNER_RADIUS(8),
          .border = {.color = C_BORDER, .width = {1, 1, 1, 1, 0}}}) {
        if (toolbar_button(ctx, s_id_btn_pack, "Pack")) {
            set_status("Pack: wired in Phase 6.");
        }
        if (toolbar_button(ctx, s_id_btn_export, "Export")) {
            set_status("Export: wired in Phase 6.");
        }
    }
}

// #region menu bar (nt_ui_menu driven; click a top item to open its dropdown)
static void close_all_menus(void) {
    s_file_state.open = false;
    s_view_state.open = false;
    s_help_state.open = false;
}

static void file_items(nt_ui_menu_ctx_t *m) {
    if (nt_ui_menu_item(m, MK_NEW, "New")) {
        set_status("New project -- lands in Phase 3.");
    }
    if (nt_ui_menu_item(m, MK_OPEN, "Open...")) {
        set_status("Open project -- lands in Phase 3.");
    }
    if (nt_ui_menu_item(m, MK_SAVE, "Save")) {
        set_status("Save project -- lands in Phase 3.");
    }
    if (nt_ui_menu_item(m, MK_SAVEAS, "Save As...")) {
        set_status("Save As -- lands in Phase 3.");
    }
    nt_ui_menu_separator(m);
    if (nt_ui_menu_item(m, MK_EXIT, "Exit")) {
        nt_app_quit(); /* same clean path as the window close button */
    }
}

static void view_items(nt_ui_menu_ctx_t *m) {
    if (nt_ui_menu_item(m, MK_ZIN, "Zoom In")) {
        set_status("Zoom In -- preview canvas lands in Phase 6.");
    }
    if (nt_ui_menu_item(m, MK_ZOUT, "Zoom Out")) {
        set_status("Zoom Out -- preview canvas lands in Phase 6.");
    }
    if (nt_ui_menu_item(m, MK_FIT, "Fit")) {
        set_status("Fit -- preview canvas lands in Phase 6.");
    }
}

static void help_items(nt_ui_menu_ctx_t *m) {
    if (nt_ui_menu_item(m, MK_ABOUT, "About")) {
        set_status("ntpacker -- Neotolis Texture Packer (skeleton). About dialog lands in Phase 6.");
    }
}

/* Trigger button in the bar. On click, toggle this menu open at its bottom-left (bbox from
 * last frame) and close the others -- classic click-driven menu bar over the context widget. */
static void menubar_entry(nt_ui_context_t *ctx, uint32_t btn_id, const char *label, nt_ui_menu_state_t *st) {
    nt_ui_button_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), btn_id, &g_menubtn,
                       &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0)},
                                                             .padding = {10, 10, 2, 2},
                                                             .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                                  .cornerRadius = CLAY_CORNER_RADIUS(4)},
                       true, NULL);
    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), label, &g_body);
    if (nt_ui_button_end(ctx)) {
        const bool was_open = st->open;
        close_all_menus();
        if (!was_open) {
            const nt_ui_bbox_t bb = nt_ui_get_bbox(ctx, btn_id);
            st->anchor_x = bb.x;
            st->anchor_y = bb.y + bb.height; /* drop the menu just below the bar item */
            st->open = true;
        }
    }
}

static void declare_menubar(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(MENUBAR_H)},
                     .padding = {4, 4, 0, 0},
                     .childGap = 2,
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_STATUS,
          .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
        menubar_entry(ctx, s_id_mb_file, "File", &s_file_state);
        menubar_entry(ctx, s_id_mb_view, "View", &s_view_state);
        menubar_entry(ctx, s_id_mb_help, "Help", &s_help_state);
    }
}

/* Declared at root (after the layout tree, before nt_ui_end): the dropdown popups are
 * screen-anchored floatings and must escape any panel scissor. Call every frame open or
 * closed -- the item calls no-op when the menu is closed (keeps the keyboard-nav record). */
static void declare_menus(nt_ui_context_t *ctx) {
    nt_ui_menu_begin(&s_file_menu, ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_menu_file, &s_file_state, &s_menu_style);
    file_items(&s_file_menu);
    nt_ui_menu_end(&s_file_menu);

    nt_ui_menu_begin(&s_view_menu, ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_menu_view, &s_view_state, &s_menu_style);
    view_items(&s_view_menu);
    nt_ui_menu_end(&s_view_menu);

    nt_ui_menu_begin(&s_help_menu, ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_menu_help, &s_help_state, &s_menu_style);
    help_items(&s_help_menu);
    nt_ui_menu_end(&s_help_menu);
}
// #endregion

static void declare_left_panel(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(LEFT_PANEL_W), CLAY_SIZING_GROW(0)},
                     .padding = CLAY_PADDING_ALL(12),
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 8,
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
          .backgroundColor = C_PANEL,
          .cornerRadius = CLAY_CORNER_RADIUS(8),
          .border = {.color = C_BORDER, .width = {1, 1, 1, 1, 0}}}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Sprites", &g_title);
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No sprites yet.", &g_caption);
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Add-folder input lands in", &g_caption);
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Phase 6 (see ux.md §2).", &g_caption);
    }
}

static void declare_canvas(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                     .padding = CLAY_PADDING_ALL(16),
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_CANVAS,
          .cornerRadius = CLAY_CORNER_RADIUS(8),
          .border = {.color = C_BORDER, .width = {1, 1, 1, 1, 0}}}) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 8, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}}}) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No atlas loaded -- pack preview lands here.", &g_canvas_hint);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Live atlas preview lands in Phase 6 (see ux.md region E).", &g_caption);
        }
    }
}

static void declare_statusbar(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(STATUSBAR_H)},
                     .padding = {12, 12, 4, 4},
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_STATUS,
          .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), s_status, &g_caption);
    }
}
// #endregion

// #region frame
static void frame(void) {
    nt_window_poll();
    nt_input_poll();
    nt_mem_scratch_reset();

    /* Esc closes an open menu (desktop convention) -- it does NOT quit. Quit paths are the
     * window close button and File > Exit. */
    if (nt_input_key_is_pressed(NT_KEY_ESCAPE)) {
        close_all_menus();
    }

    nt_resource_step();
    nt_material_step();
    try_bind_resources();

    /* Real framebuffer size every frame (never a cached startup size); the >0 guard keeps a
     * minimized/zero-size window from feeding Clay a 0 extent. */
    const float fb_w = (float)(g_nt_window.fb_width > 0 ? g_nt_window.fb_width : 800);
    const float fb_h = (float)(g_nt_window.fb_height > 0 ? g_nt_window.fb_height : 600);

    /* Desktop 1:1 scaling: logical UI space == framebuffer pixels, so menubar/toolbar/panels
     * and fonts keep a CONSTANT pixel size and only the center canvas grows with the window --
     * NOT ui_showcase's reference-resolution EXPAND (which zooms the whole UI). STRETCH with
     * ref == fb yields scale_x=scale_y=1, offset 0. No DPI/content scale is applied: nt_window
     * exposes none; wire it here when the engine surfaces monitor content scale. */
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

        nt_ui_begin(s_ctx, scale.logical_w, scale.logical_h, g_nt_app.dt, &g_nt_input.pointers[0], 1);
        nt_ui_set_viewport(s_ctx, nt_ui_viewport_from_scale(&scale));

        CLAY({.id = CLAY_ID("root"),
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                         .padding = CLAY_PADDING_ALL(10),
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = 8,
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
              .backgroundColor = C_BG}) {
            declare_menubar(s_ctx);
            declare_toolbar(s_ctx);
            /* Middle row: left sprite panel | center canvas (grows to fill). */
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 8, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}}}) {
                declare_left_panel(s_ctx);
                declare_canvas(s_ctx);
            }
            declare_statusbar(s_ctx);
        }

        /* Dropdown popups at root so they escape panel scissors (mirrors a root-declared modal). */
        declare_menus(s_ctx);

        nt_ui_end(s_ctx);

        nt_ui_target_t target = nt_ui_scale_make_target(&scale);
        nt_ui_walk(s_ctx, &target);
    }

    nt_gfx_end_pass();
    nt_gfx_end_segment();
    nt_gfx_end_frame();

    nt_window_swap_buffers();
}
// #endregion

// #region main + init/shutdown
int main(int argc, char *argv[]) {
    /* argv[1] (if any) is the project file. The skeleton does not load it (Phase 3);
     * it tolerates an unknown/missing path and just reports it in the status bar. */
    s_project_path = (argc > 1) ? argv[1] : NULL;
    if (s_project_path != NULL) {
        (void)snprintf(s_status, sizeof s_status, "project: %s -- loading lands in Phase 3", s_project_path);
    } else {
        set_status("no project");
    }

    nt_engine_config_t config = {0};
    config.app_name = "ntpacker-gui";
    config.version = 1;
    if (nt_engine_init(&config) != NT_OK) {
        return 1;
    }
    nt_log_info("ntpacker-gui: %s build (%s)", nt_engine_build_string(), nt_engine_preset_string());

    g_nt_window.width = 1280;
    g_nt_window.height = 800;
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

    nt_material_init(&(nt_material_desc_t){.max_materials = 2}); /* sprite + text */
    nt_font_init(&(nt_font_desc_t){.max_fonts = 1});

    nt_sprite_renderer_desc_t sr_desc = nt_sprite_renderer_desc_defaults();
    nt_sprite_renderer_init(&sr_desc);
    nt_text_renderer_init();

    nt_ui_module_init();
    nt_ui_create_desc_t ui_desc = nt_ui_create_desc_defaults();
    ui_desc.max_elements = UI_MAX_ELEMENTS;
    ui_desc.state_slots = 128U;
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

    nt_log_info("ntpacker-gui: starting (menu bar: File/View/Help; Esc closes menus)");

    nt_app_run(frame);

    nt_ui_destroy_context(s_ctx);
    nt_ui_module_shutdown();
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
