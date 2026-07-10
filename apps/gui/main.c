/* ntpacker-gui -- native GUI for the Neotolis Texture Packer.
 *
 * PROJECT EDITOR (this iteration): create/open/save .ntpacker_project files, manage
 * atlases, add image files/folders as sources, browse+select sprites, and preview the
 * selected SOURCE image on the center canvas. The single source of truth is the linked
 * tp_project (tp_core) -- the GUI is a thin editor (AGENTS tool-parity). In-process PACKING
 * is still blocked by engine issue #282, so the live atlas preview is deferred; the Pack
 * button surfaces the "preview stale" state per ux.md §3.3b.
 *
 * Module split: gui_project (state + dirty bits + load/save), gui_scan (display-only folder
 * enumeration), gui_canvas (the source-image preview via a custom nt_ui element). main.c is
 * init/frame/shutdown + layout + the OS file dialogs (tinyfiledialogs).
 *
 * Wiring template: external/neotolis-engine/examples/ui_showcase/main.c. */

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
#include "nt_pack_format.h"
#include "render/nt_render_defs.h"
#include "renderers/nt_sprite_renderer.h"
#include "renderers/nt_text_renderer.h"
#include "resource/nt_resource.h"
#include "ui/nt_ui.h"
#include "ui/nt_ui_button.h"
#include "ui/nt_ui_input.h"
#include "ui/nt_ui_label.h"
#include "ui/nt_ui_menu.h"
#include "ui/nt_ui_modal.h"
#include "ui/nt_ui_scale.h"
#include "ui/nt_ui_tooltip.h"
#include "ui/nt_ui_vlist.h"
#include "window/nt_window.h"

#include "ntpacker_ui_assets.h"

#include "clay.h"

#include "gui_canvas.h"
#include "gui_history.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_version.h"
#include "tinyfiledialogs.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif
// #endregion

// #region layout constants + render layers + global UI scale
#define LAYER_IMG 1
#define LAYER_TEXT 2

/* Global UI scale (owner: "too small -- make it bigger"). Every layout metric and font size
 * flows through S()/Su(), so one knob resizes the whole chrome. Seeded from the system DPI at
 * startup (a high-DPI Windows display inflates the framebuffer to physical pixels, which makes
 * fixed 1:1 metrics physically tiny -- the scale compensates) and overridable from View > UI
 * Scale. BASE_* sizes are already bumped ~15-20% over the old shell for desktop-tool density. */
static float g_ui_scale = 1.0F;
static inline float S(float px) { return px * g_ui_scale; }
static inline uint16_t Su(float px) { return (uint16_t)((px * g_ui_scale) + 0.5F); }

#define BASE_MENUBAR_H 32.0F
#define BASE_TOOLBAR_H 48.0F
#define BASE_STATUSBAR_H 34.0F
#define BASE_LEFT_PANEL_W 300.0F
#define BASE_ROW_H 27.0F

/* Base font px (scaled per frame into the g_* styles below). */
#define FS_TITLE 17.0F
#define FS_BODY 17.0F
#define FS_ROW 16.0F
#define FS_CAPTION 15.0F
#define FS_HINT 19.0F
#define FS_TAG 15.0F
// #endregion

// #region palette
static const Clay_Color C_BG = {18.0F, 18.0F, 22.0F, 255.0F};
static const Clay_Color C_PANEL = {30.0F, 34.0F, 42.0F, 255.0F};
static const Clay_Color C_CANVAS = {12.0F, 13.0F, 16.0F, 255.0F};
static const Clay_Color C_BORDER = {58.0F, 64.0F, 78.0F, 255.0F};
static const Clay_Color C_STATUS = {24.0F, 26.0F, 34.0F, 255.0F};
static const Clay_Color C_SEL = {52.0F, 78.0F, 120.0F, 255.0F};       /* selected-row accent fill */
static const Clay_Color C_HOVER = {42.0F, 48.0F, 60.0F, 255.0F};      /* row hover */
static const Clay_Color C_TAG = {158.0F, 98.0F, 70.0F, 235.0F};       /* stale/outdated amber */
static const Clay_Color C_TRANSPARENT = {0.0F, 0.0F, 0.0F, 0.0F};

/* Base label styles (font_size = base px); rescale_styles() copies these into the g_* below
 * with font_size *= g_ui_scale every frame, so scaled text stays crisp (Slug vector font). */
static const nt_ui_label_style_t g_title_base = {.font_id = 0, .font_size = FS_TITLE, .color = {170.0F, 180.0F, 196.0F, 255.0F}};
static const nt_ui_label_style_t g_body_base = {.font_id = 0, .font_size = FS_BODY, .color = {214.0F, 220.0F, 230.0F, 255.0F}};
static const nt_ui_label_style_t g_row_base = {.font_id = 0, .font_size = FS_ROW, .color = {206.0F, 212.0F, 222.0F, 255.0F}};
static const nt_ui_label_style_t g_caption_base = {.font_id = 0, .font_size = FS_CAPTION, .color = {150.0F, 156.0F, 168.0F, 255.0F}};
static const nt_ui_label_style_t g_canvas_hint_base = {.font_id = 0, .font_size = FS_HINT, .color = {120.0F, 126.0F, 140.0F, 255.0F}, .align = CLAY_TEXT_ALIGN_CENTER};
static const nt_ui_label_style_t g_tag_base = {.font_id = 0, .font_size = FS_TAG, .color = {245.0F, 238.0F, 232.0F, 255.0F}};
/* Missing-file rows / placeholder (ux.md §3.7): amber warning accent. */
static const nt_ui_label_style_t g_warn_base = {.font_id = 0, .font_size = FS_ROW, .color = {224.0F, 158.0F, 96.0F, 255.0F}};
static nt_ui_label_style_t g_title, g_body, g_row, g_caption, g_canvas_hint, g_tag, g_warn; /* scaled each frame */

static nt_ui_button_style_t g_btn = {
    .idle = {.bg_tint = 0xFF4A4238U, .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = 0xFF5A5040U, .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = 0xFF3A342CU, .scale = 0.97F, .offset_y = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = 0xFF4A4238U, .scale = 1.0F, .opacity = 0.35F},
    .transition_speed = 12.0F,
    .hit_padding_lrtb = {4, 4, 4, 4},
    .slice9_scale = 1.0F,
};
/* Accent button (stale Pack): amber fill so it reads as "action needed". */
static nt_ui_button_style_t g_btn_accent = {
    .idle = {.bg_tint = 0xFF4662A0U, .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = 0xFF5878B8U, .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = 0xFF3A5088U, .scale = 0.97F, .offset_y = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = 0xFF4662A0U, .scale = 1.0F, .opacity = 0.35F},
    .transition_speed = 12.0F,
    .hit_padding_lrtb = {4, 4, 4, 4},
    .slice9_scale = 1.0F,
};
static nt_ui_button_style_t g_btn_ghost = {
    .idle = {.bg_tint = 0xFF2A2E38U, .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = 0xFF3C4250U, .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = 0xFF242832U, .scale = 0.97F, .opacity = 1.0F},
    .disabled = {.bg_tint = 0xFF2A2E38U, .scale = 1.0F, .opacity = 0.35F},
    .transition_speed = 14.0F,
    .hit_padding_lrtb = {2, 2, 2, 2},
    .slice9_scale = 1.0F,
};
static nt_ui_button_style_t g_menubtn = {
    .idle = {.bg_tint = 0xFF221A18U, .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = 0xFF403430U, .scale = 1.0F, .opacity = 1.0F},
    .pressed = {.bg_tint = 0xFF9E6246U, .scale = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = 0xFF221A18U, .scale = 1.0F, .opacity = 0.4F},
    .transition_speed = 16.0F,
    .hit_padding_lrtb = {0, 0, 0, 0},
    .slice9_scale = 1.0F,
};
static nt_ui_menu_style_t s_menu_style;
static nt_ui_modal_style_t s_modal_style;
static nt_ui_tooltip_style_t s_tip_style;
static nt_ui_input_style_t s_rename_input; /* inline rename field (atlas + sprite) */
// #endregion

