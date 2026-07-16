/* Right settings panel view (see gui_view_settings.h). Split out of main.c (GUI decomposition
 * step 4) as a pure move -- function bodies + panel-local statics relocated verbatim, no behavior
 * change. */

#include "gui_view_settings.h"

#include "clay.h"
#include "ui/nt_ui_button.h"
#include "ui/nt_ui_dropdown.h"
#include "ui/nt_ui_image.h"
#include "ui/nt_ui_input.h"
#include "ui/nt_ui_label.h"
#include "ui/nt_ui_menu.h"
#include "ui/nt_ui_scroll.h"
#include "ui/nt_ui_slider.h"

#include "tp_core/tp_export.h" /* exporter registry -> target dropdown */
#include "tp_core/tp_validate.h"

#include "gui_defs.h"
#include "gui_state.h"
#include "gui_widgets.h"
#include "gui_actions.h"
#include "gui_rows.h"
#include "gui_project.h"
#include "gui_pack.h"
#include "gui_shell.h" /* close_menubar_menus (interim -- moves to gui_view_chrome in step 6b) */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- right settings panel: session-remembered disclosure state + dropdown open bits --- */
static bool s_region_ov_open = false;   /* Region "Packing overrides" disclosure */
static bool s_dd_shape_open, s_dd_size_open;           /* atlas shape / max-size combos */
static bool s_dd_ov_shape_open, s_dd_ov_rot_open, s_dd_ov_mv_open; /* per-region override combos */
static bool s_dd_target_open[GUI_MAX_TARGETS];         /* per-target exporter combos */
/* Numeric/text field edit buffers (game-owned; nt_ui_input edits in place). Synced from
 * the model each frame while unfocused, parsed+clamped into the model on edit. */
static char s_nb_pad[16], s_nb_margin[16], s_nb_extrude[16], s_nb_maxv[16], s_nb_ppu[24];
static char s_nb_alpha[16]; /* alpha-threshold numeric input (paired with the slider) */
static char s_nb_ox[24], s_nb_oy[24], s_nb_s9[4][16];
static char s_nb_ov_margin[16], s_nb_ov_extrude[16];
static char s_nb_target_path[GUI_MAX_TARGETS][256];
/* --- animation editor (right-panel section 4) --- */
static bool s_dd_playback_open;     /* playback-mode combo open bit */
static char s_nb_anim_fps[16];      /* fps field edit buffer */
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
    /* min width keeps the field from collapsing to 0 on a narrow-clamped panel -- a 0-width input
     * collapses its floating text/caret clip and trips the engine empty-scissor assert (max 0 = unbounded). */
    const Clay_ElementDeclaration decl = {.layout = {.sizing = {CLAY_SIZING_GROW(S(28.0F), 0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 4.0F))}}};
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
        /* Keystrokes BUFFER (coalesce, no commit); Enter is the gesture boundary that flushes the whole
         * field edit as ONE undo step (out-of-panel blur flushes via s_blur_inputs in main.c). Decision 0015. */
        if (submitted) {
            gui_request_gesture_commit();
        }
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
    /* min width keeps the field from collapsing to 0 on a narrow-clamped panel -- a 0-width input
     * collapses its floating text/caret clip and trips the engine empty-scissor assert (max 0 = unbounded). */
    const Clay_ElementDeclaration decl = {.layout = {.sizing = {CLAY_SIZING_GROW(S(28.0F), 0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 4.0F))}}};
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
        if (submitted) {
            gui_request_gesture_commit(); /* Enter = the field's gesture boundary (decision 0015) */
        }
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
    /* min width keeps the field from collapsing to 0 on a narrow-clamped panel -- a 0-width input
     * collapses its floating text/caret clip and trips the engine empty-scissor assert (max 0 = unbounded). */
    const Clay_ElementDeclaration decl = {.layout = {.sizing = {CLAY_SIZING_GROW(S(28.0F), 0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 4.0F))}}};
    bool submitted = false;
    const bool changed = nt_ui_input_text(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, id, buf, cap, &tp, &s_num_input,
                                          &decl, enabled && !s_blur_inputs, &submitted);
    if (submitted && enabled) {
        gui_request_gesture_commit(); /* Enter = the field's gesture boundary (decision 0015) */
    }
    return (changed || submitted) && enabled;
}

/* A collapsible section/disclosure header (§2.4): `header` fill (lighter than panel -> advances),
 * a 3px `accent` left-rule (main sections only), a chevron ICON, and a `section`/`title` label. Pass
 * accent_rule=true + C_HEADER for a top-level section, false + a recessed fill for a nested disclosure. */
