#include "gui_pack.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#endif

#include "tinycthread.h" /* C11 threads (vendored via nt_builder); interactive worker thread */

#include "log/nt_log.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"

#include "gui_project.h"
#include "gui_scan.h"

// #region state
#define GUI_PACK_MAX_ATLASES 64

typedef struct {
    tp_arena *arena;      /* owns `result` (destroy to free); NULL when empty */
    tp_result *result;
    bool valid;
} pack_slot;

static pack_slot s_slots[GUI_PACK_MAX_ATLASES];
static char s_work_dir[1024];

/* Export-target preview (EXP-PREVIEW): ONE arena-owned result, separate from the session slots. Keyed
 * by atlas_index so a stale preview from another atlas never binds (gui_pack_preview_result checks it).
 * Only one preview is live at a time (the GUI drops it on atlas switch / edit), so a single slot is
 * sufficient and its lifetime mirrors a session slot: destroy the arena to free the result. */
typedef struct {
    tp_arena *arena;
    tp_result *result;
    bool valid;
    int atlas_index; /* atlas this preview belongs to (-1 = none) */
    char exporter_id[64];
} preview_slot;
static preview_slot s_preview = {.atlas_index = -1};
// #endregion

// #region small helpers
static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f;
    LARGE_INTEGER c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static char *dup_str(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

/* Strip a trailing extension on the basename, keeping any folder prefix -- so the result matches
 * the sprite-tree override key (e.g. "anim/test-0.png" -> "anim/test-0"). */
static void strip_ext_keep_folder(const char *in, char *out, size_t cap) {
    (void)snprintf(out, cap, "%s", in);
    char *slash = strrchr(out, '/');
    char *base = slash ? slash + 1 : out;
    char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        *dot = '\0';
    }
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') {
            b = q + 1;
        }
    }
    return b;
}

typedef struct {
    tp_pack_sprite_desc *v;
    int n;
    int cap;
} desc_vec;

/* Appends one sprite: raw name (ext kept), decode path, and per-sprite origin/slice9 overrides
 * mapped from the project by the STRIPPED key. Returns false on OOM. */
static bool desc_add(desc_vec *dv, tp_project_atlas *a, const char *raw_name, const char *abs_path) {
    if (dv->n == dv->cap) {
        int nc = dv->cap ? dv->cap * 2 : 32;
        tp_pack_sprite_desc *nv = (tp_pack_sprite_desc *)realloc(dv->v, (size_t)nc * sizeof *nv);
        if (!nv) {
            return false;
        }
        dv->v = nv;
        dv->cap = nc;
    }
    tp_pack_sprite_desc *d = &dv->v[dv->n];
    memset(d, 0, sizeof *d);
    d->name = dup_str(raw_name);
    d->path = dup_str(abs_path);
    d->origin_x = TP_PROJECT_ORIGIN_DEFAULT;
    d->origin_y = TP_PROJECT_ORIGIN_DEFAULT;
    if (!d->name || !d->path) {
        free((void *)d->name);
        free((void *)d->path);
        return false;
    }
    char key[256];
    strip_ext_keep_folder(raw_name, key, sizeof key);
    const tp_project_sprite *ov = tp_project_atlas_find_sprite(a, key);
    if (ov) {
        d->origin_x = ov->origin_x;
        d->origin_y = ov->origin_y;
        for (int k = 0; k < 4; k++) {
            d->slice9_lrtb[k] = ov->slice9_lrtb[k];
        }
        /* Per-sprite packing overrides -> desc (engine encoding). Effective shape:
         * slice9 forces RECT (0), else sprite override, else atlas shape. The extrude
         * override is passed only when the effective shape is RECT (§3.3f effective
         * rule -- the stored value persists in the model regardless). */
        const bool slice9 = d->slice9_lrtb[0] || d->slice9_lrtb[1] || d->slice9_lrtb[2] || d->slice9_lrtb[3];
        const int eff_shape = slice9 ? 0 : (ov->ov_shape != TP_PROJECT_OV_INHERIT ? ov->ov_shape : a->shape);
        if (ov->ov_shape != TP_PROJECT_OV_INHERIT) {
            d->ov_mask |= TP_PACK_OV_SHAPE;
            d->ov_shape = (uint8_t)(ov->ov_shape + 1); /* atlas 0/1/2 -> engine RECT/CONVEX/CONCAVE 1/2/3 */
        }
        if (ov->ov_allow_rotate != TP_PROJECT_OV_INHERIT) {
            d->ov_mask |= TP_PACK_OV_ROTATE;
            d->ov_allow_rotate = TP_PACK_SPRITE_ROTATE_NO;
        }
        if (ov->ov_max_vertices != TP_PROJECT_OV_INHERIT) {
            d->ov_mask |= TP_PACK_OV_MAXVERT;
            d->ov_max_vertices = (uint8_t)ov->ov_max_vertices;
        }
        if (ov->ov_margin != TP_PROJECT_OV_INHERIT) {
            d->ov_mask |= TP_PACK_OV_MARGIN;
            d->ov_margin = (uint8_t)ov->ov_margin;
        }
        if (ov->ov_extrude != TP_PROJECT_OV_INHERIT && eff_shape == 0 /* RECT */) {
            d->ov_mask |= TP_PACK_OV_EXTRUDE;
            d->ov_extrude = (uint8_t)ov->ov_extrude;
        }
    }
    dv->n++;
    return true;
}

static void desc_free(desc_vec *dv) {
    for (int i = 0; i < dv->n; i++) {
        free((void *)dv->v[i].name);
        free((void *)dv->v[i].path);
    }
    free(dv->v);
    dv->v = NULL;
    dv->n = 0;
    dv->cap = 0;
}

