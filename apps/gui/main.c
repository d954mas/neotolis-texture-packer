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
#include "gui_history.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_scan.h"
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
#define BASE_RIGHT_PANEL_W 300.0F /* settings panel (regions F/G), fixed width, own scroll */
#define BASE_ROW_H 27.0F

/* Pack an sRGB triple into the engine's 0xAABBGGRR (opaque) -- clearer than hand-swizzling. */
#define RGBA8(r, g, b) ((uint32_t)0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

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
/* Hyperlink label (About repo link): link-blue so it reads as clickable; hover tint on the button
 * behind it is the extra affordance (no cursor-shape API). */
static const nt_ui_label_style_t g_link_base = {.font_id = 0, .font_size = FS_CAPTION, .color = {110.0F, 170.0F, 245.0F, 255.0F}};
/* Dimmed caption for stale stats (they describe the LAST pack, not current settings). */
static const nt_ui_label_style_t g_dim_base = {.font_id = 0, .font_size = FS_CAPTION, .color = {110.0F, 114.0F, 124.0F, 200.0F}};
static nt_ui_label_style_t g_title, g_body, g_row, g_caption, g_canvas_hint, g_tag, g_warn, g_link, g_dim; /* scaled each frame */
static nt_ui_label_style_t g_check;                                                                        /* checkbox tick glyph (scaled each frame) */

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
/* Stale/outdated chip: amber so it reads as "action needed"; clickable -> Pack. */
static nt_ui_button_style_t g_btn_stale = {
    .idle = {.bg_tint = 0xFF46629EU, .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = 0xFF5A78B4U, .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = 0xFF3A5088U, .scale = 0.97F, .offset_y = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = 0xFF46629EU, .scale = 1.0F, .opacity = 0.5F},
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
/* Link button: idle tint matches the panel (invisible box), faint lighten on hover/press so the
 * link-blue label reads as a hyperlink. bg_tint alpha must be non-zero (engine hides alpha=0). */
static nt_ui_button_style_t g_btn_link = {
    .idle = {.bg_tint = 0xFF2A221EU, .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = 0xFF3A322AU, .scale = 1.0F, .opacity = 1.0F},
    .pressed = {.bg_tint = 0xFF241E1AU, .scale = 0.99F, .opacity = 1.0F},
    .disabled = {.bg_tint = 0xFF2A221EU, .scale = 1.0F, .opacity = 0.4F},
    .transition_speed = 16.0F,
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

/* Right settings panel widgets (regions F/G). All draw with the app's single baked
 * WHITE region (s_white_ref) tinted per state -- checkbox/slider need art, dropdown
 * is flat-color. Sizes are rescaled each frame in apply_ui_scale. */
static nt_atlas_region_ref_t s_white_ref;
static nt_ui_dropdown_style_t s_dd_style;
static nt_ui_slider_style_t s_slider_style;
static nt_ui_input_style_t s_num_input;   /* numeric + short text fields */
static nt_ui_scroll_style_t s_panel_scroll;
// #endregion

// #region engine state
/* UI context capacities -- provisioned with generous headroom for a heavy desktop session
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
#define UI_STATE_SLOTS ((uint32_t)4096U)
#define UI_STATE_PROBE_MAX ((uint32_t)64U)
#define UI_ROW_ID_RING ((uint32_t)128U) /* per-row id recycle modulus; must exceed max visible rows */
#define UI_ARENA_SIZE ((size_t)24U * 1024U * 1024U)
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
static uint32_t s_id_canvas;      /* the atlas-page custom element (bbox drives zoom/pan/hit input) */
static uint32_t s_id_rename;      /* the single inline rename input (one edit active at a time) */
static uint32_t s_id_right_panel; /* settings-panel container (bbox: press-outside blurs focused inputs) */
static bool s_ids_ready;

static uint32_t s_id_mb_file, s_id_mb_edit, s_id_mb_view, s_id_mb_help;
static uint32_t s_id_menu_file, s_id_menu_edit, s_id_menu_view, s_id_menu_help;
static nt_ui_menu_state_t s_file_state, s_edit_state, s_view_state, s_help_state;
static nt_ui_menu_ctx_t s_file_menu, s_edit_menu, s_view_menu, s_help_menu;
enum {
    MK_NEW = 1, MK_OPEN, MK_SAVE, MK_SAVEAS, MK_REFRESH, MK_EXIT,
    MK_UNDO, MK_REDO,
    MK_ZIN, MK_ZOUT, MK_FIT, MK_ABOUT, MK_S100, MK_S125, MK_S150, MK_S200,
    MK_OV_OUTLINE, MK_OV_FRAME, MK_OV_TRIM, MK_OV_PIVOT, MK_CTX_FIT, MK_CTX_100,
    MK_CTX_RENAME, MK_CTX_REMOVE, MK_CTX_TOGGLE, MK_CTX_CREATE_ANIM, MK_CTX_PREVIEW
};

/* Right-click context menu: one cursor-anchored menu whose items depend on the row a
 * right-click armed it over (§3.3e mouse-complete access). Its actions call the same code
 * paths as the [x] buttons / inline editors. */
static uint32_t s_id_ctx_menu;
static nt_ui_menu_state_t s_ctx_state;
static nt_ui_menu_ctx_t s_ctx_menu;
enum { CTX_NONE = 0, CTX_ATLAS, CTX_SPRITE, CTX_CANVAS, CTX_TARGET, CTX_ANIM };
static int s_ctx_kind;
static int s_ctx_atlas;         /* CTX_ATLAS target index */
static int s_ctx_anim = -1;     /* CTX_ANIM animation index */
static int s_ctx_target = -1;   /* CTX_TARGET target index (enable/disable, remove) */
static int s_ctx_src = -1;      /* CTX_SPRITE source index (for Remove) */
static char s_ctx_sprite[192];  /* CTX_SPRITE override key (for Rename) */
static bool s_ctx_leaf;         /* a renamable leaf sprite (file source or folder child) */
static bool s_ctx_removable;    /* a removable source row (has an [x] today) */
// #endregion

// #region editor state
static gui_canvas s_canvas;
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

static int s_sel_atlas;      /* selected atlas index */
static int s_sel_src = -1;   /* selected source index within the atlas */
static int s_sel_child = -1; /* selected folder-child index (-1 = the source row / a file) */
static char s_sel_abs[512];  /* resolved absolute image path of the selection ("" = none/folder) */
static bool s_sel_missing;   /* selection is a missing file -> canvas shows a placeholder (§3.7) */

/* Multi-select set over leaf sprite NAMES (stable identity; rows rebuild each frame). Drives
 * "Create animation from selection" + the editor's "Add frames" (ux.md §3.7b). s_sel_src/child stays
 * the PRIMARY (last-clicked) selection for the region panel + canvas sync. */
#define MAX_MULTI_SEL 4096
static char s_multi_sel[MAX_MULTI_SEL][192];
static int s_multi_sel_count;
static int s_sel_anchor_row = -1; /* row index anchor for Shift-range selection */

/* Animation selection + editor state (ux.md §3.7b). */
static int s_sel_anim = -1;       /* selected animation index in the current atlas, -1 none */
static int s_sel_anim_frame = -1; /* selected frame row in the editor (for the Del hotkey), -1 none */

/* Animation preview player (canvas ANIM mode). s_preview_time is the master clock; the frame index is
 * a pure function of it (gui_canvas_anim_frame_at), so play/pause/step all reduce to moving the clock. */
static bool s_preview_active;
static bool s_preview_playing;
static bool s_preview_finished;
static double s_preview_time;
static int s_preview_cur;         /* resolved current frame index (0-based) this frame */
static int s_preview_frame_count; /* resolved (missing-frame-skipped) frame count this frame */

/* deferred side effects (dialogs + model mutations), applied at the top of the next frame */
static bool s_pending_open, s_pending_save, s_pending_save_as, s_pending_add_files, s_pending_add_folder, s_pending_add_atlas, s_pending_refresh;
static bool s_pending_pack, s_pending_export;
static bool s_pending_commit_edit; /* a press landed outside the active inline-edit field -> commit it */
static bool s_blur_inputs;         /* one frame: a press landed outside the panels -> declare inputs disabled so the engine drops focus */
static bool s_pending_add_anim;    /* "+ Animation" -> append empty animation, select it */
static bool s_pending_create_anim; /* "Create animation from selection" */
static bool s_pending_open_preview;/* open the anim preview player on s_ctx_anim / s_sel_anim */
static int s_pending_remove_atlas = -1;
static int s_pending_remove_source = -1;
static int s_pending_remove_anim = -1; /* animation index to remove */
enum { AFTER_NONE = 0, AFTER_NEW, AFTER_EXIT, AFTER_OPEN };
static int s_after_confirm;
static bool s_confirm_open;
static bool s_about_open;
enum { MODAL_NONE = 0, MODAL_SAVE, MODAL_DISCARD, MODAL_CANCEL };
static int s_modal_action;

/* Inline rename edit (F1): one active at a time. kind 0 none / 1 atlas / 2 sprite / 3 animation. */
enum { EDIT_NONE = 0, EDIT_ATLAS, EDIT_SPRITE, EDIT_ANIM };
static int s_edit_kind;
static int s_edit_atlas;         /* atlas being renamed (EDIT_ATLAS) */
static int s_edit_anim;          /* animation index being renamed (EDIT_ANIM) */
static char s_edit_sprite[192];  /* atlas-relative sprite name being renamed (EDIT_SPRITE) */
static char s_edit_buf[192];     /* the input buffer */

/* Pack-button state cached for the tooltip pass (declared at root). */
static bool s_pack_has_sources, s_pack_stale;
static double s_last_pack_ms;      /* wall-clock ms of the last successful pack (for the stats line) */
static int s_last_pack_atlas = -1; /* which atlas that timing belongs to */
static float s_content_w = 1280.0F; /* logical content width, for caption/status truncation */
/* Runtime (already SCALED) column widths. Clamped narrow when the window can't fit both side panels +
 * a minimal canvas, so the panels never get pushed off-screen (recomputed each frame). */
static float s_left_panel_w = 300.0F;
static float s_right_panel_w = 300.0F;

/* --- right settings panel: session-remembered disclosure state + dropdown open bits --- */
#define GUI_MAX_TARGETS 16
static bool s_sec_atlas_open = true, s_sec_region_open = true, s_sec_export_open = true;
static bool s_atlas_adv_open = false;   /* Basic/Advanced disclosure (region F) */
static bool s_region_ov_open = false;   /* Region "Packing overrides" disclosure */
static bool s_dd_shape_open, s_dd_size_open;           /* atlas shape / max-size combos */
static bool s_dd_ov_shape_open, s_dd_ov_rot_open, s_dd_ov_mv_open; /* per-region override combos */
static bool s_dd_target_open[GUI_MAX_TARGETS];         /* per-target exporter combos */
static bool s_pending_add_target;
static int s_pending_remove_target = -1;
static int s_pending_browse_target = -1; /* target whose out-path "..." dialog is queued */
/* Numeric/text field edit buffers (game-owned; nt_ui_input edits in place). Synced from
 * the model each frame while unfocused, parsed+clamped into the model on edit. */
static char s_nb_pad[16], s_nb_margin[16], s_nb_extrude[16], s_nb_maxv[16], s_nb_ppu[24];
static char s_nb_alpha[16]; /* alpha-threshold numeric input (paired with the slider) */
static char s_nb_ox[24], s_nb_oy[24], s_nb_s9[4][16];
static char s_nb_ov_margin[16], s_nb_ov_extrude[16];
static char s_nb_target_path[GUI_MAX_TARGETS][256];
/* --- animation editor (right-panel section 4) --- */
static bool s_sec_anim_open = true; /* the "Animation" section disclosure */
static bool s_dd_playback_open;     /* playback-mode combo open bit */
static char s_nb_anim_fps[16];      /* fps field edit buffer */
/* Playback mode labels, order == the Defold-pinned enum (0 once_forward .. 6 none). */
static const char *const k_playback_names[7] = {"Once forward",  "Loop forward",  "Once backward", "Loop backward",
                                                "Once pingpong", "Loop pingpong", "None"};

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

// #region multi-select + natural sort (ux.md §3.7b selection gesture)
static bool multi_sel_contains(const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    for (int i = 0; i < s_multi_sel_count; i++) {
        if (strcmp(s_multi_sel[i], name) == 0) {
            return true;
        }
    }
    return false;
}
static void multi_sel_clear(void) { s_multi_sel_count = 0; }
static void multi_sel_add(const char *name) {
    if (!name || !name[0] || multi_sel_contains(name) || s_multi_sel_count >= MAX_MULTI_SEL) {
        return;
    }
    (void)snprintf(s_multi_sel[s_multi_sel_count], sizeof s_multi_sel[0], "%s", name);
    s_multi_sel_count++;
}
static void multi_sel_remove(const char *name) {
    for (int i = 0; i < s_multi_sel_count; i++) {
        if (strcmp(s_multi_sel[i], name) == 0) {
            for (int j = i; j < s_multi_sel_count - 1; j++) {
                memcpy(s_multi_sel[j], s_multi_sel[j + 1], sizeof s_multi_sel[0]);
            }
            s_multi_sel_count--;
            return;
        }
    }
}
static void multi_sel_set_single(const char *name) {
    multi_sel_clear();
    multi_sel_add(name);
}

/* Natural order: digit runs compare numerically (walk_2 before walk_10), the rest byte-wise. */
static int nat_cmp(const char *a, const char *b) {
    while (*a && *b) {
        const bool da = (*a >= '0' && *a <= '9');
        const bool db = (*b >= '0' && *b <= '9');
        if (da && db) {
            while (*a == '0') {
                a++;
            }
            while (*b == '0') {
                b++;
            }
            const char *sa = a;
            const char *sb = b;
            while (*a >= '0' && *a <= '9') {
                a++;
            }
            while (*b >= '0' && *b <= '9') {
                b++;
            }
            const size_t la = (size_t)(a - sa);
            const size_t lb = (size_t)(b - sb);
            if (la != lb) {
                return (la < lb) ? -1 : 1;
            }
            const int c = strncmp(sa, sb, la);
            if (c != 0) {
                return c;
            }
        } else {
            if (*a != *b) {
                return ((unsigned char)*a < (unsigned char)*b) ? -1 : 1;
            }
            a++;
            b++;
        }
    }
    if (*a) {
        return 1;
    }
    if (*b) {
        return -1;
    }
    return 0;
}
static int nat_cmp_qsort(const void *a, const void *b) { return nat_cmp((const char *)a, (const char *)b); }

/* Longest common prefix of `names`, trimmed of trailing digits/separators so walk_01/walk_02 -> "walk". */
static void names_common_prefix(char names[][192], int count, char *out, size_t cap) {
    out[0] = '\0';
    if (count <= 0 || cap == 0) {
        return;
    }
    size_t pfx = strlen(names[0]);
    for (int i = 1; i < count; i++) {
        size_t k = 0;
        while (k < pfx && names[i][k] && names[0][k] == names[i][k]) {
            k++;
        }
        pfx = k;
    }
    while (pfx > 0) {
        const char c = names[0][pfx - 1];
        if ((c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ' || c == '/') {
            pfx--;
        } else {
            break;
        }
    }
    if (pfx >= cap) {
        pfx = cap - 1;
    }
    memcpy(out, names[0], pfx);
    out[pfx] = '\0';
}

/* Stops the animation preview player and restores the canvas to its atlas/source view. */
static void preview_stop(void) {
    s_preview_active = false;
    s_preview_playing = false;
    s_preview_finished = false;
    s_preview_time = 0.0;
    s_canvas.anim_sprite = -1;
    if (gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_ANIM) {
        s_canvas.mode = gui_canvas_has_atlas(&s_canvas) ? GUI_CANVAS_ATLAS : GUI_CANVAS_SOURCE;
    }
}
// #endregion

static void reset_selection(void) {
    s_sel_src = -1;
    s_sel_child = -1;
    s_sel_abs[0] = '\0';
    s_sel_missing = false;
    multi_sel_clear();
    s_sel_anchor_row = -1;
    s_sel_anim = -1;
    s_sel_anim_frame = -1;
    preview_stop();
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
    const float ell_w = nt_font_measure_n(s_font, "\xE2\x80\xA6", 3U, size, 0.0F).width; /* U+2026 ellipsis (3 UTF-8 bytes) */
    if (ell_w >= max_w) {
        (void)snprintf(out, cap, "\xE2\x80\xA6");
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
    (void)snprintf(out + len, cap - len, "\xE2\x80\xA6");
    return true;
}

/* Left-panel available text width for a row at `indent_px`, minus an optional [x] button. */
static float left_row_text_w(float indent_px, bool has_x) {
    float w = s_left_panel_w - S(24.0F) - indent_px - S(4.0F) - S(4.0F);
    if (has_x) {
        w -= S(24.0F + 4.0F);
    }
    return (w < S(20.0F)) ? S(20.0F) : w;
}

/* Right-panel text width for a row, minus `reserved_px` (a trailing widget/button cluster). Used to
 * ellipsize long names so they never overrun the (narrow-clamped) settings panel. */
static float right_panel_text_w(float reserved_px) {
    const float w = s_right_panel_w - S(20.0F) /* L+R padding */ - S(12.0F) /* scrollbar */ - reserved_px;
    return (w < S(24.0F)) ? S(24.0F) : w;
}

/* Clamp the two fixed side-panel widths so the window always fits left + right + a minimal canvas.
 * Below the fit threshold both panels scale down proportionally to a floor; the canvas (GROW) takes
 * whatever remains. All values are in scaled layout units (== scale.logical_w units). */
static void compute_panel_widths(float logical_w) {
    const float base_l = S(BASE_LEFT_PANEL_W);
    const float base_r = S(BASE_RIGHT_PANEL_W);
    const float min_canvas = S(120.0F);
    const float min_panel = S(120.0F);
    const float overhead = S(20.0F) + (S(8.0F) * 2.0F); /* root L/R padding + two inter-column gaps */
    const float base_sum = base_l + base_r;
    const float avail = logical_w - min_canvas - overhead;
    if (avail < base_sum && base_sum > 1.0F) {
        float f = avail / base_sum;
        if (f < 0.0F) {
            f = 0.0F;
        }
        s_left_panel_w = fmaxf(base_l * f, min_panel);
        s_right_panel_w = fmaxf(base_r * f, min_panel);
    } else {
        s_left_panel_w = base_l;
        s_right_panel_w = base_r;
    }
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

// #region animation + preview actions (ux.md §3.7b)
/* The selected animation of the selected atlas, or NULL. */
static tp_project_anim *current_anim(void) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || s_sel_anim < 0 || s_sel_anim >= a->animation_count) {
        return NULL;
    }
    return &a->animations[s_sel_anim];
}

/* Shared scratch for the selection-gesture sort (frame lists are small; static avoids stack blow-up). */
static char s_sel_sort_buf[MAX_MULTI_SEL][192];
static const char *s_sel_sort_ptr[MAX_MULTI_SEL];
/* Copies the multi-selection into the shared buffers, natural-sorted; returns the count. */
static int build_sorted_selection(void) {
    const int n = s_multi_sel_count;
    for (int i = 0; i < n; i++) {
        (void)snprintf(s_sel_sort_buf[i], sizeof s_sel_sort_buf[0], "%s", s_multi_sel[i]);
    }
    qsort(s_sel_sort_buf, (size_t)n, sizeof s_sel_sort_buf[0], nat_cmp_qsort);
    for (int i = 0; i < n; i++) {
        s_sel_sort_ptr[i] = s_sel_sort_buf[i];
    }
    return n;
}

/* Creates an animation from the current multi-selection: frames natural-sorted, id from the common
 * prefix (auto "animN" when there is none). Selects the new animation (opens its editor). */
static int create_animation_from_selection(void) {
    if (s_multi_sel_count <= 0) {
        return -1;
    }
    const int n = build_sorted_selection();
    char base[192];
    names_common_prefix(s_sel_sort_buf, n, base, sizeof base);
    const int idx = gui_project_create_animation(s_sel_atlas, base[0] ? base : NULL, s_sel_sort_ptr, n);
    if (idx >= 0) {
        s_sel_anim = idx;
        s_sel_anim_frame = -1;
        tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
        set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).", a->animations[idx].id, n);
    }
    return idx;
}

/* Appends the current multi-selection (natural-sorted) as frames of animation `anim_index`. */
static void add_selection_frames_to_anim(int anim_index) {
    if (s_multi_sel_count <= 0) {
        return;
    }
    const int n = build_sorted_selection();
    if (gui_project_anim_add_frames(s_sel_atlas, anim_index, s_sel_sort_ptr, n)) {
        set_statusf("Added %d frame(s) to the animation (Ctrl+Z to undo).", n);
    }
}

/* Opens the preview player on animation `anim_index` (plays from the packed regions; if the atlas is
 * not packed yet, the canvas shows a "Pack to preview" hint). */
static void open_preview(int anim_index) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || anim_index < 0 || anim_index >= a->animation_count) {
        return;
    }
    cancel_edit();
    s_sel_anim = anim_index;
    s_preview_active = true;
    s_preview_playing = true;
    s_preview_finished = false;
    s_preview_time = 0.0;
    if (!gui_pack_result(s_sel_atlas)) {
        set_status("Pack (Ctrl+P) to preview the animation on packed regions.");
    } else {
        set_statusf("Previewing '%s' \xE2\x80\x94 Space play/pause.", a->animations[anim_index].id);
    }
}

