#include "tp_core/tp_input.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_names.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"

/* Growable desc vector, local to a build; handed off to tp_pack_input on success. */
typedef struct {
    tp_pack_sprite_desc *v;
    int n;
    int cap;
} desc_vec;

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

/* Last path component of a '/'- or '\\'-separated path (file-source raw name). */
static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') {
            b = q + 1;
        }
    }
    return b;
}

/* Appends one sprite: raw name (ext kept) + decode path, with per-sprite overrides
 * mapped from the atlas by the STRIPPED export key and encoded onto the desc
 * (engine encoding, +1 shape). Returns false on OOM. Ports gui_pack.c desc_add
 * 1:1 (arch review §3.1); the effective-shape rule now lives in tp_project. */
static bool desc_add(desc_vec *dv, const tp_project_atlas *a, const char *raw_name, const char *abs_path) {
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
    tp_sprite_export_key(raw_name, key, sizeof key);
    /* Read-only override lookup; const-cast to reuse the canonical finder. */
    const tp_project_sprite *ov = tp_project_atlas_find_sprite((tp_project_atlas *)a, key);
    if (ov) {
        d->origin_x = ov->origin_x;
        d->origin_y = ov->origin_y;
        for (int k = 0; k < 4; k++) {
            d->slice9_lrtb[k] = ov->slice9_lrtb[k];
        }
        /* Effective shape (single rule): slice9 forces RECT, else the sprite shape
         * override, else the atlas shape. The extrude override is passed only when
         * the effective shape is RECT; the shape override itself is always encoded. */
        const bool slice9 = d->slice9_lrtb[0] || d->slice9_lrtb[1] || d->slice9_lrtb[2] || d->slice9_lrtb[3];
        const int eff_shape = tp_project_sprite_effective_shape(a->shape, slice9, ov->ov_shape);
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

static void desc_vec_free(desc_vec *dv) {
    for (int i = 0; i < dv->n; i++) {
        free((void *)dv->v[i].name);
        free((void *)dv->v[i].path);
    }
    free(dv->v);
    dv->v = NULL;
    dv->n = 0;
    dv->cap = 0;
}

tp_status tp_pack_input_build(const tp_project *p, int atlas_index, tp_pack_input *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack_input_build: out is NULL");
    }
    out->descs = NULL;
    out->count = 0;
    out->missing_sources = 0;
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack_input_build: project is NULL");
    }
    if (atlas_index < 0 || atlas_index >= p->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_pack_input_build: atlas index %d out of range",
                            atlas_index);
    }
    const tp_project_atlas *a = &p->atlases[atlas_index];

    desc_vec dv = {0};
    int missing = 0;
    bool oom = false;
    for (int si = 0; si < a->source_count && !oom; si++) {
        const char *src_path = a->sources[si].path;
        char abs[512];
        if (tp_project_resolve_path(p, src_path, abs, sizeof abs) != TP_STATUS_OK) {
            continue; /* unresolvable (relative source, unsaved project) -- skip, not missing */
        }
        if (!tp_scan_exists(abs)) {
            missing++;
            continue;
        }
        if (tp_scan_is_dir(abs)) {
            /* Folder: recurse (entries already sorted by rel) and append in scan
             * order. NO global sort across sources -- layout depends on input order. */
            tp_scan_result sc;
            tp_scan_dir(abs, &sc);
            for (int ci = 0; ci < sc.count; ci++) {
                if (!desc_add(&dv, a, sc.entries[ci].rel, sc.entries[ci].abs)) {
                    oom = true;
                    break;
                }
            }
            tp_scan_free(&sc);
        } else if (!desc_add(&dv, a, base_name(src_path), abs)) {
            oom = true;
        }
    }
    if (oom) {
        desc_vec_free(&dv);
        return tp_error_set(err, TP_STATUS_OOM, "tp_pack_input_build: out of memory assembling sprites");
    }

    out->descs = dv.v;
    out->count = dv.n;
    out->missing_sources = missing;
    return TP_STATUS_OK;
}

void tp_pack_input_free(tp_pack_input *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->count; i++) {
        free((void *)out->descs[i].name);
        free((void *)out->descs[i].path);
    }
    free(out->descs);
    out->descs = NULL;
    out->count = 0;
    out->missing_sources = 0;
}