/* Assembles sprite descs from atlas `ai`'s sources (files as-is, folders expanded via gui_scan).
 * *missing gets the count of missing sources skipped (ux.md §3.7). Returns false on OOM (fills err).
 * `dv` must be desc_free'd by the caller regardless. */
static bool assemble(int ai, desc_vec *dv, int *missing, char *err, size_t err_cap) {
    tp_project *p = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(p, ai);
    *missing = 0;
    if (!a) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }
    for (int si = 0; si < a->source_count; si++) {
        char abs[512];
        if (tp_project_resolve_path(p, a->sources[si], abs, sizeof abs) != TP_STATUS_OK) {
            continue;
        }
        if (!gui_scan_exists(abs)) {
            (*missing)++;
            continue;
        }
        if (gui_scan_is_dir(abs)) {
            const gui_scan_result *sc = gui_scan_get(abs);
            for (int ci = 0; ci < sc->count; ci++) {
                if (!desc_add(dv, a, sc->entries[ci].rel, sc->entries[ci].abs)) {
                    if (err) {
                        (void)snprintf(err, err_cap, "out of memory assembling sprites");
                    }
                    return false;
                }
            }
        } else if (!desc_add(dv, a, base_name(a->sources[si]), abs)) {
            if (err) {
                (void)snprintf(err, err_cap, "out of memory assembling sprites");
            }
            return false;
        }
    }
    return true;
}

/* Creates every parent directory of the file path `file_abs` (mkdir -p of its dirname). */
static void mkdirs_parent(const char *file_abs) {
    char tmp[700];
    (void)snprintf(tmp, sizeof tmp, "%s", file_abs);
    char *last = strrchr(tmp, '/');
    char *lb = strrchr(tmp, '\\');
    if (lb && (!last || lb > last)) {
        last = lb;
    }
    if (!last) {
        return;
    }
    *last = '\0';
    for (char *q = tmp; *q != '\0'; q++) {
        if ((*q == '/' || *q == '\\') && q != tmp) {
            const char sep = *q;
            *q = '\0';
#ifdef _WIN32
            (void)CreateDirectoryA(tmp, NULL);
#else
            (void)mkdir(tmp, 0755);
#endif
            *q = sep;
        }
    }
#ifdef _WIN32
    (void)CreateDirectoryA(tmp, NULL);
#else
    (void)mkdir(tmp, 0755);
#endif
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
        if (s_slots[i].arena) {
            tp_arena_destroy(s_slots[i].arena);
        }
        s_slots[i].arena = NULL;
        s_slots[i].result = NULL;
        s_slots[i].valid = false;
    }
    /* The preview is a derived view of the session pack -- clearing the session slot(s) it mirrors
     * invalidates it too (open/new/undo/redo/repack all pass -1 or the preview's atlas). */
    if (atlas_index < 0 || atlas_index == s_preview.atlas_index) {
        gui_pack_preview_clear();
    }
}

// #region async worker (one in-flight op max: pack OR export)
typedef enum { JOB_NONE = 0, JOB_PACK, JOB_EXPORT, JOB_PREVIEW } job_kind;

typedef struct {
    int atlas_index;
    desc_vec dv; /* assembled sprite descs (job-owned; abs paths) */
    int enabled; /* enabled target count (for the export report) */
} export_atlas;

typedef struct {
    job_kind kind;

    /* pack input (job-owned; worker reads, all set before thrd_create) */
    int atlas_index;
    tp_pack_settings settings; /* .sprites -> dv.v, .atlas_name -> name_owned, .work_dir -> s_work_dir */
    char *name_owned;          /* dup of the live atlas name (settings.atlas_name aliased the model) */
    desc_vec dv;               /* owns names/paths + v; freed at completion */
    tp_arena *arena;           /* worker allocates the result here */
    int missing;
    char *snap0; /* model snapshot at start (honest stale compare) */
    size_t snap0_len;
    char preview_exporter[64]; /* JOB_PREVIEW: which exporter this preview was clamped for */

    /* export input (job-owned) */
    tp_project *clone;
    export_atlas *eatlas;
    int eatlas_count;

    /* output (worker writes; main reads only after acquiring `state`) */
    tp_result *result;
    tp_status status;
    tp_error err;
    double elapsed_ms;
    int exp_targets, exp_notices, exp_atlases_ok, exp_atlases_fail;
    char exp_first_err[256];

    /* control */
    _Atomic int state; /* 0 = running, 1 = done */
    _Atomic bool cancelled;
    _Atomic int exp_cur; /* export progress (1-based) */
    _Atomic int exp_total;
    double start_ms;
    thrd_t thread;
} pack_job;

static pack_job s_job;
static bool s_job_active;                /* main-thread view: worker started, not yet joined */
static gui_pack_async_kind s_debug_busy; /* --shot-packing override */

static int pack_worker(void *arg) {
    pack_job *j = (pack_job *)arg;
    const double t0 = now_ms();
    tp_result *result = NULL;
    tp_error e = {{0}};
    const tp_status st = tp_pack(&j->settings, j->arena, &result, &e);
    j->elapsed_ms = now_ms() - t0;
    j->result = result;
    j->status = st;
    j->err = e;
    atomic_store_explicit(&j->state, 1, memory_order_release);
    return 0;
}

