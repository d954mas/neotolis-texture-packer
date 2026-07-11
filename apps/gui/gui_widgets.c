/* Shared render kit for the ntpacker GUI (see gui_widgets.h). Split out of main.c (GUI
 * decomposition step 1) as a pure move -- function bodies relocated verbatim, no behavior change. */

#include "gui_widgets.h"

#include "clay.h"
#include "font/nt_font.h"   /* nt_font_measure_n */
#include "ui/nt_ui_image.h" /* nt_ui_image + image style */
#include "ui/nt_ui_input.h" /* nt_ui_input_text + input props */

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Right settings panel widgets (regions F/G). All draw with the app's single baked
 * WHITE region (s_white_ref) tinted per state -- checkbox/slider need art, dropdown
 * is flat-color. Sizes are rescaled each frame in apply_ui_scale. */
nt_atlas_region_ref_t s_white_ref;
/* Baked Lucide icon masks (white-on-alpha), tinted per state via nt_ui_image color_packed. Resolved
 * once in try_bind_resources (like s_white_ref). Packet A wires the canvas-strip set; the rest of the
 * baked atlas (layers/folder/film/... hero) is for Packets B/C. */
nt_atlas_region_ref_t s_ic_layout_grid, s_ic_triangle_alert, s_ic_download, s_ic_refresh;
nt_atlas_region_ref_t s_ic_chevron_left, s_ic_chevron_right, s_ic_minus, s_ic_plus, s_ic_scan, s_ic_maximize;
/* Packet B row/section icons (bound in try_bind_resources alongside the strip set). */
nt_atlas_region_ref_t s_ic_chevron_down, s_ic_layers, s_ic_folder, s_ic_image, s_ic_film;
nt_atlas_region_ref_t s_ic_file_plus, s_ic_folder_plus, s_ic_x;
/* Packet C: status-bar severity icons + the 96px empty-state hero (bound alongside the rest). */
nt_atlas_region_ref_t s_ic_info, s_ic_circle_check, s_ic_octagon_alert, s_ic_folder_plus_hero;

