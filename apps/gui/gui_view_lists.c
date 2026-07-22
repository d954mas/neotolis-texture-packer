/* Left panel view (see gui_view_lists.h). */

#include "gui_view_lists.h"

#include "clay.h"
#include "ui/nt_ui_scroll.h"
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
#include "gui_shell.h" /* close_menubar_menus */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- left panel (atlases + sprites + animations) --- */
static const nt_ui_events_cfg_t s_dbl_cfg = {.long_press_secs = 0.0F, .double_click = true};

/* start_atlas_edit/start_anim_edit/start_sprite_edit_ref/start_sprite_edit moved to gui_actions
 * (the entry side of the edit lifecycle, needed by both this panel and the settings view). */

static void declare_atlas_list(nt_ui_context_t *ctx, const tp_session_snapshot *snapshot) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(20.0F))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        section_rule_label(ctx, "ATLASES");
    }
    const int atlas_count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    const int64_t revision = snapshot ? tp_session_snapshot_revision(snapshot) : 0;
    /* U-02 T2: atlas rows live in a bounded, vertically-scrollable region (height = min(content, cap)) so a
     * long list scrolls inside its own region instead of consuming the whole panel and starving the sprite
     * vlist + animations below. begin/end are guard-balanced by has_atlas_rows so the row loop is unchanged. */
    const bool has_atlas_rows = atlas_count > 0;
    if (has_atlas_rows) {
        const float atlas_rows_h = fminf((float)atlas_count * S(BASE_ROW_H), S(BASE_LIST_SECTION_CAP_H));
        nt_ui_scroll_begin(ctx, NULL, nt_ui_id("ntpacker/atlas_scroll"), &s_panel_scroll,
                           &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(atlas_rows_h)},
                                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}});
    }
    for (int i = 0; i < atlas_count; i++) {
        const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, i);
        if (!atlas) {
            continue;
        }
        char idbuf[64];
        (void)snprintf(idbuf, sizeof idbuf, "ntpacker/atlas_row_%d", i);
        const uint32_t row_id = nt_ui_id(idbuf);
        const uint32_t x_id = nt_ui_child_id(row_id, "x");
        const bool editing = gui_atlas_edit_matches(atlas->id);
        const bool selected = (i == s_sel_atlas);
        const nt_ui_events_t ev = nt_ui_events(ctx, row_id, &s_dbl_cfg);
        const nt_ui_events_t xev = nt_ui_events(ctx, x_id, NULL);
        if (xev.clicked) {
            s_pending_remove_atlas = true;
            s_pending_remove_atlas_id = atlas->id;
            s_pending_remove_atlas_revision = revision;
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
            s_ctx_atlas_id = atlas->id;
            s_ctx_atlas_revision = revision;
        }
        const bool has_x = (atlas_count > 1);
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
                        s_pending_commit_edit_enter = true; /* defer: never commit while holding `proj` */
                    }
                } else {
                    ui_label_fit(ctx, atlas->name, selected ? &g_row_strong : &g_row,
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
    if (has_atlas_rows) {
        nt_ui_scroll_end(ctx);
    }
    if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_atlas"), &s_ic_plus, 16.0F, "Atlas", &g_btn_ghost, true, 0.0F, 26.0F, &g_caption)) {
        s_pending_add_atlas = true;
    }
}

/* Applies a click on VIEW row `i` (index into s_view; Ctrl toggles, Shift range-selects from the
 * anchor in view order, plain replaces) to the multi-selection, and updates the PRIMARY selection
 * (region panel / canvas sync). The anchor s_sel_anchor_row is a VIEW index so Shift-range follows
 * the filtered/sorted order the user actually sees. */
