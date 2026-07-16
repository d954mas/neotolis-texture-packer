#include "gui_pack.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#endif

#include "log/nt_log.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_names.h"

#include "gui_project.h"

// #region state
#define GUI_PACK_MAX_ATLASES 64

typedef struct pack_ref_entry {
    uint64_t hash;
    const char *name; /* borrowed from the slot's arena-owned tp_result */
    int result_index;
    bool occupied;
} pack_ref_entry;

typedef struct {
    tp_arena *arena;      /* owns `result` (destroy to free); NULL when empty */
    tp_result *result;
    pack_ref_entry *ref_index; /* derived immutable canonical-name -> result index */
    size_t ref_index_cap;
    tp_id128 atlas_id;
    bool valid;
} pack_slot;

static pack_slot s_slots[GUI_PACK_MAX_ATLASES];
static char s_work_dir[1024];

/* Export-target preview (EXP-PREVIEW): ONE arena-owned result, separate from the session slots. Keyed
 * by stable atlas id so a structural edit cannot bind it to a different atlas.
 * Only one preview is live at a time (the GUI drops it on atlas switch / edit), so a single slot is
 * sufficient and its lifetime mirrors a session slot: destroy the arena to free the result. */
typedef struct {
    tp_arena *arena;
    tp_result *result;
    tp_id128 atlas_id;
    tp_session_input_token input_token;
    bool valid;
    int atlas_index; /* atlas this preview belongs to (-1 = none) */
    char exporter_id[64];
} preview_slot;
static preview_slot s_preview = {.atlas_index = -1};

#ifdef NTPACKER_GUI_SELFTEST
static gui_pack_ref_index_work s_ref_index_work;
void gui_pack_ref_index_work_reset(void) {
    memset(&s_ref_index_work, 0, sizeof s_ref_index_work);
}
gui_pack_ref_index_work gui_pack_ref_index_work_get(void) {
    return s_ref_index_work;
}
#endif
// #endregion

// #region small helpers
static uint64_t pack_ref_hash(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        hash ^= (uint64_t)*p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void pack_ref_index_free(pack_slot *slot) {
    free(slot->ref_index);
    slot->ref_index = NULL;
    slot->ref_index_cap = 0U;
}

static bool pack_ref_index_build(const tp_result *result,
                                 pack_ref_entry **out_entries,
                                 size_t *out_capacity) {
    *out_entries = NULL;
    *out_capacity = 0U;
    if (!result || result->sprite_count <= 0) {
        return true;
    }
    const size_t count = (size_t)result->sprite_count;
    if (count > SIZE_MAX / 2U) {
        return false;
    }
    const size_t wanted = count * 2U;
    size_t capacity = 8U;
    while (capacity < wanted) {
        if (capacity > SIZE_MAX / 2U) {
            return false;
        }
        capacity *= 2U;
    }
    pack_ref_entry *entries = calloc(capacity, sizeof *entries);
    if (!entries) {
        return false;
    }
    const size_t mask = capacity - 1U;
    for (int i = 0; i < result->sprite_count; ++i) {
        const char *name = result->sprites[i].name;
        if (!name) {
            free(entries);
            return false;
        }
        const uint64_t hash = pack_ref_hash(name);
        size_t at = (size_t)hash & mask;
        while (entries[at].occupied) {
#ifdef NTPACKER_GUI_SELFTEST
            s_ref_index_work.build_probes++;
#endif
            at = (at + 1U) & mask;
        }
#ifdef NTPACKER_GUI_SELFTEST
        s_ref_index_work.build_items++;
        s_ref_index_work.build_probes++;
#endif
        entries[at] = (pack_ref_entry){hash, name, i, true};
    }
    *out_entries = entries;
    *out_capacity = capacity;
    return true;
}

static void pack_slot_clear(pack_slot *slot) {
    pack_ref_index_free(slot);
    if (slot->arena) {
        tp_arena_destroy(slot->arena);
    }
    memset(slot, 0, sizeof *slot);
}

static int current_atlas_index(tp_id128 atlas_id) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int i = 0; i < count; i++) {
        const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, i);
        if (atlas && tp_id128_eq(atlas->id, atlas_id)) {
            return i;
        }
    }
    return -1;
}

