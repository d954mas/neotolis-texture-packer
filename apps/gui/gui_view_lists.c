/* Left panel view (see gui_view_lists.h). Split out of main.c (GUI decomposition step 5) as a pure
 * move -- function bodies + panel-local statics relocated verbatim, no behavior change. */

#include "gui_view_lists.h"

#include "clay.h"
#include "ui/nt_ui_vlist.h"

#include "input/nt_input.h"

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
#include <string.h>

/* --- left panel (atlases + sprites + animations) --- */
static const nt_ui_events_cfg_t s_dbl_cfg = {.long_press_secs = 0.0F, .double_click = true};

/* start_atlas_edit/start_anim_edit/start_sprite_edit_named/start_sprite_edit moved to gui_actions
 * (step 4 -- the entry side of the edit lifecycle, needed by both this panel and the settings view). */

static void declare_atlas_list(nt_ui_context_t *ctx, tp_project *proj) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(20.0F))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        section_rule_label(ctx, "ATLASES");
    }
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
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                ui_row_icon(ctx, &s_ic_layers, selected ? &g_row_strong : &g_caption);
                if (editing) {
                    if (render_rename_field(ctx)) {
                        commit_atlas_rename();
                    }
                } else {
                    ui_label_fit(ctx, proj->atlases[i].name, selected ? &g_row_strong : &g_row,
                                 fmaxf(left_row_text_w(S(8.0F), has_x) - S(ROW_ICON_RESERVE), S(16.0F)), row_id);
                }
            }
            if (has_x) {
                record_row_tip(x_id, "Remove atlas");
                (void)ui_icon_btn(ctx, x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                                  xev.hovered ? &g_danger : &g_caption);
            }
        }
    }
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_atlas"), &s_ic_plus, 16.0F, "Atlas", &g_btn_ghost, true, 0.0F, 26.0F, &g_caption)) {
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
        section_rule_label(ctx, "SPRITES");
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        /* Smart folder is the primary input (ux.md principle 3: folders, not files); it keeps its label so
         * the live-linked behaviour is explicit (owner 2026-07-11). Per-file adds are "the exception", so the
         * Files button is icon-only (tooltip in declare_tooltips) -- this also keeps the header on one line at
         * the owner's 450px panel where two labelled buttons overran and wrapped "Smart folder". */
        if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_files"), &s_ic_file_plus, 16.0F, NULL, &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            s_pending_add_files = true;
        }
        /* Drop the label to icon-only on a heavily-clamped panel (narrow window / high DPI) so it never
         * wraps or bleeds; the tooltip carries the meaning either way (mouse-complete). >= 240 design px keeps
         * the label at the owner's 1920/1366 @1.5 (450px panels) and every unclamped base-300 panel. */
        const char *folder_lbl = (s_left_panel_w >= S(240.0F)) ? "Smart folder" : NULL;
        if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_folder"), &s_ic_folder_plus, 16.0F, folder_lbl, &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            s_pending_add_folder = true;
        }
    }

    if (s_row_count == 0) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "No sources. Add a smart folder or files.", &g_caption);
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
            nt_ui_events_t xev = {0}; /* hoisted: the render below reads xev.hovered for the danger tint */
            if (row->is_source) {
                xev = nt_ui_events(ctx, x_id, NULL);
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
            /* Leading type icon: folder for a directory source, image for a sprite leaf (folder child or
             * file source); missing files reuse the image mask tinted warn. Label brightens on selection.
             * Smart-folder distinction (TexturePacker convention): the folder icon is AMBER (warn) vs the
             * neutral file icons, so a live-linked folder reads as a special input at a glance. */
            const nt_ui_label_style_t *lbl = row->missing ? &g_warn : (selected ? &g_row_strong : &g_row);
            nt_atlas_region_ref_t *ic = row->is_folder ? &s_ic_folder : &s_ic_image;
            const nt_ui_label_style_t *ic_tint = (row->missing || row->is_folder) ? &g_warn : (selected ? &g_row_strong : &g_caption);
            CLAY({.id = {.id = row_id},
                  .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                             .padding = {indent, Su(4), 0, 0},
                             .childGap = Su(4),
                             .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = bg,
                  .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
                CLAY({.id = {.id = hit_id},
                      .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    ui_row_icon(ctx, ic, ic_tint);
                    if (editing) {
                        if (render_rename_field(ctx)) {
                            commit_sprite_rename();
                        }
                    } else {
                        /* Folder rows carry a fixed smart-folder tooltip on the whole row (below), so skip the
                         * truncation tip here (one tip per id); leaf/file rows keep the full-text truncation tip. */
                        ui_label_fit(ctx, row->label, lbl,
                                     fmaxf(left_row_text_w(S(8.0F + (float)row->indent * 16.0F), row->is_source) - S(ROW_ICON_RESERVE), S(16.0F)),
                                     row->is_folder ? 0U : hit_id);
                    }
                    if (row->is_folder) {
                        record_row_tip(hit_id, "Smart folder: every image inside is packed automatically, "
                                               "including files added later. Press F5 to rescan.");
                    }
                }
                if (row->is_source) {
                    record_row_tip(x_id, row->is_folder ? "Remove this smart folder and all its sprites" : "Remove source");
                    (void)ui_icon_btn(ctx, x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                                      xev.hovered ? &g_danger : &g_caption);
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
        section_rule_label(ctx, "ANIMATIONS");
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_anim"), &s_ic_plus, 16.0F, "Animation", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
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
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                ui_row_icon(ctx, &s_ic_film, selected ? &g_row_strong : &g_caption);
                if (editing) {
                    if (render_rename_field(ctx)) {
                        commit_anim_rename();
                    }
                } else {
                    ui_label_fit(ctx, a->animations[i].name, selected ? &g_row_strong : &g_row,
                                 fmaxf(left_row_text_w(S(8.0F), true) - S(28.0F) - S(ROW_ICON_RESERVE), S(16.0F)), row_id);
                }
            }
            char fc[16];
            (void)snprintf(fc, sizeof fc, "%df", a->animations[i].frame_count);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), fc, &g_caption);
            record_row_tip(x_id, "Remove animation");
            (void)ui_icon_btn(ctx, x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                              xev.hovered ? &g_danger : &g_caption);
        }
    }
}

void declare_left_panel(nt_ui_context_t *ctx) {
    tp_project *proj = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(proj, s_sel_atlas);
    s_row_tip_count = 0; /* per-frame; filled by ui_label_fit when a row truncates */
    CLAY({.id = {.id = s_id_left_panel},
          .layout = {.sizing = {CLAY_SIZING_FIXED(s_left_panel_w), CLAY_SIZING_GROW(0)},
                     .padding = {Su(8), Su(8), Su(8), Su(8)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(6),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP}},
          .backgroundColor = C_PANEL,
          /* Docked region (pass 2): no card corner/border -- the 2px C_BG seam to the canvas is the divider.
           * Vertical clip only (X-clip is forbidden -- it makes children X-scrollable/unbounded): at short
           * window heights the animations hint wraps tall and would otherwise draw past the panel bottom.
           * The sprite vlist keeps its own inner scroll. */
          .clip = {.vertical = true}}) {
        declare_atlas_list(ctx, proj);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(1))}}, .backgroundColor = C_BORDER}) {}
        declare_sprite_list(ctx);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(1))}}, .backgroundColor = C_BORDER}) {}
        declare_animations_list(ctx, a);
    }
}
