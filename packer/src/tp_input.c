#include "tp_core/tp_input.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"
#include "tp_project_mutation_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_strutil.h" /* shared tp_strdup / tp_path_basename */
#include "tp_session_internal.h"

/* Growable desc vector, local to a build; handed off to tp_pack_input on success. */
typedef struct {
    tp_pack_sprite_desc *v;
    int n;
    int cap;
} desc_vec;

tp_status tp_pack_input_format_sprite_name(tp_id128 source_id,
                                           const char *source_key,
                                           char *out, size_t capacity,
                                           tp_error *err) {
    if (tp_id128_is_nil(source_id) || !source_key || !source_key[0] || !out ||
        capacity == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "pack sprite name requires canonical source/key and output");
    }
    char source_text[TP_ID_TEXT_CAP];
    tp_status status = tp_id_format(TP_ID_KIND_SOURCE, source_id, source_text,
                                    sizeof source_text, err);
    if (status != TP_STATUS_OK) {
        out[0] = '\0';
        return status;
    }
    const int written = snprintf(out, capacity, "%s:%s", source_text,
                                 source_key);
    if (written < 0 || (size_t)written >= capacity) {
        out[0] = '\0';
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "pack sprite name exceeds output capacity");
    }
    return TP_STATUS_OK;
}

/* Appends one sprite: raw name (ext kept) + decode path, with per-sprite overrides
 * mapped from the atlas by the STRIPPED export key and encoded onto the desc
 * (engine encoding, +1 shape). Returns a precise status. Ports gui_pack.c desc_add
 * 1:1 (arch review §3.1); the effective-shape rule now lives in tp_project. */
static tp_status export_key_dup(const char *raw, char **out, tp_error *err) {
    *out = NULL;
    if (!raw) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "sprite export key input is NULL");
    }
    char key[TP_SRCKEY_MAX];
    tp_sprite_export_key(raw, key, sizeof key);
    char *copy = tp_strdup(key);
    if (!copy) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "sprite export key allocation failed");
    }
    *out = copy;
    return TP_STATUS_OK;
}