// #endregion

// #region public
void gui_pack_init(const char *work_dir) {
    (void)snprintf(s_work_dir, sizeof s_work_dir, "%s", work_dir ? work_dir : ".");
#ifdef _WIN32
    (void)CreateDirectoryA(s_work_dir, NULL); /* ok if it already exists */
#else
    (void)mkdir(s_work_dir, 0755);
#endif
}

void gui_pack_clear(int atlas_index) {
    for (int i = 0; i < GUI_PACK_MAX_ATLASES; i++) {
        if (atlas_index >= 0 && i != atlas_index) {
            continue;
        }
        pack_slot_clear(&s_slots[i]);
    }
    /* The preview is a derived view of the session pack -- clearing the session slot(s) it mirrors
     * invalidates it too (open/new/undo/redo/repack all pass -1 or the preview's atlas). */
    if (atlas_index < 0 || atlas_index == s_preview.atlas_index) {
        gui_pack_preview_clear();
    }
}

// #region async adapter (the session owns the one typed Pack / Export handle)
static gui_pack_async_kind s_debug_busy; /* --shot-packing override */
static bool s_cancel_requested;          /* presentation-only: show Canceling... */

static tp_session *job_session(void) { return gui_project_session_for_jobs(); }

static bool job_active(void) {
    tp_session *session = job_session();
    return session && tp_session_job_active(session);
}

static bool report_job_start(tp_status status, const tp_error *error,
                             char *err, size_t err_cap) {
    if (status == TP_STATUS_OK) {
        s_cancel_requested = false;
        return true;
    }
    if (err && err_cap > 0U) {
        (void)snprintf(err, err_cap, "%s",
                       error && error->msg[0] ? error->msg
                                              : tp_status_str(status));
    }
    return false;
}

static bool input_changed_since(tp_session_input_token token) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return !snapshot || !tp_session_input_token_equal(
                            tp_session_snapshot_input_token(snapshot), token);
}

bool gui_pack_async_start(int atlas_index, char *err, size_t err_cap) {
    if (job_active()) {
        if (err) {
            (void)snprintf(err, err_cap, "busy -- a pack or export is already running");
        }
        return false;
    }
    if (atlas_index < 0 || atlas_index >= GUI_PACK_MAX_ATLASES) {
        if (err) {
            (void)snprintf(err, err_cap, "atlas index out of range");
        }
        return false;
    }
    gui_project_invalidate_sources();
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot
                                     ? tp_session_snapshot_atlas_at(snapshot, atlas_index)
                                     : NULL;
    if (!a) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }

    tp_error e = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = a->id,
        .work_dir = s_work_dir,
        .preview_exporter_id = NULL,
    };
    return report_job_start(
        tp_session_pack_job_start(job_session(), &request, &e), &e, err,
        err_cap);
}

static bool export_start(tp_id128 atlas_id, char *err, size_t err_cap) {
    if (job_active()) {
        if (err) {
            (void)snprintf(err, err_cap, "busy -- a pack or export is already running");
        }
        return false;
    }
    gui_project_invalidate_sources();
    tp_session *session = job_session();
    if (!session) {
        if (err) {
            (void)snprintf(err, err_cap, "no project");
        }
        return false;
    }

    tp_error e = {{0}};
    const tp_export_command_request request = {
        .work_dir = s_work_dir,
        .atlas_id = atlas_id,
    };
    return report_job_start(tp_session_export_start(session, &request, &e),
                            &e, err, err_cap);
}

bool gui_pack_export_async_start(char *err, size_t err_cap) {
    return export_start(tp_id128_nil(), err, err_cap);
}

