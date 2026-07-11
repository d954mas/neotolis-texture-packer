/* Center canvas view (see gui_view_canvas.h). Split out of main.c (GUI decomposition step 6a) as a
 * pure move -- function bodies + canvas-local statics relocated verbatim, no behavior change.
 * handle_canvas_input stays in main.c per the P-2 lead ruling (see the header comment). */

#include "gui_view_canvas.h"

#include "clay.h"
#include "ui/nt_ui_button.h"
#include "ui/nt_ui_image.h"
#include "ui/nt_ui_menu.h"

#include "gui_defs.h"
#include "gui_state.h"
#include "gui_widgets.h"
#include "gui_actions.h"
#include "gui_rows.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_shell.h" /* close_menubar_menus (interim -- moves to gui_view_chrome in step 6b) */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

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

void declare_canvas(nt_ui_context_t *ctx) {
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
