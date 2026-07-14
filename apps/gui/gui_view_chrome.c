/* Chrome view (see gui_view_chrome.h). Split out of main.c (GUI decomposition step 6b) as a pure
 * move -- function bodies + chrome-local statics relocated verbatim, no behavior change. */

#include "gui_view_chrome.h"

#include "clay.h"
#include "ui/nt_ui_button.h"
#include "ui/nt_ui_menu.h"
#include "ui/nt_ui_modal.h"
#include "ui/nt_ui_scroll.h"
#include "ui/nt_ui_tooltip.h"

#include "tp_core/tp_export.h" /* exporter registry -> target dropdown (export modal) */

#include "gui_defs.h"
#include "gui_state.h"
#include "gui_widgets.h"
#include "gui_actions.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_version.h" /* NTPACKER_VERSION/NTPACKER_ENGINE_NAME/NTPACKER_REPO_URL (About modal) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* --- menu bar + context menu state (chrome-local; nothing outside this TU ever read them) --- */
static nt_ui_menu_state_t s_file_state, s_edit_state, s_view_state, s_help_state;
static nt_ui_menu_ctx_t s_file_menu, s_edit_menu, s_view_menu, s_help_menu;
enum {
    MK_NEW = 1, MK_OPEN, MK_SAVE, MK_SAVEAS, MK_EXPORT, MK_REFRESH, MK_EXIT,
    MK_UNDO, MK_REDO,
    MK_ZIN, MK_ZOUT, MK_FIT, MK_ABOUT, MK_S100, MK_S125, MK_S150, MK_S200,
    MK_OV_OUTLINE, MK_OV_FRAME, MK_OV_TRIM, MK_OV_PIVOT, MK_OV_SLICE9, MK_CTX_FIT, MK_CTX_100,
    MK_CTX_RENAME, MK_CTX_REMOVE, MK_CTX_TOGGLE, MK_CTX_CREATE_ANIM, MK_CTX_PREVIEW
};

/* Right-click context menu: one cursor-anchored menu whose items depend on the row a right-click
 * armed it over (§3.3e mouse-complete access). Its actions call the same code paths as the [x]
 * buttons / inline editors. The trigger/payload state (s_id_ctx_menu, s_ctx_state, s_ctx_kind, the
 * CTX_ enum, s_ctx_atlas, s_ctx_anim, s_ctx_target, s_ctx_src, s_ctx_sprite, s_ctx_leaf,
 * s_ctx_removable) lives in gui_state (step 4 -- written by three different views); s_ctx_menu is the
 * declare-machinery working buffer for declare_context_menu below, chrome-local. */
static nt_ui_menu_ctx_t s_ctx_menu;

/* Opens `url` in the OS default browser. Reusable helper (About link now; future notices/docs links
 * reuse it). Windows: ShellExecuteA (shell32 -- already linked via tinyfiledialogs). POSIX:
 * xdg-open/open, best-effort. Returns true if the open was dispatched. Chrome-only (the About modal
 * is its sole caller). */
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

void close_menubar_menus(void) {
    s_file_state.open = false;
    s_edit_state.open = false;
    s_view_state.open = false;
    s_help_state.open = false;
}
void close_all_menus(void) {
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
    if (nt_ui_menu_item_ex(m, MK_EXPORT, "Export\xE2\x80\xA6", (nt_ui_menu_item_opts_t){.shortcut = "Ctrl+E"})) {
        s_export_open = true;
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
    overlay_item(m, MK_OV_SLICE9, "Slice9 guides", &s_canvas.show_slice9);
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
void declare_menubar(nt_ui_context_t *ctx) {
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_MENUBAR_H))},
                     .padding = {Su(4), Su(8), 0, 0},
                     .childGap = Su(2),
                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = C_STATUS}) { /* docked: flush to the top edge, no rounded corners */
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

/* Emit hover tooltips (full text) for the truncated rows collected this frame. */
void declare_row_tooltips(nt_ui_context_t *ctx) {
    for (int i = 0; i < s_row_tip_count; i++) {
        (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, s_row_tips[i].id, s_row_tips[i].full, &s_tip_style);
    }
}

void declare_menus(nt_ui_context_t *ctx) {
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
void declare_context_menu(nt_ui_context_t *ctx) {
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
                gui_edit_target_enabled(s_sel_atlas, s_ctx_target, !t->enabled); /* H/G3: preserves a buffered out-path edit */
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
        overlay_item(&s_ctx_menu, MK_OV_SLICE9, "Slice9 guides", &s_canvas.show_slice9);
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

void declare_tooltips(nt_ui_context_t *ctx) {
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
    /* Smart-folder + Files add buttons (make the live-linked-folder behaviour explicit -- owner 2026-07-11). */
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/add_folder"),
                        "Add a smart folder: every image inside it is packed -- including files added later. Press F5 to rescan.",
                        &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/add_files"),
                        "Add individual image files (the exception -- prefer a smart folder so new art is picked up automatically).",
                        &s_tip_style);
    /* Icon-only strip ghosts (mouse-complete). A tooltip for a control not laid out this frame (single
     * page, non-atlas mode) is a safe no-op -- it only opens on hover of an existing target. */
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/pg_prev"), "Previous page", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/pg_next"), "Next page", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/zoom_out"), "Zoom out", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/zoom_100"), "Zoom to 100% (actual size)", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/zoom_in"), "Zoom in", &s_tip_style);
    (void)nt_ui_tooltip(ctx, NT_UI_DATA_LAYER(LAYER_IMG), LAYER_TEXT, nt_ui_id("ntpacker/zoom_fit"), "Fit atlas to view", &s_tip_style);
}