static void panel_header(nt_ui_context_t *ctx, uint32_t id, const char *title, bool *open, const nt_ui_label_style_t *lbl,
                         const Clay_Color bg_col, bool accent_rule) {
    const nt_ui_events_t ev = nt_ui_events(ctx, id, NULL);
    if (ev.clicked) {
        *open = !*open;
    }
    const Clay_Color bg = ev.hovered ? C_HOVER : bg_col;
    const uint16_t padL = accent_rule ? 0U : Su(8); /* the flush rule + childGap provide the left inset */
    nt_ui_image_style_t chev = nt_ui_image_style_defaults();
    chev.color_packed = label_tint(lbl);
    CLAY({.id = {.id = id},
          .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(26.0F))},
                     .padding = {padL, Su(8), 0, 0},
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
        if (accent_rule) {
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(3.0F)), CLAY_SIZING_GROW(0)}},
                  .backgroundColor = C_ACCENT,
                  .cornerRadius = CLAY_CORNER_RADIUS(S(1.5F))}) {}
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(12.0F)), CLAY_SIZING_FIXED(S(12.0F))}}}) {
            nt_ui_image(ctx, NT_UI_DATA_LAYER(LAYER_IMG), *open ? &s_ic_chevron_down : &s_ic_chevron_right, &chev, NULL);
        }
        /* Ellipsize: an untruncated title word (e.g. "Animation" at the title size) alone exceeded a narrow
         * panel and forced the GROW scroll wider than the panel at 2x scale. */
        ui_label_fit(ctx, title, lbl, fmaxf(s_right_panel_w - S(86.0F), S(30.0F)), 0U);
    }
}

/* A small note line (info, never a warning) -- wraps within the panel. FIXED width (not GROW) so a long
 * word's min-content can't balloon the GROW scroll wider than the fixed panel (Clay word-wrap min-content
 * == longest word; at 2x scale on a narrow panel that alone exceeded the panel and pushed content off). */
static void panel_note(nt_ui_context_t *ctx, const char *text) {
    const float w = fmaxf(s_right_panel_w - S(24.0F), S(60.0F)); /* panel content inner (padding + border) */
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(w), CLAY_SIZING_FIT(0)}, .padding = {Su(4), Su(4), Su(2), Su(2)}}}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), text, &g_dim);
    }
}

/* Row scaffolding: a fixed label cell + a growable widget cell. The BEGIN macro opens
 * both; the caller emits the widget then closes with ROW_END. */
#define PANEL_ROW_BEGIN(lbl_text, lbl_style)                                                                            \
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},                                 \
                     .childGap = Su(8),                                                                                 \
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {                                    \
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(s_panel_label_w), CLAY_SIZING_GROW(0)},                           \
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {                                \
            ui_label_fit(ctx, (lbl_text), (lbl_style), s_panel_label_w, 0U);                                            \
        }                                                                                                              \
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},                                         \
                         .childGap = Su(6),                                                                            \
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}})
#define PANEL_ROW_END }

/* Ellipsize a combo preview so the engine's FIT trigger (min = s_dd_style.min_width, which the right panel
 * pins to the widget cell) can't grow past that cell on a long value like "Concave contour" and drag the
 * whole content wider than the panel. Reserves the trigger's real overhead: L/R padding + childGap + chevron. */
static const char *combo_preview_fit(const char *preview, char *buf, size_t cap) {
    const float overhead = 3.0F * (float)Su(8) + (float)s_dd_style.chevron_size; /* pad L + pad R + childGap + chevron */
    const float max_w = fmaxf((float)s_dd_style.min_width - overhead, S(8.0F));
    (void)truncate_to_width(preview ? preview : "", s_dd_style.font_size, max_w, buf, cap);
    return buf;
}

/* A labeled string-option dropdown. Returns the newly-picked index, or -1 if unchanged.
 * When !enabled it renders the preview as a static dimmed label (disabled-with-reason). */
static int row_combo(nt_ui_context_t *ctx, const char *label, uint32_t id, bool *open, const char *preview, int cur,
                     const char *const *options, int count, bool enabled) {
    int sel = -1;
    char pv[96];
    combo_preview_fit(preview, pv, sizeof pv);
    PANEL_ROW_BEGIN(label, enabled ? &g_row : &g_dim) {
        if (!enabled) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), pv, &g_dim);
        } else if (nt_ui_combo_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, id, pv, &s_dd_style, open)) {
            for (int i = 0; i < count; i++) {
                if (nt_ui_combo_selectable(ctx, (uint32_t)i, options[i], i == cur)) {
                    sel = i;
                }
            }
            nt_ui_combo_end(ctx);
        }
    }
    PANEL_ROW_END;
    if (sel >= 0) {
        gui_request_gesture_commit(); /* a dropdown pick is ONE discrete edit -> commit it now (decision 0015) */
    }
    return sel;
}

/* A labeled slider + editable numeric input, both bound to the same [mn,mx] int field (exact entry
 * without the clumsy drag-only slider). Returns true (writes *out) on change from either control. */
