#ifndef NTPACKER_GUI_PACK_H
#define NTPACKER_GUI_PACK_H

/* Thin GUI adapter over session-owned typed Pack/Export jobs (ux.md §3.2/§3.3b).
 * It owns only presentation result slots; input assembly, algorithms, and worker
 * lifetime remain below the frontend boundary.
 *
 * Project-built pack results use a collision-free internal name derived from
 * canonical {source_id, source_key}. GUI selection/preview therefore uses the
 * canonical lookup below; human/export names remain presentation only.
 *
 * gui_pack_atlas/gui_pack_export are synchronous adapters used by selftest/shot; they drain the
 * same session-owned typed jobs as interactive use. */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "tp_core/tp_model.h" /* tp_result */
#include "tp_core/tp_id.h"
#include "tp_core/tp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stores the work-dir intent used when starting typed Pack jobs and creates it.
 * Returns false instead of retaining a truncated or unusable directory. */
bool gui_pack_init(const char *work_dir);

/* Packs atlas `atlas_index` through a typed session job and stores the returned
 * result in the atlas presentation slot (the previous slot is destroyed first).
 *
 * On success returns true, writes the wall-clock pack time to *out_ms (nullable), and appends any
 * skipped-missing-file count to `notice` (nullable, cap notice_cap). On failure returns false and
 * fills `err` (nullable). An atlas with zero usable sprites is a failure (nothing to show). */
bool gui_pack_atlas(int atlas_index, double *out_ms, char *err, size_t err_cap, char *notice, size_t notice_cap);

/* The last successful result for `atlas_index`, or NULL if never packed. A
 * failed Pack leaves it intact; gui_project reports freshness separately. */
const tp_result *gui_pack_result(int atlas_index);
/* Changes whenever a successful Pack publishes a new result into this atlas slot. */
uint64_t gui_pack_result_version(int atlas_index);

/* Canonical lookup used by rows and animation frames. Project-built pack inputs
 * use a collision-free internal name derived from {source_id, source_key}; display
 * names are never authoritative here. */
int gui_pack_find_sprite_ref(int atlas_index, tp_id128 source_id,
                             const char *source_key);
bool gui_pack_sprite_matches_ref(int atlas_index, int sprite_index,
                                 tp_id128 source_id,
                                 const char *source_key);
/* Canonical lookup against the exact result a consumer is displaying. This is
 * required for export previews, whose sprite ordering may differ from the
 * native atlas slot. */
int gui_pack_find_sprite_ref_in_result(const tp_result *result,
                                       tp_id128 source_id,
                                       const char *source_key);

#ifdef NTPACKER_GUI_SELFTEST
typedef struct gui_pack_ref_index_work {
    uint64_t build_items;
    uint64_t build_probes;
    uint64_t lookup_calls;
    uint64_t lookup_probes;
} gui_pack_ref_index_work;
void gui_pack_ref_index_work_reset(void);
gui_pack_ref_index_work gui_pack_ref_index_work_get(void);
void gui_pack_preview_diff_work_reset(void);
uint64_t gui_pack_preview_diff_rebuilds(void);
#endif

/* Exports every ENABLED target of atlas `atlas_index` through a typed session job. Returns true on success
 * and writes the enabled-target count to *out_targets and the metadata-loss notice count to
 * *out_notices (both nullable); a joined notice summary goes to `notice`. On failure returns false and
 * fills `err` (e.g. unsaved project with relative output paths). */
bool gui_pack_export(int atlas_index, int *out_targets, int *out_notices, char *err, size_t err_cap, char *notice,
                     size_t notice_cap);

/* Drops the stored result for one atlas (or all with index < 0) and releases
 * its retained session-job result owner. Call on project new/open and before a repack. */
void gui_pack_clear(int atlas_index);

/* --- export-target preview (packet EXP-PREVIEW) --------------------------------------------------
 * A view-only "what would exporter <id> produce from the CURRENT settings" pack, kept in ONE arena-
 * owned preview slot SEPARATE from the session slots (the native result is never clobbered). The
 * effective settings are tp_project_atlas_to_settings clamped through the exporter's caps
 * (tp_export_effective_settings). Only one preview is live at a time (dropped on atlas switch / edit),
 * so one slot carrying the stable atlas ID guarantees coherent binding. */

/* Synchronous preview adapter for selftest/shot-preview; drains the same typed
 * session Pack job and lands its result in the preview slot. */
bool gui_pack_preview_blocking(int atlas_index, const char *exporter_id, char *err, size_t err_cap);

