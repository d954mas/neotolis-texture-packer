#ifndef NTPACKER_GUI_PACK_H
#define NTPACKER_GUI_PACK_H

/* Thin GUI adapter over session-owned typed Pack jobs and Export commands.
 * It owns only presentation result slots; input assembly, algorithms, and worker
 * lifetime remain below the frontend boundary.
 *
 * Project-built pack results use a collision-free internal name derived from
 * canonical {source_id, source_key}. GUI selection/preview therefore uses the
 * canonical lookup below; human/export names remain presentation only.
 *
 * gui_pack_atlas/gui_pack_export are synchronous adapters used by selftest/shot; they drain the
 * same session-owned typed jobs as interactive use.
 *
 * RESULT RESIDENCY (docs/architecture/jobs-pack-and-cache.md). Native results are not
 * held one-per-atlas forever. They live in a session-lifetime
 * `tp_pack_result_cache` behind this adapter: the atlas the presentation reads is
 * the store's PINNED active result, and every other packed atlas is an INACTIVE
 * entry in a byte-budget LRU. Switching atlas demotes the outgoing result rather
 * than dropping it, so switching back is a store hit and never repacks; the store
 * pins each result through the session Pack receipt, and releases that receipt --
 * destroying the Pack arena exactly once -- when the LRU evicts the entry OR when
 * the entry's pages have been compressed into the store's own cold tier.
 *
 * That cold tier is the reason a demoted atlas can stay resident at all: the
 * store compresses its pages in the BACKGROUND (one low-priority thread, applied
 * on the per-frame pump inside gui_actions_step) and decompresses them in parallel
 * when the atlas is switched back to. The only thing a caller of the functions
 * below can observe about it is that switching to a long-untouched atlas costs a
 * decode (tens of milliseconds for a big atlas) instead of nothing.
 *
 * The store is behind this boundary on purpose: no view, and no caller of the
 * functions below, sees a cache, a budget, a compression, or a residency state.
 * What a caller must know is that an evicted result reads as "not packed" (NULL
 * result, version 0) exactly like an atlas that was never packed -- which is the
 * §10.4 cache-miss presentation: the preview is out of date and the user runs
 * Pack. Nothing here ever auto-packs. */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "tp_core/tp_pack_result.h" /* tp_result */
#include "tp_core/tp_id.h"
#include "tp_core/tp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stores the work-dir intent used when starting typed Pack/Export jobs and
 * creates it. Returns false instead of retaining a truncated or unusable
 * directory. The shipped GUI passes the shared app scratch root
 * (app_scratch_root(), apps/common); tests pass their own sandbox. Each job
 * gets its own `req-` directory UNDER this one, created by the job worker. */
bool gui_pack_init(const char *work_dir);

/* Packs stable `atlas_id` through a typed session job and stores the returned
 * result in the atlas presentation slot (the previous slot is destroyed first).
 *
 * On success returns true, writes the wall-clock pack time to *out_ms (nullable), and appends any
 * skipped-missing-file count to `notice` (nullable, cap notice_cap). On failure returns false and
 * fills `err` (nullable). An atlas with zero usable sprites is a failure (nothing to show). */
bool gui_pack_atlas(tp_id128 atlas_id, double *out_ms, char *err,
                    size_t err_cap, char *notice, size_t notice_cap);

/* The last successful result for `atlas_id`, or NULL if it was never packed
 * or its cached entry has been evicted. A failed Pack leaves it intact;
 * gui_project reports freshness separately -- restoring a demoted result is
 * freshness-neutral by construction, because nothing on the freshness path
 * (input tokens, pack_input_hash, the stale bit) is touched by residency.
 *
 * This read MUTATES residency: it makes `atlas_id` the resident atlas, which
 * demotes the previously resident one into the byte-budget LRU and may evict it.
 * Two atlases are therefore never resident at once, and the returned pointer is
 * valid only until the next residency change -- a gui_pack_result for a DIFFERENT
 * atlas (anyone's, not just this caller's), a published Pack, gui_pack_clear, or
 * shutdown. It is a within-operation borrow: never hold it across a frame, and
 * never hold two atlases' results at once. A caller that must read a SECOND
 * atlas' result without taking the pin away from the one on screen uses
 * gui_pack_result_peek below. */
