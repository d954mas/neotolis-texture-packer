/* Shared mutable editor/UI state for the ntpacker GUI (see gui_state.h). Split out of main.c
 * (GUI decomposition step 1) as a pure move -- definitions relocated verbatim, no behavior change. */

#include "gui_state.h"

#include "gui_defs.h" /* RGBA8 (button-style tints) */

#include <stdarg.h>
#include <stdio.h>

/* --- global UI scale --- */
float g_ui_scale = 1.0F;

/* --- status line --- */
char s_status[256];
status_sev_t s_status_sev = STATUS_INFO;
bool s_status_dismissed;
bool s_status_fixed_time;

void set_status(const char *msg) {
    s_status_sev = STATUS_INFO;
    s_status_dismissed = false; /* a new message re-shows the pill (replaces any prior one) */
    (void)snprintf(s_status, sizeof s_status, "%s", msg);
}
void set_status_ex(status_sev_t sev, const char *msg) {
    s_status_sev = sev;
    s_status_dismissed = false;
    (void)snprintf(s_status, sizeof s_status, "%s", msg);
}
void set_statusf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_status, sizeof s_status, fmt, ap);
    va_end(ap);
    s_status_sev = STATUS_INFO;
    s_status_dismissed = false;
}
void set_statusf_ex(status_sev_t sev, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_status, sizeof s_status, fmt, ap);
    va_end(ap);
    s_status_sev = sev;
    s_status_dismissed = false;
}

/* --- executable directory --- */
char s_exe_dir[1024];

/* --- nt_ui context + canvas --- */
nt_ui_context_t *s_ctx;
gui_canvas s_canvas;

/* --- font --- */
nt_font_t s_font;
bool s_font_bound;

/* --- UI ids --- */
uint32_t s_id_btn_pack, s_id_btn_export, s_id_btn_refresh, s_id_vlist, s_id_modal, s_id_about;
uint32_t s_id_canvas;
uint32_t s_id_rename;
uint32_t s_id_right_panel;
uint32_t s_id_left_panel;
uint32_t s_id_strip;
uint32_t s_id_status_pill;
uint32_t s_id_right_content;
uint32_t s_id_export_modal;

/* --- primary selection --- */
int s_sel_atlas;
int s_sel_src = -1;
int s_sel_child = -1;
char s_sel_abs[512];
bool s_sel_missing;

/* --- multi-select set --- */
char s_multi_sel[MAX_MULTI_SEL][192];
int s_multi_sel_count;
int s_sel_anchor_row = -1;

/* --- animation selection --- */
int s_sel_anim = -1;
int s_sel_anim_frame = -1;

/* --- animation preview player --- */
bool s_preview_active;
bool s_preview_playing;
bool s_preview_finished;
double s_preview_time;
int s_preview_cur;
int s_preview_frame_count;

/* --- inline rename edit --- */
int s_edit_kind;
int s_edit_atlas;
int s_edit_anim;
char s_edit_sprite[192];
char s_edit_buf[192];

/* --- runtime column widths --- */
float s_content_w = 1280.0F;
float s_left_panel_w = 300.0F;
float s_right_panel_w = 300.0F;
float s_canvas_w = 680.0F;
float s_panel_label_w = 116.0F;

/* --- right settings panel disclosure state --- */
bool s_sec_atlas_open = true, s_sec_region_open = true, s_sec_export_open = true;
bool s_atlas_adv_open = false;
bool s_sec_anim_open = true;

/* --- modal open flags (moved from the shell in step 3; shared with the selftest) --- */
bool s_about_open;
bool s_export_open;

/* --- context-menu shared state (moved from the shell in step 4; written by left panel/canvas/settings,
 * read by the declare machinery that stays in main.c until chrome, step 6b) --- */
uint32_t s_id_ctx_menu;
nt_ui_menu_state_t s_ctx_state;
int s_ctx_kind;
int s_ctx_atlas;
int s_ctx_anim = -1;
int s_ctx_target = -1;
int s_ctx_src = -1;
char s_ctx_sprite[192];
bool s_ctx_leaf;
bool s_ctx_removable;

/* --- shell-set, view-read input-blur flag (moved from the shell in step 4) --- */
bool s_blur_inputs;

/* --- per-frame row tooltips --- */
row_tip s_row_tips[MAX_ROW_TIPS];
int s_row_tip_count;

/* --- scaled label styles (written by apply_ui_scale) --- */
nt_ui_label_style_t g_title, g_section, g_body, g_row, g_row_strong, g_caption, g_canvas_hint, g_tag, g_warn, g_link, g_dim, g_danger; /* scaled each frame */
nt_ui_label_style_t g_onaccent, g_onwarn;                                                           /* colored-button content (scaled each frame) */
nt_ui_label_style_t g_check;                                                                        /* checkbox tick glyph (scaled each frame) */

/* Button tiers (§2.5). Every tint is authored through RGBA8(r,g,b) -- never a hand-packed 0xAABBGGRR
 * literal (the byte-order footgun that swapped amber/blue before). Secondary = quiet grey (panel+8). */