static void preview_toggle_play(void) {
    if (!s_preview_active) {
        return;
    }
    if (s_preview_playing) {
        s_preview_playing = false;
    } else {
        if (s_preview_finished) {
            s_preview_time = 0.0;
            s_preview_finished = false;
        }
        s_preview_playing = true;
    }
}

/* Nudges the preview timeline by `delta` frame-ticks (pauses first). */
static void preview_step(int delta) {
    if (!s_preview_active) {
        return;
    }
    const tp_project_anim *an = current_anim();
    const float fps = (an && an->fps >= 1.0F) ? an->fps : 1.0F;
    s_preview_playing = false;
    s_preview_finished = false;
    long step = (long)floor(s_preview_time * (double)fps);
    step += delta;
    if (step < 0) {
        step = 0;
    }
    s_preview_time = ((double)step + 0.5) / (double)fps;
}

/* Resolves the selected animation's frames to packed regions and pushes the current frame to the
 * canvas each frame. Advances the clock while playing. No-op (leaves the hint) without a pack result. */
static void update_preview(void) {
    if (!s_preview_active) {
        return;
    }
    tp_project_anim *an = current_anim();
    const tp_result *pr = gui_pack_result(s_sel_atlas);
    s_canvas.anim_sprite = -1;
    s_preview_frame_count = 0;
    if (!an || !pr) {
        return; /* declare_canvas draws the "Pack to preview" hint */
    }
    static int idxs[512];
    int n = 0;
    int rw = 1;
    int rh = 1;
    for (int i = 0; i < an->frame_count && n < 512; i++) {
        const int si = gui_pack_find_sprite(s_sel_atlas, an->frames[i]);
        if (si >= 0 && si < pr->sprite_count) {
            idxs[n++] = si;
            if (pr->sprites[si].sourceSize.w > rw) {
                rw = pr->sprites[si].sourceSize.w;
            }
            if (pr->sprites[si].sourceSize.h > rh) {
                rh = pr->sprites[si].sourceSize.h;
            }
        }
    }
    s_preview_frame_count = n;
    if (n == 0) {
        return;
    }
    const float fps = (an->fps >= 1.0F) ? an->fps : 1.0F;
    if (s_preview_playing) {
        s_preview_time += (double)g_nt_app.dt;
    }
    bool finished = false;
    int cur = gui_canvas_anim_frame_at(s_preview_time, fps, an->playback, n, &finished);
    if (finished && s_preview_playing) {
        s_preview_playing = false;
    }
    s_preview_finished = finished;
    if (cur < 0) {
        cur = 0;
    }
    if (cur >= n) {
        cur = n - 1;
    }
    s_preview_cur = cur;
    s_canvas.mode = GUI_CANVAS_ANIM;
    s_canvas.anim_sprite = idxs[cur];
    s_canvas.anim_ref_w = rw;
    s_canvas.anim_ref_h = rh;
    s_canvas.anim_flip_h = an->flip_h;
    s_canvas.anim_flip_v = an->flip_v;
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
        gui_pack_clear(-1);
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

/* Best-effort relativize `abs` against the project dir (targets travel like sources).
 * Absolute paths outside the project dir are kept as-is (usable, save leaves them). */
static void relativize_to_project(const char *abs, char *out, size_t cap) {
    tp_project *p = gui_project_get();
    const char *dir = p ? p->project_dir : NULL;
    if (dir && dir[0] != '\0') {
        const size_t dl = strlen(dir);
        if (strncmp(abs, dir, dl) == 0 && (abs[dl] == '/' || abs[dl] == '\\')) {
            (void)snprintf(out, cap, "%s", abs + dl + 1);
            normalize_slashes(out);
            return;
        }
    }
    (void)snprintf(out, cap, "%s", abs);
    normalize_slashes(out);
}

/* Save dialog for a target's output path, relativized to the project like sources. */
static void do_browse_target(int ti) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || ti < 0 || ti >= a->target_count) {
        return;
    }
    const tp_project_target *t = &a->targets[ti];
    const char *path = tinyfd_saveFileDialog("Export output path", t->out_path, 0, NULL, NULL);
    if (!path) {
        return;
    }
    char rel[600];
    relativize_to_project(path, rel, sizeof rel);
    if (gui_project_set_target(s_sel_atlas, ti, t->exporter_id, rel, t->enabled)) {
        set_statusf("Output path: %s", rel);
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
        gui_pack_clear(-1);
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
/* Open routes through the same unsaved-changes confirm as New/Exit (no silent discard). The actual
 * OS open dialog runs via s_pending_open, either now (clean) or after the modal resolves. */
static void request_open(void) {
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_OPEN;
        s_confirm_open = true;
    } else {
        s_pending_open = true;
    }
}
static void confirm_perform(void) {
    if (s_after_confirm == AFTER_NEW) {
        gui_project_new();
        gui_pack_clear(-1);
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_status("New project.");
    } else if (s_after_confirm == AFTER_EXIT) {
        nt_app_quit();
    } else if (s_after_confirm == AFTER_OPEN) {
        s_pending_open = true; /* runs the open dialog next frame */
    }
    s_after_confirm = AFTER_NONE;
}
// #endregion

// #region undo/redo + refresh actions
static void do_undo(void) {
    if (gui_project_undo()) {
        gui_pack_clear(-1);
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
        gui_pack_clear(-1);
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

/* Ctrl+P / Pack: pack the selected atlas in-process (gui_pack -> tp_pack). On success clear the
 * preview-stale bit and upload the packed pages to the canvas (atlas-page view); on failure the
 * previous result + the "outdated" tag stay (ux.md §3.3b). */
static void do_pack(void) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || a->source_count == 0) {
        set_status("No sources to pack -- add files or a folder first.");
        return;
    }
    char err[256] = {0};
    char note[128] = {0};
    double ms = 0.0;
    if (gui_pack_atlas(s_sel_atlas, &ms, err, sizeof err, note, sizeof note)) {
        gui_project_mark_packed(); /* clears preview_stale for the current model */
        s_last_pack_ms = ms;
        s_last_pack_atlas = s_sel_atlas;
        /* the per-frame canvas<->atlas sync (frame()) picks up the new result pointer and uploads. */
        const tp_result *r = gui_pack_result(s_sel_atlas);
        if (note[0] != '\0') {
            set_statusf("Packed %d sprites, %d page(s) in %.0f ms (%s)", r->sprite_count, r->page_count, ms, note);
        } else {
            set_statusf("Packed %d sprites, %d page(s) in %.0f ms", r->sprite_count, r->page_count, ms);
        }
    } else {
        set_statusf("Pack failed: %s", err);
    }
}