gui_pack_done gui_pack_poll(gui_pack_result_info *out) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    tp_session *session = job_session();
    if (!session || !tp_session_job_active(session)) {
        return GUI_PACK_DONE_NONE;
    }
    tp_error error = {{0}};
    tp_session_job_progress progress;
    if (tp_session_job_poll(session, &progress, &error) != TP_STATUS_OK ||
        progress.state == TP_SESSION_JOB_RUNNING) {
        return GUI_PACK_DONE_NONE;
    }
    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    if (tp_session_job_take_result(session, &result, &error) != TP_STATUS_OK) {
        if (out) {
            (void)snprintf(out->err, sizeof out->err, "%s",
                           error.msg[0] ? error.msg : "could not take job result");
        }
        return progress.kind == TP_SESSION_JOB_EXPORT
                   ? GUI_PACK_DONE_EXPORT_FAIL
                   : GUI_PACK_DONE_PACK_FAIL;
    }
    const bool cancelled = s_cancel_requested ||
                           result.state == TP_SESSION_JOB_CANCELLED;
    const bool preview = result.kind == TP_SESSION_JOB_PACK &&
                         result.pack.preview_exporter_id[0] != '\0';
    gui_pack_done rc = GUI_PACK_DONE_NONE;

    if (result.kind == TP_SESSION_JOB_PACK) {
        if (result.state == TP_SESSION_JOB_SUCCEEDED &&
            result.status == TP_STATUS_OK && result.pack.result) {
            const int atlas_index = current_atlas_index(result.pack.atlas_id);
            if (cancelled || atlas_index < 0) {
                rc = preview ? GUI_PACK_DONE_PREVIEW_CANCELLED
                             : GUI_PACK_DONE_PACK_CANCELLED;
            } else if (preview) {
                const bool input_changed = input_changed_since(
                    result.pack.input_token_at_start);
                if (out) {
                    out->atlas_index = atlas_index;
                    out->ms = result.elapsed_ms;
                    out->input_changed = input_changed;
                }
                if (input_changed) {
                    rc = GUI_PACK_DONE_PREVIEW_OK;
                    goto pack_result_handled;
                }
                if (s_preview.arena) {
                    tp_arena_destroy(s_preview.arena);
                }
                s_preview.arena = result.pack.arena;
                s_preview.result = result.pack.result;
                s_preview.atlas_id = result.pack.atlas_id;
                s_preview.input_token = result.pack.input_token_at_start;
                s_preview.valid = true;
                s_preview.atlas_index = atlas_index;
                (void)snprintf(s_preview.exporter_id,
                               sizeof s_preview.exporter_id, "%s",
                               result.pack.preview_exporter_id);
                result.pack.arena = NULL;
                nt_log_info("gui_pack(async): preview '%s' via %s packed %d sprite(s), %d page(s) in %.1f ms",
                            s_preview.result->atlas_name,
                            s_preview.exporter_id,
                            s_preview.result->sprite_count,
                            s_preview.result->page_count, result.elapsed_ms);
                rc = GUI_PACK_DONE_PREVIEW_OK;
            } else {
                pack_ref_entry *ref_index = NULL;
                size_t ref_index_cap = 0U;
                if (!pack_ref_index_build(result.pack.result, &ref_index,
                                          &ref_index_cap)) {
                    if (out) {
                        (void)snprintf(out->err, sizeof out->err,
                                       "out of memory building canonical sprite lookup");
                    }
                    rc = GUI_PACK_DONE_PACK_FAIL;
                    goto pack_result_handled;
                }
                pack_slot_clear(&s_slots[atlas_index]);
                s_slots[atlas_index].arena = result.pack.arena;
                s_slots[atlas_index].result = result.pack.result;
                s_slots[atlas_index].ref_index = ref_index;
                s_slots[atlas_index].ref_index_cap = ref_index_cap;
                s_slots[atlas_index].atlas_id = result.pack.atlas_id;
                s_slots[atlas_index].valid = true;
                result.pack.arena = NULL;
                if (out) {
                    out->atlas_index = atlas_index;
                    out->ms = result.elapsed_ms;
                    out->missing = result.pack.missing_sources;
                    out->input_changed = input_changed_since(
                        result.pack.input_token_at_start);
                    if (result.pack.missing_sources > 0) {
                        (void)snprintf(out->note, sizeof out->note,
                                       "%d missing file(s) skipped",
                                       result.pack.missing_sources);
                    }
                }
                nt_log_info("gui_pack(async): atlas '%s' packed %d sprite(s), %d page(s) in %.1f ms",
                            s_slots[atlas_index].result->atlas_name,
                            s_slots[atlas_index].result->sprite_count,
                            s_slots[atlas_index].result->page_count,
                            result.elapsed_ms);
                rc = GUI_PACK_DONE_PACK_OK;
            }
pack_result_handled:;
        } else {
            if (cancelled) {
                rc = preview ? GUI_PACK_DONE_PREVIEW_CANCELLED
                             : GUI_PACK_DONE_PACK_CANCELLED;
            } else {
                if (out) {
                    (void)snprintf(out->err, sizeof out->err, "%s",
                                   result.error.msg[0]
                                       ? result.error.msg
                                       : tp_status_str(result.status));
                }
                rc = preview ? GUI_PACK_DONE_PREVIEW_FAIL
                             : GUI_PACK_DONE_PACK_FAIL;
            }
        }
    } else {
        if (out) {
            out->targets = result.export_result.targets;
            out->notices = result.export_result.notices;
            out->atlases_ok = result.export_result.atlases_ok;
            out->atlases_fail = result.export_result.atlases_failed;
            (void)snprintf(out->err, sizeof out->err, "%s",
                           result.export_result.first_error);
        }
        if (cancelled) {
            rc = GUI_PACK_DONE_EXPORT_CANCELLED;
        } else if (result.state == TP_SESSION_JOB_FAILED ||
                   result.export_result.atlases_failed > 0) {
            rc = GUI_PACK_DONE_EXPORT_FAIL;
        } else {
            rc = GUI_PACK_DONE_EXPORT_OK;
        }
        nt_log_info("gui_pack(async): export %d target(s), %d ok, %d fail, %d notice(s) in %.1f ms%s%s",
                    result.export_result.targets,
                    result.export_result.atlases_ok,
                    result.export_result.atlases_failed,
                    result.export_result.notices, result.elapsed_ms,
                    result.export_result.first_error[0] ? ": " : "",
                    result.export_result.first_error);
    }

    tp_session_job_result_destroy(&result);
    s_cancel_requested = false;
    if (out) {
        out->kind = rc;
    }
    return rc;
}

