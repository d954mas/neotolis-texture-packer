#ifndef NTPACKER_GUI_PACK_H
#define NTPACKER_GUI_PACK_H

/* In-process packing for the GUI (ux.md §3.2/§3.3b). Owns the tp_pack input assembly from the
 * live project and one arena-owned tp_result PER ATLAS (the atlas-page canvas renders it).
 *
 * Raw-name convention (must match the exporter/CLI path -- see tp_normalize + test_export_run):
 *   - FILE source  -> sprite raw name = the file's BASENAME, extension KEPT (e.g. "a.png").
 *   - FOLDER source-> sprite raw name = the path RELATIVE to the folder root, extension KEPT and
 *     sub-folders preserved (e.g. "anim/test-0.png").
 * The raw name is what tp_name_map hashes; tp_normalize strips the extension (folders kept) at
 * export/preview naming, so the STRIPPED raw name == the sprite-tree override key ("anim/test-0"),
 * which is how selection sync maps a canvas region back to a list row.
 *
 * gui_pack_atlas/gui_pack_export are the BLOCKING core (the selftest + --shot use them for
 * determinism). Interactive use runs them on a worker thread via the async API below, so a slow
 * concave pack never freezes the window. Missing files are SKIPPED with a notice (never fatal,
 * ux.md §3.7). */

#include <stddef.h>
#include <stdbool.h>

#include "tp_core/tp_model.h" /* tp_result */