/* Ctrl+E / Export All: for every atlas with sources + >=1 enabled target, tp_export_run packs per
 * target (project INTERSECT target caps) and writes its files. Per-atlas failures are non-fatal
 * (ux.md §3.5); notices + counts summarize in the status bar. */
static void do_export(void) {
    tp_project *p = gui_project_get();
    if (!p) {
        return;
    }
    int total_targets = 0;
    int total_notices = 0;
    int atlases_ok = 0;
    int atlases_fail = 0;
    char first_err[256] = {0};
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        bool any_enabled = false;
        for (int t = 0; t < a->target_count; t++) {
            if (a->targets[t].enabled) {
                any_enabled = true;
            }
        }
        if (!any_enabled || a->source_count == 0) {
            continue;
        }
        int tg = 0;
        int nc = 0;
        char err[256] = {0};
        char note[128] = {0};
        if (gui_pack_export(ai, &tg, &nc, err, sizeof err, note, sizeof note)) {
            total_targets += tg;
            total_notices += nc;
            atlases_ok++;
        } else {
            atlases_fail++;
            if (first_err[0] == '\0') {
                (void)snprintf(first_err, sizeof first_err, "%s: %s", a->name, err);
            }
        }
    }
    if (atlases_fail > 0) {
        set_statusf("Exported %d target(s); %d atlas(es) failed -- %s", total_targets, atlases_fail, first_err);
    } else if (atlases_ok == 0) {
        set_status("Nothing to export -- enable a target and add sources.");
    } else {
        set_statusf("Exported %d target(s)%s", total_targets,
                    total_notices > 0 ? " (metadata notices raised)" : "");
    }
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
static void commit_anim_rename(void) {
    if (s_edit_buf[0] == '\0') {
        set_status("Animation name cannot be empty.");
        return; /* keep editing */
    }
    if (gui_project_set_anim_id(s_sel_atlas, s_edit_anim, s_edit_buf)) {
        set_statusf("Renamed animation to '%s'", s_edit_buf);
        cancel_edit();
    } else {
        set_statusf("Animation '%s' already exists.", s_edit_buf); /* keep editing */
    }
}

/* Commit the active inline edit as if Enter was pressed (click-outside / model-change path).
 * `force` = the editor is being dismissed involuntarily: an invalid atlas name CANCELS instead of
 * keeping a zombie editor (the validation message stays in the status bar). Sprite rename never
 * zombies (empty clears the override). No-op when nothing is being edited. */
static void commit_active_edit(bool force) {
    if (s_edit_kind == EDIT_ATLAS) {
        char err[128];
        if (!atlas_name_valid(s_edit_buf, s_edit_atlas, err, sizeof err)) {
            set_status(err);
            if (force) {
                cancel_edit();
            }
            return;
        }
        if (gui_project_set_atlas_name(s_edit_atlas, s_edit_buf)) {
            set_statusf("Renamed atlas to '%s'", s_edit_buf);
        }
        cancel_edit();
    } else if (s_edit_kind == EDIT_SPRITE) {
        commit_sprite_rename();
    } else if (s_edit_kind == EDIT_ANIM) {
        if (s_edit_buf[0] == '\0' || !gui_project_set_anim_id(s_sel_atlas, s_edit_anim, s_edit_buf)) {
            set_status(s_edit_buf[0] == '\0' ? "Animation name cannot be empty." : "Animation name must be unique.");
            if (force) {
                cancel_edit();
            }
            return;
        }
        set_statusf("Renamed animation to '%s'", s_edit_buf);
        cancel_edit();
    }
}
// #endregion

// #region deferred side-effects (run at the top of the frame, between frames)
static void apply_pending(void) {
    /* A press landed outside the active inline editor last frame -> commit it (desktop rename UX).
     * Also fires before any pending model change (remove/refresh/open/new) so no orphaned editor
     * survives a mutation. */
    if (s_edit_kind != EDIT_NONE && s_pending_commit_edit) {
        commit_active_edit(true);
    }
    s_pending_commit_edit = false;

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
    if (s_pending_add_target) {
        const int ti = gui_project_add_target(s_sel_atlas);
        if (ti >= 0) {
            set_status("Added export target (Ctrl+Z to undo).");
        }
    }
    if (s_pending_remove_target >= 0) {
        gui_project_remove_target(s_sel_atlas, s_pending_remove_target);
        set_status("Removed export target (Ctrl+Z to undo).");
    }
    if (s_pending_browse_target >= 0) {
        do_browse_target(s_pending_browse_target);
    }
    if (s_pending_add_anim) {
        const int idx = gui_project_create_animation(s_sel_atlas, NULL, NULL, 0);
        if (idx >= 0) {
            s_sel_anim = idx;
            s_sel_anim_frame = -1;
            set_statusf("Added animation '%s' (Ctrl+Z to undo).",
                        tp_project_get_atlas(gui_project_get(), s_sel_atlas)->animations[idx].id);
        }
    }
    if (s_pending_create_anim) {
        (void)create_animation_from_selection();
    }
    if (s_pending_remove_anim >= 0) {
        tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
        if (a && s_pending_remove_anim < a->animation_count) {
            if (s_preview_active && s_sel_anim == s_pending_remove_anim) {
                preview_stop();
            }
            gui_project_remove_animation(s_sel_atlas, a->animations[s_pending_remove_anim].id);
            s_sel_anim = -1;
            s_sel_anim_frame = -1;
            set_status("Removed animation (Ctrl+Z to undo).");
        }
    }
    if (s_pending_open_preview) {
        open_preview(s_sel_anim);
    }
    if (s_pending_refresh) {
        do_refresh();
    }
    if (s_pending_pack) {
        do_pack();
    }
    if (s_pending_export) {
        do_export();
    }

    s_pending_open = s_pending_save = s_pending_save_as = false;
    s_pending_add_files = s_pending_add_folder = s_pending_add_atlas = false;
    s_pending_refresh = s_pending_pack = s_pending_export = false;
    s_pending_add_target = false;
    s_pending_add_anim = s_pending_create_anim = s_pending_open_preview = false;
    s_pending_remove_source = -1;
    s_pending_remove_atlas = -1;
    s_pending_remove_target = -1;
    s_pending_remove_anim = -1;
    s_pending_browse_target = -1;
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
            (void)snprintf(r->label, sizeof r->label, "\xE2\x9A\xA0 %s", path_last(sp)); /* U+26A0 warning */
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
    s_id_canvas = nt_ui_id("ntpacker/canvas");
    s_id_modal = nt_ui_id("ntpacker/confirm_modal");
    s_id_about = nt_ui_id("ntpacker/about_modal");
    s_id_rename = nt_ui_id("ntpacker/rename_input");
    s_id_right_panel = nt_ui_id("ntpacker/right_panel");
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

    /* Settings-panel widget styles. The atlas WHITE region (s_white_ref, bound by now
     * since can_render gates ensure_ids) is the art for checkbox/slider parts, tinted
     * per state; the dropdown is flat-color. */
    s_dd_style = nt_ui_dropdown_style_defaults();
    s_dd_style.font_id = 0;
    s_dd_style.trigger_text = RGBA8(214, 220, 230);
    s_dd_style.row_text = RGBA8(214, 220, 230);
    s_dd_style.trigger_idle.fill = RGBA8(42, 46, 56);
    s_dd_style.trigger_hover.fill = RGBA8(54, 60, 74);
    s_dd_style.trigger_pressed.fill = RGBA8(36, 40, 50);
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
    s_num_input.skin[NT_UI_INPUT_IDLE].bg_color = RGBA8(42, 46, 56);
    s_num_input.skin[NT_UI_INPUT_FOCUSED].bg_color = RGBA8(52, 58, 70);
    s_num_input.skin[NT_UI_INPUT_FOCUSED].border_color = RGBA8(160, 118, 74);
    s_num_input.skin[NT_UI_INPUT_DISABLED].bg_color = RGBA8(34, 36, 42);
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
    g_link = g_link_base;
    g_link.font_size = S(FS_CAPTION);
    g_dim = g_dim_base;
    g_dim.font_size = S(FS_CAPTION);

    s_rename_input.text.font_size = S(FS_ROW);
    s_rename_input.placeholder.font_size = S(FS_ROW);
    s_rename_input.pad_x = S(6.0F);

    s_menu_style.font_size = S(15.0F);
    s_menu_style.item_height = Su(28.0F);
    s_menu_style.min_width = Su(180.0F);
    s_tip_style.font_size = S(14.0F);
    s_tip_style.max_width = Su(360.0F);
    s_tip_style.pad = Su(8.0F);

    /* Settings-panel widgets scale with the global UI scale (their sizes are style px). */
    s_num_input.text.font_size = S(FS_ROW);
    s_num_input.placeholder.font_size = S(FS_ROW);
    s_num_input.pad_x = S(6.0F);
    s_dd_style.font_size = S(14.0F);
    s_dd_style.row_height = Su(26.0F);
    s_dd_style.min_width = Su(110.0F);
    s_dd_style.pad = Su(8.0F);
    g_check = g_row_base;
    g_check.font_size = S(13.0F);
    g_check.color = (Clay_Color){225.0F, 236.0F, 250.0F, 255.0F};
    s_slider_style.track_w = S(92.0F);
    s_slider_style.track_h = S(8.0F);
    s_slider_style.thumb_w = S(16.0F);
    s_slider_style.thumb_h = S(16.0F);
    s_panel_scroll.bar_thickness = S(8.0F);
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

/* Boolean checkbox: an outlined box that draws a centered check GLYPH (U+2713) when on. The engine
 * nt_ui_checkbox only draws atlas art for the mark, which read as a solid blue square; the font
 * already bakes the tick, so we hand-roll box + glyph. Returns true the frame it is clicked (the
 * caller flips its own value, like the menu toggles). enabled=false = inert, dimmed. */
static bool tp_checkbox(nt_ui_context_t *ctx, uint32_t id, bool cur, bool enabled) {
    nt_ui_events_t ev = {0};
    if (enabled) {
        ev = nt_ui_events(ctx, id, NULL);
    }
    Clay_Color bg = (Clay_Color){42.0F, 46.0F, 56.0F, 255.0F};
    if (!enabled) {
        bg = (Clay_Color){32.0F, 34.0F, 40.0F, 255.0F};
    } else if (ev.pressed) {
        bg = (Clay_Color){36.0F, 40.0F, 50.0F, 255.0F};
    } else if (ev.hovered) {
        bg = (Clay_Color){54.0F, 60.0F, 74.0F, 255.0F};
    }
    const Clay_Color border = cur ? (Clay_Color){78.0F, 126.0F, 192.0F, 255.0F} : (Clay_Color){96.0F, 102.0F, 116.0F, 255.0F};
    nt_ui_label_style_t glyph = g_check;
    if (!enabled) {
        glyph.color = (Clay_Color){120.0F, 126.0F, 138.0F, 255.0F};
    }
    CLAY({.id = {.id = id},
          .layout = {.sizing = {CLAY_SIZING_FIXED(S(18.0F)), CLAY_SIZING_FIXED(S(18.0F))},
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(S(4)),
          .border = {.color = border, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        if (cur) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "\xE2\x9C\x93", &glyph); /* U+2713 check */
        }
    }
    return enabled && ev.clicked;
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
        request_open();
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
static void start_anim_edit(int i) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || i < 0 || i >= a->animation_count) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_ANIM;
    s_edit_anim = i;
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", a->animations[i].id);
    set_status("Rename animation: type, Enter to commit, Esc to cancel.");
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
    /* Seed with the CURRENT name: the rename override if set, else the file-derived final name
     * (sprite_name is the ext-stripped atlas-relative key = the default export name). The input
     * string is game-owned (nt_ui_input edits s_edit_buf in place), so seeding it here is the fix
     * for the "field opens empty" bug -- previously it seeded the (empty) override. */
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", (ov && ov->rename) ? ov->rename : sprite_name);
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
            if (row->is_source) {
                const nt_ui_events_t xev = nt_ui_events(ctx, x_id, NULL);
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

/* ANIMATIONS block (ux.md §2.1 region D, §3.7b): one row per animation (id + frame count), a per-row
 * [x] remove + right-click Rename/Remove/Preview, "+ Animation" to add. Double-click a row = preview. */
static void declare_animations_list(nt_ui_context_t *ctx, tp_project_atlas *a) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(28))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "ANIMATIONS", &g_title);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        if (ui_btn(ctx, nt_ui_id("ntpacker/add_anim"), "+ Animation", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
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
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (editing) {
                    if (render_rename_field(ctx)) {
                        commit_anim_rename();
                    }
                } else {
                    ui_label_fit(ctx, a->animations[i].id, &g_row, left_row_text_w(S(8.0F), true) - S(28.0F), row_id);
                }
            }
            char fc[16];
            (void)snprintf(fc, sizeof fc, "%df", a->animations[i].frame_count);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), fc, &g_caption);
            (void)ui_btn(ctx, x_id, "x", &g_btn_ghost, true, 24.0F, 22.0F, &g_caption);
        }
    }
}