/* Compact D4 transform decode for the hover/selection readout (ux.md §2.4). */
const char *transform_decode_str(uint8_t t) {
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

/* Truncate `src` with a trailing "..." so its rendered width at `size` fits `max_w` px.
 * Uniform font per row (no per-row shrink); returns true when it truncated. Font must be
 * bound (only called on the render path). */
bool truncate_to_width(const char *src, float size, float max_w, char *out, size_t cap) {
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
float left_row_text_w(float indent_px, bool has_x) {
    float w = s_left_panel_w - S(24.0F) - indent_px - S(4.0F) - S(4.0F);
    if (has_x) {
        w -= S(24.0F + 4.0F);
    }
    return (w < S(20.0F)) ? S(20.0F) : w;
}

/* Right-panel text width for a row, minus `reserved_px` (a trailing widget/button cluster). Used to
 * ellipsize long names so they never overrun the (narrow-clamped) settings panel. */
float right_panel_text_w(float reserved_px) {
    const float w = s_right_panel_w - S(20.0F) /* L+R padding */ - S(12.0F) /* scrollbar */ - reserved_px;
    return (w < S(24.0F)) ? S(24.0F) : w;
}

/* Clamp the two fixed side-panel widths so the window always fits left + right + a minimal canvas.
 * Below the fit threshold both panels scale down proportionally to a floor; the canvas (GROW) takes
 * whatever remains. All values are in scaled layout units (== scale.logical_w units). */
void compute_panel_widths(float logical_w) {
    const float base_l = S(BASE_LEFT_PANEL_W);
    const float base_r = S(BASE_RIGHT_PANEL_W);
    /* Reserve the compact strip's real min-content for the canvas (S(120) knew nothing about the strip,
     * so at 16:9 sizes the strip forced the middle row wider than the window -> right panel off-screen). */
    const float min_canvas = S(MIN_CANVAS_W);
    const float min_panel = S(MIN_PANEL_W);
    const float overhead = S(2.0F) * 2.0F; /* root L/R padding is 0 + two inter-column gaps of Su(2) (mirrors the root/middle-row declaration) */
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
    s_canvas_w = logical_w - s_left_panel_w - s_right_panel_w - overhead; /* what the canvas GROW actually gets */
    /* Label column caps at ~45% of the right-panel content so the widget/input cell always keeps a
     * positive width -- a fixed 116px label on a narrow-clamped panel would squeeze inputs to 0 width,
     * which collapses their floating text/caret clip and trips the engine's empty-scissor assert. */
    const float content = s_right_panel_w - S(20.0F);
    float label_w = S(PANEL_LABEL_W);
    const float cap = content * 0.45F;
    if (label_w > cap) {
        label_w = cap;
    }
    s_panel_label_w = fmaxf(label_w, S(24.0F));
}

void record_row_tip(uint32_t id, const char *full) {
    if (s_row_tip_count >= MAX_ROW_TIPS) {
        return;
    }
    s_row_tips[s_row_tip_count].id = id;
    (void)snprintf(s_row_tips[s_row_tip_count].full, sizeof s_row_tips[0].full, "%s", full);
    s_row_tip_count++;
}

/* Multiplies every style's scale-dependent field by g_ui_scale. Runs each frame so a
 * runtime UI-scale change (View > UI Scale) takes effect immediately; text stays crisp
 * because the font is Slug vector text (resolution-independent -- no atlas re-bake needed). */
void apply_ui_scale(void) {
    g_title = g_title_base;
    g_title.font_size = S(FS_TITLE);
    g_section = g_section_base;
    g_section.font_size = S(FS_SECTION);
    g_section.letter_tracking = (uint16_t)(S(1.0F) + 0.5F);
    g_body = g_body_base;
    g_body.font_size = S(FS_BODY);
    g_row = g_row_base;
    g_row.font_size = S(FS_ROW);
    g_row_strong = g_row_strong_base;
    g_row_strong.font_size = S(FS_ROW);
    g_danger = g_danger_base;
    g_danger.font_size = S(FS_CAPTION);
    g_caption = g_caption_base;
    g_caption.font_size = S(FS_CAPTION);
    g_canvas_hint = g_canvas_hint_base;
    g_canvas_hint.font_size = S(FS_HINT);
    g_tag = g_tag_base;
    g_tag.font_size = S(FS_TAG);
    g_onaccent = g_onaccent_base;
    g_onaccent.font_size = S(FS_BODY);
    g_onwarn = g_onwarn_base;
    g_onwarn.font_size = S(FS_BODY);
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

bool ui_btn(nt_ui_context_t *ctx, uint32_t id, const char *text, nt_ui_button_style_t *style, bool enabled,
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

/* Pack a label tier's color into the image widget's 0xAABBGGRR tint. */
uint32_t label_tint(const nt_ui_label_style_t *lbl) {
    return ((uint32_t)(uint8_t)lbl->color.a << 24) | ((uint32_t)(uint8_t)lbl->color.b << 16) |
           ((uint32_t)(uint8_t)lbl->color.g << 8) | (uint32_t)(uint8_t)lbl->color.r;
}

/* Icon button (extends ui_btn): a baked icon mask tinted to `lbl`'s color tier, with an optional
 * trailing label. text == NULL -> icon-only (caller MUST attach a tooltip -- mouse-complete rule).
 * icon_box is the logical icon square (S(16) strip actions, S(12) chevrons). w<=0 -> FIT (button
 * hugs its content, so icon-only buttons are square and never overflow a fixed cell). The button's
 * per-state opacity inherits to the icon, so disabled dims icon + label together (0.35, like text). */
bool ui_icon_btn(nt_ui_context_t *ctx, uint32_t id, nt_atlas_region_ref_t *icon, float icon_box,
                 const char *text, nt_ui_button_style_t *style, bool enabled, float w, float h,
                 const nt_ui_label_style_t *lbl) {
    const bool icon_only = (text == NULL);
    const Clay_SizingAxis wx = (w > 0.0F) ? CLAY_SIZING_FIXED(S(w)) : CLAY_SIZING_FIT(0);
    const uint16_t px = icon_only ? Su(6) : Su(10); /* icon-only: tight symmetric pad -> square */
    nt_ui_image_style_t istyle = nt_ui_image_style_defaults();
    istyle.color_packed = label_tint(lbl);
    nt_ui_button_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), id, style,
                       &(Clay_ElementDeclaration){.layout = {.sizing = {wx, CLAY_SIZING_FIXED(S(h))},
                                                             .padding = {px, px, Su(4), Su(4)},
                                                             .childGap = Su(6), /* SP_SM icon<->label */
                                                             .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                                  .cornerRadius = CLAY_CORNER_RADIUS(S(6))},
                       enabled, NULL);
    if (icon && icon->atlas.id != 0U) {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(icon_box)), CLAY_SIZING_FIXED(S(icon_box))}}}) {
            nt_ui_image(ctx, NT_UI_DATA_LAYER(LAYER_IMG), icon, &istyle, NULL);
        }
    }
    if (!icon_only) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), text, lbl);
    }
    return nt_ui_button_end(ctx) && enabled;
}