// #region engine state
#define UI_MAX_ELEMENTS ((uint32_t)4096U)
#define UI_ARENA_SIZE ((size_t)16U * 1024U * 1024U)
#define SCRATCH_ARENA_SIZE ((size_t)4U * 1024U * 1024U)

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

static char s_status[256];
static char s_exe_dir[1024];
// #endregion

// #region ui ids + menu state
static uint32_t s_id_btn_pack, s_id_btn_export, s_id_btn_refresh, s_id_vlist, s_id_modal, s_id_about;
static uint32_t s_id_rename; /* the single inline rename input (one edit active at a time) */
static bool s_ids_ready;

static uint32_t s_id_mb_file, s_id_mb_edit, s_id_mb_view, s_id_mb_help;
static uint32_t s_id_menu_file, s_id_menu_edit, s_id_menu_view, s_id_menu_help;
static nt_ui_menu_state_t s_file_state, s_edit_state, s_view_state, s_help_state;
static nt_ui_menu_ctx_t s_file_menu, s_edit_menu, s_view_menu, s_help_menu;
enum {
    MK_NEW = 1, MK_OPEN, MK_SAVE, MK_SAVEAS, MK_REFRESH, MK_EXIT,
    MK_UNDO, MK_REDO,
    MK_ZIN, MK_ZOUT, MK_FIT, MK_ABOUT, MK_S100, MK_S125, MK_S150, MK_S200,
    MK_CTX_RENAME, MK_CTX_REMOVE
};

/* Right-click context menu: one cursor-anchored menu whose items depend on the row a
 * right-click armed it over (§3.3e mouse-complete access). Its actions call the same code
 * paths as the [x] buttons / inline editors. */
static uint32_t s_id_ctx_menu;
static nt_ui_menu_state_t s_ctx_state;
static nt_ui_menu_ctx_t s_ctx_menu;
enum { CTX_NONE = 0, CTX_ATLAS, CTX_SPRITE };
static int s_ctx_kind;
static int s_ctx_atlas;         /* CTX_ATLAS target index */
static int s_ctx_src = -1;      /* CTX_SPRITE source index (for Remove) */
static char s_ctx_sprite[192];  /* CTX_SPRITE override key (for Rename) */
static bool s_ctx_leaf;         /* a renamable leaf sprite (file source or folder child) */
static bool s_ctx_removable;    /* a removable source row (has an [x] today) */
// #endregion

// #region editor state
static gui_canvas s_canvas;

static int s_sel_atlas;      /* selected atlas index */
static int s_sel_src = -1;   /* selected source index within the atlas */
static int s_sel_child = -1; /* selected folder-child index (-1 = the source row / a file) */
static char s_sel_abs[512];  /* resolved absolute image path of the selection ("" = none/folder) */
static bool s_sel_missing;   /* selection is a missing file -> canvas shows a placeholder (§3.7) */

/* deferred side effects (dialogs + model mutations), applied at the top of the next frame */
static bool s_pending_open, s_pending_save, s_pending_save_as, s_pending_add_files, s_pending_add_folder, s_pending_add_atlas, s_pending_refresh;
static int s_pending_remove_atlas = -1;
static int s_pending_remove_source = -1;
enum { AFTER_NONE = 0, AFTER_NEW, AFTER_EXIT };
static int s_after_confirm;
static bool s_confirm_open;
static bool s_about_open;
enum { MODAL_NONE = 0, MODAL_SAVE, MODAL_DISCARD, MODAL_CANCEL };
static int s_modal_action;

/* Inline rename edit (F1): one active at a time. kind 0 none / 1 atlas / 2 sprite. */
enum { EDIT_NONE = 0, EDIT_ATLAS, EDIT_SPRITE };
static int s_edit_kind;
static int s_edit_atlas;         /* atlas being renamed (EDIT_ATLAS) */
static char s_edit_sprite[192];  /* atlas-relative sprite name being renamed (EDIT_SPRITE) */
static char s_edit_buf[192];     /* the input buffer */

/* Pack-button state cached for the tooltip pass (declared at root). */
static bool s_pack_has_sources, s_pack_stale;
static float s_content_w = 1280.0F; /* logical content width, for caption/status truncation */

/* Flattened sprite rows for the current atlas, rebuilt each frame. */
#define MAX_ROWS 4096
typedef struct sprite_row {
    int src;
    int child;
    int indent;
    bool is_source;
    bool is_folder;
    bool missing;             /* source path gone from disk (§3.7) */
    char label[224];          /* display label (rename-aware: "final (file.png)") */
    char sprite_name[192];    /* atlas-relative override key ("" for folders / missing) */
    char abs[512];
} sprite_row;
static sprite_row s_rows[MAX_ROWS];
static int s_row_count;

/* Per-frame collected row tooltips for TRUNCATED labels (full text on hover). */
#define MAX_ROW_TIPS 96
typedef struct row_tip {
    uint32_t id;
    char full[224];
} row_tip;
static row_tip s_row_tips[MAX_ROW_TIPS];
static int s_row_tip_count;
// #endregion

// #region small helpers
#if defined(__GNUC__) || defined(__clang__)
#define GUI_PRINTF(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define GUI_PRINTF(fmt_idx, args_idx)
#endif

static void set_status(const char *msg) { (void)snprintf(s_status, sizeof s_status, "%s", msg); }
static void set_statusf(const char *fmt, ...) GUI_PRINTF(1, 2);
static void set_statusf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_status, sizeof s_status, fmt, ap);
    va_end(ap);
}

static void normalize_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

static const char *path_last(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') {
            b = q + 1;
        }
    }
    return b;
}

static void path_stem(const char *p, char *buf, size_t cap) {
    (void)snprintf(buf, cap, "%s", path_last(p));
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf) {
        *dot = '\0';
    }
}

static void reset_selection(void) {
    s_sel_src = -1;
    s_sel_child = -1;
    s_sel_abs[0] = '\0';
    s_sel_missing = false;
}

static void cancel_edit(void) {
    s_edit_kind = EDIT_NONE;
    s_edit_atlas = -1;
    s_edit_sprite[0] = '\0';
    s_edit_buf[0] = '\0';
}

/* Truncate `src` with a trailing "..." so its rendered width at `size` fits `max_w` px.
 * Uniform font per row (no per-row shrink); returns true when it truncated. Font must be
 * bound (only called on the render path). */
static bool truncate_to_width(const char *src, float size, float max_w, char *out, size_t cap) {
    (void)snprintf(out, cap, "%s", src);
    if (max_w <= 1.0F || !s_font_bound) {
        return false;
    }
    const size_t n = strlen(out);
    if (nt_font_measure_n(s_font, out, n, size, 0.0F).width <= max_w) {
        return false;
    }
    const float ell_w = nt_font_measure_n(s_font, "...", 3U, size, 0.0F).width;
    if (ell_w >= max_w) {
        (void)snprintf(out, cap, "...");
        return true;
    }
    size_t len = n;
    while (len > 0U) {
        /* keep UTF-8 codepoints whole */
        while (len > 0U && ((unsigned char)out[len - 1U] & 0xC0U) == 0x80U) {
            len--;
        }
        if (len > 0U) {
            len--;
        }
        if (nt_font_measure_n(s_font, out, len, size, 0.0F).width + ell_w <= max_w) {
            break;
        }
    }
    (void)snprintf(out + len, cap - len, "...");
    return true;
}