static bool row_slider(nt_ui_context_t *ctx, const char *label, uint32_t id, char *buf, size_t cap, int cur, int mn,
                       int mx, bool enabled, int *out) {
    int v = cur;
    bool changed = false;
    /* On a narrow-clamped panel the fixed-width slider + input can't share the widget cell; drop the
     * slider (input stays, exact entry still works) so nothing overruns the panel. */
    const bool show_slider = s_right_panel_w >= S(250.0F);
    PANEL_ROW_BEGIN(label, enabled ? &g_row : &g_dim) {
        if (show_slider) {
            const Clay_ElementDeclaration sd = {
                .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(S(20.0F))}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}};
            if (nt_ui_slider_int(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, id, NULL, &v, mn, mx, 1, &s_slider_style, &sd, enabled)) {
                changed = true; /* buffer the live drag value each frame; the COMMIT is the release edge below */
            }
            /* Gesture-scoped commit (decision 0015): a live drag buffers per frame; committing the whole
             * drag as ONE undo step happens on the release edge, not the live return. */
            if (enabled && nt_ui_query_interaction(ctx, id).released_now) {
                gui_request_gesture_commit();
            }
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
    if (changed) {
        gui_request_gesture_commit(); /* a checkbox toggle is ONE discrete edit -> commit it now (decision 0015) */
    }
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
static void offer_atlas_setting(const tp_session_snapshot *snapshot,
                                const tp_snapshot_atlas *atlas,
                                gui_atlas_field field, int ivalue, float fvalue) {
    if (snapshot && atlas) {
        gui_queue_atlas_setting(atlas->id, tp_session_snapshot_revision(snapshot),
                                field, ivalue, fvalue);
    }
}

static void declare_atlas_settings(nt_ui_context_t *ctx,
                                   const tp_session_snapshot *snapshot,
                                   const tp_snapshot_atlas *a) {
    /* Basic: shape, max size, padding, allow transform. */
    const int ns = row_combo(ctx, "Shape", nt_ui_id("set/shape"), &s_dd_shape_open,
                             (a->shape >= 0 && a->shape < 3) ? k_shape_names[a->shape] : "?", a->shape, k_shape_names, 3, true);
    if (ns >= 0 && ns != a->shape) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_SHAPE, ns, 0.0F);
    }
    char szpv[16];
    (void)snprintf(szpv, sizeof szpv, "%d", a->max_size);
    static const char *const size_labels[7] = {"256", "512", "1024", "2048", "4096", "8192", "16384"};
    const int nsz = row_combo(ctx, "Max page size", nt_ui_id("set/size"), &s_dd_size_open, szpv, size_preset_index(a->max_size),
                              size_labels, 7, true);
    if (nsz >= 0 && k_size_presets[nsz] != a->max_size) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_MAX_SIZE, k_size_presets[nsz], 0.0F);
    }
    if (a->max_size > 4096) {
        panel_note(ctx, "Pages over 4096 may not load on mobile GPUs / stock engine runtime.");
    }
    int iv = 0;
    /* No committed-value guard (`iv != a->padding`) here or on the other coalescable knobs below
     * (F2-05b-ii-A #1): during a buffered gesture the committed model is FROZEN, so a control returned
     * to its committed value would SKIP the correcting enqueue while the pending buffer kept the stale
     * intermediate -> the flush then committed the WRONG value (data loss). Every changed frame now
     * enqueues (coalesced, latest wins); a gesture that nets back to committed is dropped by the flush-
     * time no-op suppression (#3), so no phantom commit results. */
    if (row_int(ctx, "Padding", nt_ui_id("set/pad"), s_nb_pad, sizeof s_nb_pad, a->padding, 0, 16384, true, &iv)) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_PADDING, iv, 0.0F);
    }
    bool bv = false;
    if (row_check(ctx, "Allow transform", nt_ui_id("set/xform"), a->allow_transform, true, &bv)) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_ALLOW_TRANSFORM, bv ? 1 : 0, 0.0F);
    }

    /* Advanced disclosure. */
    panel_header(ctx, nt_ui_id("set/adv"), "ADVANCED", &s_atlas_adv_open, &g_section, C_BG, false);
    if (!s_atlas_adv_open) {
        return;
    }
    if (row_int(ctx, "Margin", nt_ui_id("set/margin"), s_nb_margin, sizeof s_nb_margin, a->margin, 0, 16384, true, &iv)) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_MARGIN, iv, 0.0F);
    }
    const bool extrude_ok = (a->shape == 0 /* RECT */);
    if (row_int(ctx, "Extrude", nt_ui_id("set/extrude"), s_nb_extrude, sizeof s_nb_extrude, a->extrude, 0, 255, extrude_ok,
                &iv)) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_EXTRUDE, iv, 0.0F);
    }
    if (!extrude_ok) {
        panel_note(ctx, "Extrude requires Rect shape \xE2\x80\x94 use Padding for polygon modes.");
    }
    if (row_slider(ctx, "Alpha threshold", nt_ui_id("set/alpha"), s_nb_alpha, sizeof s_nb_alpha, a->alpha_threshold, 0,
                   255, true, &iv)) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_ALPHA_THRESHOLD, iv, 0.0F);
    }
    if (row_int(ctx, "Max vertices", nt_ui_id("set/maxv"), s_nb_maxv, sizeof s_nb_maxv, a->max_vertices, 1, 16, true, &iv)) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_MAX_VERTICES, iv, 0.0F);
    }
    if (row_check(ctx, "Power of two", nt_ui_id("set/pot"), a->power_of_two, true, &bv)) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_POWER_OF_TWO, bv ? 1 : 0, 0.0F);
    }
    float fv = 0.0F;
    if (row_float(ctx, "Pixels/unit", nt_ui_id("set/ppu"), s_nb_ppu, sizeof s_nb_ppu, a->pixels_per_unit, 0.0001F,
                  100000.0F, true, &fv)) {
        offer_atlas_setting(snapshot, a, GUI_ATLAS_PIXELS_PER_UNIT, 0, fv);
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

/* A "<value> [Rename]" row that never overruns the panel and shows the value at its ACTUAL free width.
 * Two rows: the value on its own line (aligned to the label column, using nearly the whole widget cell --
 * ~207px at 1920x1080@1.5, so a 14-char export name shows un-truncated), then a right-aligned Rename below.
 * The old single-line layout parked the value behind the fixed 174px label cell AND over-reserved a fat
 * S(96) guess for the button, cramming it to ~84px ("roun..."); but measurement proves label(123) +
 * value(172) + Rename(110) = 405 > the 402px panel content, so all three CANNOT share one line at 300px --
 * the value must own its row. Returns clicked. */
static bool right_panel_rename_row(nt_ui_context_t *ctx, const char *label, const char *value, uint32_t btn_id) {
    bool clicked = false;
    PANEL_ROW_BEGIN(label, &g_row) {
        ui_label_fit(ctx, value, &g_body, right_panel_text_w(s_panel_label_w + S(14.0F)), 0U);
    }
    PANEL_ROW_END;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                     .childAlignment = {CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER}}}) {
        if (ui_btn(ctx, btn_id, "Rename", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            clicked = true;
        }
    }
    return clicked;
}

