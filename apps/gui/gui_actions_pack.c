#include "gui_actions_internal.h"

#include <stdio.h>

#include "gui_defs.h"
#include "gui_state.h"
#include "gui_project.h"
#include "gui_pack.h"

#include "tp_core/tp_export.h"

// #region pack / export actions
/* Ctrl+P / Pack: start the selected atlas's typed session Pack job. On success clear the
 * preview-stale bit and upload the packed pages to the canvas (atlas-page view); on failure the
 * previous result + the "outdated" tag stay (ux.md §3.3b). */
/* Blocking pack of the selected atlas (deterministic path for selftest + --shot). Interactive
 * Pack starts the same session job and polls its typed result at frame boundaries. */
void do_pack_blocking(void) {
    if (gui_actions__flush_failed()) {
        return; /* A rejected buffered edit must not produce a stale Pack result. */
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
    if (!a || a->source_count == 0) {
        set_status_ex(STATUS_WARNING, "No sources to pack -- add a smart folder or files first.");
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
        const double shown_ms = s_status_fixed_time ? 0.0 : ms;
        if (note[0] != '\0') {
            set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms (%s)", r->sprite_count, r->page_count, shown_ms, note);
        } else {
            set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms", r->sprite_count, r->page_count, shown_ms);
        }
    } else {
        set_statusf_ex(STATUS_ERROR, "Pack failed: %s", err);
    }
}

/* Interactive Pack (Ctrl+P / strip / stale chip): starts the pack on a worker thread so the window
 * never freezes. Completion is applied at a frame boundary by poll_async (apply_pending). */
void do_pack(void) {
    if (gui_actions__flush_failed()) {
        return; /* A rejected buffered edit must not produce a stale Pack result. */
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
    if (!a || a->source_count == 0) {
        set_status_ex(STATUS_WARNING, "No sources to pack -- add a smart folder or files first.");
        return;
    }
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Busy -- a pack or export is already running.");
        return;
    }
    char err[256] = {0};
    if (gui_pack_async_start(s_sel_atlas, err, sizeof err)) {
        set_status_ex(STATUS_INFO, "Packing\xE2\x80\xA6");   /* result lands via poll_async */
    } else {
        set_statusf_ex(STATUS_ERROR, "Pack failed: %s", err);
    }
}

/* Ctrl+E / Export: starts an async export of every atlas with sources + >=1 enabled target on a
 * worker thread (per-atlas failures non-fatal, ux.md §3.5). Completion reported via poll_async. */
static void do_export(void) {
    if (gui_actions__flush_failed()) {
        return; /* A rejected buffered edit must not produce a stale Export. */
    }
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Busy -- a pack or export is already running.");
        return;
    }
    char err[256] = {0};
    if (gui_pack_export_async_start(err, sizeof err)) {
        set_status_ex(STATUS_INFO, "Exporting\xE2\x80\xA6"); /* progress + result via poll_async */
    } else {
        set_status_ex(STATUS_WARNING, err);
    }
}

// #region export-target preview (packet EXP-PREVIEW)
/* Back to Native: drop the preview state + free gui_pack's preview slot. Idempotent. */
void preview_target_reset(void) {
    s_preview_target = 0;
    gui_pack_preview_clear();
}

/* The result the canvas should BIND this frame: the export preview when one is selected, has landed, and
 * the strip selector is actually visible (single-row tier); otherwise the native session pack. The anim
 * player owns the canvas in its own mode, so it always falls back to native (its frame indices are into
 * the native result). Single source of truth -- used by main.c's canvas bind AND the canvas stats line. */
const tp_result *preview_target_result(void) {
    const tp_result *native = gui_pack_result(s_sel_atlas);
    if (s_preview_target == 0 || s_preview_active) {
        return native;
    }
    if (s_canvas_w < S(STRIP_PREVIEW_MIN_W)) {
        return native; /* the selector folded away (compact / narrow single-row) -> show the honest session pack */
    }
    const tp_result *pv = gui_pack_preview_result(s_sel_atlas);
    return pv ? pv : native; /* preview not landed yet (async) -> native until it does */
}

/* Starts the preview pack for a strip-selector pick. combo 0 (or a bad index) -> Native. */
static void preview_target_start(int combo_index) {
    if (combo_index <= 0) {
        preview_target_reset();
        return;
    }
    const tp_exporter *e = tp_exporter_at(combo_index - 1);
    if (!e) {
        preview_target_reset();
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
    if (!a || a->source_count == 0) {
        set_status_ex(STATUS_WARNING, "Add sources first to preview an export target.");
        preview_target_reset();
        return;
    }
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Busy -- wait for the current pack or export to finish.");
        preview_target_reset();
        return;
    }
    char err[256] = {0};
    if (gui_pack_preview_async_start(s_sel_atlas, e->id, err, sizeof err)) {
        s_preview_target = combo_index;
        set_statusf_ex(STATUS_INFO, "Preview: %s\xE2\x80\xA6", e->display_name ? e->display_name : e->id);
    } else {
        preview_target_reset();
        set_statusf_ex(STATUS_ERROR, "Preview failed: %s", err);
    }
}