/* Standalone leading type-icon for a list row (§3 grid: S(14) box). Pure draw (no events) -- keeps the
 * virtualized rows render-pure for the touch-on-render guard. `tint` is a label tier; the mask tints to
 * its color so one baked white icon serves dim/text/strong/selected. */
void ui_row_icon(nt_ui_context_t *ctx, nt_atlas_region_ref_t *icon, const nt_ui_label_style_t *tint) {
    if (!icon || icon->atlas.id == 0U) {
        return;
    }
    nt_ui_image_style_t istyle = nt_ui_image_style_defaults();
    istyle.color_packed = label_tint(tint);
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(14.0F)), CLAY_SIZING_FIXED(S(14.0F))}}}) {
        nt_ui_image(ctx, NT_UI_DATA_LAYER(LAYER_IMG), icon, &istyle, NULL);
    }
}

/* Section caption: a 3px `accent` left-rule + an UPPERCASE `section`-style label. Emitted as two children
 * of a horizontal row (the caller supplies the row + childGap). The rule is the "you are in a section"
 * signal (§2.4); used on the left panel's ATLASES/SPRITES/ANIMATIONS zones. */
void section_rule_label(nt_ui_context_t *ctx, const char *text) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(3.0F)), CLAY_SIZING_FIXED(S(14.0F))}},
          .backgroundColor = C_ACCENT,
          .cornerRadius = CLAY_CORNER_RADIUS(S(1.5F))}) {}
    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), text, &g_section);
}

/* Ellipsis-truncated label so text never draws past `max_w`; records a hover tooltip
 * (full text) against `tip_id` when it truncated (tip_id 0 = no tooltip). */
void ui_label_fit(nt_ui_context_t *ctx, const char *text, const nt_ui_label_style_t *lbl, float max_w, uint32_t tip_id) {
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
bool tp_checkbox(nt_ui_context_t *ctx, uint32_t id, bool cur, bool enabled) {
    nt_ui_events_t ev = {0};
    if (enabled) {
        ev = nt_ui_events(ctx, id, NULL);
    }
    /* Field well (§2.7 item 7): recessed `input` fill; the BORDER carries state -- `border` idle,
     * `border-strong` when checked/active, dim when disabled. */
    Clay_Color bg = C_INPUT;
    if (!enabled) {
        bg = (Clay_Color){26.0F, 28.0F, 36.0F, 255.0F};
    } else if (ev.pressed) {
        bg = (Clay_Color){28.0F, 31.0F, 40.0F, 255.0F};
    }
    Clay_Color border = cur ? C_BORDER_STRONG : C_BORDER;
    if (!enabled) {
        border = (Clay_Color){40.0F, 44.0F, 54.0F, 255.0F};
    } else if (ev.hovered || ev.pressed) {
        border = C_BORDER_STRONG;
    }
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

static const nt_ui_input_props_t s_rename_props = {
    .placeholder = "name", .allow = NULL, .max_length = 0U, .keyboard = NT_UI_KB_TEXT, .password = false};

/* The single inline rename field, sized to fill its (bounded) parent so it clips to the row. */
bool render_rename_field(nt_ui_context_t *ctx) {
    bool submitted = false;
    /* min width: a 0-width field collapses its floating text/caret clip -> empty-scissor assert. */
    const Clay_ElementDeclaration decl = {
        .layout = {.sizing = {CLAY_SIZING_GROW(S(28.0F), 0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 5.0F))}}};
    (void)nt_ui_input_text(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_id_rename, s_edit_buf, sizeof s_edit_buf,
                           &s_rename_props, &s_rename_input, &decl, true, &submitted);
    return submitted;
}