static int export_worker(void *arg) {
    pack_job *j = (pack_job *)arg;
    const double t0 = now_ms();
    int targets = 0;
    int notices = 0;
    int ok = 0;
    int fail = 0;
    char first_err[256] = {0};
    for (int i = 0; i < j->eatlas_count; i++) {
        if (atomic_load_explicit(&j->cancelled, memory_order_relaxed)) {
            break; /* already-written atlases stay; no further atlases started */
        }
        atomic_store_explicit(&j->exp_cur, i + 1, memory_order_relaxed);
        export_atlas *ea = &j->eatlas[i];
        tp_arena *ar = tp_arena_create(0);
        if (!ar) {
            fail++;
            continue;
        }
        tp_export_notices nts;
        tp_export_notices_init(&nts);
        tp_error e = {{0}};
        int runs = 0;
        const tp_status st =
            tp_export_run(j->clone, ea->atlas_index, ea->dv.v, ea->dv.n, s_work_dir, ar, &nts, &runs, &e);
        if (st == TP_STATUS_OK) {
            targets += ea->enabled;
            notices += nts.count;
            ok++;
        } else {
            fail++;
            if (first_err[0] == '\0') {
                const tp_project_atlas *ca = tp_project_get_atlas(j->clone, ea->atlas_index);
                (void)snprintf(first_err, sizeof first_err, "%s: %s", ca ? ca->name : "?",
                               e.msg[0] ? e.msg : tp_status_str(st));
            }
        }
        tp_export_notices_free(&nts);
        tp_arena_destroy(ar);
    }
    j->exp_targets = targets;
    j->exp_notices = notices;
    j->exp_atlases_ok = ok;
    j->exp_atlases_fail = fail;
    memcpy(j->exp_first_err, first_err, sizeof first_err);
    j->elapsed_ms = now_ms() - t0;
    atomic_store_explicit(&j->state, 1, memory_order_release);
    return 0;
}

static bool model_changed_since(const char *snap, size_t len) {
    if (!snap) {
        return true; /* couldn't snapshot at start -> assume changed (safe: shows stale) */
    }
    char *cur = NULL;
    size_t cl = 0;
    tp_error e = {{0}};
    if (tp_project_save_buffer(gui_project_get(), &cur, &cl, &e) != TP_STATUS_OK || !cur) {
        return true;
    }
    const bool diff = (cl != len) || memcmp(cur, snap, len) != 0;
    free(cur);
    return diff;
}

bool gui_pack_async_start(int atlas_index, char *err, size_t err_cap) {
    if (s_job_active) {
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
    tp_project *p = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }

    desc_vec dv = {0};
    int missing = 0;
    if (!assemble(atlas_index, &dv, &missing, err, err_cap)) {
        desc_free(&dv);
        return false;
    }
    if (dv.n == 0) {
        desc_free(&dv);
        if (err) {
            (void)snprintf(err, err_cap, "atlas '%s' has no usable images%s", a->name,
                           missing > 0 ? " (all sources missing)" : "");
        }
        return false;
    }

    tp_pack_settings settings;
    tp_error e = {{0}};
    if (tp_project_atlas_to_settings(p, atlas_index, &settings, &e) != TP_STATUS_OK) {
        desc_free(&dv);
        if (err) {
            (void)snprintf(err, err_cap, "%s", e.msg);
        }
        return false;
    }
    if (settings.shape != 0 /* not RECT */) {
        settings.extrude = 0;
    }
    char *name_owned = dup_str(a->name); /* settings.atlas_name aliases the live model -- own a copy */
    tp_arena *arena = tp_arena_create(0);
    if (!name_owned || !arena) {
        desc_free(&dv);
        free(name_owned);
        if (arena) {
            tp_arena_destroy(arena);
        }
        if (err) {
            (void)snprintf(err, err_cap, "out of memory");
        }
        return false;
    }
    settings.work_dir = s_work_dir; /* stable static; never mutated after init */
    settings.atlas_name = name_owned;
    settings.sprites = dv.v;
    settings.sprite_count = dv.n;

    char *snap = NULL;
    size_t snap_len = 0;
    (void)tp_project_save_buffer(p, &snap, &snap_len, &e); /* best-effort; NULL -> treated as changed */

    memset(&s_job, 0, sizeof s_job);
    s_job.kind = JOB_PACK;
    s_job.atlas_index = atlas_index;
    s_job.settings = settings;
    s_job.name_owned = name_owned;
    s_job.dv = dv;
    s_job.arena = arena;
    s_job.missing = missing;
    s_job.snap0 = snap;
    s_job.snap0_len = snap_len;
    s_job.start_ms = now_ms();
    atomic_store_explicit(&s_job.state, 0, memory_order_relaxed);
    atomic_store_explicit(&s_job.cancelled, false, memory_order_relaxed);
    if (thrd_create(&s_job.thread, pack_worker, &s_job) != thrd_success) {
        desc_free(&s_job.dv);
        free(s_job.name_owned);
        free(s_job.snap0);
        tp_arena_destroy(s_job.arena);
        memset(&s_job, 0, sizeof s_job);
        if (err) {
            (void)snprintf(err, err_cap, "could not start worker thread");
        }
        return false;
    }
    s_job_active = true;
    return true;
}