static tp_status desc_add(desc_vec *dv, const tp_project_atlas *a,
                          tp_id128 source_id, const char *src_key,
                          const char *abs_path, tp_error *err) {
    if (dv->n == dv->cap) {
        if (dv->cap > INT_MAX / 2) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "pack input has too many sprite descriptors");
        }
        int nc = dv->cap ? dv->cap * 2 : 32;
        if ((size_t)nc > SIZE_MAX / sizeof *dv->v) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "pack input descriptor table overflows size_t");
        }
        tp_pack_sprite_desc *nv =
            (tp_pack_sprite_desc *)realloc(dv->v,
                                           (size_t)nc * sizeof *nv);
        if (!nv) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "pack input descriptor allocation failed");
        }
        dv->v = nv;
        dv->cap = nc;
    }
    tp_pack_sprite_desc *d = &dv->v[dv->n];
    memset(d, 0, sizeof *d);
    char normalized_key[TP_SRCKEY_MAX];
    tp_status status = tp_srckey_normalize(src_key, normalized_key,
                                           sizeof normalized_key, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    char canonical_name[TP_PACK_INTERNAL_NAME_CAP];
    status = tp_pack_input_format_sprite_name(source_id, normalized_key,
                                              canonical_name,
                                              sizeof canonical_name, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    char *internal_name = tp_strdup(canonical_name);
    d->name = internal_name;
    d->path = tp_strdup(abs_path);
    d->source_id = source_id;
    d->source_key = tp_strdup(normalized_key);
    char *logical_name = NULL;
    status = export_key_dup(normalized_key, &logical_name, err);
    d->logical_name = logical_name;
    d->origin_x = TP_PROJECT_ORIGIN_DEFAULT;
    d->origin_y = TP_PROJECT_ORIGIN_DEFAULT;
    if (status != TP_STATUS_OK || !d->name || !d->path || !d->source_key) {
        free((void *)d->name);
        free((void *)d->path);
        free((void *)d->source_key);
        free((void *)d->logical_name);
        memset(d, 0, sizeof *d);
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_OOM,
                                  "pack input sprite string allocation failed");
    }
    const tp_project_sprite *ov =
        tp_project_atlas_find_sprite_by_source_key(
            (tp_project_atlas *)a, source_id, normalized_key);
    if (ov) {
        status = tp_project_validate_sprite_pack_overrides(ov, err);
        if (status != TP_STATUS_OK) {
            free((void *)d->name);
            free((void *)d->path);
            free((void *)d->source_key);
            free((void *)d->logical_name);
            memset(d, 0, sizeof *d);
            return status;
        }
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
    return TP_STATUS_OK;
}

static void desc_vec_free(desc_vec *dv) {
    for (int i = 0; i < dv->n; i++) {
        free((void *)dv->v[i].name);
        free((void *)dv->v[i].path);
        free((void *)dv->v[i].source_key);
        free((void *)dv->v[i].logical_name);
    }
    free(dv->v);
    dv->v = NULL;
    dv->n = 0;
    dv->cap = 0;
}

tp_status tp_pack_input_build(const tp_project *p, int atlas_index, tp_pack_input *out, tp_error *err) {
    return tp_pack_input_build_cancellable(p, atlas_index, out, NULL, err);
}

tp_status tp_pack_input_build_cancellable(const tp_project *p, int atlas_index,
                                          tp_pack_input *out,
                                          const tp_cancel_token *cancel,
                                          tp_error *err) {
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
    tp_status canonical = tp_project_validate_canonical(p, err);
    if (canonical != TP_STATUS_OK) {
        return canonical;
    }
    const tp_project_atlas *a = &p->atlases[atlas_index];

    desc_vec dv = {0};
    int missing = 0;
    for (int si = 0; si < a->source_count; si++) {
        /* Poll between sources: a long source list (or a cancel raised while the
         * previous folder walked) stops here rather than resolving/scanning more. */
        if (tp_cancel_requested(cancel)) {
            desc_vec_free(&dv);
            return tp_error_set(err, TP_STATUS_CANCELLED,
                                "pack input build cancelled");
        }
        const char *src_path = a->sources[si].path;
        char abs[TP_IDENTITY_PATH_MAX];
        tp_status resolve_status =
            tp_project_resolve_source_path(p, src_path, abs, sizeof abs);
        if (resolve_status != TP_STATUS_OK) {
            desc_vec_free(&dv);
            return tp_error_set(err, resolve_status,
                                "tp_pack_input_build: source path %d could not be resolved",
                                si);
        }
        if (!tp_scan_exists(abs)) {
            missing++;
            continue;
        }
        if (tp_scan_is_dir(abs)) {
            /* Folder: recurse (entries already sorted by rel) and append in scan
             * order. NO global sort across sources -- layout depends on input order.
             * The walk polls `cancel` per entry; a cancelled walk returns
             * TP_STATUS_CANCELLED, freeing the partial input below. */
            tp_scan_result sc = {0};
            tp_status scan_status = tp_scan_dir_cancellable(abs, &sc, cancel, err);
            if (scan_status == TP_STATUS_NOT_FOUND) {
                missing++;
                continue;
            }
            if (scan_status != TP_STATUS_OK) {
                desc_vec_free(&dv);
                return scan_status;
            }
            for (int ci = 0; ci < sc.count; ci++) {
                tp_status status = desc_add(
                    &dv, a, a->sources[si].id, sc.entries[ci].rel,
                    sc.entries[ci].abs, err);
                if (status != TP_STATUS_OK) {
                    tp_scan_free(&sc);
                    desc_vec_free(&dv);
                    return status;
                }
            }
            tp_scan_free(&sc);
        } else {
            const char *key = tp_path_basename(src_path);
            tp_status status = desc_add(&dv, a, a->sources[si].id, key, abs,
                                        err);
            if (status != TP_STATUS_OK) {
                desc_vec_free(&dv);
                return status;
            }
        }
    }

    out->descs = dv.v;
    out->count = dv.n;
    out->missing_sources = missing;
    return TP_STATUS_OK;
}

tp_status tp_pack_input_build_snapshot(const tp_session_snapshot *snapshot,
                                       tp_id128 atlas_id, tp_pack_input *out,
                                       tp_error *err) {
    return tp_pack_input_build_snapshot_cancellable(snapshot, atlas_id, out, NULL,
                                                    err);
}

tp_status tp_pack_input_build_snapshot_cancellable(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, tp_pack_input *out,
    const tp_cancel_token *cancel, tp_error *err) {
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "pack snapshot input requires snapshot");
    }
    const tp_project *project = tp_session_snapshot_project_internal(snapshot);
    const int atlas_index = tp_project_find_atlas_by_id(project, atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "pack snapshot atlas id was not found");
    }
    return tp_pack_input_build_cancellable(project, atlas_index, out, cancel, err);
}

tp_status tp_pack_settings_build_snapshot(const tp_session_snapshot *snapshot,
                                          tp_id128 atlas_id,
                                          tp_pack_settings *out,
                                          tp_error *err) {
    if (!snapshot || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "pack snapshot settings require snapshot and output");
    }
    const tp_project *project = tp_session_snapshot_project_internal(snapshot);
    const int atlas_index = tp_project_find_atlas_by_id(project, atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "pack snapshot atlas id was not found");
    }
    return tp_project_atlas_to_settings(project, atlas_index, out, err);
}

void tp_pack_input_free(tp_pack_input *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->count; i++) {
        free((void *)out->descs[i].name);
        free((void *)out->descs[i].path);
        free((void *)out->descs[i].source_key);
        free((void *)out->descs[i].logical_name);
    }
    free(out->descs);
    out->descs = NULL;
    out->count = 0;
    out->missing_sources = 0;
}