/* Async preview pack (interactive): uses the session-owned Pack handle; result lands in the preview
 * slot at a frame boundary (gui_pack_poll -> GUI_PACK_DONE_PREVIEW_*). false (fills err) if busy. */
bool gui_pack_preview_async_start(int atlas_index, const char *exporter_id, char *err, size_t err_cap);

/* The stored preview result IF it belongs to `atlas_index`, else NULL (coherent binding: a stale slot
 * from another atlas never shows). */
const tp_result *gui_pack_preview_result(int atlas_index);
/* Changes whenever a successful export preview publishes a new result. */
uint64_t gui_pack_preview_result_version(int atlas_index);

/* Drops the preview slot and releases its retained session-job result owner.
 * Call on back-to-Native / atlas switch / model edit. */
void gui_pack_preview_clear(void);

/* Degradation summary for `exporter_id` on `atlas_index`: diffs the native session settings
 * against the caps-clamped effective settings, plus the caps-vs-usage metadata drops (slice9/pivot).
 * Writes a SHORT chip caption to `chip` (empty when the format expresses everything) and a longer
 * field-by-field breakdown to `tip` (nullable). Returns the number of degradations found. */
int gui_pack_preview_diff(int atlas_index, const char *exporter_id, char *chip, size_t chip_cap, char *tip,
                          size_t tip_cap);

/* --- async packing (interactive; owned worker process) -----------------------------------------
 * One in-flight op MAX (pack OR export). The session owns the concrete worker handle and immutable
 * input; this frontend only captures intent, polls typed progress, and maps the typed result at a
 * frame boundary. The synchronous adapters above reuse and drain this exact path. */
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
    tp_session_job_rejection rejection;
    tp_status status;
    int atlas_index;    /* pack: which atlas landed */
    tp_id128 atlas_id;  /* stable owner of the landed pack */
    double ms;          /* pack: wall-clock pack time */
    bool input_changed; /* pack: model/source token differs -> keep preview stale */
    int missing;        /* pack: skipped-missing-source count */
    int targets;        /* export: enabled targets written */
    int files;          /* export: files committed by successful targets */
    int notices;        /* export: metadata-loss notices */
    int atlases_ok;     /* export: atlases exported OK */
    int atlases_fail;   /* export: atlases that failed */
    int atlases_skipped; /* export: atlases skipped for no usable input */
    bool partial_publication; /* cancelled export left committed outputs */
    bool publication_uncertain; /* failed writer may have published artifacts */
    char err[256];      /* failure / first-error text */
    char note[128];     /* pack: notice text */
} gui_pack_result_info;

/* Formats a typed Export cancellation. Returns true when the message reports
 * irrevocably published targets/files and should be presented as a warning. */
bool gui_pack_format_export_cancelled(const gui_pack_result_info *info,
                                      char *out, size_t cap);
/* Formats the uncertainty warning for a failed direct writer. Returns false
 * when the failure has no known publication risk. */
bool gui_pack_format_export_failed(const gui_pack_result_info *info,
                                   char *out, size_t cap);

/* Starts an async pack of `atlas_index`. false (fills err) if busy or the input can't assemble. */
bool gui_pack_async_start(int atlas_index, char *err, size_t err_cap);
/* Starts an async export of every exporting atlas. false (fills err) if busy / nothing to export /
 * relative out-paths need a saved project. */
bool gui_pack_export_async_start(char *err, size_t err_cap);
/* Consumes one completion only after host drain + atomic observation classified
 * its envelope. Applies a Pack slot swap only for an accepted result. */
gui_pack_done gui_pack_poll(gui_pack_result_info *out);
bool gui_pack_async_busy(void);
/* True for real queued/admitted/staged host work; excludes screenshot-only
 * synthetic busy presentation. */
bool gui_pack_worker_active(void);
gui_pack_async_kind gui_pack_async_active_kind(void);
double gui_pack_async_elapsed_sec(void);
void gui_pack_export_progress(int *cur, int *total); /* export "atlas cur/total" for the strip */
/* Enqueues typed cancellation for the host admission phase. Rejections remain
 * structured (duplicate, closed host, or no live job). */
tp_status gui_pack_async_cancel(tp_error *err);
bool gui_pack_async_cancelling(void);
/* DEV (--shot-packing): force the busy strip state without a real worker, for screenshots.
 * Dev seam: declared and defined only under NTPACKER_GUI_DEV_SEAMS. */
#ifdef NTPACKER_GUI_DEV_SEAMS
void gui_pack_debug_force_busy(gui_pack_async_kind kind);
#endif

/* Non-blocking: stops host ingress and releases presentation-owned results.
 * The frame/shutdown host pump owns cancellation and terminal drain. */
void gui_pack_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PACK_H */