bool gui_pack_export_async_start(char *err, size_t err_cap) {
    if (s_job_active) {
        if (err) {
            (void)snprintf(err, err_cap, "busy -- a pack or export is already running");
        }
        return false;
    }
    tp_project *p = gui_project_get();
    if (!p) {
        if (err) {
            (void)snprintf(err, err_cap, "no project");
        }
        return false;
    }

    export_atlas tmp[GUI_PACK_MAX_ATLASES];
    int n = 0;
    for (int ai = 0; ai < p->atlas_count && ai < GUI_PACK_MAX_ATLASES; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        int enabled = 0;
        for (int t = 0; t < a->target_count; t++) {
            if (a->targets[t].enabled) {
                enabled++;
            }
        }
        if (enabled == 0 || a->source_count == 0) {
            continue;
        }
        bool relerr = false;
        for (int t = 0; t < a->target_count; t++) {
            if (!a->targets[t].enabled) {
                continue;
            }
            char out_abs[600];
            if (tp_project_resolve_path(p, a->targets[t].out_path, out_abs, sizeof out_abs) != TP_STATUS_OK) {
                relerr = true;
                break;
            }
            mkdirs_parent(out_abs);
        }
        if (relerr) {
            for (int k = 0; k < n; k++) {
                desc_free(&tmp[k].dv);
            }
            if (err) {
                (void)snprintf(err, err_cap, "save the project first (relative output paths need a project dir)");
            }
            return false;
        }
        desc_vec dv = {0};
        int missing = 0;
        if (!assemble(ai, &dv, &missing, err, err_cap)) {
            desc_free(&dv);
            for (int k = 0; k < n; k++) {
                desc_free(&tmp[k].dv);
            }
            return false;
        }
        if (dv.n == 0) {
            desc_free(&dv);
            continue; /* no usable images: skip this atlas (mirrors do_export) */
        }
        tmp[n].atlas_index = ai;
        tmp[n].dv = dv;
        tmp[n].enabled = enabled;
        n++;
    }
    if (n == 0) {
        if (err) {
            (void)snprintf(err, err_cap, "Nothing to export -- enable a target and add sources.");
        }
        return false;
    }

    /* Snapshot the whole project so the worker never reads live-editable memory. */
    char *buf = NULL;
    size_t blen = 0;
    tp_error e = {{0}};
    if (tp_project_save_buffer(p, &buf, &blen, &e) != TP_STATUS_OK || !buf) {
        for (int k = 0; k < n; k++) {
            desc_free(&tmp[k].dv);
        }
        if (err) {
            (void)snprintf(err, err_cap, "snapshot failed: %s", e.msg);
        }
        return false;
    }
    tp_project *clone = NULL;
    const tp_status st = tp_project_load_buffer(buf, blen, &clone, &e);
    free(buf);
    if (st != TP_STATUS_OK || !clone) {
        for (int k = 0; k < n; k++) {
            desc_free(&tmp[k].dv);
        }
        if (err) {
            (void)snprintf(err, err_cap, "snapshot parse failed: %s", e.msg);
        }
        return false;
    }
    /* load_buffer leaves project_dir NULL; out-path resolution in the worker needs it. */
    clone->project_dir = p->project_dir ? dup_str(p->project_dir) : NULL;

    export_atlas *eatlas = (export_atlas *)malloc((size_t)n * sizeof *eatlas);
    if (!eatlas) {
        for (int k = 0; k < n; k++) {
            desc_free(&tmp[k].dv);
        }
        tp_project_destroy(clone);
        if (err) {
            (void)snprintf(err, err_cap, "out of memory");
        }
        return false;
    }
    memcpy(eatlas, tmp, (size_t)n * sizeof *eatlas);

    memset(&s_job, 0, sizeof s_job);
    s_job.kind = JOB_EXPORT;
    s_job.clone = clone;
    s_job.eatlas = eatlas;
    s_job.eatlas_count = n;
    s_job.start_ms = now_ms();
    atomic_store_explicit(&s_job.state, 0, memory_order_relaxed);
    atomic_store_explicit(&s_job.cancelled, false, memory_order_relaxed);
    atomic_store_explicit(&s_job.exp_cur, 0, memory_order_relaxed);
    atomic_store_explicit(&s_job.exp_total, n, memory_order_relaxed);
    if (thrd_create(&s_job.thread, export_worker, &s_job) != thrd_success) {
        for (int k = 0; k < s_job.eatlas_count; k++) {
            desc_free(&s_job.eatlas[k].dv);
        }
        free(s_job.eatlas);
        tp_project_destroy(s_job.clone);
        memset(&s_job, 0, sizeof s_job);
        if (err) {
            (void)snprintf(err, err_cap, "could not start worker thread");
        }
        return false;
    }
    s_job_active = true;
    return true;
}