/* Deterministic selftest/shot adapter: synchronously drain the same session-owned
 * typed job used by the interactive UI. This is orchestration only; it does not
 * assemble Pack input or invoke Pack/Export algorithms in the frontend. */
static gui_pack_done wait_for_job(gui_pack_result_info *out) {
    for (;;) {
        const gui_pack_done done = gui_pack_poll(out);
        if (done != GUI_PACK_DONE_NONE) {
            return done;
        }
        if (!job_active()) {
            if (out) {
                (void)snprintf(out->err, sizeof out->err,
                               "session job ended without a typed result");
            }
            return GUI_PACK_DONE_PACK_FAIL;
        }
#ifdef _WIN32
        Sleep(1);
#else
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&pause, NULL);
#endif
    }
}

bool gui_pack_async_busy(void) { return job_active() || s_debug_busy != GUI_PACK_ASYNC_NONE; }

bool gui_pack_worker_active(void) { return job_active(); } /* excludes --shot debug busy */

gui_pack_async_kind gui_pack_async_active_kind(void) {
    if (s_debug_busy != GUI_PACK_ASYNC_NONE) {
        return s_debug_busy;
    }
    tp_session *session = job_session();
    if (!session || !tp_session_job_active(session)) {
        return GUI_PACK_ASYNC_NONE;
    }
    tp_session_job_progress progress;
    return tp_session_job_poll(session, &progress, NULL) == TP_STATUS_OK &&
                   progress.kind == TP_SESSION_JOB_EXPORT
               ? GUI_PACK_ASYNC_EXPORT
               : GUI_PACK_ASYNC_PACK;
}