static void declare_left_panel(nt_ui_context_t *ctx) {
    tp_project *proj = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(proj, s_sel_atlas);
    s_row_tip_count = 0; /* per-frame; filled by ui_label_fit when a row truncates */
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(s_left_panel_w), CLAY_SIZING_GROW(0)},
                     .padding = {Su(12), Su(12), Su(12), Su(12)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
          .backgroundColor = C_PANEL,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
          .clip = {.horizontal = true},
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
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
/* Selects the sprite-tree row matching a canvas region (region -> row selection sync). The region's
 * raw name stripped of its extension == the row's override key. */
static void select_row_for_region(int region_idx) {
    const tp_result *r = gui_pack_result(s_sel_atlas);
    if (!r || region_idx < 0 || region_idx >= r->sprite_count) {
        return;
    }
    char key[192];
    strip_ext(r->sprites[region_idx].name, key, sizeof key);
    for (int i = 0; i < s_row_count; i++) {
        if (!s_rows[i].is_folder && s_rows[i].sprite_name[0] != '\0' && strcmp(s_rows[i].sprite_name, key) == 0) {
            s_sel_src = s_rows[i].src;
            s_sel_child = s_rows[i].child;
            s_sel_missing = false;
            (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", s_rows[i].abs);
            return;
        }
    }
}

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
        s_confirm_open || s_about_open || s_edit_kind != EDIT_NONE) {
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
    const bool inside = p->x >= box[0] && p->x < (box[0] + box[2]) && p->y >= box[1] && p->y < (box[1] + box[3]);

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

/* Compact D4 transform decode for the hover/selection readout (ux.md §2.4). */
static const char *transform_decode_str(uint8_t t) {
    switch (t & 7u) {
        case 0: return "--";
        case 1: return "flip H";
        case 2: return "flip V";
        case 3: return "rot 180";
        case 4: return "transpose";
        case 5: return "rot 90";
        case 6: return "rot 270";
        default: return "anti-transpose";
    }
}

/* Canvas action strip (E', replaces the old toolbar row): [Pack][Export][refresh] | page nav |
 * zoom controls | stale chip. Semi-transparent bar at the TOP of the canvas so the atlas gets the
 * freed vertical space. Every control is also reachable from the menus / context menu (§3.3e). */
static void declare_canvas_strip(nt_ui_context_t *ctx, bool atlas) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    s_pack_has_sources = a && a->source_count > 0;
    s_pack_stale = gui_project_is_stale();
    const bool accent = s_pack_has_sources && s_pack_stale;
    const int pc = gui_canvas_page_count(&s_canvas);
    const int cur = gui_canvas_cur_page(&s_canvas);
    const Clay_Color strip_bg = {30.0F, 34.0F, 42.0F, 205.0F}; /* semi-transparent */

    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(34))},
                     .padding = {Su(8), Su(8), Su(4), Su(4)},
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = strip_bg,
          .cornerRadius = CLAY_CORNER_RADIUS(S(6))}) {
        /* Pack (accent + warning glyph when stale) / Export / Refresh */
        if (ui_btn(ctx, s_id_btn_pack, accent ? "Pack \xE2\x9A\xA0" : "Pack", accent ? &g_btn_accent : &g_btn,
                   s_pack_has_sources, 78.0F, 26.0F, &g_body)) {
            s_pending_pack = true;
        }
        if (ui_btn(ctx, s_id_btn_export, "Export", &g_btn, s_pack_has_sources, 78.0F, 26.0F, &g_body)) {
            s_pending_export = true;
        }
        if (ui_btn(ctx, s_id_btn_refresh, "\xE2\x9F\xB3", &g_btn_ghost, true, 34.0F, 26.0F, &g_body)) { /* U+27F3 */
            s_pending_refresh = true;
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(1)), CLAY_SIZING_FIXED(S(20))}}, .backgroundColor = C_BORDER}) {}
        if (atlas && pc > 1) {
            if (ui_btn(ctx, nt_ui_id("ntpacker/pg_prev"), "\xE2\x97\x80", &g_btn_ghost, cur > 0, 28.0F, 24.0F,
                       &g_caption)) { /* U+25C0 */
                gui_canvas_set_page(&s_canvas, cur - 1);
            }
            char pl[32];
            (void)snprintf(pl, sizeof pl, "page %d/%d", cur + 1, pc);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), pl, &g_caption);
            if (ui_btn(ctx, nt_ui_id("ntpacker/pg_next"), "\xE2\x96\xB6", &g_btn_ghost, cur < pc - 1, 28.0F, 24.0F,
                       &g_caption)) { /* U+25B6 */
                gui_canvas_set_page(&s_canvas, cur + 1);
            }
        }
        if (atlas) {
            if (ui_btn(ctx, nt_ui_id("ntpacker/zoom_out"), "\xE2\x88\x92", &g_btn_ghost, true, 28.0F, 24.0F,
                       &g_caption)) { /* U+2212 minus */
                gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, gui_canvas_zoom_pct(&s_canvas) * 0.8F);
            }
            char zl[16];
            (void)snprintf(zl, sizeof zl, "%.0f%%", (double)gui_canvas_zoom_pct(&s_canvas));
            if (ui_btn(ctx, nt_ui_id("ntpacker/zoom_100"), zl, &g_btn_ghost, true, 60.0F, 24.0F, &g_caption)) {
                gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, 100.0F);
            }
            if (ui_btn(ctx, nt_ui_id("ntpacker/zoom_in"), "+", &g_btn_ghost, true, 28.0F, 24.0F, &g_caption)) {
                gui_canvas_set_zoom_pct(&s_canvas, s_canvas.last_bb, gui_canvas_zoom_pct(&s_canvas) * 1.25F);
            }
            if (ui_btn(ctx, nt_ui_id("ntpacker/zoom_fit"), "Fit", &g_btn_ghost, true, 40.0F, 24.0F, &g_caption)) {
                gui_canvas_fit(&s_canvas);
            }
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        /* Clickable stale chip -> Pack (owner rule: actionable hints are buttons, not labels). */
        if (s_pack_stale && s_pack_has_sources) {
            if (ui_btn(ctx, nt_ui_id("ntpacker/stale_chip"), "outdated \xE2\x80\x94 press Pack", &g_btn_stale, true, 0.0F,
                       24.0F, &g_tag)) { /* U+2014 em dash */
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
                     .padding = {Su(10), Su(10), Su(10), Su(10)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(8),
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_CANVAS,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
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
                     .padding = {Su(10), Su(10), Su(10), Su(10)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(8),
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_CANVAS,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
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
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No atlas preview yet -- press Pack (Ctrl+P) to build it.", &g_canvas_hint);
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Or select a sprite on the left to preview its source image.", &g_caption);
            }
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
            /* Clickable repo link: looks like a hyperlink (link-blue + hover tint), opens the browser. */
            if (ui_btn(ctx, nt_ui_id("ntpacker/about_link"), NTPACKER_REPO_URL, &g_btn_link, true, 0.0F, 24.0F, &g_link)) {
                if (gui_open_url(NTPACKER_REPO_URL)) {
                    set_statusf("Opening %s", NTPACKER_REPO_URL);
                } else {
                    set_status("Could not open browser -- " NTPACKER_REPO_URL);
                }
            }
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

// #region right settings panel (regions F/G + per-region packing overrides)
#define PANEL_LABEL_W 116.0F
static const char *const k_shape_names[3] = {"Rect", "Convex hull", "Concave contour"};
static const int k_size_presets[7] = {256, 512, 1024, 2048, 4096, 8192, 16384};

/* Numeric field: shows *cur while unfocused, edits `buf` in place; on edit parses +
 * clamps into *out (the model stays valid every keystroke; the buffer reformats on
 * blur). Returns true the frame the value changed. */
static bool ui_int_field(nt_ui_context_t *ctx, uint32_t id, char *buf, size_t cap, int cur, int mn, int mx,
                         bool enabled, int *out) {
    if (!nt_ui_input_focused(ctx, id)) {
        (void)snprintf(buf, cap, "%d", cur);
    }
    static const nt_ui_input_props_t np = {
        .placeholder = NULL, .allow = nt_ui_filter_numeric, .max_length = 0U, .keyboard = NT_UI_KB_NUMERIC, .password = false};
    const Clay_ElementDeclaration decl = {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 4.0F))}}};
    bool submitted = false;
    const bool changed = nt_ui_input_text(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, id, buf, cap, &np, &s_num_input,
                                          &decl, enabled && !s_blur_inputs, &submitted);
    if ((changed || submitted) && enabled) {
        long v = strtol(buf, NULL, 10);
        if (v < mn) {
            v = mn;
        }
        if (v > mx) {
            v = mx;
        }
        *out = (int)v;
        return true;
    }
    return false;
}

static bool ui_float_field(nt_ui_context_t *ctx, uint32_t id, char *buf, size_t cap, float cur, float mn, float mx,
                           bool enabled, float *out) {
    if (!nt_ui_input_focused(ctx, id)) {
        (void)snprintf(buf, cap, "%g", (double)cur);
    }
    static const nt_ui_input_props_t np = {
        .placeholder = NULL, .allow = nt_ui_filter_numeric, .max_length = 0U, .keyboard = NT_UI_KB_NUMERIC, .password = false};
    const Clay_ElementDeclaration decl = {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 4.0F))}}};
    bool submitted = false;
    const bool changed = nt_ui_input_text(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, id, buf, cap, &np, &s_num_input,
                                          &decl, enabled && !s_blur_inputs, &submitted);
    if ((changed || submitted) && enabled) {
        double v = strtod(buf, NULL);
        if (!(v > (double)mn)) {
            v = (double)mn;
        }
        if (v > (double)mx) {
            v = (double)mx;
        }
        *out = (float)v;
        return true;
    }
    return false;
}

/* Short free-text field (target out path). Returns true on edit; caller reads `buf`. */
static bool ui_text_field(nt_ui_context_t *ctx, uint32_t id, char *buf, size_t cap, const char *cur, bool enabled,
                          const char *placeholder) {
    if (!nt_ui_input_focused(ctx, id)) {
        (void)snprintf(buf, cap, "%s", cur ? cur : "");
    }
    const nt_ui_input_props_t tp = {
        .placeholder = placeholder, .allow = NULL, .max_length = 0U, .keyboard = NT_UI_KB_TEXT, .password = false};
    const Clay_ElementDeclaration decl = {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 4.0F))}}};
    bool submitted = false;
    const bool changed = nt_ui_input_text(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, id, buf, cap, &tp, &s_num_input,
                                          &decl, enabled && !s_blur_inputs, &submitted);
    return (changed || submitted) && enabled;
}

/* A collapsible section/disclosure header: full-width clickable row + chevron. */
static void panel_header(nt_ui_context_t *ctx, uint32_t id, const char *title, bool *open, const nt_ui_label_style_t *lbl,
                         const Clay_Color bg_col) {
    const nt_ui_events_t ev = nt_ui_events(ctx, id, NULL);
    if (ev.clicked) {
        *open = !*open;
    }
    const Clay_Color bg = ev.hovered ? C_HOVER : bg_col;
    CLAY({.id = {.id = id},
          .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(26.0F))},
                     .padding = {Su(8), Su(8), 0, 0},
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), *open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", &g_caption); /* U+25BE / U+25B8 */
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), title, lbl);
    }
}

/* A small full-width note line (info, never a warning) -- wraps within the panel. */
static void panel_note(nt_ui_context_t *ctx, const char *text) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = {Su(4), Su(4), Su(2), Su(2)}}}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), text, &g_dim);
    }
}

/* Row scaffolding: a fixed label cell + a growable widget cell. The BEGIN macro opens
 * both; the caller emits the widget then closes with ROW_END. */
#define PANEL_ROW_BEGIN(lbl_text, lbl_style)                                                                            \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},                                 \
                     .childGap = Su(8),                                                                                 \
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {                                    \
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(PANEL_LABEL_W)), CLAY_SIZING_GROW(0)},                          \
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},                                   \
              .clip = {.horizontal = true}}) {                                                                          \
            ui_label_fit(ctx, (lbl_text), (lbl_style), S(PANEL_LABEL_W), 0U);                                           \
        }                                                                                                              \
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},                                         \
                         .childGap = Su(6),                                                                            \
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}})
#define PANEL_ROW_END }

/* A labeled string-option dropdown. Returns the newly-picked index, or -1 if unchanged.
 * When !enabled it renders the preview as a static dimmed label (disabled-with-reason). */
static int row_combo(nt_ui_context_t *ctx, const char *label, uint32_t id, bool *open, const char *preview, int cur,
                     const char *const *options, int count, bool enabled) {
    int sel = -1;
    PANEL_ROW_BEGIN(label, enabled ? &g_row : &g_dim) {
        if (!enabled) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), preview ? preview : "", &g_dim);
        } else if (nt_ui_combo_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, id, preview, &s_dd_style, open)) {
            for (int i = 0; i < count; i++) {
                if (nt_ui_combo_selectable(ctx, (uint32_t)i, options[i], i == cur)) {
                    sel = i;
                }
            }
            nt_ui_combo_end(ctx);
        }
    }
    PANEL_ROW_END;
    return sel;
}

/* A labeled slider + editable numeric input, both bound to the same [mn,mx] int field (exact entry
 * without the clumsy drag-only slider). Returns true (writes *out) on change from either control. */
static bool row_slider(nt_ui_context_t *ctx, const char *label, uint32_t id, char *buf, size_t cap, int cur, int mn,
                       int mx, bool enabled, int *out) {
    int v = cur;
    bool changed = false;
    PANEL_ROW_BEGIN(label, enabled ? &g_row : &g_dim) {
        const Clay_ElementDeclaration sd = {
            .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(S(20.0F))}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}};
        if (nt_ui_slider_int(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, id, NULL, &v, mn, mx, 1, &s_slider_style, &sd, enabled)) {
            changed = true;
        }
        int iv = 0;
        if (ui_int_field(ctx, nt_ui_child_id(id, "num"), buf, cap, v, mn, mx, enabled, &iv)) {
            v = iv;
            changed = true;
        }
    }
    PANEL_ROW_END;
    if (changed) {
        *out = v;
    }
    return changed;
}

/* A labeled checkbox (indicator only; the text lives in the label cell). Returns true
 * (writes the flipped value to *out) on change. */
static bool row_check(nt_ui_context_t *ctx, const char *label, uint32_t id, bool cur, bool enabled, bool *out) {
    bool changed = false;
    PANEL_ROW_BEGIN(label, enabled ? &g_row : &g_dim) {
        if (tp_checkbox(ctx, id, cur, enabled)) {
            *out = !cur;
            changed = true;
        }
    }
    PANEL_ROW_END;
    return changed;
}