const tp_result *gui_pack_result(tp_id128 atlas_id);
/* The same result, read WITHOUT changing residency: no reside, no demotion, no
 * LRU touch, no eviction, no decompression. This is the read for a consumer that
 * is not the one driving what the canvas shows (the animation preview player), so
 * two such consumers can never fight over the single active pin frame after
 * frame.
 *
 * NULL when the atlas was never packed or its entry was evicted -- the same
 * §10.4 cache-miss presentation as gui_pack_result, except that a miss here does
 * not retire the presentation slot (a read decides nothing about residency).
 * An atlas that is merely COLD (demoted long enough for the store to have
 * compressed it) is NOT a miss and must never read as one: it answers with its
 * result, so a consumer that latches "the result was released" on NULL cannot
 * fire on an atlas that is still perfectly available.
 *
 * GEOMETRY ONLY. What a peek promises is the sprite/page geometry, not the
 * pixels: for a cold atlas `pages[i].rgba` is NULL, because inflating the
 * compressed pages is a residency decision and this read is defined not to make
 * one. A consumer that needs page pixels (the canvas) uses gui_pack_result, which
 * resides the atlas and therefore decompresses it.
 *
 * The pointer carries the SAME lifetime rule as gui_pack_result's: it survives
 * only until the next residency change, so it is a within-operation borrow. */
const tp_result *gui_pack_result_peek(tp_id128 atlas_id);
/* Changes whenever a successful Pack publishes a new result into this atlas slot. */
uint64_t gui_pack_result_version(tp_id128 atlas_id);

/* Canonical lookup used by rows and animation frames. Project-built pack inputs
 * use a collision-free internal name derived from {source_id, source_key}; display
 * names are never authoritative here. */
int gui_pack_find_sprite_ref(tp_id128 atlas_id, tp_id128 source_id,
                             const char *source_key);