/* --- Region (selected sprite) + per-region packing overrides --- */
static void declare_region_settings(nt_ui_context_t *ctx,
                                    const tp_session_snapshot *snapshot,
                                    const tp_snapshot_atlas *atlas) {
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
    const gui_sprite_ref sprite_ref = {atlas->id, row->source_id, row->source_key,
                                       tp_session_snapshot_revision(snapshot)};
    const tp_snapshot_sprite *ov = tp_session_snapshot_sprite_by_key(
        snapshot, atlas->id, row->source_id, row->source_key);
    const tp_result *pr = gui_pack_result(s_sel_atlas);
    const int ri = pr ? gui_pack_find_sprite_ref(s_sel_atlas, row->source_id,
                                                  row->source_key)
                      : -1;

    /* Final name + Rename (reuse the existing inline rename path). */
    char fname[224];
    (void)snprintf(fname, sizeof fname, "%s", (ov && ov->rename) ? ov->rename : sprite);
    if (right_panel_rename_row(ctx, "Final name", fname, nt_ui_id("reg/rename"))) {
        start_sprite_edit_ref(&sprite_ref, sprite);
    }

    /* Source file + size (from the last pack result when available). */
    char src[256];
    if (pr && ri >= 0) {
        (void)snprintf(src, sizeof src, "%s  \xC2\xB7  %d x %d", path_last(row->abs), pr->sprites[ri].sourceSize.w,
                       pr->sprites[ri].sourceSize.h);
    } else {
        (void)snprintf(src, sizeof src, "%s  (pack to measure)", path_last(row->abs));
    }
    PANEL_ROW_BEGIN("Source", &g_caption) {
        ui_label_fit(ctx, src, &g_caption, right_panel_text_w(s_panel_label_w + S(14.0F)), 0U);
    }
    PANEL_ROW_END;
    /* Smart-folder provenance (owner 2026-07-11): a folder child (!is_source) came in via its parent
     * smart folder -- name it. The row model already carries src (index into a->sources), so this is free. */
    if (!row->is_source && row->src >= 0 && row->src < atlas->source_count) {
        const tp_snapshot_source *source = tp_session_snapshot_source_at(snapshot, atlas->id,
                                                                         row->src);
        char via[224];
        (void)snprintf(via, sizeof via, "via smart folder %s/",
                       source ? path_last(source->path) : "?");
        panel_note(ctx, via);
    }

    const float ox = ov ? ov->origin_x : TP_PROJECT_ORIGIN_DEFAULT;
    const float oy = ov ? ov->origin_y : TP_PROJECT_ORIGIN_DEFAULT;
    float fv = 0.0F;
    /* Pivot X/Y edit ONE component each (axis 0/1): the setter seeds the OTHER component from the
     * committed record after flushing the buffered axis, so editing X then Y never loses X (#2). The
     * view no longer does the stale read-modify-write that dropped a buffered X. */
    if (row_float(ctx, "Pivot X", nt_ui_id("reg/ox"), s_nb_ox, sizeof s_nb_ox, ox, -100.0F, 100.0F, true, &fv)) {
        gui_queue_sprite_origin(&sprite_ref, 0, fv);
    }
    if (row_float(ctx, "Pivot Y", nt_ui_id("reg/oy"), s_nb_oy, sizeof s_nb_oy, oy, -100.0F, 100.0F, true, &fv)) {
        gui_queue_sprite_origin(&sprite_ref, 1, fv);
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
        /* No committed-value guard (`iv != cur`): the committed record is frozen mid-gesture (#1), so
         * a slice9 component returned to its committed value must still enqueue the correction. A net-
         * zero gesture is dropped by the flush-time no-op suppression (#3). */
        if (row_int(ctx, s9_labels[k], nt_ui_id(s9_ids[k]), s_nb_s9[k], sizeof s_nb_s9[k], cur, 0, 4096, true, &iv)) {
            gui_queue_sprite_slice9(&sprite_ref, k, iv);
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
            ui_label_fit(ctx, rd, &g_caption, right_panel_text_w(s_panel_label_w + S(14.0F)), 0U);
        }
        PANEL_ROW_END;
    }

    /* Per-region packing overrides (owner scope 2026-07-10). */
    panel_header(ctx, nt_ui_id("reg/ov"), "PACKING OVERRIDES", &s_region_ov_open, &g_section, C_BG, false);
    if (!s_region_ov_open) {
        return;
    }
    const int ov_shape = ov ? ov->override_shape : TP_PROJECT_OV_INHERIT;
    const int ov_rot = ov ? ov->override_allow_rotate : TP_PROJECT_OV_INHERIT;
    const int ov_mv = ov ? ov->override_max_vertices : TP_PROJECT_OV_INHERIT;
    const int ov_margin = ov ? ov->override_margin : TP_PROJECT_OV_INHERIT;
    const int ov_extrude = ov ? ov->override_extrude : TP_PROJECT_OV_INHERIT;

    /* Slice9 auto-forces RECT + no-rotate: show the shape/rotate overrides disabled. */
    if (any_s9) {
        panel_note(ctx, "Shape & rotation overrides are set by slice-9 (Rect, no rotation).");
    }
    char shape_def[48];
    (void)snprintf(shape_def, sizeof shape_def, "Default (%s)", (atlas->shape >= 0 && atlas->shape < 3) ? k_shape_names[atlas->shape] : "?");
    const int ps = row_override_combo(ctx, "Shape", nt_ui_id("reg/ov_shape"), &s_dd_ov_shape_open, ov_shape, 0,
                                      k_shape_names, 3, shape_def, !any_s9);
    if (ps != OV_UNCHANGED && ps != ov_shape) {
        gui_queue_sprite_override(&sprite_ref, GUI_SPRITE_OV_SHAPE, ps);
    }
    static const char *const rot_values[1] = {"No rotation"};
    const char *rot_def = atlas->allow_transform ? "Default (rotate/flip)" : "Default (no transform)";
    const int prv = row_override_combo(ctx, "Rotation", nt_ui_id("reg/ov_rot"), &s_dd_ov_rot_open, ov_rot, 0, rot_values,
                                       1, rot_def, !any_s9);
    if (prv != OV_UNCHANGED && prv != ov_rot) {
        gui_queue_sprite_override(&sprite_ref, GUI_SPRITE_OV_ROTATE, prv);
    }
    static const char *const mv_values[16] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                              "9", "10", "11", "12", "13", "14", "15", "16"};
    char mv_def[40];
    (void)snprintf(mv_def, sizeof mv_def, "Default (%d)", atlas->max_vertices);
    const int pmv = row_override_combo(ctx, "Max vertices", nt_ui_id("reg/ov_mv"), &s_dd_ov_mv_open, ov_mv, 1, mv_values,
                                       16, mv_def, true);
    if (pmv != OV_UNCHANGED && pmv != ov_mv) {
        gui_queue_sprite_override(&sprite_ref, GUI_SPRITE_OV_MAXVERT, pmv);
    }

    /* margin / extrude overrides: a "override?" checkbox + numeric (1..255). extrude is
     * disabled unless the sprite's effective shape is RECT (§3.3f, per sprite). */
    const int eff_shape = tp_project_sprite_effective_shape(atlas->shape, any_s9, ov_shape);
    {
        bool on = (ov_margin != TP_PROJECT_OV_INHERIT);
        PANEL_ROW_BEGIN("Margin ovr", &g_row) {
            const bool cbc = tp_checkbox(ctx, nt_ui_id("reg/ov_mcb"), on, true);
            if (cbc) {
                on = !on;
            }
            const int seed = (atlas->margin >= 1) ? (atlas->margin > 255 ? 255 : atlas->margin) : 1;
            if (cbc) {
                gui_queue_sprite_override(&sprite_ref, GUI_SPRITE_OV_MARGIN,
                                         on ? seed : TP_PROJECT_OV_INHERIT);
                gui_request_gesture_commit(); /* discrete toggle -> commit now (coalescable override, decision 0015) */
            }
            const int disp = (ov_margin != TP_PROJECT_OV_INHERIT) ? ov_margin : seed;
            int iv = 0;
            /* Drop the committed-value guard (`iv != ov_margin`) -- same stale-committed-value lost-edit
             * class as #1 (this override field is coalescable; the flush-time no-op suppression #3 drops a
             * net-zero gesture). Keep the `on` gate (only enqueue while the override is active). */
            if (ui_int_field(ctx, nt_ui_id("reg/ov_mf"), s_nb_ov_margin, sizeof s_nb_ov_margin, disp, 1, 255,
                             on && !cbc, &iv) &&
                on) {
                gui_queue_sprite_override(&sprite_ref, GUI_SPRITE_OV_MARGIN, iv);
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
            const int seed = (atlas->extrude >= 1) ? (atlas->extrude > 255 ? 255 : atlas->extrude) : 1;
            if (cbc && ex_enabled) {
                gui_queue_sprite_override(&sprite_ref, GUI_SPRITE_OV_EXTRUDE,
                                         on ? seed : TP_PROJECT_OV_INHERIT);
                gui_request_gesture_commit(); /* discrete toggle -> commit now (coalescable override, decision 0015) */
            }
            const int disp = (ov_extrude != TP_PROJECT_OV_INHERIT) ? ov_extrude : seed;
            int iv = 0;
            /* Drop the committed-value guard (`iv != ov_extrude`) -- same #1 class; #3 drops a net-zero. */
            if (ui_int_field(ctx, nt_ui_id("reg/ov_ef"), s_nb_ov_extrude, sizeof s_nb_ov_extrude, disp, 1, 255,
                             ex_enabled && on && !cbc, &iv) &&
                on) {
                gui_queue_sprite_override(&sprite_ref, GUI_SPRITE_OV_EXTRUDE, iv);
            }
        }
        PANEL_ROW_END;
        if (!ex_enabled) {
            panel_note(ctx, "Extrude override needs the sprite's effective shape to be Rect.");
        }
    }
}