static void select_sprite_row(int i, bool ctrl, bool shift) {
    if (i < 0 || i >= s_view_count) {
        return;
    }
    const sprite_row *row = &s_rows[s_view[i]];
    const bool leaf = (!row->is_folder && !row->missing && row->sprite_name &&
                       row->sprite_name[0] != '\0');
    s_sel_src = row->src;
    s_sel_child = row->child;
    s_sel_missing = row->missing;
    (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", row->abs);
    if (leaf) {
        if (shift && s_sel_anchor_row >= 0 && s_sel_anchor_row < s_view_count) {
            multi_sel_clear();
            const int lo = (s_sel_anchor_row < i) ? s_sel_anchor_row : i;
            const int hi = (s_sel_anchor_row < i) ? i : s_sel_anchor_row;
            for (int k = lo; k <= hi; k++) {
                const sprite_row *rk = &s_rows[s_view[k]];
                if (!rk->is_folder && !rk->missing && rk->sprite_name &&
                    rk->sprite_name[0] != '\0') {
                    multi_sel_add_ref(rk->source_id, rk->source_key);
                }
            }
        } else if (ctrl) {
            if (multi_sel_contains_ref(row->source_id, row->source_key)) {
                multi_sel_remove_ref(row->source_id, row->source_key);
            } else {
                multi_sel_add_ref(row->source_id, row->source_key);
            }
            s_sel_anchor_row = i;
        } else {
            multi_sel_set_single_ref(row->source_id, row->source_key);
            s_sel_anchor_row = i;
        }
    } else if (!ctrl && !shift) {
        multi_sel_clear();
        s_sel_anchor_row = -1;
    }
    if (gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_ATLAS && leaf) {
        gui_canvas_select(&s_canvas,
                          gui_pack_find_sprite_ref(s_sel_atlas, row->source_id,
                                                   row->source_key));
    }
}

/* --- keyboard focus model (U-02 T3) ---
 * s_focus_view indexes s_view (kept in gui_state so reset_selection can clear it on atlas switch).
 * s_focus_follow asks declare_sprite_list to ensure-visible ONCE after a keyboard move, so manual
 * wheel scrolling is never yanked back. */
static bool s_focus_follow;

static void focus_clamp(void) {
    if (s_view_count <= 0) {
        s_focus_view = -1;
    } else if (s_focus_view >= s_view_count) {
        s_focus_view = s_view_count - 1;
    }
}

void gui_list_focus_step(int delta, bool extend) {
    if (s_view_count <= 0) {
        s_focus_view = -1;
        return;
    }
    int f = s_focus_view;
    if (f < 0) {
        f = (delta > 0) ? 0 : (s_view_count - 1);
    } else {
        f += delta;
        if (f < 0) {
            f = 0;
        } else if (f >= s_view_count) {
            f = s_view_count - 1;
        }
    }
    s_focus_view = f;
    s_focus_follow = true;
    select_sprite_row(f, false, extend);
}

void gui_list_focus_edge(bool end, bool extend) {
    if (s_view_count <= 0) {
        s_focus_view = -1;
        return;
    }
    s_focus_view = end ? (s_view_count - 1) : 0;
    s_focus_follow = true;
    select_sprite_row(s_focus_view, false, extend);
}

void gui_list_focus_activate(void) {
    focus_clamp();
    if (s_focus_view < 0) {
        return;
    }
    const sprite_row *row = &s_rows[s_view[s_focus_view]];
    if (row->is_folder) {
        gui_rows_toggle_collapsed(row->source_id);
    } else {
        select_sprite_row(s_focus_view, false, false);
    }
    s_focus_follow = true;
}

void gui_list_focus_rename(void) {
    focus_clamp();
    if (s_focus_view < 0) {
        return;
    }
    const sprite_row *row = &s_rows[s_view[s_focus_view]];
    const bool leaf = (!row->is_folder && !row->missing && row->sprite_name &&
                       row->sprite_name[0] != '\0');
    if (leaf) {
        select_sprite_row(s_focus_view, false, false);
        start_sprite_edit(row);
    }
}

void gui_list_focus_collapse(bool expand) {
    focus_clamp();
    if (s_focus_view < 0) {
        return;
    }
    const sprite_row *row = &s_rows[s_view[s_focus_view]];
    if (row->is_folder) {
        const bool collapsed = gui_rows_is_collapsed(row->source_id);
        if (expand ? collapsed : !collapsed) {
            gui_rows_toggle_collapsed(row->source_id);
        }
    } else if (!expand && row->child >= 0) {
        /* Left on a folder child jumps focus to its parent source row. */
        for (int k = s_focus_view - 1; k >= 0; --k) {
            if (s_rows[s_view[k]].is_source) {
                s_focus_view = k;
                select_sprite_row(k, false, false);
                break;
            }
        }
    }
    s_focus_follow = true;
}

/* --- Ctrl+F speed-search (U-02 T1) ---
 * The engine exposes no programmatic text-field focus (focus is click-driven, and the arbiter is
 * engine-internal / read-only). So the filter is a host-driven speed-search: while armed, typed chars
 * from the platform char ring edit the sprite-tree filter directly -- no engine input field needed.
 * The seam stays model-first: this only feeds gui_rows_set_filter(), which build_view() consumes. */
static int utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80U) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800U) {
        out[0] = (char)(0xC0U | (cp >> 6));
        out[1] = (char)(0x80U | (cp & 0x3FU));
        return 2;
    }
    if (cp < 0x10000U) {
        out[0] = (char)(0xE0U | (cp >> 12));
        out[1] = (char)(0x80U | ((cp >> 6) & 0x3FU));
        out[2] = (char)(0x80U | (cp & 0x3FU));
        return 3;
    }
    if (cp <= 0x10FFFFU) {
        out[0] = (char)(0xF0U | (cp >> 18));
        out[1] = (char)(0x80U | ((cp >> 12) & 0x3FU));
        out[2] = (char)(0x80U | ((cp >> 6) & 0x3FU));
        out[3] = (char)(0x80U | (cp & 0x3FU));
        return 4;
    }
    return 0;
}