/* A labeled int field row. Returns true (writes *out) on edit. */
static bool row_int(nt_ui_context_t *ctx, const char *label, uint32_t id, char *buf, size_t cap, int cur, int mn,
                    int mx, bool enabled, int *out) {
    bool changed = false;
    PANEL_ROW_BEGIN(label, enabled ? &g_row : &g_dim) {
        changed = ui_int_field(ctx, id, buf, cap, cur, mn, mx, enabled, out);
    }
    PANEL_ROW_END;
    return changed;
}

static bool row_float(nt_ui_context_t *ctx, const char *label, uint32_t id, char *buf, size_t cap, float cur, float mn,
                      float mx, bool enabled, float *out) {
    bool changed = false;
    PANEL_ROW_BEGIN(label, enabled ? &g_row : &g_dim) {
        changed = ui_float_field(ctx, id, buf, cap, cur, mn, mx, enabled, out);
    }
    PANEL_ROW_END;
    return changed;
}

static int size_preset_index(int v) {
    for (int i = 0; i < 7; i++) {
        if (k_size_presets[i] == v) {
            return i;
        }
    }
    return -1;
}

/* --- Atlas settings (region F) --- */
static void declare_atlas_settings(nt_ui_context_t *ctx, tp_project_atlas *a) {
    /* Basic: shape, max size, padding, allow transform. */
    const int ns = row_combo(ctx, "Shape", nt_ui_id("set/shape"), &s_dd_shape_open,
                             (a->shape >= 0 && a->shape < 3) ? k_shape_names[a->shape] : "?", a->shape, k_shape_names, 3, true);
    if (ns >= 0 && ns != a->shape) {
        a->shape = ns;
        gui_project_touch_setting();
    }
    char szpv[16];
    (void)snprintf(szpv, sizeof szpv, "%d", a->max_size);
    static const char *const size_labels[7] = {"256", "512", "1024", "2048", "4096", "8192", "16384"};
    const int nsz = row_combo(ctx, "Max page size", nt_ui_id("set/size"), &s_dd_size_open, szpv, size_preset_index(a->max_size),
                              size_labels, 7, true);
    if (nsz >= 0 && k_size_presets[nsz] != a->max_size) {
        a->max_size = k_size_presets[nsz];
        gui_project_touch_setting();
    }
    if (a->max_size > 4096) {
        panel_note(ctx, "Pages over 4096 may not load on mobile GPUs / stock engine runtime.");
    }
    int iv = 0;
    if (row_int(ctx, "Padding", nt_ui_id("set/pad"), s_nb_pad, sizeof s_nb_pad, a->padding, 0, 16384, true, &iv) &&
        iv != a->padding) {
        a->padding = iv;
        gui_project_touch_setting();
    }
    bool bv = false;
    if (row_check(ctx, "Allow transform", nt_ui_id("set/xform"), a->allow_transform, true, &bv)) {
        a->allow_transform = bv;
        gui_project_touch_setting();
    }

    /* Advanced disclosure. */
    panel_header(ctx, nt_ui_id("set/adv"), "Advanced", &s_atlas_adv_open, &g_body, C_BG);
    if (!s_atlas_adv_open) {
        return;
    }
    if (row_int(ctx, "Margin", nt_ui_id("set/margin"), s_nb_margin, sizeof s_nb_margin, a->margin, 0, 16384, true, &iv) &&
        iv != a->margin) {
        a->margin = iv;
        gui_project_touch_setting();
    }
    const bool extrude_ok = (a->shape == 0 /* RECT */);
    if (row_int(ctx, "Extrude", nt_ui_id("set/extrude"), s_nb_extrude, sizeof s_nb_extrude, a->extrude, 0, 255, extrude_ok,
                &iv) &&
        iv != a->extrude) {
        a->extrude = iv;
        gui_project_touch_setting();
    }
    if (!extrude_ok) {
        panel_note(ctx, "Extrude requires Rect shape \xE2\x80\x94 use Padding for polygon modes.");
    }
    if (row_slider(ctx, "Alpha threshold", nt_ui_id("set/alpha"), s_nb_alpha, sizeof s_nb_alpha, a->alpha_threshold, 0,
                   255, true, &iv) &&
        iv != a->alpha_threshold) {
        a->alpha_threshold = iv;
        gui_project_touch_setting();
    }
    if (row_int(ctx, "Max vertices", nt_ui_id("set/maxv"), s_nb_maxv, sizeof s_nb_maxv, a->max_vertices, 1, 16, true, &iv) &&
        iv != a->max_vertices) {
        a->max_vertices = iv;
        gui_project_touch_setting();
    }
    if (row_check(ctx, "Power of two", nt_ui_id("set/pot"), a->power_of_two, true, &bv)) {
        a->power_of_two = bv;
        gui_project_touch_setting();
    }
    float fv = 0.0F;
    if (row_float(ctx, "Pixels/unit", nt_ui_id("set/ppu"), s_nb_ppu, sizeof s_nb_ppu, a->pixels_per_unit, 0.0001F,
                  100000.0F, true, &fv) &&
        fv != a->pixels_per_unit) {
        a->pixels_per_unit = fv;
        gui_project_touch_setting();
    }
}

/* Currently-selected leaf sprite row (a file source or a folder child), or NULL. */
static const sprite_row *selected_leaf_row(void) {
    for (int i = 0; i < s_row_count; i++) {
        const sprite_row *r = &s_rows[i];
        const bool sel = r->is_source ? (s_sel_src == r->src && s_sel_child == -1)
                                      : (s_sel_src == r->src && s_sel_child == r->child);
        if (sel && !r->is_folder && !r->missing && r->sprite_name[0] != '\0') {
            return r;
        }
    }
    return NULL;
}

/* Per-sprite "Default (inherited: ..) then explicit values" override combo. `cur_ov`
 * is TP_PROJECT_OV_INHERIT or an explicit index; row 0 is Default. Returns the new
 * override value (or TP_PROJECT_OV_INHERIT), or INT_MIN if unchanged. */
#define OV_UNCHANGED INT_MIN
static int row_override_combo(nt_ui_context_t *ctx, const char *label, uint32_t id, bool *open, int cur_ov,
                              int explicit_base, const char *const *values, int value_count, const char *default_label,
                              bool enabled) {
    /* Build the option list: [default_label, values...] in a small stack array. */
    const char *opts[20];
    int n = 0;
    opts[n++] = default_label;
    for (int i = 0; i < value_count && n < 20; i++) {
        opts[n++] = values[i];
    }
    const int cur_row = (cur_ov == TP_PROJECT_OV_INHERIT) ? 0 : (cur_ov - explicit_base + 1);
    const char *preview = (cur_row >= 0 && cur_row < n) ? opts[cur_row] : default_label;
    const int pick = row_combo(ctx, label, id, open, preview, cur_row, opts, n, enabled);
    if (pick < 0) {
        return OV_UNCHANGED;
    }
    return (pick == 0) ? TP_PROJECT_OV_INHERIT : (explicit_base + pick - 1);
}

/* --- Region (selected sprite) + per-region packing overrides --- */
static void declare_region_settings(nt_ui_context_t *ctx, tp_project_atlas *a) {
    const sprite_row *row = selected_leaf_row();
    if (!row) {
        if (s_sel_missing) {
            panel_note(ctx, "Selected file is missing \xE2\x80\x94 restore it and press Refresh (F5).");
        } else {
            panel_note(ctx, "Select a sprite (list or canvas) to edit its region.");
        }
        return;
    }
    const char *sprite = row->sprite_name;
    const tp_project_sprite *ov = tp_project_atlas_find_sprite(a, sprite);
    const tp_result *pr = gui_pack_result(s_sel_atlas);
    const int ri = pr ? gui_pack_find_sprite(s_sel_atlas, sprite) : -1;

    /* Final name + Rename (reuse the existing inline rename path). */
    char fname[224];
    (void)snprintf(fname, sizeof fname, "%s", (ov && ov->rename) ? ov->rename : sprite);
    PANEL_ROW_BEGIN("Final name", &g_row) {
        ui_label_fit(ctx, fname, &g_body, S(PANEL_LABEL_W + 30.0F), 0U);
        if (ui_btn(ctx, nt_ui_id("reg/rename"), "Rename", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            start_sprite_edit_named(sprite);
        }
    }
    PANEL_ROW_END;

    /* Source file + size (from the last pack result when available). */
    char src[256];
    if (pr && ri >= 0) {
        (void)snprintf(src, sizeof src, "%s  \xC2\xB7  %d x %d", path_last(row->abs), pr->sprites[ri].sourceSize.w,
                       pr->sprites[ri].sourceSize.h);
    } else {
        (void)snprintf(src, sizeof src, "%s  (pack to measure)", path_last(row->abs));
    }
    PANEL_ROW_BEGIN("Source", &g_caption) {
        ui_label_fit(ctx, src, &g_caption, S(150.0F), 0U);
    }
    PANEL_ROW_END;

    const float ox = ov ? ov->origin_x : TP_PROJECT_ORIGIN_DEFAULT;
    const float oy = ov ? ov->origin_y : TP_PROJECT_ORIGIN_DEFAULT;
    float fv = 0.0F;
    if (row_float(ctx, "Pivot X", nt_ui_id("reg/ox"), s_nb_ox, sizeof s_nb_ox, ox, -100.0F, 100.0F, true, &fv)) {
        gui_project_set_sprite_origin(s_sel_atlas, sprite, fv, oy);
    }
    if (row_float(ctx, "Pivot Y", nt_ui_id("reg/oy"), s_nb_oy, sizeof s_nb_oy, oy, -100.0F, 100.0F, true, &fv)) {
        gui_project_set_sprite_origin(s_sel_atlas, sprite, ox, fv);
    }

    static const char *const s9_labels[4] = {"Slice9 L", "Slice9 R", "Slice9 T", "Slice9 B"};
    static const char *const s9_ids[4] = {"reg/s9l", "reg/s9r", "reg/s9t", "reg/s9b"};
    bool any_s9 = false;
    for (int k = 0; k < 4; k++) {
        const int cur = ov ? ov->slice9_lrtb[k] : 0;
        if (cur != 0) {
            any_s9 = true;
        }
        int iv = 0;
        if (row_int(ctx, s9_labels[k], nt_ui_id(s9_ids[k]), s_nb_s9[k], sizeof s_nb_s9[k], cur, 0, 4096, true, &iv) &&
            iv != cur) {
            gui_project_set_sprite_slice9(s_sel_atlas, sprite, k, iv);
        }
    }
    if (any_s9) {
        panel_note(ctx, "Slice-9 forces Rect shape and no rotation for this sprite.");
    }

    /* Read-only packed readout. */
    if (pr && ri >= 0) {
        const tp_sprite *s = &pr->sprites[ri];
        char geom[24];
        if (s->vert_count > 4) {
            (void)snprintf(geom, sizeof geom, "%d verts", s->vert_count);
        } else {
            (void)snprintf(geom, sizeof geom, "rect");
        }
        char rd[192];
        (void)snprintf(rd, sizeof rd, "frame %dx%d @ %d,%d  \xC2\xB7  %s  \xC2\xB7  %s", s->frame.w, s->frame.h,
                       s->frame.x, s->frame.y, transform_decode_str(s->transform), geom);
        PANEL_ROW_BEGIN("Packed", &g_caption) {
            ui_label_fit(ctx, rd, &g_caption, S(150.0F), 0U);
        }
        PANEL_ROW_END;
    }

    /* Per-region packing overrides (owner scope 2026-07-10). */
    panel_header(ctx, nt_ui_id("reg/ov"), "Packing overrides", &s_region_ov_open, &g_body, C_BG);
    if (!s_region_ov_open) {
        return;
    }
    const int ov_shape = ov ? ov->ov_shape : TP_PROJECT_OV_INHERIT;
    const int ov_rot = ov ? ov->ov_allow_rotate : TP_PROJECT_OV_INHERIT;
    const int ov_mv = ov ? ov->ov_max_vertices : TP_PROJECT_OV_INHERIT;
    const int ov_margin = ov ? ov->ov_margin : TP_PROJECT_OV_INHERIT;
    const int ov_extrude = ov ? ov->ov_extrude : TP_PROJECT_OV_INHERIT;

    /* Slice9 auto-forces RECT + no-rotate: show the shape/rotate overrides disabled. */
    if (any_s9) {
        panel_note(ctx, "Shape & rotation overrides are set by slice-9 (Rect, no rotation).");
    }
    char shape_def[48];
    (void)snprintf(shape_def, sizeof shape_def, "Default (%s)", (a->shape >= 0 && a->shape < 3) ? k_shape_names[a->shape] : "?");
    const int ps = row_override_combo(ctx, "Shape", nt_ui_id("reg/ov_shape"), &s_dd_ov_shape_open, ov_shape, 0,
                                      k_shape_names, 3, shape_def, !any_s9);
    if (ps != OV_UNCHANGED && ps != ov_shape) {
        gui_project_set_sprite_override(s_sel_atlas, sprite, GUI_SPRITE_OV_SHAPE, ps);
    }
    static const char *const rot_values[1] = {"No rotation"};
    const char *rot_def = a->allow_transform ? "Default (rotate/flip)" : "Default (no transform)";
    const int prv = row_override_combo(ctx, "Rotation", nt_ui_id("reg/ov_rot"), &s_dd_ov_rot_open, ov_rot, 0, rot_values,
                                       1, rot_def, !any_s9);
    if (prv != OV_UNCHANGED && prv != ov_rot) {
        gui_project_set_sprite_override(s_sel_atlas, sprite, GUI_SPRITE_OV_ROTATE, prv);
    }
    static const char *const mv_values[16] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                              "9", "10", "11", "12", "13", "14", "15", "16"};
    char mv_def[40];
    (void)snprintf(mv_def, sizeof mv_def, "Default (%d)", a->max_vertices);
    const int pmv = row_override_combo(ctx, "Max vertices", nt_ui_id("reg/ov_mv"), &s_dd_ov_mv_open, ov_mv, 1, mv_values,
                                       16, mv_def, true);
    if (pmv != OV_UNCHANGED && pmv != ov_mv) {
        gui_project_set_sprite_override(s_sel_atlas, sprite, GUI_SPRITE_OV_MAXVERT, pmv);
    }

    /* margin / extrude overrides: a "override?" checkbox + numeric (1..255). extrude is
     * disabled unless the sprite's effective shape is RECT (§3.3f, per sprite). */
    const int eff_shape = any_s9 ? 0 : (ov_shape != TP_PROJECT_OV_INHERIT ? ov_shape : a->shape);
    {
        bool on = (ov_margin != TP_PROJECT_OV_INHERIT);
        PANEL_ROW_BEGIN("Margin ovr", &g_row) {
            const bool cbc = tp_checkbox(ctx, nt_ui_id("reg/ov_mcb"), on, true);
            if (cbc) {
                on = !on;
            }
            const int seed = (a->margin >= 1) ? (a->margin > 255 ? 255 : a->margin) : 1;
            if (cbc) {
                gui_project_set_sprite_override(s_sel_atlas, sprite, GUI_SPRITE_OV_MARGIN, on ? seed : TP_PROJECT_OV_INHERIT);
            }
            const int disp = (ov_margin != TP_PROJECT_OV_INHERIT) ? ov_margin : seed;
            int iv = 0;
            if (ui_int_field(ctx, nt_ui_id("reg/ov_mf"), s_nb_ov_margin, sizeof s_nb_ov_margin, disp, 1, 255,
                             on && !cbc, &iv) &&
                on && iv != ov_margin) {
                gui_project_set_sprite_override(s_sel_atlas, sprite, GUI_SPRITE_OV_MARGIN, iv);
            }
        }
        PANEL_ROW_END;
    }
    {
        const bool ex_enabled = (eff_shape == 0 /* RECT */);
        bool on = (ov_extrude != TP_PROJECT_OV_INHERIT);
        PANEL_ROW_BEGIN("Extrude ovr", ex_enabled ? &g_row : &g_dim) {
            const bool cbc = tp_checkbox(ctx, nt_ui_id("reg/ov_ecb"), on, ex_enabled);
            if (cbc) {
                on = !on;
            }
            const int seed = (a->extrude >= 1) ? (a->extrude > 255 ? 255 : a->extrude) : 1;
            if (cbc && ex_enabled) {
                gui_project_set_sprite_override(s_sel_atlas, sprite, GUI_SPRITE_OV_EXTRUDE, on ? seed : TP_PROJECT_OV_INHERIT);
            }
            const int disp = (ov_extrude != TP_PROJECT_OV_INHERIT) ? ov_extrude : seed;
            int iv = 0;
            if (ui_int_field(ctx, nt_ui_id("reg/ov_ef"), s_nb_ov_extrude, sizeof s_nb_ov_extrude, disp, 1, 255,
                             ex_enabled && on && !cbc, &iv) &&
                on && iv != ov_extrude) {
                gui_project_set_sprite_override(s_sel_atlas, sprite, GUI_SPRITE_OV_EXTRUDE, iv);
            }
        }
        PANEL_ROW_END;
        if (!ex_enabled) {
            panel_note(ctx, "Extrude override needs the sprite's effective shape to be Rect.");
        }
    }
}

