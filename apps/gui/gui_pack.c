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
}

void gui_pack_shutdown(void) { gui_pack_clear(-1); }

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