void filter_type_pump(void) {
    if (!s_filter_active || nt_ui_input_any_focused(s_ctx)) {
        return; /* not armed, or an engine text field owns typed chars this frame */
    }
    if (s_confirm_open || s_about_open || s_export_open || s_recovery_open ||
        s_edit_kind != EDIT_NONE) {
        return; /* a modal / inline-rename owns the keyboard -- don't steal its chars into the filter */
    }
    char buf[256];
    (void)snprintf(buf, sizeof buf, "%s", gui_rows_filter());
    size_t len = strlen(buf);
    bool changed = false;
    if (nt_input_key_is_pressed(NT_KEY_BACKSPACE) && len > 0) {
        size_t n = len - 1;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0U) == 0x80U) {
            n--; /* step back over UTF-8 continuation bytes to delete a whole codepoint */
        }
        buf[n] = '\0';
        len = n;
        changed = true;
    }
    uint32_t cp = 0;
    while (nt_input_pop_char(&cp)) {
        if (cp < 0x20U || cp == 0x7FU) {
            continue; /* drop control chars */
        }
        char enc[4];
        const int k = utf8_encode(cp, enc);
        if (k <= 0 || len + (size_t)k >= sizeof buf) {
            continue;
        }
        memcpy(buf + len, enc, (size_t)k);
        len += (size_t)k;
        buf[len] = '\0';
        changed = true;
    }
    if (changed) {
        gui_rows_set_filter(buf);
    }
}

/* Compact clickable chip for the sort controls (U-02 T7). Returns true on click. */
static bool ui_sort_chip(nt_ui_context_t *ctx, uint32_t id, const char *text, bool active) {
    const nt_ui_events_t ev = nt_ui_events(ctx, id, NULL);
    const Clay_Color bg = active ? C_SEL : (ev.hovered ? C_HOVER : C_TRANSPARENT);
    CLAY({.id = {.id = id},
          .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(S(20.0F))},
                     .padding = {Su(6), Su(6), 0, 0},
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), text, active ? &g_row_strong : &g_caption);
    }
    return ev.clicked;
}