/* Left-panel available text width for a row at `indent_px`, minus an optional [x] button. */
static float left_row_text_w(float indent_px, bool has_x) {
    float w = S(BASE_LEFT_PANEL_W - 24.0F) - indent_px - S(4.0F) - S(4.0F);
    if (has_x) {
        w -= S(24.0F + 4.0F);
    }
    return (w < S(20.0F)) ? S(20.0F) : w;
}

static void record_row_tip(uint32_t id, const char *full) {
    if (s_row_tip_count >= MAX_ROW_TIPS) {
        return;
    }
    s_row_tips[s_row_tip_count].id = id;
    (void)snprintf(s_row_tips[s_row_tip_count].full, sizeof s_row_tips[0].full, "%s", full);
    s_row_tip_count++;
}

/* Atlas-name validation (F1): non-empty, unique among atlases, and normalization-safe
 * (no path separators, not dots-only). Fills `err` on failure. */
static bool atlas_name_valid(const char *name, int self_idx, char *err, size_t cap) {
    if (!name || name[0] == '\0') {
        (void)snprintf(err, cap, "Atlas name cannot be empty");
        return false;
    }
    bool only_dots = true;
    for (const char *c = name; *c; c++) {
        if (*c == '/' || *c == '\\') {
            (void)snprintf(err, cap, "Atlas name cannot contain / or \\");
            return false;
        }
        if (*c != '.') {
            only_dots = false;
        }
    }
    if (only_dots) {
        (void)snprintf(err, cap, "Atlas name cannot be dots-only");
        return false;
    }
    tp_project *p = gui_project_get();
    for (int i = 0; p && i < p->atlas_count; i++) {
        if (i != self_idx && strcmp(p->atlases[i].name, name) == 0) {
            (void)snprintf(err, cap, "Atlas '%s' already exists", name);
            return false;
        }
    }
    return true;
}

static void clamp_selection(void) {
    tp_project *p = gui_project_get();
    if (!p || p->atlas_count == 0) {
        s_sel_atlas = 0;
        reset_selection();
        return;
    }
    if (s_sel_atlas >= p->atlas_count) {
        s_sel_atlas = p->atlas_count - 1;
    }
    if (s_sel_atlas < 0) {
        s_sel_atlas = 0;
    }
}
// #endregion

// #region file dialogs (tinyfiledialogs)
static void ensure_project_ext(const char *in, char *out, size_t cap) {
    (void)snprintf(out, cap, "%s", in);
    const char *base = path_last(out);
    if (strrchr(base, '.') == NULL) {
        size_t len = strlen(out);
        (void)snprintf(out + len, cap - len, ".ntpacker_project");
    }
}

static void do_open(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *path = tinyfd_openFileDialog("Open Project", "", 1, filt, "ntpacker project", 0);
    if (!path) {
        return;
    }
    if (!gui_scan_exists(path)) {
        set_statusf("project not found: %s", path); /* never fatal (F6b) */
        return;
    }
    char err[256];
    if (gui_project_open(path, err, sizeof err) == TP_STATUS_OK) {
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_statusf("Opened %s", gui_project_display_name());
    } else {
        set_statusf("Open failed: %s", err);
    }
}

static void do_save_as(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *def = gui_project_has_path() ? gui_project_path() : "untitled.ntpacker_project";
    const char *path = tinyfd_saveFileDialog("Save Project As", def, 1, filt, "ntpacker project");
    if (!path) {
        return;
    }
    char full[600];
    ensure_project_ext(path, full, sizeof full);
    char err[256];
    if (gui_project_save_as(full, err, sizeof err) == TP_STATUS_OK) {
        set_statusf("Saved %s", gui_project_display_name());
    } else {
        set_statusf("Save failed: %s", err);
    }
}

static void do_save(void) {
    if (!gui_project_has_path()) {
        do_save_as();
        return;
    }
    char err[256];
    if (gui_project_save(err, sizeof err) == TP_STATUS_OK) {
        set_statusf("Saved %s", gui_project_display_name());
    } else {
        set_statusf("Save failed: %s", err);
    }
}

static void do_add_files(void) {
    static const char *filt[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga"};
    const char *res = tinyfd_openFileDialog("Add Image Files", "", 5, filt, "image files", 1);
    if (!res) {
        return;
    }
    char buf[8192];
    (void)snprintf(buf, sizeof buf, "%s", res);
    int added = 0;
    int dup = 0;
    char *start = buf;
    for (;;) {
        char *bar = strchr(start, '|');
        if (bar) {
            *bar = '\0';
        }
        if (start[0] != '\0') {
            normalize_slashes(start);
            const gui_add_status r = gui_project_add_source(s_sel_atlas, start);
            if (r == GUI_ADD_ADDED) {
                added++;
            } else if (r == GUI_ADD_DUPLICATE) {
                dup++;
            }
        }
        if (!bar) {
            break;
        }
        start = bar + 1;
    }
    if (dup > 0) {
        set_statusf("Added %d file source(s); %d already added", added, dup);
    } else {
        set_statusf("Added %d file source(s)", added);
    }
}

static void do_add_folder(void) {
    const char *dir = tinyfd_selectFolderDialog("Add Folder", "");
    if (!dir) {
        return;
    }
    char norm[600];
    (void)snprintf(norm, sizeof norm, "%s", dir);
    normalize_slashes(norm);
    const gui_add_status r = gui_project_add_source(s_sel_atlas, norm);
    if (r == GUI_ADD_ADDED) {
        set_statusf("Added folder %s", path_last(norm));
    } else if (r == GUI_ADD_DUPLICATE) {
        set_statusf("already added: %s", path_last(norm));
    } else {
        set_status("Add folder failed.");
    }
}
// #endregion

// #region new/exit confirm flow
static void request_new(void) {
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_NEW;
        s_confirm_open = true;
    } else {
        gui_project_new();
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_status("New project.");
    }
}
static void request_exit(void) {
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_EXIT;
        s_confirm_open = true;
    } else {
        nt_app_quit();
    }
}
static void confirm_perform(void) {
    if (s_after_confirm == AFTER_NEW) {
        gui_project_new();
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_status("New project.");
    } else if (s_after_confirm == AFTER_EXIT) {
        nt_app_quit();
    }
    s_after_confirm = AFTER_NONE;
}
// #endregion

// #region undo/redo + refresh actions
static void do_undo(void) {
    if (gui_project_undo()) {
        cancel_edit();
        clamp_selection();
        reset_selection();
        gui_canvas_invalidate(&s_canvas);
        set_statusf("Undo (undo:%d redo:%d)", gui_history_undo_depth(), gui_history_redo_depth());
    } else {
        set_status("Nothing to undo.");
    }
}
static void do_redo(void) {
    if (gui_project_redo()) {
        cancel_edit();
        clamp_selection();
        reset_selection();
        gui_canvas_invalidate(&s_canvas);
        set_statusf("Redo (undo:%d redo:%d)", gui_history_undo_depth(), gui_history_redo_depth());
    } else {
        set_status("Nothing to redo.");
    }
}

/* Fingerprint every source (folders expand to their scanned children, files stat
 * directly) so a Refresh can diff added/removed/changed. Missing entries carry
 * size==-1 so a vanish/restore reads as removed/added. */
typedef struct fp_entry {
    char abs[512];
    long long size;
    long long mtime;
} fp_entry;