gui_pack_done gui_pack_poll(gui_pack_result_info *out) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!s_job_active) {
        return GUI_PACK_DONE_NONE;
    }
    if (atomic_load_explicit(&s_job.state, memory_order_acquire) == 0) {
        return GUI_PACK_DONE_NONE; /* worker still running */
    }
    (void)thrd_join(s_job.thread, NULL);
    const bool cancelled = atomic_load_explicit(&s_job.cancelled, memory_order_relaxed);
    gui_pack_done rc = GUI_PACK_DONE_NONE;

    if (s_job.kind == JOB_PACK) {
        if (s_job.status == TP_STATUS_OK && s_job.result) {
            if (cancelled) {
                tp_arena_destroy(s_job.arena); /* discard: abandoned result */
                rc = GUI_PACK_DONE_PACK_CANCELLED;
            } else {
                if (s_slots[s_job.atlas_index].arena) {
                    tp_arena_destroy(s_slots[s_job.atlas_index].arena);
                }
                s_slots[s_job.atlas_index].arena = s_job.arena;
                s_slots[s_job.atlas_index].result = s_job.result;
                s_slots[s_job.atlas_index].valid = true;
                if (out) {
                    out->atlas_index = s_job.atlas_index;
                    out->ms = s_job.elapsed_ms;
                    out->missing = s_job.missing;
                    out->model_changed = model_changed_since(s_job.snap0, s_job.snap0_len);
                    if (s_job.missing > 0) {
                        (void)snprintf(out->note, sizeof out->note, "%d missing file(s) skipped", s_job.missing);
                    }
                }
                nt_log_info("gui_pack(async): atlas '%s' packed %d sprite(s), %d page(s) in %.1f ms",
                            s_job.result->atlas_name, s_job.result->sprite_count, s_job.result->page_count,
                            s_job.elapsed_ms);
                rc = GUI_PACK_DONE_PACK_OK;
            }
        } else {
            tp_arena_destroy(s_job.arena); /* failure: keep the previous slot (canvas stays) */
            if (cancelled) {
                rc = GUI_PACK_DONE_PACK_CANCELLED;
            } else {
                if (out) {
                    (void)snprintf(out->err, sizeof out->err, "%s",
                                   s_job.err.msg[0] ? s_job.err.msg : tp_status_str(s_job.status));
                }
                rc = GUI_PACK_DONE_PACK_FAIL;
            }
        }
        desc_free(&s_job.dv);
        free(s_job.name_owned);
        free(s_job.snap0);
    } else if (s_job.kind == JOB_PREVIEW) {
        if (s_job.status == TP_STATUS_OK && s_job.result && !cancelled) {
            if (s_preview.arena) {
                tp_arena_destroy(s_preview.arena); /* free any prior preview */
            }
            s_preview.arena = s_job.arena;
            s_preview.result = s_job.result;
            s_preview.valid = true;
            s_preview.atlas_index = s_job.atlas_index;
            (void)snprintf(s_preview.exporter_id, sizeof s_preview.exporter_id, "%s", s_job.preview_exporter);
            if (out) {
                out->atlas_index = s_job.atlas_index;
                out->ms = s_job.elapsed_ms;
            }
            nt_log_info("gui_pack(async): preview '%s' via %s packed %d sprite(s), %d page(s) in %.1f ms",
                        s_job.result->atlas_name, s_job.preview_exporter, s_job.result->sprite_count,
                        s_job.result->page_count, s_job.elapsed_ms);
            rc = GUI_PACK_DONE_PREVIEW_OK;
        } else {
            tp_arena_destroy(s_job.arena); /* discard: cancelled or failed -- the session slot is untouched */
            if (cancelled) {
                rc = GUI_PACK_DONE_PREVIEW_CANCELLED;
            } else {
                if (out) {
                    (void)snprintf(out->err, sizeof out->err, "%s",
                                   s_job.err.msg[0] ? s_job.err.msg : tp_status_str(s_job.status));
                }
                rc = GUI_PACK_DONE_PREVIEW_FAIL;
            }
        }
        desc_free(&s_job.dv);
        free(s_job.name_owned);
    } else { /* JOB_EXPORT */
        if (out) {
            out->targets = s_job.exp_targets;
            out->notices = s_job.exp_notices;
            out->atlases_ok = s_job.exp_atlases_ok;
            out->atlases_fail = s_job.exp_atlases_fail;
            (void)snprintf(out->err, sizeof out->err, "%s", s_job.exp_first_err);
        }
        if (cancelled) {
            rc = GUI_PACK_DONE_EXPORT_CANCELLED;
        } else if (s_job.exp_atlases_fail > 0) {
            rc = GUI_PACK_DONE_EXPORT_FAIL;
        } else {
            rc = GUI_PACK_DONE_EXPORT_OK;
        }
        nt_log_info("gui_pack(async): export %d target(s), %d ok, %d fail, %d notice(s) in %.1f ms", s_job.exp_targets,
                    s_job.exp_atlases_ok, s_job.exp_atlases_fail, s_job.exp_notices, s_job.elapsed_ms);
        for (int i = 0; i < s_job.eatlas_count; i++) {
            desc_free(&s_job.eatlas[i].dv);
        }
        free(s_job.eatlas);
        tp_project_destroy(s_job.clone);
    }

    memset(&s_job, 0, sizeof s_job);
    s_job_active = false;
    if (out) {
        out->kind = rc;
    }
    return rc;
}

bool gui_pack_async_busy(void) { return s_job_active || s_debug_busy != GUI_PACK_ASYNC_NONE; }

bool gui_pack_worker_active(void) { return s_job_active; } /* real worker only (no --shot debug busy) */

gui_pack_async_kind gui_pack_async_active_kind(void) {
    if (s_debug_busy != GUI_PACK_ASYNC_NONE) {
        return s_debug_busy;
    }
    if (!s_job_active) {
        return GUI_PACK_ASYNC_NONE;
    }
    return s_job.kind == JOB_EXPORT ? GUI_PACK_ASYNC_EXPORT : GUI_PACK_ASYNC_PACK;
}

double gui_pack_async_elapsed_sec(void) {
    if (s_debug_busy != GUI_PACK_ASYNC_NONE) {
        return 3.2; /* fixed for reproducible --shot-packing captures */
    }
    return s_job_active ? (now_ms() - s_job.start_ms) / 1000.0 : 0.0;
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
    if (cur) {
        *cur = s_job_active ? atomic_load_explicit(&s_job.exp_cur, memory_order_relaxed) : 0;
    }
    if (total) {
        *total = s_job_active ? atomic_load_explicit(&s_job.exp_total, memory_order_relaxed) : 0;
    }
}

void gui_pack_async_cancel(void) {
    if (s_job_active) {
        atomic_store_explicit(&s_job.cancelled, true, memory_order_relaxed);
    }
}

bool gui_pack_async_cancelling(void) {
    return s_job_active && atomic_load_explicit(&s_job.cancelled, memory_order_relaxed);
}

void gui_pack_debug_force_busy(gui_pack_async_kind kind) { s_debug_busy = kind; }
// #endregion

// #region export-target preview (EXP-PREVIEW)
/* Assembles descs for `atlas_index` and computes the caps-clamped effective settings for `exporter_id`.
 * On success fills `dv` (caller desc_free's it regardless), *out_name (malloc'd atlas name; caller frees
 * on success, this frees it on its own later failure) and *out (effective settings with .sprites/.name/
 * .work_dir wired). false fills err. Mirrors the session assembly in gui_pack_atlas + the §3.3f
 * effective-extrude rule, then clamps through the exporter caps. */