/* The exporter dropdown cell for one target row (its own element so it can sit inline on a wide panel or
 * drop to a dedicated row when narrow). `preview` must already be width-fit (combo_preview_fit). */
static void declare_target_exporter_combo(nt_ui_context_t *ctx, uint32_t row_id, int ti,
                                          const gui_target_ref *target,
                                          const char *const *exp_labels, int nlabels, int cur_exp, const char *preview) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        if (nt_ui_combo_begin(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_child_id(row_id, "exp"), preview,
                              &s_dd_style, &s_dd_target_open[ti])) {
            for (int i = 0; i < nlabels; i++) {
                if (nt_ui_combo_selectable(ctx, (uint32_t)i, exp_labels[i], i == cur_exp)) {
                    const tp_exporter *e = tp_exporter_at(i);
                    if (e) {
                        gui_edit_target_exporter(target, e->id); /* H/G3: preserves a buffered out-path edit */
                    }
                }
            }
            nt_ui_combo_end(ctx);
        }
    }
}

/* --- Export targets (region G, audit I1) --- */
static void declare_export_targets(nt_ui_context_t *ctx,
                                   const tp_session_snapshot *snapshot,
                                   const tp_snapshot_atlas *a) {
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
        const tp_snapshot_target *t = tp_session_snapshot_target_at(snapshot, a->id, ti);
        if (!t) {
            continue;
        }
        gui_target_ref target;
        if (!gui_project_target_ref_at(s_sel_atlas, ti, &target)) {
            continue;
        }
        char idbuf[48];
        (void)snprintf(idbuf, sizeof idbuf, "tgt/row_%d", ti);
        const uint32_t row_id = nt_ui_id(idbuf);
        if (nt_ui_menu_open_trigger(ctx, s_id_ctx_menu, row_id, false, &s_ctx_state)) {
            close_menubar_menus();
            s_ctx_kind = CTX_TARGET;
            s_ctx_target_atlas_id = target.atlas_id;
            s_ctx_target_id = target.target_id;
            s_ctx_target_revision = target.expected_revision;
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
            /* row 1: enabled checkbox + exporter dropdown + remove. The dropdown value is width-fit, and on a
             * narrow panel it drops to its own row below (checkbox + combo + x can't share a narrow line). */
            char pvbuf[96];
            combo_preview_fit((cur_exp >= 0) ? exp_labels[cur_exp] : t->exporter_id, pvbuf, sizeof pvbuf);
            const bool tgt_narrow = s_right_panel_w < S(210.0F);
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (tp_checkbox(ctx, nt_ui_child_id(row_id, "en"), t->enabled, true)) {
                    gui_edit_target_enabled(&target, !t->enabled); /* H/G3: preserves a buffered out-path edit */
                }
                if (!tgt_narrow) {
                    declare_target_exporter_combo(ctx, row_id, ti, &target,
                                                  exp_labels, nlabels, cur_exp,
                                                  pvbuf);
                } else {
                    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {} /* push x to the right */
                }
                const uint32_t rm_id = nt_ui_child_id(row_id, "rm");
                record_row_tip(rm_id, "Remove target");
                if (ui_icon_btn(ctx, rm_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                                nt_ui_query_events(ctx, rm_id).hovered ? &g_danger : &g_caption)) {
                    gui_request_remove_target(ti);
                }
            }
            if (tgt_narrow) {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    declare_target_exporter_combo(ctx, row_id, ti, &target,
                                                  exp_labels, nlabels, cur_exp,
                                                  pvbuf);
                }
            }
            /* row 2: out path + browse */
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    if (ui_text_field(ctx, nt_ui_child_id(row_id, "path"), s_nb_target_path[ti], sizeof s_nb_target_path[ti],
                                      t->out_path, true, "out/atlas.json")) {
                        gui_edit_target_out_path(&target, s_nb_target_path[ti]); /* H/G3: coalesce -> ONE undo step */
                    }
                }
                if (ui_btn(ctx, nt_ui_child_id(row_id, "browse"), "\xE2\x80\xA6", &g_btn_ghost, true, 28.0F, 22.0F, &g_caption)) { /* U+2026 */
                    gui_request_browse_target(s_sel_atlas, ti);
                }
            }
        }
        tp_target_validation_report diagnostics;
        tp_error validation_error = {0};
        if (tp_validate_session_snapshot_target(snapshot, a->id, t->id,
                                                &diagnostics,
                                                &validation_error) == TP_STATUS_OK) {
            for (size_t issue = 0; issue < diagnostics.issue_count; ++issue) {
                const char *code = diagnostics.issues[issue].code;
                if (strcmp(code, TP_VALIDATION_CODE_TARGET_NO_OUT_PATH) == 0) {
                    panel_note(ctx, "This target has no output path -- it won't export.");
                } else if (strcmp(code, TP_VALIDATION_CODE_DUPLICATE_OUT_PATH) == 0) {
                    panel_note(ctx, "Another target already exports to this path -- they overwrite each other.");
                } else if (strcmp(code, TP_VALIDATION_CODE_UNKNOWN_EXPORTER) == 0) {
                    panel_note(ctx, "Unknown exporter -- this target won't export.");
                }
            }
        }
    }
    /* The UI arrays (s_dd_target_open / s_nb_target_path) are fixed at GUI_MAX_TARGETS, so targets past
     * that are not editable here -- but export still writes ALL of them, so surface the hidden tail
     * instead of dropping it silently (P2). */
    if (a->target_count > GUI_MAX_TARGETS) {
        char more[80];
        (void)snprintf(more, sizeof more, "+%d more target(s) not editable here (still exported).",
                       a->target_count - GUI_MAX_TARGETS);
        panel_note(ctx, more);
    }
    if (ui_icon_btn(ctx, nt_ui_id("tgt/add"), &s_ic_plus, 16.0F, "Target", &g_btn_ghost, true, 0.0F, 26.0F, &g_caption)) {
        gui_request_add_target(s_sel_atlas);
    }
}