static void fp_collect(fp_entry **arr, int *count, int *cap) {
    tp_project *p = gui_project_get();
    for (int ai = 0; p && ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        for (int si = 0; si < a->source_count; si++) {
            char abs[512];
            if (tp_project_resolve_path(p, a->sources[si], abs, sizeof abs) != TP_STATUS_OK) {
                continue;
            }
            if (gui_scan_is_dir(abs)) {
                const gui_scan_result *sc = gui_scan_get(abs);
                for (int ci = 0; ci < sc->count; ci++) {
                    if (*count == *cap) {
                        int nc = *cap ? *cap * 2 : 64;
                        fp_entry *ne = (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
                        if (!ne) {
                            return;
                        }
                        *arr = ne;
                        *cap = nc;
                    }
                    (void)snprintf((*arr)[*count].abs, sizeof (*arr)[0].abs, "%s", sc->entries[ci].abs);
                    (*arr)[*count].size = sc->entries[ci].size;
                    (*arr)[*count].mtime = sc->entries[ci].mtime;
                    (*count)++;
                }
            } else {
                if (*count == *cap) {
                    int nc = *cap ? *cap * 2 : 64;
                    fp_entry *ne = (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
                    if (!ne) {
                        return;
                    }
                    *arr = ne;
                    *cap = nc;
                }
                long long sz = -1;
                long long mt = -1;
                (void)gui_scan_stat(abs, &sz, &mt);
                (void)snprintf((*arr)[*count].abs, sizeof (*arr)[0].abs, "%s", abs);
                (*arr)[*count].size = sz;
                (*arr)[*count].mtime = mt;
                (*count)++;
            }
        }
    }
}

static const fp_entry *fp_find(const fp_entry *arr, int n, const char *abs) {
    for (int i = 0; i < n; i++) {
        if (strcmp(arr[i].abs, abs) == 0) {
            return &arr[i];
        }
    }
    return NULL;
}

/* F4: rescan all sources, diff, evict the canvas cache, mark preview stale (NOT dirty). */
static void do_refresh(void) {
    fp_entry *before = NULL;
    int bn = 0;
    int bc = 0;
    fp_collect(&before, &bn, &bc);

    gui_scan_invalidate_all(); /* drop per-dir caches so fp_collect below rescans disk */

    fp_entry *after = NULL;
    int an = 0;
    int ac = 0;
    fp_collect(&after, &an, &ac);

    int added = 0;
    int removed = 0;
    int changed = 0;
    for (int i = 0; i < an; i++) {
        const fp_entry *b = fp_find(before, bn, after[i].abs);
        if (!b) {
            added++;
        } else if (b->size != after[i].size || b->mtime != after[i].mtime) {
            changed++;
        }
    }
    for (int i = 0; i < bn; i++) {
        if (!fp_find(after, an, before[i].abs)) {
            removed++;
        }
    }
    free(before);
    free(after);

    gui_canvas_invalidate(&s_canvas); /* force the shown image to reload (or show missing) */
    gui_project_mark_stale();         /* disk changed -> preview stale, project NOT dirtied */
    set_statusf("Refresh: +%d new, %d removed, %d changed", added, removed, changed);
}
// #endregion

// #region inline rename commit
static void commit_atlas_rename(void) {
    char err[128];
    if (!atlas_name_valid(s_edit_buf, s_edit_atlas, err, sizeof err)) {
        set_status(err); /* keep editing on invalid input */
        return;
    }
    if (gui_project_set_atlas_name(s_edit_atlas, s_edit_buf)) {
        set_statusf("Renamed atlas to '%s'", s_edit_buf);
    }
    cancel_edit();
}
static void commit_sprite_rename(void) {
    /* empty input clears the override back to the file-derived name */
    if (gui_project_set_sprite_rename(s_sel_atlas, s_edit_sprite, s_edit_buf)) {
        if (s_edit_buf[0] == '\0') {
            set_statusf("Cleared rename on '%s'", s_edit_sprite);
        } else {
            set_statusf("Renamed '%s' -> '%s'", s_edit_sprite, s_edit_buf);
        }
    }
    cancel_edit();
}
// #endregion

// #region deferred side-effects (run at the top of the frame, between frames)
static void apply_pending(void) {
    if (s_modal_action == MODAL_SAVE) {
        do_save();
        s_confirm_open = false;
        if (!gui_project_is_dirty()) {
            confirm_perform();
        } else {
            s_after_confirm = AFTER_NONE; /* save cancelled -> abort the pending action */
        }
    } else if (s_modal_action == MODAL_DISCARD) {
        s_confirm_open = false;
        confirm_perform();
    } else if (s_modal_action == MODAL_CANCEL) {
        s_confirm_open = false;
        s_after_confirm = AFTER_NONE;
    }
    s_modal_action = MODAL_NONE;

    if (s_pending_open) {
        do_open();
    }
    if (s_pending_save) {
        do_save();
    }
    if (s_pending_save_as) {
        do_save_as();
    }
    if (s_pending_add_files) {
        do_add_files();
    }
    if (s_pending_add_folder) {
        do_add_folder();
    }
    if (s_pending_add_atlas) {
        int idx = gui_project_add_atlas();
        if (idx >= 0) {
            s_sel_atlas = idx;
            reset_selection();
            set_statusf("Added atlas '%s'", tp_project_get_atlas(gui_project_get(), idx)->name);
        }
    }
    if (s_pending_remove_source >= 0) {
        gui_project_remove_source(s_sel_atlas, s_pending_remove_source);
        reset_selection();
        set_status("Removed source (Ctrl+Z to undo).");
    }
    if (s_pending_remove_atlas >= 0) {
        gui_project_remove_atlas(s_pending_remove_atlas);
        clamp_selection();
        reset_selection();
        set_status("Removed atlas (Ctrl+Z to undo).");
    }
    if (s_pending_refresh) {
        do_refresh();
    }

    s_pending_open = s_pending_save = s_pending_save_as = false;
    s_pending_add_files = s_pending_add_folder = s_pending_add_atlas = false;
    s_pending_refresh = false;
    s_pending_remove_source = -1;
    s_pending_remove_atlas = -1;
}
// #endregion

// #region row model
/* Strip only a trailing extension on the basename, keeping any folder prefix (so a
 * folder child's override key is atlas-relative, e.g. "tank/walk_01"). */
static void strip_ext(const char *in, char *out, size_t cap) {
    (void)snprintf(out, cap, "%s", in);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && dot != out && (!slash || dot > slash)) {
        *dot = '\0';
    }
}

/* Rename-aware display label: a renamed sprite shows "final (file.png)" so the mapping
 * stays visible; otherwise the file-derived base label. */
static void row_display(tp_project_atlas *a, const char *sprite_name, const char *base_label, const char *paren, char *out, size_t cap) {
    const tp_project_sprite *ov = tp_project_atlas_find_sprite(a, sprite_name);
    if (ov && ov->rename) {
        (void)snprintf(out, cap, "%s (%s)", ov->rename, paren);
    } else {
        (void)snprintf(out, cap, "%s", base_label);
    }
}

static void build_rows(tp_project *proj, tp_project_atlas *a) {
    s_row_count = 0;
    if (!a) {
        return;
    }
    for (int si = 0; si < a->source_count && s_row_count < MAX_ROWS; si++) {
        const char *sp = a->sources[si];
        char abs[512];
        if (tp_project_resolve_path(proj, sp, abs, sizeof abs) != TP_STATUS_OK) {
            abs[0] = '\0';
        }
        const bool exists = gui_scan_exists(abs);
        const bool is_dir = exists && gui_scan_is_dir(abs);
        sprite_row *r = &s_rows[s_row_count++];
        memset(r, 0, sizeof *r);
        r->src = si;
        r->child = -1;
        r->is_source = true;
        r->is_folder = is_dir;
        r->indent = 0;
        if (!exists) { /* missing source: row stays, warning badge, selectable (§3.7) */
            r->missing = true;
            (void)snprintf(r->label, sizeof r->label, "(!) %s", path_last(sp));
            (void)snprintf(r->abs, sizeof r->abs, "%s", abs);
        } else if (is_dir) {
            (void)snprintf(r->label, sizeof r->label, "%s/", path_last(sp));
            r->abs[0] = '\0';
            const gui_scan_result *sc = gui_scan_get(abs);
            for (int ci = 0; ci < sc->count && s_row_count < MAX_ROWS; ci++) {
                sprite_row *cr = &s_rows[s_row_count++];
                memset(cr, 0, sizeof *cr);
                cr->src = si;
                cr->child = ci;
                cr->is_source = false;
                cr->indent = 1;
                strip_ext(sc->entries[ci].rel, cr->sprite_name, sizeof cr->sprite_name);
                row_display(a, cr->sprite_name, sc->entries[ci].rel, path_last(sc->entries[ci].rel), cr->label, sizeof cr->label);
                (void)snprintf(cr->abs, sizeof cr->abs, "%s", sc->entries[ci].abs);
            }
        } else { /* file source: a leaf sprite */
            char stem[192];
            path_stem(sp, stem, sizeof stem);
            (void)snprintf(r->sprite_name, sizeof r->sprite_name, "%s", stem);
            row_display(a, r->sprite_name, stem, path_last(sp), r->label, sizeof r->label);
            (void)snprintf(r->abs, sizeof r->abs, "%s", abs);
        }
    }
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
    s_id_modal = nt_ui_id("ntpacker/confirm_modal");
    s_id_about = nt_ui_id("ntpacker/about_modal");
    s_id_rename = nt_ui_id("ntpacker/rename_input");
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
    s_rename_input.skin[NT_UI_INPUT_IDLE].bg_color = 0xFF2A2E38U;
    s_rename_input.skin[NT_UI_INPUT_FOCUSED].bg_color = 0xFF343A46U;
    s_rename_input.skin[NT_UI_INPUT_FOCUSED].border_color = 0xFFA0764AU;
    s_ids_ready = true;
}

/* Multiplies every style's scale-dependent field by g_ui_scale. Runs each frame so a
 * runtime UI-scale change (View > UI Scale) takes effect immediately; text stays crisp
 * because the font is Slug vector text (resolution-independent -- no atlas re-bake needed). */
static void apply_ui_scale(void) {
    g_title = g_title_base;
    g_title.font_size = S(FS_TITLE);
    g_body = g_body_base;
    g_body.font_size = S(FS_BODY);
    g_row = g_row_base;
    g_row.font_size = S(FS_ROW);
    g_caption = g_caption_base;
    g_caption.font_size = S(FS_CAPTION);
    g_canvas_hint = g_canvas_hint_base;
    g_canvas_hint.font_size = S(FS_HINT);
    g_tag = g_tag_base;
    g_tag.font_size = S(FS_TAG);
    g_warn = g_warn_base;
    g_warn.font_size = S(FS_ROW);

    s_rename_input.text.font_size = S(FS_ROW);
    s_rename_input.placeholder.font_size = S(FS_ROW);
    s_rename_input.pad_x = S(6.0F);

    s_menu_style.font_size = S(15.0F);
    s_menu_style.item_height = Su(28.0F);
    s_menu_style.min_width = Su(180.0F);
    s_tip_style.font_size = S(14.0F);
    s_tip_style.max_width = Su(360.0F);
    s_tip_style.pad = Su(8.0F);
}

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

// #region generic widgets
static bool ui_btn(nt_ui_context_t *ctx, uint32_t id, const char *text, nt_ui_button_style_t *style, bool enabled,
                   float w, float h, const nt_ui_label_style_t *lbl) {
    Clay_SizingAxis wx = (w > 0.0F) ? CLAY_SIZING_FIXED(S(w)) : CLAY_SIZING_FIT(0);
    nt_ui_button_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), id, style,
                       &(Clay_ElementDeclaration){.layout = {.sizing = {wx, CLAY_SIZING_FIXED(S(h))},
                                                             .padding = {Su(10), Su(10), Su(4), Su(4)},
                                                             .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                                  .cornerRadius = CLAY_CORNER_RADIUS(S(6))},
                       enabled, NULL);
    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), text, lbl);
    return nt_ui_button_end(ctx) && enabled;
}