static bool preview_prepare(int atlas_index, const char *exporter_id, desc_vec *dv, char **out_name,
                            tp_pack_settings *out, char *err, size_t err_cap) {
    *out_name = NULL;
    if (atlas_index < 0 || atlas_index >= GUI_PACK_MAX_ATLASES) {
        if (err) {
            (void)snprintf(err, err_cap, "atlas index out of range");
        }
        return false;
    }
    const tp_exporter *e = tp_exporter_find(exporter_id);
    if (!e) {
        if (err) {
            (void)snprintf(err, err_cap, "unknown exporter '%s'", exporter_id ? exporter_id : "(null)");
        }
        return false;
    }
    tp_project *p = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }
    int missing = 0;
    if (!assemble(atlas_index, dv, &missing, err, err_cap)) {
        return false;
    }
    if (dv->n == 0) {
        if (err) {
            (void)snprintf(err, err_cap, "atlas '%s' has no usable images%s", a->name,
                           missing > 0 ? " (all sources missing)" : "");
        }
        return false;
    }
    tp_pack_settings native;
    tp_error te = {{0}};
    if (tp_project_atlas_to_settings(p, atlas_index, &native, &te) != TP_STATUS_OK) {
        if (err) {
            (void)snprintf(err, err_cap, "%s", te.msg);
        }
        return false;
    }
    if (native.shape != 0 /* not RECT */) {
        native.extrude = 0; /* session effective-extrude rule (§3.3f); the diff baseline matches the native pack */
    }
    tp_pack_settings eff;
    if (tp_export_effective_settings(&native, &e->caps, &eff) != TP_STATUS_OK) {
        if (err) {
            (void)snprintf(err, err_cap, "could not clamp settings for '%s'", exporter_id);
        }
        return false;
    }
    if (eff.shape != 0 /* not RECT */) {
        eff.extrude = 0; /* keep tp_pack's RECT-only-extrude invariant after the clamp */
    }
    char *name = dup_str(a->name); /* eff.atlas_name would alias the live model -- own a copy */
    if (!name) {
        if (err) {
            (void)snprintf(err, err_cap, "out of memory");
        }
        return false;
    }
    eff.work_dir = s_work_dir;
    eff.atlas_name = name;
    eff.sprites = dv->v;
    eff.sprite_count = dv->n;
    *out_name = name;
    *out = eff;
    return true;
}

bool gui_pack_preview_blocking(int atlas_index, const char *exporter_id, char *err, size_t err_cap) {
    desc_vec dv = {0};
    char *name = NULL;
    tp_pack_settings settings;
    if (!preview_prepare(atlas_index, exporter_id, &dv, &name, &settings, err, err_cap)) {
        desc_free(&dv);
        free(name);
        return false;
    }
    tp_arena *arena = tp_arena_create(0);
    if (!arena) {
        desc_free(&dv);
        free(name);
        if (err) {
            (void)snprintf(err, err_cap, "out of memory (arena)");
        }
        return false;
    }
    tp_result *result = NULL;
    tp_error e = {{0}};
    const tp_status st = tp_pack(&settings, arena, &result, &e);
    desc_free(&dv); /* names/paths consumed by tp_pack */
    free(name);
    if (st != TP_STATUS_OK || !result) {
        tp_arena_destroy(arena);
        if (err) {
            (void)snprintf(err, err_cap, "%s", e.msg[0] ? e.msg : tp_status_str(st));
        }
        return false;
    }
    if (s_preview.arena) {
        tp_arena_destroy(s_preview.arena);
    }
    s_preview.arena = arena;
    s_preview.result = result;
    s_preview.valid = true;
    s_preview.atlas_index = atlas_index;
    (void)snprintf(s_preview.exporter_id, sizeof s_preview.exporter_id, "%s", exporter_id);
    nt_log_info("gui_pack: preview '%s' via %s -> %d sprite(s), %d page(s)", result->atlas_name, exporter_id,
                result->sprite_count, result->page_count);
    return true;
}

bool gui_pack_preview_async_start(int atlas_index, const char *exporter_id, char *err, size_t err_cap) {
    if (s_job_active) {
        if (err) {
            (void)snprintf(err, err_cap, "busy -- a pack or export is already running");
        }
        return false;
    }
    desc_vec dv = {0};
    char *name = NULL;
    tp_pack_settings settings;
    if (!preview_prepare(atlas_index, exporter_id, &dv, &name, &settings, err, err_cap)) {
        desc_free(&dv);
        free(name);
        return false;
    }
    tp_arena *arena = tp_arena_create(0);
    if (!arena) {
        desc_free(&dv);
        free(name);
        if (err) {
            (void)snprintf(err, err_cap, "out of memory");
        }
        return false;
    }
    memset(&s_job, 0, sizeof s_job);
    s_job.kind = JOB_PREVIEW;
    s_job.atlas_index = atlas_index;
    s_job.settings = settings;
    s_job.name_owned = name;
    s_job.dv = dv;
    s_job.arena = arena;
    (void)snprintf(s_job.preview_exporter, sizeof s_job.preview_exporter, "%s", exporter_id);
    s_job.start_ms = now_ms();
    atomic_store_explicit(&s_job.state, 0, memory_order_relaxed);
    atomic_store_explicit(&s_job.cancelled, false, memory_order_relaxed);
    if (thrd_create(&s_job.thread, pack_worker, &s_job) != thrd_success) {
        desc_free(&s_job.dv);
        free(s_job.name_owned);
        tp_arena_destroy(s_job.arena);
        memset(&s_job, 0, sizeof s_job);
        if (err) {
            (void)snprintf(err, err_cap, "could not start worker thread");
        }
        return false;
    }
    s_job_active = true;
    return true;
}