/* Short chip caption for each §61.1 sort key. */
static const char *sort_key_label(row_sort_key key) {
    switch (key) {
    case ROW_SORT_SIZE:
        return "Size";
    case ROW_SORT_MTIME:
        return "Time";
    case ROW_SORT_ADDED:
        return "Added";
    case ROW_SORT_NAME:
    default:
        return "Name";
    }
}

/* Pure §61.1 click->sort decision, factored out so the mapping is reviewable in isolation (and mirrored
 * by test_gui_view.c through the public sort API -- the headless view test target does not link this
 * Clay/nt_ui TU). Clicking the ALREADY-active key FLIPS its direction (asc<->desc); clicking any OTHER
 * key selects it ascending. This makes "Name-desc -> click Name -> Name-asc" (the spec behaviour) and
 * keeps every key independently reachable. */
static void sort_chip_next(row_sort_key active_key, bool active_desc, row_sort_key clicked_key,
                           row_sort_key *out_key, bool *out_desc) {
    if (clicked_key == active_key) {
        *out_key = active_key;
        *out_desc = !active_desc; /* re-click the active key: flip direction */
    } else {
        *out_key = clicked_key;
        *out_desc = false; /* select a different key ascending */
    }
}

/* Sort controls (§61.1): FOUR selectable key chips (Name / Size / Time / Added) plus one "warnings on
 * top" toggle. Each key has two directions -- clicking a non-active key selects it ascending, re-clicking
 * the active key flips its direction. The active chip shows its direction arrow; the internal
 * ROW_SORT_BUILD baseline is never exposed. This is view-only state (gui_rows_set_sort); it never touches
 * the model. */