/* Ellipsis-truncated label so text never draws past `max_w`; records a hover tooltip
 * (full text) against `tip_id` when it truncated (tip_id 0 = no tooltip). */
static void ui_label_fit(nt_ui_context_t *ctx, const char *text, const nt_ui_label_style_t *lbl, float max_w, uint32_t tip_id) {
    char buf[256];
    const bool cut = truncate_to_width(text, lbl->font_size, max_w, buf, sizeof buf);
    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), buf, lbl);
    if (cut && tip_id != 0U) {
        record_row_tip(tip_id, text);
    }
}
// #endregion

// #region menu bar
static void close_menubar_menus(void) {
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
        s_pending_open = true;
    }
    if (nt_ui_menu_item_ex(m, MK_SAVE, "Save", (nt_ui_menu_item_opts_t){.shortcut = "Ctrl+S"})) {
        s_pending_save = true;
    }
    if (nt_ui_menu_item_ex(m, MK_SAVEAS, "Save As...", (nt_ui_menu_item_opts_t){.shortcut = "Ctrl+Shift+S"})) {
        s_pending_save_as = true;
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
/* Radio-style UI-scale item. The ASCII font has no check glyph, so the active one is marked
 * with an ASCII "(on)" rather than the menu's built-in checkmark. */
static void scale_item(nt_ui_menu_ctx_t *m, uint32_t key, const char *pct, float value) {
    const bool active = (g_ui_scale > (value - 0.01F)) && (g_ui_scale < (value + 0.01F));
    char buf[28];
    (void)snprintf(buf, sizeof buf, active ? "%s  (on)" : "%s", pct);
    if (nt_ui_menu_item(m, key, buf)) {
        g_ui_scale = value; /* TODO: persist in an app-settings file (not the project) later */
        set_statusf("UI scale %s", pct);
    }
}
static void view_items(nt_ui_menu_ctx_t *m) {
    if (nt_ui_menu_item(m, MK_ZIN, "Zoom In")) {
        set_status("Zoom -- lands with the atlas-page canvas.");
    }
    if (nt_ui_menu_item(m, MK_ZOUT, "Zoom Out")) {
        set_status("Zoom -- lands with the atlas-page canvas.");
    }
    if (nt_ui_menu_item(m, MK_FIT, "Fit")) {
        set_status("Fit -- source preview already fits to canvas.");
    }
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
          .backgroundColor = C_STATUS,
          .cornerRadius = CLAY_CORNER_RADIUS(S(6))}) {
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

// #region toolbar (Pack / Export)
static void declare_toolbar(nt_ui_context_t *ctx) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    s_pack_has_sources = a && a->source_count > 0;
    s_pack_stale = gui_project_is_stale();
    const bool accent = s_pack_has_sources && s_pack_stale;

    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_TOOLBAR_H))},
                     .padding = {Su(10), Su(10), Su(6), Su(6)},
                     .childGap = Su(10),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_PANEL,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        if (ui_btn(ctx, s_id_btn_pack, accent ? "Pack  (stale)" : "Pack", accent ? &g_btn_accent : &g_btn,
                   s_pack_has_sources, 132.0F, 36.0F, &g_body)) {
            set_status("In-process packing blocked by neotolis-engine#282 -- see README.");
        }
        if (ui_btn(ctx, s_id_btn_export, "Export All", &g_btn, s_pack_has_sources, 122.0F, 36.0F, &g_body)) {
            set_status("Exporters land in Phase 2.");
        }
        if (ui_btn(ctx, s_id_btn_refresh, "Refresh", &g_btn_ghost, true, 100.0F, 36.0F, &g_body)) {
            s_pending_refresh = true;
        }
    }
}
// #endregion