/* --- Export targets (region G, audit I1) --- */
static void declare_export_targets(nt_ui_context_t *ctx, tp_project_atlas *a) {
    const int ne = tp_exporter_count();
    const char *exp_labels[24];
    int nlabels = 0;
    for (int i = 0; i < ne && nlabels < 24; i++) {
        const tp_exporter *e = tp_exporter_at(i);
        exp_labels[nlabels++] = (e && e->display_name) ? e->display_name : (e ? e->id : "?");
    }
    if (a->target_count == 0) {
        panel_note(ctx, "No export targets. Add one so this atlas exports files.");
    }
    const int shown = (a->target_count < GUI_MAX_TARGETS) ? a->target_count : GUI_MAX_TARGETS;
    for (int ti = 0; ti < shown; ti++) {
        tp_project_target *t = &a->targets[ti];
        char idbuf[48];
        (void)snprintf(idbuf, sizeof idbuf, "tgt/row_%d", ti);
        const uint32_t row_id = nt_ui_id(idbuf);
        if (nt_ui_menu_open_trigger(ctx, s_id_ctx_menu, row_id, false, &s_ctx_state)) {
            close_menubar_menus();
            s_ctx_kind = CTX_TARGET;
            s_ctx_target = ti;
        }
        /* find current exporter index for the combo selection */
        int cur_exp = -1;
        for (int i = 0; i < nlabels; i++) {
            const tp_exporter *e = tp_exporter_at(i);
            if (e && strcmp(e->id, t->exporter_id) == 0) {
                cur_exp = i;
                break;
            }
        }
        CLAY({.id = {.id = row_id},
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                         .padding = {Su(4), Su(4), Su(4), Su(4)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(4)},
              .backgroundColor = C_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
            /* row 1: enabled checkbox + exporter dropdown */
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (tp_checkbox(ctx, nt_ui_child_id(row_id, "en"), t->enabled, true)) {
                    gui_project_set_target(s_sel_atlas, ti, t->exporter_id, t->out_path, !t->enabled);
                }
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    const char *pv = (cur_exp >= 0) ? exp_labels[cur_exp] : t->exporter_id;
                    if (nt_ui_combo_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_child_id(row_id, "exp"), pv,
                                          &s_dd_style, &s_dd_target_open[ti])) {
                        for (int i = 0; i < nlabels; i++) {
                            if (nt_ui_combo_selectable(ctx, (uint32_t)i, exp_labels[i], i == cur_exp)) {
                                const tp_exporter *e = tp_exporter_at(i);
                                if (e) {
                                    gui_project_set_target(s_sel_atlas, ti, e->id, t->out_path, t->enabled);
                                }
                            }
                        }
                        nt_ui_combo_end(ctx);
                    }
                }
                if (ui_btn(ctx, nt_ui_child_id(row_id, "rm"), "x", &g_btn_ghost, true, 24.0F, 22.0F, &g_caption)) {
                    s_pending_remove_target = ti;
                }
            }
            /* row 2: out path + browse */
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    if (ui_text_field(ctx, nt_ui_child_id(row_id, "path"), s_nb_target_path[ti], sizeof s_nb_target_path[ti],
                                      t->out_path, true, "out/atlas.json")) {
                        gui_project_set_target(s_sel_atlas, ti, t->exporter_id, s_nb_target_path[ti], t->enabled);
                    }
                }
                if (ui_btn(ctx, nt_ui_child_id(row_id, "browse"), "\xE2\x80\xA6", &g_btn_ghost, true, 28.0F, 22.0F, &g_caption)) { /* U+2026 */
                    s_pending_browse_target = ti;
                }
            }
        }
    }
    if (ui_btn(ctx, nt_ui_id("tgt/add"), "+ Target", &g_btn_ghost, true, 0.0F, 26.0F, &g_caption)) {
        s_pending_add_target = true;
    }
}

/* --- Animation editor (ux.md §3.7b): id / fps / playback / flips + ordered frame list --- */
static void declare_animation_editor(nt_ui_context_t *ctx, tp_project_atlas *a) {
    if (s_sel_anim < 0 || s_sel_anim >= a->animation_count) {
        panel_note(ctx, "Select an animation (left panel) to edit its frames, fps, playback and flips.");
        return;
    }
    tp_project_anim *an = &a->animations[s_sel_anim];
    const bool editing_id = (s_edit_kind == EDIT_ANIM && s_edit_anim == s_sel_anim);

    PANEL_ROW_BEGIN("Id", &g_row) {
        if (editing_id) {
            if (render_rename_field(ctx)) {
                commit_anim_rename();
            }
        } else {
            ui_label_fit(ctx, an->id, &g_body, S(PANEL_LABEL_W - 20.0F), 0U);
            if (ui_btn(ctx, nt_ui_id("anim/rename"), "Rename", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
                start_anim_edit(s_sel_anim);
            }
        }
    }
    PANEL_ROW_END;

    PANEL_ROW_BEGIN("Preview", &g_row) {
        if (ui_btn(ctx, nt_ui_id("anim/play"), s_preview_active ? "Playing\xE2\x80\xA6" : "Play", &g_btn, true, 0.0F, 24.0F,
                   &g_caption)) {
            s_pending_open_preview = true;
        }
    }
    PANEL_ROW_END;

    float fv = 0.0F;
    if (row_float(ctx, "FPS", nt_ui_id("anim/fps"), s_nb_anim_fps, sizeof s_nb_anim_fps, an->fps, 1.0F, 240.0F, true,
                  &fv)) {
        gui_project_set_anim_fps(s_sel_atlas, s_sel_anim, fv);
    }
    const char *pv = (an->playback >= 0 && an->playback < 7) ? k_playback_names[an->playback] : "?";
    const int npb = row_combo(ctx, "Playback", nt_ui_id("anim/pb"), &s_dd_playback_open, pv, an->playback,
                              k_playback_names, 7, true);
    if (npb >= 0 && npb != an->playback) {
        gui_project_set_anim_playback(s_sel_atlas, s_sel_anim, npb);
    }
    bool bv = false;
    if (row_check(ctx, "Flip H", nt_ui_id("anim/fh"), an->flip_h, true, &bv)) {
        gui_project_set_anim_flip(s_sel_atlas, s_sel_anim, bv, an->flip_v);
    }
    if (row_check(ctx, "Flip V", nt_ui_id("anim/fv"), an->flip_v, true, &bv)) {
        gui_project_set_anim_flip(s_sel_atlas, s_sel_anim, an->flip_h, bv);
    }

    /* Frames header + "Add frames" (from the current sprite multi-selection). */
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        char fh[32];
        (void)snprintf(fh, sizeof fh, "Frames (%d)", an->frame_count);
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), fh, &g_row);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        if (ui_btn(ctx, nt_ui_id("anim/addf"), "Add frames", &g_btn_ghost, s_multi_sel_count > 0, 0.0F, 24.0F,
                   &g_caption)) {
            add_selection_frames_to_anim(s_sel_anim);
        }
    }
    if (an->frame_count == 0) {
        panel_note(ctx, "No frames. Multi-select sprites on the left, then press Add frames.");
    }

    /* Frame rows: reorder (up/down) + remove. Mutations are captured and applied after the loop so the
     * array is not resized mid-iteration. */
    int fact = 0; /* 1 remove, 2 up, 3 down */
    int fidx = -1;
    const int fcount = an->frame_count;
    for (int fi = 0; fi < fcount; fi++) {
        char idbuf[48];
        (void)snprintf(idbuf, sizeof idbuf, "anim/frame_%d", fi);
        const uint32_t row_id = nt_ui_id(idbuf);
        const nt_ui_events_t ev = nt_ui_events(ctx, row_id, NULL);
        if (ev.clicked) {
            s_sel_anim_frame = fi;
        }
        const bool sel = (fi == s_sel_anim_frame);
        const Clay_Color bg = sel ? C_SEL : (ev.hovered ? C_HOVER : C_BG);
        CLAY({.id = {.id = row_id},
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                         .padding = {Su(6), Su(4), 0, 0},
                         .childGap = Su(4),
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = bg,
              .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
            char lab[224];
            (void)snprintf(lab, sizeof lab, "%02d  %s", fi + 1, an->frames[fi]);
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
                  .clip = {.horizontal = true}}) {
                ui_label_fit(ctx, lab, &g_row, right_panel_text_w(S(98.0F)), row_id); /* reserve: up/down/x + gaps */
            }
            if (ui_btn(ctx, nt_ui_child_id(row_id, "up"), "\xE2\x86\x91", &g_btn_ghost, fi > 0, 26.0F, 22.0F, &g_caption)) {
                fact = 2;
                fidx = fi;
            }
            if (ui_btn(ctx, nt_ui_child_id(row_id, "dn"), "\xE2\x86\x93", &g_btn_ghost, fi < fcount - 1, 26.0F, 22.0F,
                       &g_caption)) {
                fact = 3;
                fidx = fi;
            }
            if (ui_btn(ctx, nt_ui_child_id(row_id, "x"), "x", &g_btn_ghost, true, 24.0F, 22.0F, &g_caption)) {
                fact = 1;
                fidx = fi;
            }
        }
    }
    if (fact == 1 && fidx >= 0) {
        gui_project_anim_remove_frame(s_sel_atlas, s_sel_anim, fidx);
        s_sel_anim_frame = -1;
    } else if (fact == 2 && fidx >= 0) {
        gui_project_anim_move_frame(s_sel_atlas, s_sel_anim, fidx, -1);
        s_sel_anim_frame = fidx - 1;
    } else if (fact == 3 && fidx >= 0) {
        gui_project_anim_move_frame(s_sel_atlas, s_sel_anim, fidx, +1);
        s_sel_anim_frame = fidx + 1;
    }
}

static void declare_right_panel(nt_ui_context_t *ctx) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    CLAY({.id = {.id = s_id_right_panel},
          .layout = {.sizing = {CLAY_SIZING_FIXED(s_right_panel_w), CLAY_SIZING_GROW(0)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = C_PANEL,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
          .clip = {.horizontal = true},
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        nt_ui_scroll_begin(ctx, NULL, nt_ui_id("panel/scroll"), &s_panel_scroll,
                           &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}});
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                         .padding = {Su(10), Su(10), Su(10), Su(12)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(4)}}) {
            if (!a) {
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No atlas selected.", &g_caption);
            } else {
                char title[96];
                (void)snprintf(title, sizeof title, "Atlas settings \xC2\xB7 %s", a->name);
                panel_header(ctx, nt_ui_id("sec/atlas"), title, &s_sec_atlas_open, &g_title, C_STATUS);
                if (s_sec_atlas_open) {
                    declare_atlas_settings(ctx, a);
                }
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(6))}}}) {}
                panel_header(ctx, nt_ui_id("sec/region"), "Region", &s_sec_region_open, &g_title, C_STATUS);
                if (s_sec_region_open) {
                    declare_region_settings(ctx, a);
                }
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(6))}}}) {}
                panel_header(ctx, nt_ui_id("sec/anim"), "Animation", &s_sec_anim_open, &g_title, C_STATUS);
                if (s_sec_anim_open) {
                    declare_animation_editor(ctx, a);
                }
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(6))}}}) {}
                panel_header(ctx, nt_ui_id("sec/export"), "Export targets", &s_sec_export_open, &g_title, C_STATUS);
                if (s_sec_export_open) {
                    declare_export_targets(ctx, a);
                }
            }
        }
        nt_ui_scroll_end(ctx);
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

