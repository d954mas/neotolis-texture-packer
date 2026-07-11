#ifndef NTPACKER_GUI_WIDGETS_H
#define NTPACKER_GUI_WIDGETS_H

/* Shared render kit for the ntpacker GUI: the per-frame style rescale (apply_ui_scale), the generic
 * button/icon/label/checkbox widgets, the inline rename field, the panel-width layout math, the
 * text-measure/truncation + per-frame row-tooltip helpers, the baked icon-atlas region refs, and the
 * D4-transform-to-string formatter shared by the canvas readout and the settings "Packed" row. Split
 * out of main.c (GUI decomposition step 1) as a pure move -- no behavior change. Include discipline:
 * widgets -> gui_defs + gui_state (+ engine ui headers); it must never include a sibling view/actions/
 * rows header.
 *
 * Icon refs (step 4): the ref VALUES are resolved once by main.c's bind_icon_ref/try_bind_resources
 * (they read init-only engine resource handles, so binding stays shell territory), but the refs
 * themselves are read by every view TU, so the refs live here per the plan's original §2 assignment
 * (step 1 deferred this "for now"; step 4 completes it). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/nt_atlas.h"  /* nt_atlas_region_ref_t */
#include "ui/nt_ui.h"        /* nt_ui_context_t */
#include "ui/nt_ui_button.h" /* nt_ui_button_style_t */
#include "ui/nt_ui_label.h"  /* nt_ui_label_style_t */

#include "gui_defs.h"
#include "gui_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Multiplies every style's scale-dependent field by g_ui_scale. Runs each frame so a
 * runtime UI-scale change (View > UI Scale) takes effect immediately; text stays crisp
 * because the font is Slug vector text (resolution-independent -- no atlas re-bake needed). */
void apply_ui_scale(void);

/* Clamp the two fixed side-panel widths so the window always fits left + right + a minimal canvas. */
void compute_panel_widths(float logical_w);

/* Truncate `src` with a trailing "..." so its rendered width at `size` fits `max_w` px. */
bool truncate_to_width(const char *src, float size, float max_w, char *out, size_t cap);

/* Left-panel available text width for a row at `indent_px`, minus an optional [x] button. */
float left_row_text_w(float indent_px, bool has_x);

/* Right-panel text width for a row, minus `reserved_px` (a trailing widget/button cluster). */
float right_panel_text_w(float reserved_px);

/* Records a per-frame hover tooltip (full text) against `id` for the tooltip declare pass. */
void record_row_tip(uint32_t id, const char *full);

/* Secondary/primary/etc button with a centered label. Returns true the frame it is clicked. */
bool ui_btn(nt_ui_context_t *ctx, uint32_t id, const char *text, nt_ui_button_style_t *style, bool enabled,
            float w, float h, const nt_ui_label_style_t *lbl);

/* Pack a label tier's color into an image widget's 0xAABBGGRR tint. */
uint32_t label_tint(const nt_ui_label_style_t *lbl);

/* Icon button (extends ui_btn): a baked icon mask tinted to `lbl`'s color tier, with an optional
 * trailing label. text == NULL -> icon-only (caller MUST attach a tooltip). */
bool ui_icon_btn(nt_ui_context_t *ctx, uint32_t id, nt_atlas_region_ref_t *icon, float icon_box,
                 const char *text, nt_ui_button_style_t *style, bool enabled, float w, float h,
                 const nt_ui_label_style_t *lbl);

/* Standalone leading type-icon for a list row (§3 grid: S(14) box). Pure draw (no events). */
void ui_row_icon(nt_ui_context_t *ctx, nt_atlas_region_ref_t *icon, const nt_ui_label_style_t *tint);
#define ROW_ICON_RESERVE 20.0F /* icon box S(14) + childGap S(6): subtract from a row's text width */

/* Section caption: a 3px `accent` left-rule + an UPPERCASE `section`-style label. */
void section_rule_label(nt_ui_context_t *ctx, const char *text);

/* Ellipsis-truncated label so text never draws past `max_w`; records a hover tooltip
 * (full text) against `tip_id` when it truncated (tip_id 0 = no tooltip). */
void ui_label_fit(nt_ui_context_t *ctx, const char *text, const nt_ui_label_style_t *lbl, float max_w, uint32_t tip_id);

/* Boolean checkbox: an outlined box that draws a centered check GLYPH (U+2713) when on. */
bool tp_checkbox(nt_ui_context_t *ctx, uint32_t id, bool cur, bool enabled);

/* The single inline rename field, sized to fill its (bounded) parent so it clips to the row. */
bool render_rename_field(nt_ui_context_t *ctx);

/* Compact D4 transform decode for the hover/selection readout (ux.md §2.4). */
const char *transform_decode_str(uint8_t t);

/* --- baked icon-atlas region refs (resolved once by main.c's bind_icon_ref/try_bind_resources at
 * startup; read by every view). The single baked WHITE region (s_white_ref) is the art for
 * checkbox/slider parts (tinted per state); the rest are Lucide icon masks (white-on-alpha), tinted
 * per state via nt_ui_image color_packed. --- */
extern nt_atlas_region_ref_t s_white_ref;
extern nt_atlas_region_ref_t s_ic_layout_grid, s_ic_triangle_alert, s_ic_download, s_ic_refresh;
extern nt_atlas_region_ref_t s_ic_chevron_left, s_ic_chevron_right, s_ic_minus, s_ic_plus, s_ic_scan, s_ic_maximize;
extern nt_atlas_region_ref_t s_ic_chevron_down, s_ic_layers, s_ic_folder, s_ic_image, s_ic_film;
extern nt_atlas_region_ref_t s_ic_file_plus, s_ic_folder_plus, s_ic_x;
extern nt_atlas_region_ref_t s_ic_info, s_ic_circle_check, s_ic_octagon_alert, s_ic_folder_plus_hero;

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_WIDGETS_H */