// #region left panel (atlases + sprites)
static const nt_ui_input_props_t s_rename_props = {
    .placeholder = "name", .allow = NULL, .max_length = 0U, .keyboard = NT_UI_KB_TEXT, .password = false};
static const nt_ui_events_cfg_t s_dbl_cfg = {.long_press_secs = 0.0F, .double_click = true};

static void start_atlas_edit(int i) {
    tp_project *p = gui_project_get();
    if (!p || i < 0 || i >= p->atlas_count) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_ATLAS;
    s_edit_atlas = i;
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", p->atlases[i].name);
    set_status("Rename atlas: type, Enter to commit, Esc to cancel.");
}
static void start_sprite_edit_named(const char *sprite_name) {
    if (!sprite_name || sprite_name[0] == '\0') {
        return;
    }
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    cancel_edit();
    s_edit_kind = EDIT_SPRITE;
    (void)snprintf(s_edit_sprite, sizeof s_edit_sprite, "%s", sprite_name);
    const tp_project_sprite *ov = a ? tp_project_atlas_find_sprite(a, sprite_name) : NULL;
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", (ov && ov->rename) ? ov->rename : "");
    set_status("Rename region: type, Enter to commit, Esc clears/cancels.");
}
static void start_sprite_edit(const sprite_row *row) {
    if (!row || row->is_folder || row->missing || row->sprite_name[0] == '\0') {
        return;
    }
    start_sprite_edit_named(row->sprite_name);
}

/* The single inline rename field, sized to fill its (bounded) parent so it clips to the row. */
static bool render_rename_field(nt_ui_context_t *ctx) {
    bool submitted = false;
    const Clay_ElementDeclaration decl = {
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 5.0F))}}};
    (void)nt_ui_input_text(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_rename, s_edit_buf, sizeof s_edit_buf,
                           &s_rename_props, &s_rename_input, &decl, true, &submitted);
    return submitted;
}

static void declare_atlas_list(nt_ui_context_t *ctx, tp_project *proj) {
    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "ATLASES", &g_title);
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
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (editing) {
                    if (render_rename_field(ctx)) {
                        commit_atlas_rename();
                    }
                } else {
                    ui_label_fit(ctx, proj->atlases[i].name, &g_row, left_row_text_w(S(8.0F), has_x), row_id);
                }
            }
            if (has_x) {
                (void)ui_btn(ctx, x_id, "x", &g_btn_ghost, true, 24.0F, 22.0F, &g_caption);
            }
        }
    }
    if (ui_btn(ctx, nt_ui_id("ntpacker/add_atlas"), "+ Atlas", &g_btn_ghost, true, 0.0F, 26.0F, &g_caption)) {
        s_pending_add_atlas = true;
    }
}

static void declare_sprite_list(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(28))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "SPRITES", &g_title);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        if (ui_btn(ctx, nt_ui_id("ntpacker/add_files"), "+ Files", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            s_pending_add_files = true;
        }
        if (ui_btn(ctx, nt_ui_id("ntpacker/add_folder"), "+ Folder", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            s_pending_add_folder = true;
        }
    }

    if (s_row_count == 0) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No sources. Use + Files / + Folder.", &g_caption);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        return;
    }

    nt_ui_vlist_style_t vs = nt_ui_vlist_style_defaults();
    vs.overscan = 3;
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
            const bool selected = (row->is_source ? (s_sel_src == row->src && s_sel_child == -1)
                                                  : (s_sel_src == row->src && s_sel_child == row->child));
            const nt_ui_events_t ev = nt_ui_events(ctx, hit_id, &s_dbl_cfg);
            bool x_clicked = false;
            if (row->is_source) {
                const nt_ui_events_t xev = nt_ui_events(ctx, x_id, NULL);
                x_clicked = xev.clicked;
                if (x_clicked) {
                    s_pending_remove_source = row->src;
                }
            }
            if (ev.double_clicked && !row->is_folder && !row->missing) {
                s_sel_src = row->src;
                s_sel_child = row->child;
                s_sel_missing = false;
                (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", row->abs);
                start_sprite_edit(row);
            } else if (ev.clicked && !x_clicked) {
                s_sel_src = row->src;
                s_sel_child = row->child;
                s_sel_missing = row->missing;
                (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", row->abs);
            }
            if (nt_ui_menu_open_trigger(ctx, s_id_ctx_menu, hit_id, false, &s_ctx_state)) {
                close_menubar_menus();
                s_sel_src = row->src; /* right-click selects the row first */
                s_sel_child = row->child;
                s_sel_missing = row->missing;
                (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", row->abs);
                s_ctx_kind = CTX_SPRITE;
                s_ctx_src = row->src;
                (void)snprintf(s_ctx_sprite, sizeof s_ctx_sprite, "%s", row->sprite_name);
                s_ctx_leaf = (!row->is_folder && !row->missing && row->sprite_name[0] != '\0');
                s_ctx_removable = row->is_source;
            }
            const Clay_Color bg = selected ? C_SEL : (ev.hovered ? C_HOVER : C_TRANSPARENT);
            const uint16_t indent = Su(8.0F + ((float)row->indent * 16.0F));
            const nt_ui_label_style_t *lbl = row->missing ? &g_warn : (row->is_folder ? &g_body : &g_row);
            CLAY({.id = {.id = row_id},
                  .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                             .padding = {indent, Su(4), 0, 0},
                             .childGap = Su(4),
                             .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = bg,
                  .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
                CLAY({.id = {.id = hit_id},
                      .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    if (editing) {
                        if (render_rename_field(ctx)) {
                            commit_sprite_rename();
                        }
                    } else {
                        ui_label_fit(ctx, row->label, lbl, left_row_text_w(S(8.0F + (float)row->indent * 16.0F), row->is_source), hit_id);
                    }
                }
                if (row->is_source) {
                    (void)ui_btn(ctx, x_id, "x", &g_btn_ghost, true, 24.0F, 22.0F, &g_caption);
                }
            }
        }
    }
    nt_ui_vlist_end(ctx);
}

static void declare_left_panel(nt_ui_context_t *ctx) {
    tp_project *proj = gui_project_get();
    s_row_tip_count = 0; /* per-frame; filled by ui_label_fit when a row truncates */
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(BASE_LEFT_PANEL_W)), CLAY_SIZING_GROW(0)},
                     .padding = {Su(12), Su(12), Su(12), Su(12)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
          .backgroundColor = C_PANEL,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        declare_atlas_list(ctx, proj);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(1))}}, .backgroundColor = C_BORDER}) {}
        declare_sprite_list(ctx);
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
static void declare_canvas(nt_ui_context_t *ctx) {
    const bool has_img = gui_canvas_has_image(&s_canvas);
    char label[256];
    if (has_img) {
        (void)snprintf(label, sizeof label, "%s  --  %d x %d", path_last(s_sel_abs), gui_canvas_img_w(&s_canvas),
                       gui_canvas_img_h(&s_canvas));
    } else if (s_sel_missing) {
        (void)snprintf(label, sizeof label, "file missing: %s", s_sel_abs);
    } else {
        (void)snprintf(label, sizeof label, "No image selected");
    }
    const float cap_w = s_content_w - S(BASE_LEFT_PANEL_W) - S(60.0F);

    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                     .padding = {Su(10), Su(10), Su(10), Su(10)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(8),
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_CANVAS,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        /* view area: the custom-drawn source image, OR a missing/placeholder + the stale tag */
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(8),
                         .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}}}) {
            if (has_img) {
                nt_ui_custom(ctx, NT_UI_DATA_LAYER(LAYER_IMG), &s_canvas);
            } else if (s_sel_missing) {
                ui_label_fit(ctx, label, &g_warn, cap_w, 0U);
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Restore the file and press Refresh (F5) to bring it back.", &g_caption);
            } else {
                /* atlas-preview placeholder -> the "outdated" tag lives here (not over a source view) */
                if (s_pack_stale && s_pack_has_sources) {
                    CLAY({.layout = {.padding = {Su(8), Su(8), Su(3), Su(3)}}, .backgroundColor = C_TAG, .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
                        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "outdated -- press Pack", &g_tag);
                    }
                }
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No atlas preview yet -- packing is blocked by engine #282.", &g_canvas_hint);
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Select a sprite on the left to preview its source image.", &g_caption);
            }
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(22))}, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}}}) {
            ui_label_fit(ctx, label, &g_caption, cap_w, 0U);
        }
    }
}
// #endregion