double gui_pack_async_elapsed_sec(void) {
    if (s_debug_busy != GUI_PACK_ASYNC_NONE) {
        return 3.2; /* fixed for reproducible --shot-packing captures */
    }
    tp_session *session = job_session();
    tp_session_job_progress progress;
    return session && tp_session_job_poll(session, &progress, NULL) == TP_STATUS_OK
               ? progress.elapsed_ms / 1000.0
               : 0.0;
}

void gui_pack_export_progress(int *cur, int *total) {
    if (s_debug_busy == GUI_PACK_ASYNC_EXPORT) {
        if (cur) {
            *cur = 2;
        }
        if (total) {
            *total = 3;
        }
        return;
    }
    tp_session_job_progress progress;
    tp_session *session = job_session();
    const bool have = session &&
                      tp_session_job_poll(session, &progress, NULL) == TP_STATUS_OK;
    if (cur) {
        *cur = have ? progress.current : 0;
    }
    if (total) {
        *total = have ? progress.total : 0;
    }
}

void gui_pack_async_cancel(void) {
    tp_session *session = job_session();
    if (session && tp_session_job_cancel(session, NULL) == TP_STATUS_OK) {
        s_cancel_requested = true;
    }
}

bool gui_pack_async_cancelling(void) { return job_active() && s_cancel_requested; }

void gui_pack_debug_force_busy(gui_pack_async_kind kind) { s_debug_busy = kind; }
// #endregion

// #region export-target preview (EXP-PREVIEW)
/* Synchronous preview for deterministic selftests/shots. It drains the same
 * session-owned typed Pack job as the interactive adapter.
 */
bool gui_pack_preview_blocking(int atlas_index, const char *exporter_id, char *err, size_t err_cap) {
    if (!gui_pack_preview_async_start(atlas_index, exporter_id, err, err_cap)) {
        return false;
    }
    gui_pack_result_info info;
    const gui_pack_done done = wait_for_job(&info);
    if (done == GUI_PACK_DONE_PREVIEW_OK && info.atlas_index == atlas_index &&
        !info.input_changed) {
        return true;
    }
    if (err && err_cap > 0U) {
        (void)snprintf(err, err_cap, "%s",
                       info.err[0] ? info.err : "preview pack did not complete");
    }
    return false;
}

bool gui_pack_preview_async_start(int atlas_index, const char *exporter_id, char *err, size_t err_cap) {
    if (job_active()) {
        if (err) {
            (void)snprintf(err, err_cap, "busy -- a pack or export is already running");
        }
        return false;
    }
    gui_project_invalidate_sources();
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    if (!atlas) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }
    tp_error error = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas->id,
        .work_dir = s_work_dir,
        .preview_exporter_id = exporter_id,
    };
    return report_job_start(
        tp_session_pack_job_start(job_session(), &request, &error), &error,
        err, err_cap);
}

const tp_result *gui_pack_preview_result(int atlas_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!s_preview.valid || !atlas || !tp_id128_eq(s_preview.atlas_id, atlas->id) ||
        !tp_session_input_token_equal(tp_session_snapshot_input_token(snapshot),
                                      s_preview.input_token)) {
        return NULL;
    }
    return s_preview.result;
}

void gui_pack_preview_clear(void) {
    if (s_preview.arena) {
        tp_arena_destroy(s_preview.arena);
    }
    s_preview.arena = NULL;
    s_preview.result = NULL;
    s_preview.atlas_id = tp_id128_nil();
    s_preview.input_token = (tp_session_input_token){0};
    s_preview.valid = false;
    s_preview.atlas_index = -1;
    s_preview.exporter_id[0] = '\0';
}

/* Maps one predicted-loss axis (core field_id) to the chip's short token + the
 * tooltip's long line. These strings are GUI presentation, byte-pinned by selftest
 * phase 11; the core predict pass supplies only the structured field_id. Returns
 * false for an axis the chip does not surface (alias/multipage never reach it --
 * the chip passes a NULL prep, so predict never emits those). */