bool gui_pack_sprite_matches_ref(tp_id128 atlas_id, int sprite_index,
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

/* Exports every ENABLED target of atlas `atlas_id` through a typed session job. Returns true on success
 * and writes the enabled-target count to *out_targets and the metadata-loss notice count to
 * *out_notices (both nullable); a joined notice summary goes to `notice`. On failure returns false and
 * fills `err` (e.g. unsaved project with relative output paths). */
bool gui_pack_export(tp_id128 atlas_id, int *out_targets, int *out_notices, char *err, size_t err_cap, char *notice,
                     size_t notice_cap);

/* Drops the stored result for one atlas (or the whole store with a nil ID) and
 * releases its retained session-job result owner. Call on project new/open. */
void gui_pack_clear(tp_id128 atlas_id);

/* --- export-target preview (packet EXP-PREVIEW) --------------------------------------------------
 * A view-only "what would exporter <id> produce from the CURRENT settings" pack, kept in ONE arena-
 * owned preview slot SEPARATE from the session slots (the native result is never clobbered). The
 * effective settings are tp_project_atlas_to_settings clamped through the exporter's caps
 * (tp_export_effective_settings). Only one preview is live at a time (dropped on atlas switch / edit),
 * so one slot carrying the stable atlas ID guarantees coherent binding. */

/* Synchronous preview adapter for selftest/shot-preview; drains the same typed
 * session Pack job and lands its result in the preview slot. */
bool gui_pack_preview_blocking(tp_id128 atlas_id, const char *exporter_id,
                               char *err, size_t err_cap);

/* Async preview pack (interactive): uses the session-owned Pack handle; result lands in the preview
 * slot at a step boundary (gui_actions_step -> GUI_PACK_DONE_PREVIEW_*). false (fills err) if busy. */
bool gui_pack_preview_async_start(tp_id128 atlas_id, const char *exporter_id,
                                  char *err, size_t err_cap);

/* The stored preview result IF it belongs to `atlas_id`, else NULL (coherent binding: a stale slot
 * from another atlas never shows). */
const tp_result *gui_pack_preview_result(tp_id128 atlas_id);
/* Changes whenever a successful export preview publishes a new result. */
uint64_t gui_pack_preview_result_version(tp_id128 atlas_id);

/* Drops the preview slot and releases its retained session-job result owner.
 * Call on back-to-Native / atlas switch / model edit. */
void gui_pack_preview_clear(void);

/* Degradation summary for `exporter_id` on `atlas_id`: diffs the native session settings
 * against the caps-clamped effective settings, plus the caps-vs-usage metadata drops (slice9/pivot).
 * Writes a SHORT chip caption to `chip` (empty when the format expresses everything) and a longer
 * field-by-field breakdown to `tip` (nullable). Returns the number of degradations found. */
int gui_pack_preview_diff(tp_id128 atlas_id, const char *exporter_id, char *chip, size_t chip_cap, char *tip,
                          size_t tip_cap);

/* --- async packing (interactive; owned worker process) -----------------------------------------
 * One in-flight op MAX (pack OR export). The session owns the concrete worker handle and immutable
 * input; this frontend only captures intent, polls typed progress, and maps the typed result at a
 * frame boundary. The synchronous adapters above reuse and drain this exact path. */
typedef enum {
    GUI_PACK_ASYNC_NONE = 0,
    GUI_PACK_ASYNC_PACK,
    GUI_PACK_ASYNC_EXPORT,
    GUI_PACK_ASYNC_REFRESH
} gui_pack_async_kind;

typedef enum {
    GUI_PACK_DONE_NONE = 0, /* nothing landed this frame */
    GUI_PACK_DONE_PACK_OK,
    GUI_PACK_DONE_PACK_FAIL,
    GUI_PACK_DONE_PACK_CANCELLED,
    GUI_PACK_DONE_EXPORT_OK,
    GUI_PACK_DONE_EXPORT_FAIL,
    GUI_PACK_DONE_EXPORT_CANCELLED,
    GUI_PACK_DONE_REFRESH_OK,
    GUI_PACK_DONE_REFRESH_FAIL,
    GUI_PACK_DONE_REFRESH_CANCELLED,
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
    int added;
    int removed;
    int changed;
    int unavailable;
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

/* Starts an async pack of `atlas_id`. false (fills err) if busy or the input can't assemble. */
bool gui_pack_async_start(tp_id128 atlas_id, char *err, size_t err_cap);
/* Starts an async export of every exporting atlas. false (fills err) if busy / nothing to export /
 * relative out-paths need a saved project. */
bool gui_pack_export_async_start(char *err, size_t err_cap);
bool gui_refresh_async_start(char *err, size_t err_cap);
/* Consumes one completion only after host drain + atomic observation classified
 * its envelope. Applies a Pack slot swap only for an accepted result. */
/* Classifies and consumes one owned result transferred by gui_project_step.
 * Always destroys/zeros `completion`; NONE still advances the cold-result
 * store's once-per-step maintenance. */
gui_pack_done gui_pack_consume_completion(
    tp_session_job_result *completion,
    gui_pack_result_info *out);
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

#if defined(TP_ENABLE_TEST_SEAMS) && \
    !defined(NTPACKER_GUI_PACK_IMPLEMENTATION)
/* Tests may address fixture atlases by frame-local index. Production entry
 * points remain stable-ID-only; these adapters resolve immediately and never
 * store the index. */
bool gui_pack_atlas_at_index(int, double *, char *, size_t,
                             char *, size_t);
const tp_result *gui_pack_result_at_index(int);
const tp_result *gui_pack_result_peek_at_index(int);
uint64_t gui_pack_result_version_at_index(int);
int gui_pack_find_sprite_ref_at_index(
    int, tp_id128, const char *);
bool gui_pack_sprite_matches_ref_at_index(
    int, int, tp_id128, const char *);
bool gui_pack_export_at_index(
    int, int *, int *, char *, size_t, char *, size_t);
void gui_pack_clear_at_index(int);
bool gui_pack_preview_blocking_at_index(
    int, const char *, char *, size_t);
bool gui_pack_preview_async_start_at_index(
    int, const char *, char *, size_t);
const tp_result *gui_pack_preview_result_at_index(int);
uint64_t gui_pack_preview_result_version_at_index(int);
int gui_pack_preview_diff_at_index(
    int, const char *, char *, size_t, char *, size_t);
bool gui_pack_async_start_at_index(int, char *, size_t);

#define gui_pack_atlas(owner, ...) \
    _Generic((owner), tp_id128: gui_pack_atlas, \
             default: gui_pack_atlas_at_index)(owner, __VA_ARGS__)
#define gui_pack_result(owner) \
    _Generic((owner), tp_id128: gui_pack_result, \
             default: gui_pack_result_at_index)(owner)
#define gui_pack_result_peek(owner) \
    _Generic((owner), tp_id128: gui_pack_result_peek, \
             default: gui_pack_result_peek_at_index)(owner)
#define gui_pack_result_version(owner) \
    _Generic((owner), tp_id128: gui_pack_result_version, \
             default: gui_pack_result_version_at_index)(owner)
#define gui_pack_find_sprite_ref(owner, ...) \
    _Generic((owner), tp_id128: gui_pack_find_sprite_ref, \
             default: gui_pack_find_sprite_ref_at_index)(owner, __VA_ARGS__)
#define gui_pack_sprite_matches_ref(owner, ...) \
    _Generic((owner), tp_id128: gui_pack_sprite_matches_ref, \
             default: gui_pack_sprite_matches_ref_at_index)(owner, __VA_ARGS__)
#define gui_pack_export(owner, ...) \
    _Generic((owner), tp_id128: gui_pack_export, \
             default: gui_pack_export_at_index)(owner, __VA_ARGS__)
#define gui_pack_clear(owner) \
    _Generic((owner), tp_id128: gui_pack_clear, \
             default: gui_pack_clear_at_index)(owner)
#define gui_pack_preview_blocking(owner, ...) \
    _Generic((owner), tp_id128: gui_pack_preview_blocking, \
             default: gui_pack_preview_blocking_at_index)(owner, __VA_ARGS__)
#define gui_pack_preview_async_start(owner, ...) \
    _Generic((owner), tp_id128: gui_pack_preview_async_start, \
             default: gui_pack_preview_async_start_at_index)(owner, __VA_ARGS__)
#define gui_pack_preview_result(owner) \
    _Generic((owner), tp_id128: gui_pack_preview_result, \
             default: gui_pack_preview_result_at_index)(owner)
#define gui_pack_preview_result_version(owner) \
    _Generic((owner), tp_id128: gui_pack_preview_result_version, \
             default: gui_pack_preview_result_version_at_index)(owner)
#define gui_pack_preview_diff(owner, ...) \
    _Generic((owner), tp_id128: gui_pack_preview_diff, \
             default: gui_pack_preview_diff_at_index)(owner, __VA_ARGS__)
#define gui_pack_async_start(owner, ...) \
    _Generic((owner), tp_id128: gui_pack_async_start, \
             default: gui_pack_async_start_at_index)(owner, __VA_ARGS__)
#endif

/* Non-blocking: stops host ingress and releases presentation-owned results.
 * The frame/shutdown host pump owns cancellation and terminal drain. */
void gui_pack_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PACK_H */