// #region status bar + menus + tooltips
static void declare_statusbar(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_STATUSBAR_H))},
                     .padding = {Su(12), Su(12), Su(4), Su(4)},
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_STATUS,
          .cornerRadius = CLAY_CORNER_RADIUS(S(6))}) {
        ui_label_fit(ctx, s_status, &g_caption, s_content_w - S(40.0F), 0U); /* clip, never wrap/overflow */
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
        if (s_ctx_removable) {
            if (nt_ui_menu_item(&s_ctx_menu, MK_CTX_REMOVE, "Remove")) {
                s_pending_remove_source = s_ctx_src;
            }
        }
    }
    nt_ui_menu_end(&s_ctx_menu);
}

static void declare_tooltips(nt_ui_context_t *ctx) {
    const char *pack_tip = s_pack_stale
        ? "Pack (Ctrl+P): repack now with current settings and refresh the preview. Sources or settings changed -- press to repack. (Packing is blocked until engine fix #282 -- coming soon.)"
        : "Pack (Ctrl+P): repack now with current settings (session preview only, no files exported). (Packing is blocked until engine fix #282 -- coming soon.)";
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_btn_pack, pack_tip, &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_btn_export,
                        "Export All (Ctrl+E): pack per enabled target and write each target's files to its output path. Exporters land in Phase 2.",
                        &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_btn_refresh,
                        "Refresh (F5): rescan all source folders/files from disk; updates the sprite list and marks the preview stale.",
                        &s_tip_style);
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
                if (ui_btn(ctx, nt_ui_id("ntpacker/modal_save"), "Save", &g_btn_accent, true, 100.0F, 34.0F, &g_body)) {
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
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), NTPACKER_REPO_URL, &g_caption);
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(4))}}}) {}
            CLAY({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (ui_btn(ctx, nt_ui_id("ntpacker/about_ok"), "OK", &g_btn_accent, true, 100.0F, 34.0F, &g_body)) {
                    s_about_open = false;
                }
            }
        }
        nt_ui_modal_end(ctx);
    }
}
// #endregion

// #region self-test (headless smoke of the project ops; OFF by default)
#ifdef NTPACKER_GUI_SELFTEST
static void to_abs(const char *rel, char *out, size_t cap) {
#ifdef _WIN32
    if (GetFullPathNameA(rel, (DWORD)cap, out, NULL) == 0) {
        (void)snprintf(out, cap, "%s", rel);
    }
    normalize_slashes(out);
#else
    (void)snprintf(out, cap, "%s", rel);
#endif
}