nt_ui_button_style_t g_btn = {
    .idle = {.bg_tint = RGBA8(38, 42, 52), .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = RGBA8(50, 55, 68), .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = RGBA8(30, 34, 43), .scale = 0.97F, .offset_y = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = RGBA8(38, 42, 52), .scale = 1.0F, .opacity = 0.35F},
    .transition_speed = 12.0F,
    .hit_padding_lrtb = {4, 4, 4, 4},
    .slice9_scale = 1.0F,
};
/* Primary (NEW): the one bright saturated-blue accent -- the hero affirmative in a region (Pack when
 * ready; Export-dialog run; modal Save; About OK). Exactly one visible per region. */
nt_ui_button_style_t g_btn_primary = {
    .idle = {.bg_tint = RGBA8(64, 140, 214), .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = RGBA8(84, 158, 228), .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = RGBA8(48, 112, 182), .scale = 0.97F, .offset_y = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = RGBA8(64, 140, 214), .scale = 1.0F, .opacity = 0.35F},
    .transition_speed = 12.0F,
    .hit_padding_lrtb = {4, 4, 4, 4},
    .slice9_scale = 1.0F,
};
/* Stale/action (RE-TINTED to warn amber): Pack when the preview is stale (§3.3b). Amber is distinct
 * from primary-blue AND selection-blue -- fixes the audit's blue-on-blue. */
nt_ui_button_style_t g_btn_accent = {
    .idle = {.bg_tint = RGBA8(228, 158, 92), .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = RGBA8(238, 172, 110), .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = RGBA8(198, 134, 74), .scale = 0.97F, .offset_y = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = RGBA8(228, 158, 92), .scale = 1.0F, .opacity = 0.35F},
    .transition_speed = 12.0F,
    .hit_padding_lrtb = {4, 4, 4, 4},
    .slice9_scale = 1.0F,
};
/* Stale/outdated chip (RE-TINTED to warn amber): clickable -> Pack. */
nt_ui_button_style_t g_btn_stale = {
    .idle = {.bg_tint = RGBA8(228, 158, 92), .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = RGBA8(238, 172, 110), .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = RGBA8(198, 134, 74), .scale = 0.97F, .offset_y = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = RGBA8(228, 158, 92), .scale = 1.0F, .opacity = 0.5F},
    .transition_speed = 12.0F,
    .hit_padding_lrtb = {4, 4, 4, 4},
    .slice9_scale = 1.0F,
};
/* Ghost / icon-only: near-invisible idle (blends the semi-transparent strip), `hover` fill on hover.
 * Icon-only ghosts REQUIRE a tooltip. bg_tint alpha must be non-zero (engine hides alpha=0). */
nt_ui_button_style_t g_btn_ghost = {
    .idle = {.bg_tint = RGBA8(28, 32, 40), .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = RGBA8(46, 52, 66), .scale = 1.02F, .opacity = 1.0F},
    .pressed = {.bg_tint = RGBA8(24, 27, 34), .scale = 0.97F, .opacity = 1.0F},
    .disabled = {.bg_tint = RGBA8(28, 32, 40), .scale = 1.0F, .opacity = 0.35F},
    .transition_speed = 14.0F,
    .hit_padding_lrtb = {2, 2, 2, 2},
    .slice9_scale = 1.0F,
};
/* Link button: idle tint matches the panel (invisible box), faint lighten on hover/press so the
 * link-blue label reads as a hyperlink. */
nt_ui_button_style_t g_btn_link = {
    .idle = {.bg_tint = RGBA8(28, 31, 40), .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = RGBA8(40, 45, 55), .scale = 1.0F, .opacity = 1.0F},
    .pressed = {.bg_tint = RGBA8(24, 27, 34), .scale = 0.99F, .opacity = 1.0F},
    .disabled = {.bg_tint = RGBA8(28, 31, 40), .scale = 1.0F, .opacity = 0.4F},
    .transition_speed = 16.0F,
    .hit_padding_lrtb = {2, 2, 2, 2},
    .slice9_scale = 1.0F,
};
nt_ui_button_style_t g_menubtn = {
    .idle = {.bg_tint = RGBA8(22, 24, 30), .scale = 1.0F, .opacity = 1.0F},
    .hover = {.bg_tint = RGBA8(44, 49, 61), .scale = 1.0F, .opacity = 1.0F},
    .pressed = {.bg_tint = RGBA8(64, 140, 214), .scale = 1.0F, .opacity = 1.0F},
    .disabled = {.bg_tint = RGBA8(22, 24, 30), .scale = 1.0F, .opacity = 0.4F},
    .transition_speed = 16.0F,
    .hit_padding_lrtb = {0, 0, 0, 0},
    .slice9_scale = 1.0F,
};

/* --- scaled widget styles (seeded in ensure_ids, metric fields scaled by apply_ui_scale) --- */
nt_ui_menu_style_t s_menu_style;
nt_ui_modal_style_t s_modal_style;
nt_ui_tooltip_style_t s_tip_style;
nt_ui_input_style_t s_rename_input; /* inline rename field (atlas + sprite) */
nt_ui_dropdown_style_t s_dd_style;
nt_ui_slider_style_t s_slider_style;
nt_ui_input_style_t s_num_input;   /* numeric + short text fields */
nt_ui_scroll_style_t s_panel_scroll;