/* Writes a tiny valid 2x2 32-bit uncompressed TGA (stb decodes it) -- cheap procedural sprite. */
static void write_tga_2x2(const char *path) {
    const unsigned char hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28};
    unsigned char px[2 * 2 * 4];
    for (int i = 0; i < 4; i++) {
        px[i * 4 + 0] = 200; /* B */
        px[i * 4 + 1] = 180; /* G */
        px[i * 4 + 2] = 160; /* R */
        px[i * 4 + 3] = 255; /* A */
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        (void)fwrite(hdr, 1, sizeof hdr, f);
        (void)fwrite(px, 1, sizeof px, f);
        (void)fclose(f);
    }
}

/* UTF-8 "тест_спрайт" (a Cyrillic sprite name) -- exercises multi-byte names end-to-end. */
#define CYR_STEM "\xD1\x82\xD0\xB5\xD1\x81\xD1\x82_\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82"

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

    /* --- in-process pack of the demo atlases: real tp_pack via gui_pack (timing + assertions) --- */
    {
        char proj[600];
        to_abs("examples/defold-demo/defold-demo.ntpacker_project", proj, sizeof proj);
        char perr[256] = {0};
        if (gui_project_open(proj, perr, sizeof perr) == TP_STATUS_OK) {
            tp_project *dp = gui_project_get();
            int i_rotate = -1;
            int i_basic = -1;
            for (int i = 0; i < dp->atlas_count; i++) {
                if (strcmp(dp->atlases[i].name, "rotate") == 0) {
                    i_rotate = i;
                } else if (strcmp(dp->atlases[i].name, "basic") == 0) {
                    i_basic = i;
                }
            }
            gui_scan_invalidate_all();
            double ms_r = 0.0;
            double ms_b = 0.0;
            char pe[256] = {0};
            char note[128] = {0};
            const bool okr = (i_rotate >= 0) && gui_pack_atlas(i_rotate, &ms_r, pe, sizeof pe, note, sizeof note);
            const tp_result *rr = gui_pack_result(i_rotate);
            nt_log_info("SELFTEST: pack 'rotate' -> %d in %.1f ms sprites=%d pages=%d (find 'a'=%d) %s", okr, ms_r,
                        rr ? rr->sprite_count : -1, rr ? rr->page_count : -1, gui_pack_find_sprite(i_rotate, "a"),
                        okr ? "" : pe);
            NT_ASSERT(okr && rr && rr->sprite_count == 3 && rr->page_count >= 1 && "pack rotate");
            NT_ASSERT(gui_pack_find_sprite(i_rotate, "a") >= 0 && "region lookup 'a'");
            char pe2[256] = {0};
            const bool okb = (i_basic >= 0) && gui_pack_atlas(i_basic, &ms_b, pe2, sizeof pe2, note, sizeof note);
            const tp_result *rb = gui_pack_result(i_basic);
            nt_log_info("SELFTEST: pack 'basic' -> %d in %.1f ms sprites=%d pages=%d %s", okb, ms_b,
                        rb ? rb->sprite_count : -1, rb ? rb->page_count : -1, okb ? "" : pe2);

            /* export 'rotate' via gui_pack_export, ISOLATED to a throwaway base under the build dir so
             * the demo's committed exports (owned by another agent) are never touched: disable the
             * atlas's other targets, point json-neotolis at the temp base, then assert the files exist.
             * tp_export_run uses the target out_path as the exporter BASE and appends .json / -N.png. */
            tp_project_atlas *rot_a = tp_project_get_atlas(dp, i_rotate);
            int jtarget = -1;
            for (int k = 0; rot_a && k < rot_a->target_count; k++) {
                if (strcmp(rot_a->targets[k].exporter_id, "json-neotolis") == 0) {
                    jtarget = k;
                } else {
                    gui_project_set_target(i_rotate, k, rot_a->targets[k].exporter_id, rot_a->targets[k].out_path, false);
                }
            }
            char tbase[700] = {0};
            (void)snprintf(tbase, sizeof tbase, "%s/selftest_rotate_export", s_exe_dir);
            if (jtarget >= 0) {
                gui_project_set_target(i_rotate, jtarget, "json-neotolis", tbase, true);
            }
            int etg = 0;
            int enc = 0;
            char eerr[256] = {0};
            char enote[128] = {0};
            const bool oke = (i_rotate >= 0 && jtarget >= 0) &&
                             gui_pack_export(i_rotate, &etg, &enc, eerr, sizeof eerr, enote, sizeof enote);
            char jpath[720] = {0};
            char ppath[720] = {0};
            (void)snprintf(jpath, sizeof jpath, "%s.json", tbase);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", tbase);
            bool jok = false;
            bool pok = false;
            {
                FILE *jf = fopen(jpath, "rb");
                if (jf) {
                    jok = (fgetc(jf) == '{'); /* lightweight parse check; full parse is in ctest test_export_json */
                    (void)fclose(jf);
                }
                FILE *pf = fopen(ppath, "rb");
                if (pf) {
                    pok = (fgetc(pf) != EOF); /* exists AND non-empty */
                    (void)fclose(pf);
                }
            }
            nt_log_info("SELFTEST: export 'rotate' -> ok=%d targets=%d notices=%d json{=%d png0=%d %s", oke, etg, enc,
                        jok, pok, oke ? "" : eerr);
            NT_ASSERT(oke && jok && pok && "export rotate: json + page png must exist");
            (void)remove(jpath); /* throwaway under the build dir */
            (void)remove(ppath);
        } else {
            nt_log_info("SELFTEST: demo project open failed: %s", perr);
        }
    }

    /* --- stress: 520 procedural sprites incl. a Cyrillic name -> pack + row model + Cyrillic RT --- */
    {
        char sdir[700];
        (void)snprintf(sdir, sizeof sdir, "%s/selftest_stress", s_exe_dir);
#ifdef _WIN32
        (void)CreateDirectoryA(sdir, NULL);
#endif
        const int N = 520;
        for (int i = 0; i < N; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/spr_%03d.tga", sdir, i);
            write_tga_2x2(fp);
        }
        char cfp[840];
        (void)snprintf(cfp, sizeof cfp, "%s/%s.tga", sdir, CYR_STEM);
        write_tga_2x2(cfp);

        const int sidx = gui_project_add_atlas();
        if (sidx >= 0) {
            (void)gui_project_add_source(sidx, sdir);
            gui_scan_invalidate_all();
            double sms = 0.0;
            char serr[256] = {0};
            char snote[128] = {0};
            const bool oks = gui_pack_atlas(sidx, &sms, serr, sizeof serr, snote, sizeof snote);
            const tp_result *sr = gui_pack_result(sidx);
            const int cyr_idx = gui_pack_find_sprite(sidx, CYR_STEM);
            nt_log_info("SELFTEST: stress pack -> %d in %.1f ms sprites=%d pages=%d cyr_idx=%d %s", oks, sms,
                        sr ? sr->sprite_count : -1, sr ? sr->page_count : -1, cyr_idx, oks ? "" : serr);
            NT_ASSERT(oks && sr && sr->sprite_count >= N + 1 && "stress pack 520+ sprites");
            NT_ASSERT(cyr_idx >= 0 && "Cyrillic-named region lookup");

            /* Cyrillic rename + save/load round-trip (multi-byte name survives serialization). */
            gui_project_set_sprite_rename(sidx, CYR_STEM, "\xD0\xB8\xD0\xBC\xD1\x8F"); /* "имя" */
            char *sbuf = NULL;
            size_t slen = 0;
            tp_error sbe = {0};
            tp_project *slp = NULL;
            tp_error sle = {0};
            const tp_status sbst = tp_project_save_buffer(gui_project_get(), &sbuf, &slen, &sbe);
            const tp_status slst = (sbst == TP_STATUS_OK) ? tp_project_load_buffer(sbuf, slen, &slp, &sle) : sbst;
            const tp_project_sprite *ov =
                (slp && slp->atlas_count > sidx) ? tp_project_atlas_find_sprite(&slp->atlases[sidx], CYR_STEM) : NULL;
            nt_log_info("SELFTEST: Cyrillic rename RT save=%s load=%s override='%s'", tp_status_str(sbst),
                        tp_status_str(slst), (ov && ov->rename) ? ov->rename : "(none)");
            NT_ASSERT(ov && ov->rename && strcmp(ov->rename, "\xD0\xB8\xD0\xBC\xD1\x8F") == 0 &&
                      "Cyrillic name survives save/load");
            tp_project_destroy(slp);
            free(sbuf);

            /* Row model materializes 520+ rows (incl. the Cyrillic label) without overflow. */
            s_sel_atlas = sidx;
            build_rows(gui_project_get(), tp_project_get_atlas(gui_project_get(), sidx));
            bool cyr_row = false;
            for (int i = 0; i < s_row_count; i++) {
                if (strcmp(s_rows[i].sprite_name, CYR_STEM) == 0) {
                    cyr_row = true;
                    break;
                }
            }
            nt_log_info("SELFTEST: stress rows=%d cyr_row=%d | state pool slots=%u probe=%u ring=%u (bounded, no overflow)",
                        s_row_count, cyr_row, (unsigned)UI_STATE_SLOTS, (unsigned)UI_STATE_PROBE_MAX,
                        (unsigned)UI_ROW_ID_RING);
            NT_ASSERT(s_row_count >= N + 1 && cyr_row && "stress row model incl. Cyrillic");
        }
        /* cleanup scratch sprites (keep the tree clean). The no-overflow guarantee is id_ring x
         * state_slots capacity, verified above + interactively. */
        for (int i = 0; i < N; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/spr_%03d.tga", sdir, i);
            (void)remove(fp);
        }
        (void)remove(cfp);
#ifdef _WIN32
        (void)RemoveDirectoryA(sdir);
#endif
    }

    /* --- settings panel: stale-on-change, effective-extrude, per-region RECT override,
     *     and a fresh-project seeded-target export (regions F/G, §3.3f, owner overrides) --- */
    {
        gui_project_new();
        gui_pack_clear(-1);
        tp_project *fp = gui_project_get();
        NT_ASSERT(fp && fp->atlas_count == 1 && fp->atlases[0].target_count >= 1 && "fresh project seeds a target (I1)");
        nt_log_info("SELFTEST: fresh target[0]=%s base=%s", fp->atlases[0].targets[0].exporter_id,
                    fp->atlases[0].targets[0].out_path);

        char afolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
        (void)gui_project_add_source(0, afolder);
        gui_scan_invalidate_all();

        tp_project_atlas *a0 = tp_project_get_atlas(gui_project_get(), 0);
        gui_project_mark_packed(); /* pretend current, then a setting change must set stale */
        a0->padding = 7;
        gui_project_touch_setting();
        nt_log_info("SELFTEST: setting change stale=%d (expect 1)", gui_project_is_stale());
        NT_ASSERT(gui_project_is_stale() && "a setting change sets preview stale");

        /* shape=concave + extrude=3 -> preview pack succeeds via the effective-extrude-0 rule */
        a0->shape = 2; /* CONCAVE_CONTOUR */
        a0->extrude = 3;
        gui_project_touch_setting();
        double pms = 0.0;
        char perr[256] = {0};
        char pnote[128] = {0};
        const bool okc = gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
        nt_log_info("SELFTEST: concave+extrude3 pack -> %d in %.1fms (%s)", okc, pms, okc ? "effective extrude 0" : perr);
        NT_ASSERT(okc && "concave+extrude=3 packs (effective extrude 0)");

        /* per-sprite shape=RECT override -> that region packs as an exact 4-vert rect */
        char afabs[512];
        if (tp_project_resolve_path(gui_project_get(), gui_project_get()->atlases[0].sources[0], afabs, sizeof afabs) ==
            TP_STATUS_OK) {
            const gui_scan_result *sc = gui_scan_get(afabs);
            if (sc->count > 0) {
                char spn[192];
                (void)snprintf(spn, sizeof spn, "%s", sc->entries[0].rel);
                char *dot = strrchr(spn, '.');
                if (dot) {
                    *dot = '\0';
                }
                gui_project_set_sprite_override(0, spn, GUI_SPRITE_OV_SHAPE, 0 /* RECT */);
                (void)gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
                const int rri = gui_pack_find_sprite(0, spn);
                const tp_result *rr = gui_pack_result(0);
                const int vc = (rr && rri >= 0) ? rr->sprites[rri].vert_count : -1;
                nt_log_info("SELFTEST: sprite '%s' RECT override -> vert_count=%d (expect 4)", spn, vc);
                NT_ASSERT(vc == 4 && "RECT per-sprite override packs a 4-vert rect");
            }
        }

        /* Restore a valid export state: the EXPORT path (tp_export_run) does not yet
         * apply the effective-extrude-0 rule (point-7 follow-up in the parallel exporter
         * agent's file), so concave+extrude>0 would be rejected at core validation. */
        a0->extrude = 0;
        gui_project_touch_setting();

        /* save + export a fresh GUI project -> the seeded target writes files (audit I1) */
        char fpath[1200];
        (void)snprintf(fpath, sizeof fpath, "%s/selftest_fresh.ntpacker_project", s_exe_dir);
        char serr[256] = {0};
        (void)gui_project_save_as(fpath, serr, sizeof serr);
        int etg = 0;
        int enc = 0;
        char eerr[256] = {0};
        char enote[128] = {0};
        const bool oke = gui_pack_export(0, &etg, &enc, eerr, sizeof eerr, enote, sizeof enote);
        char jbase[600] = {0};
        char jpath[640] = {0};
        char ppath[640] = {0};
        bool jok = false;
        bool pok = false;
        if (tp_project_resolve_path(gui_project_get(), "out/atlas1", jbase, sizeof jbase) == TP_STATUS_OK) {
            (void)snprintf(jpath, sizeof jpath, "%s.json", jbase);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", jbase);
            FILE *jf = fopen(jpath, "rb");
            if (jf) {
                jok = (fgetc(jf) == '{');
                (void)fclose(jf);
            }
            FILE *pf = fopen(ppath, "rb");
            if (pf) {
                pok = true;
                (void)fclose(pf);
            }
        }
        nt_log_info("SELFTEST: fresh export ok=%d targets=%d json{=%d png0=%d %s", oke, etg, jok, pok, oke ? "" : eerr);
        NT_ASSERT(oke && jok && pok && "fresh GUI project exports its seeded target");
        (void)remove(jpath);
        (void)remove(ppath);
        (void)remove(fpath);
    }

    /* --- animations (ux.md §3.7b): pure playback map, create-from-selection natural sort, reorder,
     *     round-trip preserves frames order + playback + flips, remove-frame path --- */
    {
        bool fin = false;
        NT_ASSERT(gui_canvas_anim_frame_at(0.0, 10.0F, 2, 4, &fin) == 3 && !fin && "once_backward step0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.35, 10.0F, 2, 4, &fin) == 0 && fin && "once_backward finishes at 0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.45, 10.0F, 3, 4, &fin) == 3 && "loop_backward wraps");
        NT_ASSERT(gui_canvas_anim_frame_at(0.35, 10.0F, 4, 3, &fin) == 1 && "once_pingpong return leg");
        NT_ASSERT(gui_canvas_anim_frame_at(0.45, 10.0F, 4, 3, &fin) == 0 && fin && "once_pingpong finishes at 0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.55, 10.0F, 5, 3, &fin) == 1 && "loop_pingpong wraps");

        const int aidx = gui_project_add_atlas();
        s_sel_atlas = aidx;
        multi_sel_clear();
        multi_sel_add("walk_10"); /* deliberately out of natural order */
        multi_sel_add("walk_2");
        multi_sel_add("walk_1");
        const int ai = create_animation_from_selection();
        tp_project_atlas *aa = tp_project_get_atlas(gui_project_get(), aidx);
        NT_ASSERT(ai == 0 && aa && aa->animation_count == 1 && "create animation from selection");
        tp_project_anim *an = &aa->animations[0];
        nt_log_info("SELFTEST: anim '%s' frames [%s,%s,%s]", an->id, an->frames[0], an->frames[1], an->frames[2]);
        NT_ASSERT(an->frame_count == 3 && strcmp(an->frames[0], "walk_1") == 0 && strcmp(an->frames[1], "walk_2") == 0 &&
                  strcmp(an->frames[2], "walk_10") == 0 && "frames natural-sorted (walk_2 before walk_10)");

        gui_project_set_anim_playback(aidx, 0, 5); /* loop pingpong */
        gui_project_set_anim_flip(aidx, 0, true, false);
        gui_project_set_anim_fps(aidx, 0, 12.0F);
        gui_project_anim_move_frame(aidx, 0, 0, 2); /* walk_1 rides to the end */
        aa = tp_project_get_atlas(gui_project_get(), aidx);
        an = &aa->animations[0];
        NT_ASSERT(strcmp(an->frames[0], "walk_2") == 0 && strcmp(an->frames[2], "walk_1") == 0 && "reorder a frame");

        char *abuf = NULL;
        size_t alen = 0;
        tp_error abe = {0};
        tp_project *alp = NULL;
        tp_error ale = {0};
        const tp_status abs_st = tp_project_save_buffer(gui_project_get(), &abuf, &alen, &abe);
        const tp_status als_st = (abs_st == TP_STATUS_OK) ? tp_project_load_buffer(abuf, alen, &alp, &ale) : abs_st;
        const tp_project_anim *rl = (alp && alp->atlas_count > aidx && alp->atlases[aidx].animation_count > 0)
                                        ? &alp->atlases[aidx].animations[0]
                                        : NULL;
        nt_log_info("SELFTEST: anim RT save=%s load=%s playback=%d flip_h=%d fps=%g", tp_status_str(abs_st),
                    tp_status_str(als_st), rl ? rl->playback : -1, rl ? rl->flip_h : -1, rl ? (double)rl->fps : 0.0);
        NT_ASSERT(rl && rl->frame_count == 3 && rl->playback == 5 && rl->flip_h && !rl->flip_v && rl->fps == 12.0F &&
                  strcmp(rl->frames[0], "walk_2") == 0 && strcmp(rl->frames[2], "walk_1") == 0 &&
                  "round-trip preserves frame order + playback + flips");
        tp_project_destroy(alp);
        free(abuf);

        NT_ASSERT(gui_project_anim_remove_frame(aidx, 0, 1) && aa->animations[0].frame_count == 2 && "remove a frame");
        nt_log_info("SELFTEST: animation create/reorder/round-trip OK");

        multi_sel_clear();
        s_sel_anim = -1;
        s_sel_anim_frame = -1;
        s_sel_atlas = 0;
    }

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
    /* Render coverage: leave a real animation selected + previewing so the auto-quit frames exercise the
     * left-panel animations rows, the right-panel editor, and the canvas preview (draw_anim_frame on the
     * packed regions) -- a Clay layout bug in the new UI would crash these frames. */
    {
        s_sel_atlas = 0;
        tp_project_atlas *pa = tp_project_get_atlas(gui_project_get(), 0);
        const tp_result *pr = gui_pack_result(0);
        if (pa && pr && pr->sprite_count > 0) {
            multi_sel_clear();
            for (int i = 0; i < pr->sprite_count && i < 4; i++) {
                char key[192];
                strip_ext(pr->sprites[i].name, key, sizeof key);
                multi_sel_add(key);
            }
            const int pai = create_animation_from_selection();
            if (pai >= 0) {
                open_preview(pai);
                nt_log_info("SELFTEST: preview anim '%s' active=%d frames=%d", pa->animations[pai].id, s_preview_active,
                            pa->animations[pai].frame_count);
            }
            multi_sel_clear();
        }
    }
    g_ui_scale = 1.5F; /* exercise the scaled layout during the auto-quit frames */
    nt_log_info("SELFTEST: end (undo:%d redo:%d history:%zuB; selection '%s')", gui_history_undo_depth(),
                gui_history_redo_depth(), gui_history_bytes(), s_sel_abs);
}