/* Export dialog (mouse-complete): every atlas's targets, toggle/browse per target, then Export runs the
 * same do_export path. All edits enqueue via gui_edit_target (committed at frame top; dirty + undo,
 * no parallel state) -- never commit while holding p/a/t (F2-05b-i UAF fix). */
void declare_export_modal(nt_ui_context_t *ctx) {
    if (!nt_ui_modal_visible(ctx, s_id_export_modal, &s_modal_style, &s_export_open)) {
        return;
    }
    tp_project *p = gui_project_get();
    int enabled_targets = 0;
    int atlases_with = 0;
    int total_targets = 0;
    int line_count = 0;
    for (int ai = 0; p && ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        if (a->target_count > 0) {
            atlases_with++;
            line_count += 1 + a->target_count;
        }
        for (int ti = 0; ti < a->target_count; ti++) {
            total_targets++;
            enabled_targets += a->targets[ti].enabled ? 1 : 0;
        }
    }
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(560)), CLAY_SIZING_FIT(0)},
                     .padding = {Su(22), Su(22), Su(20), Su(20)},
                     .layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = Su(14)},
          .backgroundColor = C_PANEL,
          .cornerRadius = CLAY_CORNER_RADIUS(S(8)),
          .border = {.color = C_BORDER, .width = {Su(1), Su(1), Su(1), Su(1), 0}}}) {
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "Export", &g_body);
        if (total_targets == 0) {
            nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), "This project has no export targets yet.", &g_caption);
            CLAY({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = Su(12), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (ui_btn(ctx, nt_ui_id("export/goto"), "Add targets in the panel", &g_btn_primary, true, 0.0F, 34.0F, &g_onaccent)) {
                    s_export_open = false;
                    s_sec_export_open = true;
                }
                if (ui_btn(ctx, nt_ui_id("export/close0"), "Close", &g_btn, true, 100.0F, 34.0F, &g_body)) {
                    s_export_open = false;
                }
            }
            nt_ui_modal_end(ctx);
            return;
        }
        const int ne = tp_exporter_count();
        float list_h = (float)line_count * S(30.0F) + S(6.0F);
        const float list_cap = S(330.0F);
        if (list_h > list_cap) {
            list_h = list_cap;
        }
        nt_ui_scroll_begin(ctx, NULL, nt_ui_id("export/scroll"), &s_panel_scroll,
                           &(Clay_ElementDeclaration){.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(list_h)}}});
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = Su(4), .padding = {0, Su(8), 0, 0}}}) {
            for (int ai = 0; ai < p->atlas_count; ai++) {
                tp_project_atlas *a = &p->atlases[ai];
                if (a->target_count == 0) {
                    continue;
                }
                nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), a->name, &g_row);
                for (int ti = 0; ti < a->target_count && ti < GUI_MAX_TARGETS; ti++) {
                    tp_project_target *t = &a->targets[ti];
                    char idb[48];
                    (void)snprintf(idb, sizeof idb, "export/a%d_t%d", ai, ti);
                    const uint32_t rid = nt_ui_id(idb);
                    const char *exp_name = t->exporter_id;
                    for (int i = 0; i < ne; i++) {
                        const tp_exporter *e = tp_exporter_at(i);
                        if (e && strcmp(e->id, t->exporter_id) == 0) {
                            exp_name = (e->display_name && e->display_name[0]) ? e->display_name : e->id;
                            break;
                        }
                    }
                    const bool has_path = (t->out_path && t->out_path[0] != '\0');
                    const nt_ui_events_t pev = nt_ui_events(ctx, nt_ui_child_id(rid, "path"), NULL);
                    if (pev.clicked) {
                        s_pending_export_browse_atlas = ai;
                        s_pending_export_browse_target = ti;
                    }
                    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(BASE_ROW_H))},
                                     .padding = {Su(8), Su(6), 0, 0},
                                     .childGap = Su(8),
                                     .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                        if (tp_checkbox(ctx, nt_ui_child_id(rid, "en"), t->enabled, true)) {
                            gui_edit_target_enabled(ai, ti, !t->enabled); /* H/G3: preserves a buffered out-path edit */
                        }
                        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(S(96)), CLAY_SIZING_GROW(0)}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                            ui_label_fit(ctx, exp_name, &g_caption, S(96), 0U);
                        }
                        CLAY({.id = {.id = nt_ui_child_id(rid, "path")},
                              .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = {Su(6), Su(6), 0, 0}, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}},
                              .backgroundColor = pev.hovered ? C_HOVER : C_BG,
                              .cornerRadius = CLAY_CORNER_RADIUS(S(4))}) {
                            ui_label_fit(ctx, has_path ? t->out_path : "(click to set output path)", has_path ? &g_row : &g_dim, S(300), 0U);
                        }
                        if (ui_btn(ctx, nt_ui_child_id(rid, "br"), "\xE2\x80\xA6", &g_btn_ghost, true, 28.0F, 22.0F, &g_caption)) {
                            s_pending_export_browse_atlas = ai;
                            s_pending_export_browse_target = ti;
                        }
                    }
                }
                /* Targets past GUI_MAX_TARGETS aren't listed (the per-target UI arrays are fixed), but they
                 * DO export -- surface the hidden tail rather than dropping it silently (P2). */
                if (a->target_count > GUI_MAX_TARGETS) {
                    char more[80];
                    (void)snprintf(more, sizeof more, "+%d more target(s) not shown (still exported).",
                                   a->target_count - GUI_MAX_TARGETS);
                    nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), more, &g_dim);
                }
            }
        }
        nt_ui_scroll_end(ctx);
        char summary[96];
        (void)snprintf(summary, sizeof summary, "%d target%s enabled across %d atlas%s", enabled_targets, enabled_targets == 1 ? "" : "s", atlases_with,
                       atlases_with == 1 ? "" : "es");
        nt_ui_label(ctx, NT_UI_DATA_LAYER(LAYER_TEXT), summary, &g_caption);
        CLAY({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = Su(12), .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
            if (ui_btn(ctx, nt_ui_id("export/run"), gui_pack_async_busy() ? "Exporting\xE2\x80\xA6" : "Export", &g_btn_primary,
                       enabled_targets > 0 && !gui_pack_async_busy(), 120.0F, 34.0F, &g_onaccent)) {
                s_pending_export = true;
                s_export_open = false;
            }
            if (ui_btn(ctx, nt_ui_id("export/cancel"), "Cancel", &g_btn, true, 100.0F, 34.0F, &g_body)) {
                s_export_open = false;
            }
        }
    }
    nt_ui_modal_end(ctx);
}

void declare_confirm_modal(nt_ui_context_t *ctx) {
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
                if (ui_btn(ctx, nt_ui_id("ntpacker/modal_save"), "Save", &g_btn_primary, true, 100.0F, 34.0F, &g_onaccent)) {
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

void declare_about_modal(nt_ui_context_t *ctx) {
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
                    set_status_ex(STATUS_WARNING, "Could not open browser -- " NTPACKER_REPO_URL);
                }
            }
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(S(4))}}}) {}
            CLAY({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                if (ui_btn(ctx, nt_ui_id("ntpacker/about_ok"), "OK", &g_btn_primary, true, 100.0F, 34.0F, &g_onaccent)) {
                    s_about_open = false;
                }
            }
        }
        nt_ui_modal_end(ctx);
    }
}