static bool preview_field_phrases(int field_id, const char **short_tok, const char **long_line) {
    switch (field_id) {
        case TP_NOTICE_FIELD_TRANSFORM:
            *short_tok = "no rotate/flip";
            *long_line = "Rotations/flips off -- this format can't encode the full D4 orientation set";
            return true;
        case TP_NOTICE_FIELD_POLYGON:
            *short_tok = "polygons \xE2\x86\x92 rect";
            *long_line = "Polygon hulls flattened to rectangles -- this format stores quads only";
            return true;
        case TP_NOTICE_FIELD_SLICE9:
            *short_tok = "slice9 dropped";
            *long_line = "9-slice borders dropped -- this format does not store them";
            return true;
        case TP_NOTICE_FIELD_PIVOT:
            *short_tok = "pivot dropped";
            *long_line = "Per-sprite pivots dropped -- this format does not store them";
            return true;
        default:
            return false;
    }
}

int gui_pack_preview_diff(int atlas_index, const char *exporter_id, char *chip, size_t chip_cap, char *tip,
                          size_t tip_cap) {
    if (chip && chip_cap) {
        chip[0] = '\0';
    }
    if (tip && tip_cap) {
        tip[0] = '\0';
    }
    const tp_exporter *e = tp_exporter_find(exporter_id);
    if (!e) {
        return 0;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, atlas_index)
                                         : NULL;
    if (!atlas) {
        return 0;
    }
    /* One core enumeration for both frontends (review §3.1); NULL prep = project-only
     * preview (no alias/multipage axes -- those need the packed result). */
    tp_export_notices nz;
    tp_export_notices_init(&nz);
    tp_error te = {{0}};
    if (tp_export_predict_loss_snapshot(snapshot, atlas->id, &e->caps, exporter_id,
                                        NULL, &nz, &te) != TP_STATUS_OK) {
        tp_export_notices_free(&nz);
        return 0;
    }

    int n = 0;
    /* Rebuild the exact chip/tooltip strings from the structured notices. Guarded so the
     * write pointer never forms past-end and a truncating snprintf can't advance out of range. */
    size_t clen = 0;
    size_t tlen = 0;
    for (int i = 0; i < nz.count; i++) {
        const char *short_tok = NULL;
        const char *long_line = NULL;
        if (!preview_field_phrases(nz.items[i].field_id, &short_tok, &long_line)) {
            continue;
        }
        if (chip && clen < chip_cap) {
            const int w_ = snprintf(chip + clen, chip_cap - clen, "%s%s", n > 0 ? ", " : "", short_tok);
            if (w_ > 0) {
                clen += (size_t)w_;
            }
        }
        if (tip && tlen < tip_cap) {
            const int w2_ = snprintf(tip + tlen, tip_cap - tlen, "%s%s", n > 0 ? "\n" : "", long_line);
            if (w2_ > 0) {
                tlen += (size_t)w2_;
            }
        }
        n++;
    }
    tp_export_notices_free(&nz);
    return n;
}
// #endregion

void gui_pack_shutdown(void) {
    if (job_active()) {
        /* The session owns and joins the concrete handle. Polling here only drains
         * its typed result so shutdown also works in the headless selftest path. */
        gui_pack_async_cancel();
        while (job_active()) {
            if (gui_pack_poll(NULL) != GUI_PACK_DONE_NONE) {
                break;
            }
#ifdef _WIN32
            Sleep(1);
#else
            const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
            (void)nanosleep(&pause, NULL);
#endif
        }
    }
    gui_pack_clear(-1);
    gui_pack_preview_clear();
}

const tp_result *gui_pack_result(int atlas_index) {
    if (atlas_index < 0 || atlas_index >= GUI_PACK_MAX_ATLASES || !s_slots[atlas_index].valid) {
        return NULL;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!atlas || !tp_id128_eq(s_slots[atlas_index].atlas_id, atlas->id)) {
        return NULL;
    }
    return s_slots[atlas_index].result;
}