static void run_selftest(void) {
    nt_log_info("SELFTEST: begin");
    gui_project_init();
    tp_project *p = gui_project_get();
    NT_ASSERT(p && p->atlas_count == 1);
    (void)p;

    /* Absolute paths (from cwd=workspace) so they survive relativize-on-save + resolve-on-load. */
    char folder[512];
    char file[512];
    to_abs("examples/defold-demo/examples/anim_trim/anims", folder, sizeof folder);
    to_abs("examples/defold-demo/examples/anim_trim/anims/sq1.png", file, sizeof file);

    const gui_add_status a1 = gui_project_add_source(0, folder);
    nt_log_info("SELFTEST: add folder -> %d (dirty=%d stale=%d)", (int)a1, gui_project_is_dirty(), gui_project_is_stale());
    const gui_add_status a2 = gui_project_add_source(0, file);
    nt_log_info("SELFTEST: add file -> %d", (int)a2);
    const gui_add_status a3 = gui_project_add_source(0, folder); /* dedupe (F6c): expect DUPLICATE(2) */
    nt_log_info("SELFTEST: dedupe add folder again -> %d (expect %d)", (int)a3, (int)GUI_ADD_DUPLICATE);

    char err[256] = {0};
    const bool dec = gui_canvas_set_image(&s_canvas, file, err, sizeof err);
    nt_log_info("SELFTEST: decode+upload -> %d (%dx%d) %s", dec, gui_canvas_img_w(&s_canvas), gui_canvas_img_h(&s_canvas), dec ? "" : err);

    char save_path[1200];
    (void)snprintf(save_path, sizeof save_path, "%s/selftest.ntpacker_project", s_exe_dir);
    tp_status st = gui_project_save_as(save_path, err, sizeof err);
    nt_log_info("SELFTEST: save '%s' -> %s (dirty=%d)", save_path, tp_status_str(st), gui_project_is_dirty());

    st = gui_project_open(save_path, err, sizeof err);
    const int nsrc = gui_project_get() ? gui_project_get()->atlases[0].source_count : -1;
    nt_log_info("SELFTEST: reload -> %s, atlas0 sources=%d (dirty=%d)", tp_status_str(st), nsrc, gui_project_is_dirty());

    /* --- rename atlas + undo/redo (verify model reverts and dirty recomputes) --- */
    char name0[64];
    (void)snprintf(name0, sizeof name0, "%s", gui_project_get()->atlases[0].name);
    gui_project_set_atlas_name(0, "hero_atlas");
    nt_log_info("SELFTEST: rename atlas '%s' -> '%s' (dirty=%d)", name0, gui_project_get()->atlases[0].name, gui_project_is_dirty());
    const bool undone = gui_project_undo();
    nt_log_info("SELFTEST: undo -> %d name='%s' (dirty=%d) [expect name reverted, dirty=0]", undone, gui_project_get()->atlases[0].name, gui_project_is_dirty());
    const bool redone = gui_project_redo();
    nt_log_info("SELFTEST: redo -> %d name='%s' (dirty=%d)", redone, gui_project_get()->atlases[0].name, gui_project_is_dirty());

    /* --- rename a region (sprite override), verify it is stored on the model --- */
    char folder_abs[512];
    if (tp_project_resolve_path(gui_project_get(), gui_project_get()->atlases[0].sources[0], folder_abs, sizeof folder_abs) == TP_STATUS_OK) {
        const gui_scan_result *sc = gui_scan_get(folder_abs);
        nt_log_info("SELFTEST: folder scan found %d image(s)", sc->count);
        if (sc->count > 0) {
            char sprite[192];
            (void)snprintf(sprite, sizeof sprite, "%s", sc->entries[0].rel);
            char *dot = strrchr(sprite, '.');
            if (dot) {
                *dot = '\0';
            }
            gui_project_set_sprite_rename(0, sprite, "renamed_region");
            tp_project_atlas *a0 = tp_project_get_atlas(gui_project_get(), 0);
            const tp_project_sprite *ov = tp_project_atlas_find_sprite(a0, sprite);
            nt_log_info("SELFTEST: rename region '%s' -> override='%s'", sprite, (ov && ov->rename) ? ov->rename : "(none)");
        }
    }

    /* --- save_buffer / load_buffer round-trip in-app --- */
    char *bb = NULL;
    size_t bl = 0;
    tp_error be = {0};
    const tp_status bst = tp_project_save_buffer(gui_project_get(), &bb, &bl, &be);
    tp_project *lp = NULL;
    tp_error le = {0};
    const tp_status lst = (bst == TP_STATUS_OK) ? tp_project_load_buffer(bb, bl, &lp, &le) : bst;
    nt_log_info("SELFTEST: save_buffer(%zuB)->%s; load_buffer->%s atlas0='%s'", bl, tp_status_str(bst), tp_status_str(lst),
                (lp && lp->atlas_count > 0) ? lp->atlases[0].name : "(none)");
    tp_project_destroy(lp);
    free(bb);

    /* --- refresh cycle: create + delete a temp png, observe the scan change --- */
    char rdir[600];
    char rfile[700];
    (void)snprintf(rdir, sizeof rdir, "%s/selftest_refresh", s_exe_dir);
#ifdef _WIN32
    (void)CreateDirectoryA(rdir, NULL);
#endif
    (void)snprintf(rfile, sizeof rfile, "%s/temp.png", rdir);
    FILE *tf = fopen(rfile, "wb");
    if (tf) {
        (void)fputs("PNGDATA", tf);
        (void)fclose(tf);
    }
    gui_scan_invalidate_all();
    const int before_n = gui_scan_get(rdir)->count;
    (void)remove(rfile);
    gui_scan_invalidate_all();
    const int after_n = gui_scan_get(rdir)->count;
    nt_log_info("SELFTEST: refresh cycle temp png before=%d after=%d (removed=%d)", before_n, after_n, before_n - after_n);
#ifdef _WIN32
    (void)RemoveDirectoryA(rdir);
#endif

    /* --- About modal: open it so the auto-quit frames render it (OK/Esc close it interactively) --- */
    s_about_open = true;
    nt_log_info("SELFTEST: About modal opened=%d", s_about_open);

    /* Leave a live selection so the auto-quit frames draw the decoded image. */
    tp_project *cur = gui_project_get();
    const int ns = cur ? cur->atlases[0].source_count : 0;
    if (cur && ns > 0) {
        char resolved[512];
        if (tp_project_resolve_path(cur, cur->atlases[0].sources[ns - 1], resolved, sizeof resolved) == TP_STATUS_OK) {
            (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", resolved);
            s_sel_atlas = 0;
            s_sel_src = ns - 1;
            s_sel_child = -1;
            s_sel_missing = false;
        }
    }
    g_ui_scale = 1.5F; /* exercise the scaled layout during the auto-quit frames */
    nt_log_info("SELFTEST: end (undo:%d redo:%d history:%zuB; selection '%s')", gui_history_undo_depth(),
                gui_history_redo_depth(), gui_history_bytes(), s_sel_abs);
}
#endif
// #endregion

// #region keyboard shortcuts (ux.md §3.3d)
/* Global shortcuts routed through the SAME actions as the menus. Text-input focus swallows
 * them first (no accidental global actions while typing); an open modal blocks them too. */
static void handle_shortcuts(void) {
    if (nt_ui_input_any_focused(s_ctx) || s_confirm_open || s_about_open) {
        return;
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
        s_pending_open = true;
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
        set_status("In-process packing blocked by neotolis-engine#282 -- see README.");
    } else if (nt_input_key_is_pressed(NT_KEY_E)) {
        set_status("Export All: exporters land in Phase 2.");
    }
}
// #endregion

// #region frame
static void frame(void) {
    nt_window_poll();
    nt_input_poll();
    nt_mem_scratch_reset();
    gui_project_tick(g_nt_app.time); /* history coalescing clock */

#ifdef NTPACKER_GUI_SELFTEST
    /* Verification build: render a few real frames (proves the canvas draw + walk), then quit
     * cleanly so the process returns 0 without interaction. */
    static float s_selftest_elapsed = 0.0F;
    s_selftest_elapsed += g_nt_app.dt;
    if (s_selftest_elapsed > 2.5F) {
        nt_app_quit();
    }
#endif

    /* dialogs + model mutations queued last frame run here, cleanly between frames */
    apply_pending();

    if (nt_input_key_is_pressed(NT_KEY_ESCAPE)) {
        if (s_edit_kind != EDIT_NONE) {
            cancel_edit();
            set_status("Rename cancelled.");
        } else if (s_about_open) {
            s_about_open = false;
        } else if (s_confirm_open) {
            s_confirm_open = false;
            s_after_confirm = AFTER_NONE;
        } else {
            close_all_menus();
        }
    }

    handle_shortcuts();

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

        clamp_selection();
        build_rows(gui_project_get(), tp_project_get_atlas(gui_project_get(), s_sel_atlas));
        s_content_w = scale.logical_w; /* for caption/status truncation */

        nt_ui_begin(s_ctx, scale.logical_w, scale.logical_h, g_nt_app.dt, &g_nt_input.pointers[0], 1);
        nt_ui_set_viewport(s_ctx, nt_ui_viewport_from_scale(&scale));

        CLAY({.id = CLAY_ID("root"),
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                         .padding = {Su(10), Su(10), Su(10), Su(10)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(8),
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
              .backgroundColor = C_BG}) {
            declare_menubar(s_ctx);
            declare_toolbar(s_ctx);
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = Su(8), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}}}) {
                declare_left_panel(s_ctx);
                declare_canvas(s_ctx);
            }
            declare_statusbar(s_ctx);
        }

        declare_row_tooltips(s_ctx);
        declare_menus(s_ctx);
        declare_context_menu(s_ctx);
        declare_tooltips(s_ctx);
        declare_confirm_modal(s_ctx);
        declare_about_modal(s_ctx);

        nt_ui_end(s_ctx);

        /* selection captured this frame -> refresh the canvas texture before the walk draws it */
        if (s_sel_missing) {
            gui_canvas_clear(&s_canvas); /* placeholder is drawn by declare_canvas (§3.7) */
        } else if (s_sel_abs[0] != '\0') {
            char err[256];
            if (!gui_canvas_set_image(&s_canvas, s_sel_abs, err, sizeof err)) {
                set_statusf("Decode failed: %s", err);
                s_sel_missing = true; /* show the missing placeholder instead of a blank canvas */
            }
        } else {
            gui_canvas_clear(&s_canvas);
        }

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

    nt_material_init(&(nt_material_desc_t){.max_materials = 2});
    nt_font_init(&(nt_font_desc_t){.max_fonts = 1});

    nt_sprite_renderer_desc_t sr_desc = nt_sprite_renderer_desc_defaults();
    nt_sprite_renderer_init(&sr_desc);
    nt_text_renderer_init();

    nt_ui_module_init();
    nt_ui_create_desc_t ui_desc = nt_ui_create_desc_defaults();
    ui_desc.max_elements = UI_MAX_ELEMENTS;
    ui_desc.state_slots = 512U;
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

    /* editor state + the canvas custom-draw handler (registered outside begin/end) */
    gui_canvas_init(&s_canvas);
    nt_ui_set_custom_handler(s_ctx, gui_canvas_handler, &s_canvas);
    gui_project_init();

    /* open a project passed on the command line (errors go to the status bar) */
    if (argc > 1) {
        char err[256];
        if (!gui_scan_exists(argv[1])) {
            set_statusf("project not found: %s", argv[1]); /* stale argv -> continue with untitled (F6b) */
        } else if (gui_project_open(argv[1], err, sizeof err) == TP_STATUS_OK) {
            set_statusf("Opened %s", gui_project_display_name());
        } else {
            set_statusf("Open '%s' failed: %s", argv[1], err);
        }
    } else {
        set_status("Ready. New project -- add files or a folder to start.");
    }

#ifdef NTPACKER_GUI_SELFTEST
    run_selftest();
#endif

    clamp_selection();
    nt_log_info("ntpacker-gui: starting (project editor; packing blocked by engine #282)");

    nt_app_run(frame);

    gui_canvas_shutdown(&s_canvas);
    gui_scan_shutdown();
    gui_project_shutdown();
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