#ifdef __cplusplus
extern "C" {
#endif

/* Stores the session work dir (where tp_pack writes the transient .ntpack) and creates it. */
void gui_pack_init(const char *work_dir);

/* Packs atlas `atlas_index` of the live project: assembles sprites from its sources (files as-is,
 * folders expanded via gui_scan), maps per-sprite origin/slice9 overrides, runs tp_pack, and stores
 * the arena-owned tp_result in the atlas's slot (the previous arena is destroyed first).
 *
 * On success returns true, writes the wall-clock pack time to *out_ms (nullable), and appends any
 * skipped-missing-file count to `notice` (nullable, cap notice_cap). On failure returns false and
 * fills `err` (nullable). An atlas with zero usable sprites is a failure (nothing to show). */
bool gui_pack_atlas(int atlas_index, double *out_ms, char *err, size_t err_cap, char *notice, size_t notice_cap);

/* The stored result for `atlas_index`, or NULL if never packed / last pack failed. */
const tp_result *gui_pack_result(int atlas_index);

/* Sprite index within the stored result whose STRIPPED raw name (ext removed, folders kept) equals
 * `key` (the sprite-tree override key), or -1. Powers list-row -> canvas-region selection sync. */
int gui_pack_find_sprite(int atlas_index, const char *key);

/* Exports every ENABLED target of atlas `atlas_index` (tp_export_run packs per target with the
 * atlas settings INTERSECT each target's capabilities, then writes files). Assembles the same sprite
 * set as gui_pack_atlas; creates each target's output parent directory first. Returns true on success
 * and writes the enabled-target count to *out_targets and the metadata-loss notice count to
 * *out_notices (both nullable); a joined notice summary goes to `notice`. On failure returns false and
 * fills `err` (e.g. unsaved project with relative output paths). */
bool gui_pack_export(int atlas_index, int *out_targets, int *out_notices, char *err, size_t err_cap, char *notice,
                     size_t notice_cap);

/* Drops the stored result for one atlas (or all with index < 0). Frees its arena. Call on
 * project new/open and before a repack. */
void gui_pack_clear(int atlas_index);

/* --- export-target preview (packet EXP-PREVIEW) --------------------------------------------------
 * A view-only "what would exporter <id> produce from the CURRENT settings" pack, kept in ONE arena-
 * owned preview slot SEPARATE from the session slots (the native result is never clobbered). The
 * effective settings are tp_project_atlas_to_settings clamped through the exporter's caps
 * (tp_export_effective_settings). Only one preview is live at a time (dropped on atlas switch / edit),
 * so a single slot keyed by atlas_index guarantees coherent binding. */

/* Blocking preview pack of `atlas_index` for exporter `exporter_id` (deterministic path for the
 * selftest + --shot-preview). Mirrors gui_pack_atlas but lands in the preview slot. false fills err. */
bool gui_pack_preview_blocking(int atlas_index, const char *exporter_id, char *err, size_t err_cap);

/* Async preview pack (interactive): reuses the worker thread; result lands in the preview slot at a
 * frame boundary (gui_pack_poll -> GUI_PACK_DONE_PREVIEW_*). false (fills err) if busy / can't assemble. */
bool gui_pack_preview_async_start(int atlas_index, const char *exporter_id, char *err, size_t err_cap);

/* The stored preview result IF it belongs to `atlas_index`, else NULL (coherent binding: a stale slot
 * from another atlas never shows). */
const tp_result *gui_pack_preview_result(int atlas_index);

/* Drops the preview slot (frees its arena). Call on back-to-Native / atlas switch / model edit. */
void gui_pack_preview_clear(void);

/* Degradation summary for `exporter_id` on `atlas_index`: diffs the native (session) tp_pack_settings
 * against the caps-clamped effective settings, plus the caps-vs-usage metadata drops (slice9/pivot).
 * Writes a SHORT chip caption to `chip` (empty when the format expresses everything) and a longer
 * field-by-field breakdown to `tip` (nullable). Returns the number of degradations found. */
int gui_pack_preview_diff(int atlas_index, const char *exporter_id, char *chip, size_t chip_cap, char *tip,
                          size_t tip_cap);

/* --- async packing (interactive; ux.md §3 worker thread) --------------------------------------
 * One in-flight op MAX (pack OR export). The heavy tp_pack/tp_export_run runs on a worker thread
 * over a self-contained snapshot; the UI stays interactive. Poll each frame and swap the result in
 * at a frame boundary (gui_pack_poll). The blocking gui_pack_atlas/gui_pack_export above stay the
 * deterministic path used by the selftest and --shot. */
typedef enum {
    GUI_PACK_ASYNC_NONE = 0,
    GUI_PACK_ASYNC_PACK,
    GUI_PACK_ASYNC_EXPORT
} gui_pack_async_kind;

typedef enum {
    GUI_PACK_DONE_NONE = 0, /* nothing landed this frame */
    GUI_PACK_DONE_PACK_OK,
    GUI_PACK_DONE_PACK_FAIL,
    GUI_PACK_DONE_PACK_CANCELLED,
    GUI_PACK_DONE_EXPORT_OK,
    GUI_PACK_DONE_EXPORT_FAIL,
    GUI_PACK_DONE_EXPORT_CANCELLED,
    /* Export-target PREVIEW pack (EXP-PREVIEW): lands in the SEPARATE preview slot, never the session
     * slot; the native pack/export/stale state is untouched. */
    GUI_PACK_DONE_PREVIEW_OK,
    GUI_PACK_DONE_PREVIEW_FAIL,
    GUI_PACK_DONE_PREVIEW_CANCELLED
} gui_pack_done;

typedef struct {
    gui_pack_done kind;
    int atlas_index;    /* pack: which atlas landed */
    double ms;          /* pack: wall-clock pack time */
    bool model_changed; /* pack: model differs from the packed snapshot -> keep preview stale */
    int missing;        /* pack: skipped-missing-source count */
    int targets;        /* export: enabled targets written */
    int notices;        /* export: metadata-loss notices */
    int atlases_ok;     /* export: atlases exported OK */
    int atlases_fail;   /* export: atlases that failed */
    char err[256];      /* failure / first-error text */
    char note[128];     /* pack: notice text */
} gui_pack_result_info;

/* Starts an async pack of `atlas_index`. false (fills err) if busy or the input can't assemble. */
bool gui_pack_async_start(int atlas_index, char *err, size_t err_cap);
/* Starts an async export of every exporting atlas. false (fills err) if busy / nothing to export /
 * relative out-paths need a saved project. */
bool gui_pack_export_async_start(char *err, size_t err_cap);
/* Call once per frame. If a worker finished, joins it, applies the pack slot swap (pack), and
 * returns the completion (else GUI_PACK_DONE_NONE). Fills *out. */
gui_pack_done gui_pack_poll(gui_pack_result_info *out);
bool gui_pack_async_busy(void);
gui_pack_async_kind gui_pack_async_active_kind(void);
double gui_pack_async_elapsed_sec(void);
void gui_pack_export_progress(int *cur, int *total); /* export "atlas cur/total" for the strip */
/* Requests cancel: the non-interruptible worker runs to completion, but its result is discarded when
 * it lands (pack) / no further atlases are started (export). */
void gui_pack_async_cancel(void);
bool gui_pack_async_cancelling(void);
/* DEV (--shot-packing): force the busy strip state without a real worker, for screenshots. */
void gui_pack_debug_force_busy(gui_pack_async_kind kind);

void gui_pack_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PACK_H */