const tp_result *gui_pack_preview_result(int atlas_index) {
    if (!s_preview.valid || s_preview.atlas_index != atlas_index) {
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
    s_preview.valid = false;
    s_preview.atlas_index = -1;
    s_preview.exporter_id[0] = '\0';
}

/* True if any sprite override in the atlas carries a non-zero slice9 border. */
static bool atlas_uses_slice9(const tp_project_atlas *a) {
    for (int i = 0; a && i < a->sprite_count; i++) {
        const tp_project_sprite *s = &a->sprites[i];
        if (s->slice9_lrtb[0] || s->slice9_lrtb[1] || s->slice9_lrtb[2] || s->slice9_lrtb[3]) {
            return true;
        }
    }
    return false;
}

/* True if any sprite override carries a non-default pivot. */
static bool atlas_uses_pivot(const tp_project_atlas *a) {
    for (int i = 0; a && i < a->sprite_count; i++) {
        const tp_project_sprite *s = &a->sprites[i];
        if (s->origin_x != TP_PROJECT_ORIGIN_DEFAULT || s->origin_y != TP_PROJECT_ORIGIN_DEFAULT) {
            return true;
        }
    }
    return false;
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
    tp_project *p = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!e || !a) {
        return 0;
    }
    tp_pack_settings native;
    tp_error te = {{0}};
    if (tp_project_atlas_to_settings(p, atlas_index, &native, &te) != TP_STATUS_OK) {
        return 0;
    }
    if (native.shape != 0) {
        native.extrude = 0;
    }
    tp_pack_settings eff;
    if (tp_export_effective_settings(&native, &e->caps, &eff) != TP_STATUS_OK) {
        return 0;
    }
    int n = 0;
    /* One collector: a short token for the chip ("a, b, c"), a longer line for the tooltip. Guarded so the
     * write pointer never forms past-end and a truncating snprintf can't advance the cursor out of range. */
    size_t clen = 0;
    size_t tlen = 0;
#define PREVIEW_ADD(short_tok, long_line)                                                                      \
    do {                                                                                                       \
        if (chip && clen < chip_cap) {                                                                         \
            const int w_ = snprintf(chip + clen, chip_cap - clen, "%s%s", n > 0 ? ", " : "", (short_tok));     \
            if (w_ > 0) {                                                                                      \
                clen += (size_t)w_;                                                                            \
            }                                                                                                  \
        }                                                                                                      \
        if (tip && tlen < tip_cap) {                                                                           \
            const int w2_ = snprintf(tip + tlen, tip_cap - tlen, "%s%s", n > 0 ? "\n" : "", (long_line));      \
            if (w2_ > 0) {                                                                                     \
                tlen += (size_t)w2_;                                                                           \
            }                                                                                                  \
        }                                                                                                      \
        n++;                                                                                                   \
    } while (0)

    if (native.allow_transform && !eff.allow_transform) {
        PREVIEW_ADD("no rotate/flip", "Rotations/flips off -- this format can't encode the full D4 orientation set");
    }
    if (native.shape != eff.shape) {
        PREVIEW_ADD("polygons \xE2\x86\x92 rect", "Polygon hulls flattened to rectangles -- this format stores quads only");
    }
    if (!e->caps.slice9 && atlas_uses_slice9(a)) {
        PREVIEW_ADD("slice9 dropped", "9-slice borders dropped -- this format does not store them");
    }
    if (!e->caps.pivot && atlas_uses_pivot(a)) {
        PREVIEW_ADD("pivot dropped", "Per-sprite pivots dropped -- this format does not store them");
    }
#undef PREVIEW_ADD
    return n;
}
// #endregion

void gui_pack_shutdown(void) {
    if (s_job_active) {
        /* Non-interruptible pack + no thrd_detach in this tinycthread -> join (covers the window
         * X-button close path, which bypasses the interactive running-op guard). Result discarded. */
        atomic_store_explicit(&s_job.cancelled, true, memory_order_relaxed);
        (void)thrd_join(s_job.thread, NULL);
        if (s_job.kind == JOB_PACK || s_job.kind == JOB_PREVIEW) {
            if (s_job.arena) {
                tp_arena_destroy(s_job.arena);
            }
            desc_free(&s_job.dv);
            free(s_job.name_owned);
            free(s_job.snap0);
        } else if (s_job.kind == JOB_EXPORT) {
            for (int i = 0; i < s_job.eatlas_count; i++) {
                desc_free(&s_job.eatlas[i].dv);
            }
            free(s_job.eatlas);
            tp_project_destroy(s_job.clone);
        }
        memset(&s_job, 0, sizeof s_job);
        s_job_active = false;
    }
    gui_pack_clear(-1);
    gui_pack_preview_clear();
}

const tp_result *gui_pack_result(int atlas_index) {
    if (atlas_index < 0 || atlas_index >= GUI_PACK_MAX_ATLASES || !s_slots[atlas_index].valid) {
        return NULL;
    }
    return s_slots[atlas_index].result;
}

int gui_pack_find_sprite(int atlas_index, const char *key) {
    const tp_result *r = gui_pack_result(atlas_index);
    if (!r || !key) {
        return -1;
    }
    for (int i = 0; i < r->sprite_count; i++) {
        char sk[256];
        strip_ext_keep_folder(r->sprites[i].name, sk, sizeof sk);
        if (strcmp(sk, key) == 0) {
            return i;
        }
    }
    return -1;
}