/* Per-frame reconciliation: a model edit since the preview packed makes it stale -> drop to Native (never
 * show a silently-wrong preview). Atlas switch / undo / redo / open / new drop it via reset_selection. */
static void preview_target_sync(void) {
    if (s_preview_target != 0 && !gui_pack_async_busy() &&
        !gui_pack_preview_result(s_sel_atlas)) {
        preview_target_reset();
    }
}
// #endregion

/* Lands a finished async pack/export at a frame boundary: the pack slot swap is done inside
 * gui_pack_poll; here we recompute stale honestly (mark_packed only when the model still matches
 * the packed snapshot) and route the outcome through the severity status. Called from apply_pending. */
static void poll_async(void) {
    gui_pack_result_info info;
    switch (gui_pack_poll(&info)) {
        case GUI_PACK_DONE_PACK_OK: {
            /* The core-captured immutable input token covers both committed
             * model state and source-runtime refreshes. */
            if (!info.input_changed) {
                gui_project_mark_packed();
            }
            s_last_pack_ms = info.ms;
            s_last_pack_atlas = info.atlas_index;
            const tp_result *r = gui_pack_result(info.atlas_index);
            const char *stale = info.input_changed ? " -- inputs changed, stale" : "";
            if (r && info.note[0] != '\0') {
                set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms (%s)%s", r->sprite_count, r->page_count, info.ms, info.note, stale);
            } else if (r) {
                set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms%s", r->sprite_count, r->page_count, info.ms, stale);
            }
            break;
        }
        case GUI_PACK_DONE_PACK_FAIL:
            set_statusf_ex(STATUS_ERROR, "Pack failed: %s", info.err);
            break;
        case GUI_PACK_DONE_PACK_CANCELLED:
            set_status_ex(STATUS_INFO, "Pack cancelled.");
            break;
        case GUI_PACK_DONE_EXPORT_OK:
            set_statusf_ex(info.notices > 0 ? STATUS_WARNING : STATUS_SUCCESS, "Exported %d target(s)%s", info.targets,
                           info.notices > 0 ? " (metadata notices raised)" : "");
            break;
        case GUI_PACK_DONE_EXPORT_FAIL:
            set_statusf_ex(STATUS_ERROR, "Exported %d target(s); %d atlas(es) failed -- %s", info.targets, info.atlases_fail, info.err);
            break;
        case GUI_PACK_DONE_EXPORT_CANCELLED:
            set_status_ex(STATUS_INFO, "Export cancelled.");
            break;
        case GUI_PACK_DONE_PREVIEW_OK:
            if (info.input_changed) {
                preview_target_reset();
                set_status_ex(STATUS_WARNING,
                              "Preview inputs changed -- run Preview again.");
            } else if (s_preview_target == 0) {
                gui_pack_preview_clear(); /* selection reset/changed while packing -> drop the orphan slot */
            } else {
                /* The degradation chip is width-gated (STRIP_CHIP_MIN_W) and drops on common window
                 * sizes, so the pill also carries the summary -- it is width-independent. */
                const tp_exporter *pe = tp_exporter_at(s_preview_target - 1);
                if (pe) {
                    char chip[96] = {0};
                    char tip[256] = {0};
                    const int nd = gui_pack_preview_diff(s_sel_atlas, pe->id, chip, sizeof chip, tip, sizeof tip);
                    if (nd > 0) {
                        set_statusf_ex(STATUS_WARNING, "Previewing %s export: %s", pe->display_name ? pe->display_name : pe->id, chip);
                    } else {
                        set_statusf_ex(STATUS_SUCCESS, "Previewing %s export: same layout rules as native.",
                                       pe->display_name ? pe->display_name : pe->id);
                    }
                }
            }
            break;
        case GUI_PACK_DONE_PREVIEW_FAIL:
            preview_target_reset();
            set_statusf_ex(STATUS_ERROR, "Preview failed: %s", info.err);
            break;
        case GUI_PACK_DONE_PREVIEW_CANCELLED:
            preview_target_reset();
            set_status_ex(STATUS_INFO, "Preview cancelled.");
            break;
        case GUI_PACK_DONE_NONE:
        default:
            break;
    }
    /* Surface a pending transaction REJECT (core rejected a mutator's op -- out-of-range value
     * or bad reference): the model was left byte-unchanged, so report it as a soft error.
     * In practice the widgets clamp valid ranges, so this rarely fires. */
    char op_err[256];
    if (gui_project_take_op_error(op_err, sizeof op_err)) {
        set_statusf_ex(STATUS_WARNING, "Edit rejected: %s", op_err);
    }
}
// #endregion


void gui_actions__poll_pack(void) {
    poll_async();
    preview_target_sync();
}

void gui_actions__apply_pack_requests(void) {
    if (s_pending_pack) {
        do_pack();
    }
    if (s_pending_export) {
        do_export();
    }
    if (s_pending_preview_target >= 0) {
        preview_target_start(s_pending_preview_target);
    }
}