/* --- Animation editor (ux.md §3.7b): id / fps / playback / flips + ordered frame list --- */
static void declare_animation_editor(nt_ui_context_t *ctx,
                                     const tp_session_snapshot *snapshot,
                                     const tp_snapshot_atlas *a) {
    if (s_sel_anim < 0 || s_sel_anim >= a->animation_count) {
        panel_note(ctx, "Select an animation (left panel) to edit its frames, fps, playback and flips.");
        return;
    }
    const tp_snapshot_animation *an = tp_session_snapshot_animation_at(
        snapshot, a->id, s_sel_anim);
    if (!an) {
        panel_note(ctx, "The selected animation is no longer available.");
        return;
    }
    gui_animation_ref animation_ref;
    if (!gui_project_animation_ref_at(s_sel_atlas, s_sel_anim,
                                      &animation_ref)) {
        panel_note(ctx, "The selected animation is no longer available.");
        return;
    }
    const bool editing_id = gui_animation_edit_matches(a->id, an->id);

    if (editing_id) {
        PANEL_ROW_BEGIN("Id", &g_row) {
            if (render_rename_field(ctx)) {
                s_pending_commit_edit_enter = true; /* defer: never commit while holding `an` (F2-05b-i) */
            }
        }
        PANEL_ROW_END;
    } else if (right_panel_rename_row(ctx, "Id", an->name, nt_ui_id("anim/rename"))) {
        start_anim_edit(s_sel_anim);
    }

    PANEL_ROW_BEGIN("Preview", &g_row) {
        if (ui_btn(ctx, nt_ui_id("anim/play"), s_preview_active ? "Playing\xE2\x80\xA6" : "Play", &g_btn, true, 0.0F, 24.0F,
                   &g_caption)) {
            gui_request_open_preview(&animation_ref);
        }
    }
    PANEL_ROW_END;

    float fv = 0.0F;
    if (row_float(ctx, "FPS", nt_ui_id("anim/fps"), s_nb_anim_fps, sizeof s_nb_anim_fps, an->fps, 1.0F, 240.0F, true,
                  &fv)) {
        gui_edit_anim_fps(&animation_ref, fv);
    }
    const char *pv = (an->playback >= 0 && an->playback < 7) ? k_playback_names[an->playback] : "?";
    const int npb = row_combo(ctx, "Playback", nt_ui_id("anim/pb"), &s_dd_playback_open, pv, an->playback,
                              k_playback_names, 7, true);
    if (npb >= 0 && npb != an->playback) {
        gui_edit_anim_playback(&animation_ref, npb);
    }
    bool bv = false;
    if (row_check(ctx, "Flip H", nt_ui_id("anim/fh"), an->flip_h, true, &bv)) {
        gui_edit_anim_flip(&animation_ref, bv, an->flip_v);
    }
    if (row_check(ctx, "Flip V", nt_ui_id("anim/fv"), an->flip_v, true, &bv)) {
        gui_edit_anim_flip(&animation_ref, an->flip_h, bv);
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
            const tp_snapshot_frame *frame = tp_session_snapshot_animation_frame_at(
                snapshot, a->id, an->id, fi);
            (void)snprintf(lab, sizeof lab, "%02d  %s", fi + 1,
                           frame ? frame->name : "?");
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
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
            const uint32_t frm_x_id = nt_ui_child_id(row_id, "x");
            record_row_tip(frm_x_id, "Remove frame");
            if (ui_icon_btn(ctx, frm_x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                            nt_ui_query_events(ctx, frm_x_id).hovered ? &g_danger : &g_caption)) {
                fact = 1;
                fidx = fi;
            }
        }
    }
    if (fact == 1 && fidx >= 0) {
        gui_edit_anim_frame_remove(&animation_ref, fidx);
        s_sel_anim_frame = -1;
    } else if (fact == 2 && fidx >= 0) {
        gui_edit_anim_frame_move(&animation_ref, fidx, -1);
        s_sel_anim_frame = fidx - 1;
    } else if (fact == 3 && fidx >= 0) {
        gui_edit_anim_frame_move(&animation_ref, fidx, +1);
        s_sel_anim_frame = fidx + 1;
    }
}