/* --- Overlay pixel probe (F) + touch-on-render guard, driven across the auto-quit frames --- */
static int s_st_phase;      /* 0 warmup, 1 outline pixel probe, 2 touch-on-render guard, 3 done */
static int s_st_pf;         /* frames spent in the current phase */
static int s_st_cyan0;      /* outline-OFF cyan count (baseline of the diff test) */
static char *s_st_baseline; /* fresh-project bytes captured with zero input */
static size_t s_st_baseline_n;

/* Count blue/cyan overlay pixels in the current canvas box (framebuffer read, top-left origin). The
 * region-outline colour is (0.30,0.72,1.0): B high, B>>R, G>R -- distinct from grey checker + sprites. */
static int selftest_probe_cyan(void) {
    if (gui_canvas_get_mode(&s_canvas) != GUI_CANVAS_ATLAS || !gui_canvas_has_atlas(&s_canvas)) {
        return -1;
    }
    const float *bb = s_canvas.last_bb;
    int x = (int)bb[0];
    int y = (int)bb[1];
    int w = (int)bb[2];
    int h = (int)bb[3];
    if (w < 8 || h < 8) {
        return -1;
    }
    if (w > 900) {
        w = 900;
    }
    if (h > 900) {
        h = 900;
    }
    const uint32_t capn = (uint32_t)w * (uint32_t)h * 4u;
    uint8_t *px = (uint8_t *)malloc(capn);
    if (!px) {
        return -1;
    }
    int cyan = -1;
    if (nt_gfx_read_pixels(x, y, w, h, px, capn)) {
        cyan = 0;
        for (uint32_t i = 0; i + 3u < capn; i += 4u) {
            const int r = px[i];
            const int g = px[i + 1];
            const int b = px[i + 2];
            if (b > 150 && b > r + 40 && g > r + 25 && g > 110) {
                cyan++;
            }
        }
    }
    free(px);
    return cyan;
}

/* Top-of-frame phase driver: sets up each phase's scene BEFORE the layout/walk. */
static void selftest_pre_frame(void) {
    s_st_pf++;
    if (s_st_phase == 0) {
        if (s_st_pf < 12) {
            return; /* warm up: first scene + GL page uploads settle */
        }
        s_about_open = false;
        preview_stop();
        int found = -1;
        tp_project *p = gui_project_get();
        for (int i = 0; p && i < p->atlas_count; i++) {
            const tp_result *r = gui_pack_result(i);
            if (r && r->sprite_count > 0 && r->page_count > 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            s_sel_atlas = 0;
            do_pack();
            found = (gui_pack_result(0) && gui_pack_result(0)->sprite_count > 0) ? 0 : -1;
        }
        s_sel_atlas = (found >= 0) ? found : 0;
        gui_canvas_select(&s_canvas, -1); /* no selection -> plain hull outlines */
        s_canvas.mode = GUI_CANVAS_ATLAS;
        s_canvas.show_outline = true;
        s_canvas.show_trim = false;
        s_canvas.show_frame = false;
        s_canvas.show_pivot = false;
        s_st_phase = 1;
        s_st_pf = 0;
    } else if (s_st_phase == 1) {
        s_canvas.mode = GUI_CANVAS_ATLAS; /* hold ATLAS mode through the probe frames */
        if (s_st_pf == 3) {
            s_canvas.show_outline = false; /* OFF frame (diff baseline) */
        } else if (s_st_pf == 6) {
            s_canvas.show_outline = true; /* ON frame */
        }
    } else if (s_st_phase == 2) {
        if (s_st_pf > 10) {
            const bool dirty = gui_project_is_dirty();
            char *nb = NULL;
            size_t nn = 0;
            tp_error e = {0};
            const bool saved = tp_project_save_buffer(gui_project_get(), &nb, &nn, &e) == TP_STATUS_OK;
            const bool same = saved && s_st_baseline && nn == s_st_baseline_n && memcmp(nb, s_st_baseline, nn) == 0;
            nt_log_info("SELFTEST: touch-on-render guard dirty=%d bytes_match=%d (%zu vs %zu)", dirty, same, nn, s_st_baseline_n);
            NT_ASSERT(!dirty); /* a control that writes its widget value on first render flips this */
            NT_ASSERT(same);
            free(nb);
            free(s_st_baseline);
            s_st_baseline = NULL;
            s_st_phase = 3;
        }
    } else {
        nt_app_quit();
    }
}

/* Post-walk hook: pixel readbacks happen after nt_ui_walk has drawn the overlay. */
static void selftest_post_draw(void) {
    if (s_st_phase != 1) {
        return;
    }
    if (s_st_pf == 3) {
        s_st_cyan0 = selftest_probe_cyan();
    } else if (s_st_pf == 6) {
        const int c1 = selftest_probe_cyan();
        nt_log_info("SELFTEST: outline pixel probe cyan off=%d on=%d delta=%d", s_st_cyan0, c1, c1 - s_st_cyan0);
        NT_ASSERT(s_st_cyan0 >= 0 && c1 >= 0);
        NT_ASSERT(c1 - s_st_cyan0 >= 8); /* the hull outline MUST add cyan pixels (regression: cam on-plane -> 0-width) */
        /* Hand off to the touch-on-render guard: a truly fresh project, no input, all sections expanded. */
        gui_project_new();
        s_sel_atlas = 0;
        reset_selection();
        s_about_open = false;
        s_sec_atlas_open = true;
        s_atlas_adv_open = true;
        s_sec_region_open = true;
        s_sec_anim_open = true;
        s_sec_export_open = true;
        free(s_st_baseline);
        s_st_baseline = NULL;
        s_st_baseline_n = 0;
        tp_error e = {0};
        (void)tp_project_save_buffer(gui_project_get(), &s_st_baseline, &s_st_baseline_n, &e);
        s_st_phase = 2;
        s_st_pf = 0;
    }
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
        s_pending_export = true;
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
    /* Verification build: render real frames (proves the canvas draw + walk + a pixel-level proof that
     * region outlines actually rasterize), verify no control writes the model without input, then quit. */
    selftest_pre_frame();
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

        CLAY({.id = CLAY_ID("root"),
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                         .padding = {Su(10), Su(10), Su(10), Su(10)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(8),
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
              .backgroundColor = C_BG}) {
            declare_menubar(s_ctx);
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = Su(8), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}}}) {
                declare_left_panel(s_ctx);
                declare_canvas(s_ctx);
                declare_right_panel(s_ctx);
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

        /* SOURCE mode: refresh the decoded image for the selection before the walk draws it. In
         * ATLAS mode the canvas draws the packed pages, so leave the source texture alone. */
        if (gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_SOURCE) {
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
        }

        gui_canvas_upload_pages(&s_canvas);          /* GL upload of packed pages (deferred from pack) */
        if (s_canvas.upload_failed) {
            set_status("Page too large for this GPU \xE2\x80\x94 lower Max page size to preview it.");
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

    /* editor state + the canvas custom-draw handler (registered outside begin/end) */
    gui_canvas_init(&s_canvas);
    nt_ui_set_custom_handler(s_ctx, gui_canvas_handler, &s_canvas);
    gui_project_init();

    /* in-process packing: session .ntpack goes under the exe dir (existing convention) */
    char pack_session[1152];
    (void)snprintf(pack_session, sizeof pack_session, "%s/pack_session", s_exe_dir);
    gui_pack_init(pack_session);

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