bool gui_pack_atlas(int atlas_index, double *out_ms, char *err, size_t err_cap, char *notice, size_t notice_cap) {
    if (atlas_index < 0 || atlas_index >= GUI_PACK_MAX_ATLASES) {
        if (err) {
            (void)snprintf(err, err_cap, "atlas index out of range");
        }
        return false;
    }
    tp_project *p = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }

    /* Assemble sprites from the atlas's sources: files as-is, folders expanded (gui_scan). */
    desc_vec dv = {0};
    int missing = 0;
    if (!assemble(atlas_index, &dv, &missing, err, err_cap)) {
        desc_free(&dv);
        return false;
    }

    if (dv.n == 0) {
        desc_free(&dv);
        if (err) {
            (void)snprintf(err, err_cap, "atlas '%s' has no usable images%s", a->name,
                           missing > 0 ? " (all sources missing)" : "");
        }
        return false;
    }

    tp_pack_settings settings;
    tp_error e = {{0}};
    if (tp_project_atlas_to_settings(p, atlas_index, &settings, &e) != TP_STATUS_OK) {
        desc_free(&dv);
        if (err) {
            (void)snprintf(err, err_cap, "%s", e.msg);
        }
        return false;
    }
    /* §3.3f effective rule: extrude is only valid for RECT. The project KEEPS the
     * stored extrude value; the preview pack passes 0 while shape != RECT (core
     * tp_pack stays strict as the safety net). */
    if (settings.shape != 0 /* not RECT */) {
        settings.extrude = 0;
    }
    settings.work_dir = s_work_dir;
    settings.sprites = dv.v;
    settings.sprite_count = dv.n;

    tp_arena *arena = tp_arena_create(0);
    if (!arena) {
        desc_free(&dv);
        if (err) {
            (void)snprintf(err, err_cap, "out of memory (arena)");
        }
        return false;
    }

    const double t0 = now_ms();
    tp_result *result = NULL;
    tp_status st = tp_pack(&settings, arena, &result, &e);
    const double dt = now_ms() - t0;
    desc_free(&dv); /* names/paths consumed by tp_pack; result has its own arena copies */

    if (st != TP_STATUS_OK || !result) {
        tp_arena_destroy(arena); /* keep any previous result for this slot (canvas stays) */
        if (err) {
            (void)snprintf(err, err_cap, "%s", e.msg[0] ? e.msg : tp_status_str(st));
        }
        return false;
    }

    /* Success: swap in the new result, freeing the previous arena. */
    if (s_slots[atlas_index].arena) {
        tp_arena_destroy(s_slots[atlas_index].arena);
    }
    s_slots[atlas_index].arena = arena;
    s_slots[atlas_index].result = result;
    s_slots[atlas_index].valid = true;

    if (out_ms) {
        *out_ms = dt;
    }
    if (notice && notice_cap) {
        if (missing > 0) {
            (void)snprintf(notice, notice_cap, "%d missing file(s) skipped", missing);
        } else {
            notice[0] = '\0';
        }
    }
    nt_log_info("gui_pack: atlas '%s' packed %d sprite(s), %d page(s) in %.1f ms%s", result->atlas_name,
                result->sprite_count, result->page_count, dt, missing > 0 ? " (missing skipped)" : "");
    return true;
}

bool gui_pack_export(int atlas_index, int *out_targets, int *out_notices, char *err, size_t err_cap, char *notice,
                     size_t notice_cap) {
    tp_project *p = gui_project_get();
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }
    /* Resolve + create each enabled target's output parent dir (tp_export_run requires it to exist).
     * A relative out_path in an unsaved project cannot resolve -> ask the user to save first. */
    int enabled = 0;
    for (int t = 0; t < a->target_count; t++) {
        if (!a->targets[t].enabled) {
            continue;
        }
        char out_abs[600];
        if (tp_project_resolve_path(p, a->targets[t].out_path, out_abs, sizeof out_abs) != TP_STATUS_OK) {
            if (err) {
                (void)snprintf(err, err_cap, "save the project first (relative output paths need a project dir)");
            }
            return false;
        }
        mkdirs_parent(out_abs);
        enabled++;
    }
    if (enabled == 0) {
        if (err) {
            (void)snprintf(err, err_cap, "atlas '%s' has no enabled targets", a->name);
        }
        return false;
    }

    desc_vec dv = {0};
    int missing = 0;
    if (!assemble(atlas_index, &dv, &missing, err, err_cap)) {
        desc_free(&dv);
        return false;
    }
    if (dv.n == 0) {
        desc_free(&dv);
        if (err) {
            (void)snprintf(err, err_cap, "atlas '%s' has no usable images", a->name);
        }
        return false;
    }

    tp_arena *arena = tp_arena_create(0);
    if (!arena) {
        desc_free(&dv);
        if (err) {
            (void)snprintf(err, err_cap, "out of memory (arena)");
        }
        return false;
    }
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error e = {{0}};
    int runs = 0;
    tp_status st = tp_export_run(p, atlas_index, dv.v, dv.n, s_work_dir, arena, &notices, &runs, &e);
    desc_free(&dv);

    const int nc = notices.count;
    if (out_targets) {
        *out_targets = enabled;
    }
    if (out_notices) {
        *out_notices = nc;
    }
    if (notice && notice_cap) {
        if (nc > 0) {
            (void)snprintf(notice, notice_cap, "%s%s", notices.items[0].msg, nc > 1 ? " (+more)" : "");
        } else {
            notice[0] = '\0';
        }
    }
    tp_export_notices_free(&notices);
    tp_arena_destroy(arena);

    if (st != TP_STATUS_OK) {
        if (err) {
            (void)snprintf(err, err_cap, "%s", e.msg[0] ? e.msg : tp_status_str(st));
        }
        return false;
    }
    nt_log_info("gui_pack: atlas '%s' exported %d target(s), %d pack run(s), %d notice(s)", a->name, enabled, runs, nc);
    return true;
}
// #endregion