void declare_right_panel(nt_ui_context_t *ctx) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *snapshot_atlas = snapshot
                                                  ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                                                  : NULL;
    /* Pin the combo trigger min-width to the widget cell so a FIT combo can't grow past the panel (Su(110)
     * scales up at 1.5/2.0 and would otherwise bleed on a narrow panel). Restored after the sections so the
     * wider export-modal combos keep their own default. */
    const uint16_t saved_dd_mw = s_dd_style.min_width;
    s_dd_style.min_width = (uint16_t)right_panel_text_w(s_panel_label_w); /* == widget cell (already floored >= S(24)) */
    CLAY({.id = {.id = s_id_right_panel},
          .layout = {.sizing = {CLAY_SIZING_FIXED(s_right_panel_w), CLAY_SIZING_GROW(0)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = C_PANEL}) { /* docked region (pass 2): no card corner/border; 2px C_BG seam divides */
        nt_ui_scroll_begin(ctx, NULL, nt_ui_id("panel/scroll"), &s_panel_scroll,
                           &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}});
        CLAY({.id = {.id = s_id_right_content},
              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                         .padding = {Su(8), Su(8), Su(8), Su(10)},
                         .layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = Su(4)}}) {
            if (!snapshot_atlas) {
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No atlas selected.", &g_caption);
            } else {
                char title[96];
                (void)snprintf(title, sizeof title, "Atlas settings \xC2\xB7 %s", snapshot_atlas->name);
                panel_header(ctx, nt_ui_id("sec/atlas"), title, &s_sec_atlas_open, &g_title, C_HEADER, true);
                if (s_sec_atlas_open) {
                    declare_atlas_settings(ctx, snapshot, snapshot_atlas);
                }
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(6))}}}) {}
                panel_header(ctx, nt_ui_id("sec/region"), "REGION", &s_sec_region_open, &g_section, C_HEADER, true);
                if (s_sec_region_open) {
                    declare_region_settings(ctx, snapshot, snapshot_atlas);
                }
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(6))}}}) {}
                panel_header(ctx, nt_ui_id("sec/anim"), "ANIMATION", &s_sec_anim_open, &g_section, C_HEADER, true);
                if (s_sec_anim_open) {
                    declare_animation_editor(ctx, snapshot, snapshot_atlas);
                }
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(6))}}}) {}
                panel_header(ctx, nt_ui_id("sec/export"), "EXPORT TARGETS", &s_sec_export_open, &g_section, C_HEADER, true);
                if (s_sec_export_open) {
                    declare_export_targets(ctx, snapshot, snapshot_atlas);
                }
            }
        }
        nt_ui_scroll_end(ctx);
    }
    s_dd_style.min_width = saved_dd_mw;
}
