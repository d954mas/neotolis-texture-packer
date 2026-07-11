#ifndef NTPACKER_GUI_WIDGETS_H
#define NTPACKER_GUI_WIDGETS_H

/* Shared render kit for the ntpacker GUI: the per-frame style rescale (apply_ui_scale), the generic
 * button/icon/label/checkbox widgets, the inline rename field, the panel-width layout math, and the
 * text-measure/truncation + per-frame row-tooltip helpers. Split out of main.c (GUI decomposition
 * step 1) as a pure move -- no behavior change. Include discipline: widgets -> gui_defs + gui_state
 * (+ engine ui headers); it must never include a sibling view/actions/rows header. */

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

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_WIDGETS_H */