bool gui_pack_sprite_matches_ref(int atlas_index, int sprite_index,
                                 tp_id128 source_id,
                                 const char *source_key) {
    const tp_result *result = gui_pack_result(atlas_index);
    if (!result || sprite_index < 0 || sprite_index >= result->sprite_count ||
        tp_id128_is_nil(source_id) || !source_key) {
        return false;
    }
    char canonical_name[TP_PACK_INTERNAL_NAME_CAP];
    if (tp_pack_input_format_sprite_name(
            source_id, source_key, canonical_name, sizeof canonical_name,
            NULL) != TP_STATUS_OK) {
        return false;
    }
    const char *packed_name = result->sprites[sprite_index].name;
    return packed_name && strcmp(packed_name, canonical_name) == 0;
}

int gui_pack_find_sprite_ref(int atlas_index, tp_id128 source_id,
                             const char *source_key) {
    if (!gui_pack_result(atlas_index) || atlas_index < 0 ||
        atlas_index >= GUI_PACK_MAX_ATLASES) {
        return -1;
    }
    char canonical_name[TP_PACK_INTERNAL_NAME_CAP];
    if (tp_pack_input_format_sprite_name(
            source_id, source_key, canonical_name, sizeof canonical_name,
            NULL) != TP_STATUS_OK) {
        return -1;
    }
    const pack_slot *slot = &s_slots[atlas_index];
#ifdef NTPACKER_GUI_SELFTEST
    s_ref_index_work.lookup_calls++;
#endif
    if (!slot->ref_index || slot->ref_index_cap == 0U) {
        return -1;
    }
    const uint64_t hash = pack_ref_hash(canonical_name);
    const size_t mask = slot->ref_index_cap - 1U;
    size_t at = (size_t)hash & mask;
    for (size_t probe = 0U; probe < slot->ref_index_cap; ++probe) {
        const pack_ref_entry *entry = &slot->ref_index[at];
#ifdef NTPACKER_GUI_SELFTEST
        s_ref_index_work.lookup_probes++;
#endif
        if (!entry->occupied) {
            return -1;
        }
        if (entry->hash == hash && strcmp(entry->name, canonical_name) == 0) {
            return entry->result_index;
        }
        at = (at + 1U) & mask;
    }
    return -1;
}

bool gui_pack_atlas(int atlas_index, double *out_ms, char *err, size_t err_cap, char *notice, size_t notice_cap) {
    if (!gui_pack_async_start(atlas_index, err, err_cap)) {
        return false;
    }
    gui_pack_result_info info;
    const gui_pack_done done = wait_for_job(&info);
    if (done != GUI_PACK_DONE_PACK_OK || info.atlas_index != atlas_index) {
        if (err && err_cap > 0U) {
            (void)snprintf(err, err_cap, "%s",
                           info.err[0] ? info.err : "pack did not complete");
        }
        return false;
    }
    if (out_ms) {
        *out_ms = info.ms;
    }
    if (notice && notice_cap > 0U) {
        (void)snprintf(notice, notice_cap, "%s", info.note);
    }
    return true;
}

bool gui_pack_export(int atlas_index, int *out_targets, int *out_notices, char *err, size_t err_cap, char *notice,
                     size_t notice_cap) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    if (!atlas) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }
    const tp_id128 atlas_id = atlas->id;
    if (!export_start(atlas_id, err, err_cap)) {
        return false;
    }
    gui_pack_result_info info;
    const gui_pack_done done = wait_for_job(&info);
    if (out_targets) {
        *out_targets = info.targets;
    }
    if (out_notices) {
        *out_notices = info.notices;
    }
    if (notice && notice_cap > 0U) {
        if (info.notices > 0) {
            (void)snprintf(notice, notice_cap, "%d metadata notice(s)",
                           info.notices);
        } else {
            notice[0] = '\0';
        }
    }
    if (done != GUI_PACK_DONE_EXPORT_OK) {
        if (err && err_cap > 0U) {
            (void)snprintf(err, err_cap, "%s",
                           info.err[0] ? info.err : "export did not complete");
        }
        return false;
    }
    return true;
}
// #endregion