static void declare_sort_chips(nt_ui_context_t *ctx) {
    row_sort_key key = ROW_SORT_NAME;
    bool desc = false;
    bool warn = false;
    gui_rows_get_sort(&key, &desc, &warn);

    static const row_sort_key keys[] = {ROW_SORT_NAME, ROW_SORT_SIZE, ROW_SORT_MTIME, ROW_SORT_ADDED};
    static const char *const key_ids[] = {"ntpacker/sort_name", "ntpacker/sort_size",
                                          "ntpacker/sort_mtime", "ntpacker/sort_added"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        const bool active = (key == keys[i]);
        char sc[24];
        if (active) {
            (void)snprintf(sc, sizeof sc, "%s %s", sort_key_label(keys[i]),
                           desc ? "\xE2\x96\xBE" : "\xE2\x96\xB4"); /* active key shows its direction */
        } else {
            (void)snprintf(sc, sizeof sc, "%s", sort_key_label(keys[i]));
        }
        const uint32_t chip_id = nt_ui_id(key_ids[i]);
        if (ui_sort_chip(ctx, chip_id, sc, active)) {
            row_sort_key next_key = key;
            bool next_desc = desc;
            sort_chip_next(key, desc, keys[i], &next_key, &next_desc);
            gui_rows_set_sort(next_key, next_desc, warn);
        }
        record_row_tip(chip_id, active ? "Active sort key: click to flip ascending/descending"
                                       : "Sort sprites by this key (re-click the active key to flip)");
    }

    const uint32_t warn_id = nt_ui_id("ntpacker/sort_warn");
    if (ui_sort_chip(ctx, warn_id, "\xE2\x9A\xA0", warn)) {
        gui_rows_set_sort(key, desc, !warn);
    }
    record_row_tip(warn_id, "Toggle: missing / warning rows on top");
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

    /* U-02 #7: the sort controls get their OWN full-width row directly under the SPRITES header. Four key chips
     * + warn toggle overran the header row (label + chips + two Add buttons) on a standard/narrow panel where
     * Clay can't wrap and X-clip is forbidden; on their own row (~212px at scale 1) they fit with margin. */
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(24))}, .childGap = Su(4), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        declare_sort_chips(ctx); /* U-02 T7: sort key/dir + warn-on-top */
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
    }

    /* Speed-search filter bar: shown while armed (Ctrl+F) or whenever a query is set. */
    if (s_filter_active || gui_rows_filter_active()) {
        const uint32_t clr_id = nt_ui_id("ntpacker/filter_clear");
        const nt_ui_events_t clr_ev = nt_ui_events(ctx, clr_id, NULL);
        if (clr_ev.clicked) {
            gui_rows_set_filter("");
            s_filter_active = false;
        }
        char shown[300];
        const char *q = gui_rows_filter();
        (void)snprintf(shown, sizeof shown, "Filter: %s%s", q, s_filter_active ? "|" : "");
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H - 3.0F))},
                         .padding = {Su(8), Su(4), Su(2), Su(2)},
                         .childGap = Su(4),
                         .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = C_HOVER,
              .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), shown,
                        s_filter_active ? &g_row_strong : &g_row);
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
            record_row_tip(clr_id, "Clear filter (Esc)");
            (void)ui_icon_btn(ctx, clr_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                              clr_ev.hovered ? &g_danger : &g_caption);
        }
    }

    if (s_view_count == 0) {
        const char *msg = gui_rows_filter_active() ? "No sprites match the filter."
                                                   : "No sources. Add a smart folder or files.";
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), msg, &g_caption);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        return;
    }

    focus_clamp();
    nt_ui_vlist_style_t vs = nt_ui_vlist_style_defaults();
    vs.overscan = 3;
    vs.id_ring = UI_ROW_ID_RING; /* bound per-row state to the viewport, not project size */
    const nt_ui_vlist_range_t r = nt_ui_vlist_begin(
        ctx, NULL, s_id_vlist, (uint32_t)s_view_count, S(BASE_ROW_H), NT_UI_AXIS_Y, &vs,
        &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}});
    /* Ensure-visible: after a keyboard focus move, scroll the vlist so the focused row is in view
     * (once — s_focus_follow is consumed here so wheel scrolling is never fought). */
    if (s_focus_follow && s_focus_view >= 0 && s_focus_view < s_view_count) {
        const uint32_t fv = (uint32_t)s_focus_view;
        if (r.last < r.first || fv < r.first) {
            nt_ui_scroll_to(ctx, s_id_vlist, 0.0F, -(float)fv * S(BASE_ROW_H));
        } else if (fv > r.last) {
            const uint32_t win = r.last - r.first + 1U;
            int top = (int)fv - (int)win + 1;
            if (top < 0) {
                top = 0;
            }
            nt_ui_scroll_to(ctx, s_id_vlist, 0.0F, -(float)top * S(BASE_ROW_H));
        }
        s_focus_follow = false;
    }
    if (r.first <= r.last) {
        for (uint32_t i = r.first; i <= r.last; i++) {
            const sprite_row *row = &s_rows[s_view[i]];
            const uint32_t row_id = nt_ui_vlist_item_id(ctx, i);
            const uint32_t hit_id = nt_ui_child_id(row_id, "hit");
            const uint32_t x_id = nt_ui_child_id(row_id, "x");
            const bool editing = row->sprite_name && row->sprite_name[0] != '\0' &&
                                 gui_sprite_edit_matches(row);
            const bool leaf_row = (!row->is_folder && !row->missing &&
                                   row->sprite_name &&
                                   row->sprite_name[0] != '\0');
            const bool primary = (row->is_source ? (s_sel_src == row->src && s_sel_child == -1)
                                                  : (s_sel_src == row->src && s_sel_child == row->child));
            const bool selected = primary ||
                                  (leaf_row && multi_sel_contains_ref(
                                                   row->source_id,
                                                   row->source_key));
            const nt_ui_events_t ev = nt_ui_events(ctx, hit_id, &s_dbl_cfg);
            bool x_clicked = false;
            nt_ui_events_t xev = {0}; /* hoisted: the render below reads xev.hovered for the danger tint */
            if (row->is_source) {
                xev = nt_ui_events(ctx, x_id, NULL);
                x_clicked = xev.clicked;
                if (x_clicked) {
                    const tp_session_snapshot *snapshot = gui_project_snapshot();
                    const tp_snapshot_atlas *atlas = snapshot
                                                         ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                                                         : NULL;
                    const tp_snapshot_source *source = atlas
                                                           ? tp_session_snapshot_source_at(
                                                                 snapshot, atlas->id, row->src)
                                                           : NULL;
                    if (source) {
                        s_pending_remove_source = true;
                        s_pending_remove_source_atlas_id = atlas->id;
                        s_pending_remove_source_id = source->id;
                        s_pending_remove_source_revision =
                            tp_session_snapshot_revision(snapshot);
                    }
                }
            }
            if (ev.double_clicked && row->is_folder) {
                gui_rows_toggle_collapsed(row->source_id); /* U-02 T2: double-click a folder collapses/expands it */
                s_focus_view = (int)i;
            } else if (ev.double_clicked && !row->is_folder && !row->missing) {
                select_sprite_row((int)i, false, false);
                s_focus_view = (int)i;
                start_sprite_edit(row);
            } else if (ev.clicked && !x_clicked) {
                const bool ctrl = nt_input_key_is_down(NT_KEY_LCTRL) || nt_input_key_is_down(NT_KEY_RCTRL);
                const bool shift = nt_input_key_is_down(NT_KEY_LSHIFT) || nt_input_key_is_down(NT_KEY_RSHIFT);
                select_sprite_row((int)i, ctrl, shift);
                s_focus_view = (int)i; /* click moves keyboard focus here too */
            }
            if (nt_ui_menu_open_trigger(ctx, s_id_ctx_menu, hit_id, false, &s_ctx_state)) {
                close_menubar_menus();
                /* right-click a row NOT in the multi-set selects just it; keep an existing set otherwise */
                if (!(leaf_row && multi_sel_contains_ref(row->source_id,
                                                         row->source_key))) {
                    select_sprite_row((int)i, false, false);
                } else {
                    s_sel_src = row->src;
                    s_sel_child = row->child;
                    s_sel_missing = row->missing;
                    (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", row->abs);
                }
                s_ctx_kind = CTX_SPRITE;
                const tp_session_snapshot *snapshot = gui_project_snapshot();
                const tp_snapshot_atlas *atlas = snapshot
                    ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                    : NULL;
                s_ctx_sprite_atlas_id = atlas ? atlas->id : tp_id128_nil();
                s_ctx_sprite_source_id = row->source_id;
                s_ctx_sprite_revision = snapshot
                    ? tp_session_snapshot_revision(snapshot)
                    : 0;
                (void)snprintf(s_ctx_sprite_source_key,
                               sizeof s_ctx_sprite_source_key, "%s",
                               row->source_key ? row->source_key : ""); /* folder/source/missing rows have no key */
                (void)snprintf(s_ctx_sprite_display_name,
                               sizeof s_ctx_sprite_display_name, "%s",
                               gui_rows_effective_name(row)); /* F10: Copy name / Rename use the rename */
                (void)snprintf(s_ctx_sprite_abs, sizeof s_ctx_sprite_abs, "%s",
                               row->abs ? row->abs : ""); /* F12: freeze the reveal path at arm time */
                s_ctx_leaf = leaf_row;
                s_ctx_removable = row->is_source;
            }
            const Clay_Color bg = selected ? C_SEL : (ev.hovered ? C_HOVER : C_TRANSPARENT);
            const uint16_t fw = ((int)i == s_focus_view) ? Su(1) : 0; /* keyboard focus ring */
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
                  .border = {.color = C_BORDER_STRONG, .width = {fw, fw, fw, fw, 0}},
                  .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
                CLAY({.id = {.id = hit_id},
                      .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    if (row->is_folder) {
                        /* U-02 T2: disclosure chevron reflects collapsed state (double-click / arrows toggle). */
                        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT),
                                    gui_rows_is_collapsed(row->source_id) ? "\xE2\x96\xB8" : "\xE2\x96\xBE",
                                    selected ? &g_row_strong : &g_caption);
                    }
                    ui_row_icon(ctx, ic, ic_tint);
                    if (editing) {
                        if (render_rename_field(ctx)) {
                            s_pending_commit_edit_enter = true; /* defer: never commit while holding row pointers */
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
static void declare_animations_list(nt_ui_context_t *ctx,
                                    const tp_session_snapshot *snapshot,
                                    const tp_snapshot_atlas *a) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(28))}, .childGap = Su(6), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
        section_rule_label(ctx, "ANIMATIONS");
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
        if (ui_icon_btn(ctx, nt_ui_id("ntpacker/add_anim"), &s_ic_plus, 16.0F, "Animation", &g_btn_ghost, true, 0.0F, 24.0F, &g_caption)) {
            gui_request_add_animation(s_sel_atlas);
        }
    }
    if (!a || a->animation_count == 0) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "None. Multi-select sprites, then right-click \xE2\x86\x92 Create animation.",
                    &g_caption);
        return;
    }
    /* U-02 T2: animation rows in a bounded, vertically-scrollable region (height = min(content, cap)) so a
     * long list scrolls inside its own region instead of drawing past the panel bottom. Reached only when
     * animation_count > 0 (the empty-state hint returns early), so begin/end are unconditional here. */
    const float anim_rows_h = fminf((float)a->animation_count * S(BASE_ROW_H), S(BASE_LIST_SECTION_CAP_H));
    nt_ui_scroll_begin(ctx, NULL, nt_ui_id("ntpacker/anim_scroll"), &s_panel_scroll,
                       &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(anim_rows_h)},
                                                             .layoutDirection = CLAY_TOP_TO_BOTTOM}});
    for (int i = 0; i < a->animation_count; i++) {
        const tp_snapshot_animation *animation = tp_session_snapshot_animation_at(
            snapshot, a->id, i);
        if (!animation) {
            continue;
        }
        char idbuf[64];
        (void)snprintf(idbuf, sizeof idbuf, "ntpacker/anim_row_%d", i);
        const uint32_t row_id = nt_ui_id(idbuf);
        const uint32_t x_id = nt_ui_child_id(row_id, "x");
        const bool editing = gui_animation_edit_matches(a->id, animation->id);
        const bool selected = (i == s_sel_anim);
        const nt_ui_events_t ev = nt_ui_events(ctx, row_id, &s_dbl_cfg);
        const nt_ui_events_t xev = nt_ui_events(ctx, x_id, NULL);
        if (xev.clicked) {
            gui_request_remove_animation(i);
        } else if (ev.double_clicked) {
            s_sel_anim = i;
            s_sel_anim_frame = -1;
            const gui_animation_ref preview = {
                a->id, animation->id,
                tp_session_snapshot_revision(snapshot)};
            gui_request_open_preview(&preview);
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
            s_ctx_anim_atlas_id = a->id;
            s_ctx_anim_id = animation->id;
            s_ctx_anim_revision = tp_session_snapshot_revision(snapshot);
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
                        s_pending_commit_edit_enter = true; /* defer: never commit while holding `a` */
                    }
                } else {
                    ui_label_fit(ctx, animation->name, selected ? &g_row_strong : &g_row,
                                 fmaxf(left_row_text_w(S(8.0F), true) - S(28.0F) - S(ROW_ICON_RESERVE), S(16.0F)), row_id);
                }
            }
            char fc[16];
            (void)snprintf(fc, sizeof fc, "%df", animation->frame_count);
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), fc, &g_caption);
            record_row_tip(x_id, "Remove animation");
            (void)ui_icon_btn(ctx, x_id, &s_ic_x, 12.0F, NULL, &g_btn_ghost, true, 24.0F, 22.0F,
                              xev.hovered ? &g_danger : &g_caption);
        }
    }
    nt_ui_scroll_end(ctx);
}

void declare_left_panel(nt_ui_context_t *ctx) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot
                                     ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                                     : NULL;
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
        declare_atlas_list(ctx, snapshot);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(1))}}, .backgroundColor = C_BORDER}) {}
        declare_sprite_list(ctx);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(1))}}, .backgroundColor = C_BORDER}) {}
        declare_animations_list(ctx, snapshot, a);
    }
}
