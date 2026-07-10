#include "tp_core/tp_pack.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_core/tp_pack_read.h"

#include "nt_builder.h"

/* Reader recovers exact frame rects only for page dims <= 4096 (plan §2.5), and
 * that is also the builder's own texture-size cap (nt_builder.h:43). */
#define TP_PACK_MAX_PAGE_DIM NT_BUILD_MAX_TEXTURE_SIZE

// #region validation
/* Normalization-invariant per plan §5: reject anything nt_builder_normalize_path
 * would rewrite, since that would desync the atlas blob's raw "<atlas>/texN"
 * hash from the normalized entry-table hash and miss every page lookup (R2). */
static tp_status validate_atlas_name(const char *name, tp_error *err) {
    if (!name || name[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: atlas_name is required (non-empty)");
    }
    size_t len = strlen(name);
    if (strchr(name, '\\')) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: atlas_name '%s' contains a backslash", name);
    }
    if (strstr(name, "..")) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: atlas_name '%s' contains '..'", name);
    }
    if (strstr(name, "//")) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: atlas_name '%s' contains '//'", name);
    }
    if (strstr(name, "./")) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: atlas_name '%s' contains './'", name);
    }
    if (name[0] == '/') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: atlas_name '%s' has a leading '/'", name);
    }
    if (name[len - 1] == '/') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: atlas_name '%s' has a trailing '/'", name);
    }
    return TP_STATUS_OK;
}

/* Every check here prevents a downstream NT_BUILD_ASSERT (builder aborts on bad
 * input); tp_core must return a status instead of crashing. */
static tp_status validate_settings(const tp_pack_settings *s, tp_error *err) {
    if (!s) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: settings is NULL");
    }
    tp_status st = validate_atlas_name(s->atlas_name, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    if (!s->work_dir || s->work_dir[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: work_dir is required (non-empty)");
    }
    if (!s->sprites || s->sprite_count <= 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: at least one sprite is required");
    }
    if (s->max_size < 1 || s->max_size > TP_PACK_MAX_PAGE_DIM) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: max_size %d out of range [1..%d]", s->max_size,
                            (int)TP_PACK_MAX_PAGE_DIM);
    }
    if (s->padding < 0 || s->margin < 0 || s->extrude < 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: padding/margin/extrude must be >= 0");
    }
    if (s->alpha_threshold < 0 || s->alpha_threshold > 255) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: alpha_threshold %d out of range [0..255]",
                            s->alpha_threshold);
    }
    if (s->max_vertices < 1 || s->max_vertices > 16) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: max_vertices %d out of range [1..16]",
                            s->max_vertices);
    }
    if (s->shape < NT_ATLAS_SHAPE_RECT || s->shape > NT_ATLAS_SHAPE_CONCAVE_CONTOUR) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: shape %d out of range [0..2]", s->shape);
    }
    if (s->extrude > 0 && s->shape != NT_ATLAS_SHAPE_RECT) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: extrude > 0 is only valid for shape RECT (got shape %d)", s->shape);
    }
    if (!(s->pixels_per_unit > 0.0f) || !isfinite(s->pixels_per_unit)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: pixels_per_unit must be positive and finite");
    }

    for (int i = 0; i < s->sprite_count; i++) {
        const tp_pack_sprite_desc *sp = &s->sprites[i];
        if (!sp->name || sp->name[0] == '\0') {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: sprite %d has no name", i);
        }
        if (!isfinite(sp->origin_x) || !isfinite(sp->origin_y)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: sprite '%s' pivot must be finite", sp->name);
        }
        if (sp->path) {
            /* Builder asserts on a missing/unreadable file -- pre-check instead. */
            FILE *f = fopen(sp->path, "rb");
            if (!f) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: sprite '%s' cannot open file '%s'",
                                    sp->name, sp->path);
            }
            (void)fclose(f);
        } else {
            if (!sp->rgba) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                    "tp_pack: sprite '%s' has neither path nor rgba pixels", sp->name);
            }
            if (sp->w < 1 || sp->h < 1 || sp->w > TP_PACK_MAX_PAGE_DIM || sp->h > TP_PACK_MAX_PAGE_DIM) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: sprite '%s' size %dx%d out of range",
                                    sp->name, sp->w, sp->h);
            }
        }
        /* Unique names: the name map treats identical re-inserts as a no-op, so a
         * duplicate name would slip through to the builder's abort (nt_builder_
         * atlas.c:1604-1609). Reject here (O(n^2); sprite counts are modest). */
        for (int j = 0; j < i; j++) {
            if (strcmp(sp->name, s->sprites[j].name) == 0) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: duplicate sprite name '%s'", sp->name);
            }
        }
    }
    return TP_STATUS_OK;
}
// #endregion

/* tp_pack_settings_defaults() moved to tp_core (src/tp_pack_settings.c): the project
 * model needs it and the GUI links tp_core without the builder (#282). */

/* Build the reverse map the reader needs to resolve region name_hash -> name
 * (plan §2.8): atlas display name + every sprite name. Distinct names that hash
 * equal surface as TP_STATUS_HASH_COLLISION. Caller owns/destroys the map. */
static tp_status build_name_map(const tp_pack_settings *s, tp_name_map **out_map, tp_error *err) {
    *out_map = NULL;
    tp_name_map *map = tp_name_map_create();
    if (!map) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_pack: name map alloc failed");
    }
    tp_status st = tp_name_map_insert(map, s->atlas_name);
    if (st != TP_STATUS_OK) {
        tp_name_map_destroy(map);
        return tp_error_set(err, st, "tp_pack: atlas name '%s' hash collision", s->atlas_name);
    }
    for (int i = 0; i < s->sprite_count; i++) {
        st = tp_name_map_insert(map, s->sprites[i].name);
        if (st != TP_STATUS_OK) {
            tp_name_map_destroy(map);
            return tp_error_set(err, st, "tp_pack: sprite name '%s' hash collision", s->sprites[i].name);
        }
    }
    *out_map = map;
    return TP_STATUS_OK;
}

/* Drive nt_builder for one atlas into `path`. Single-threaded (determinism). */
static tp_status run_builder(const tp_pack_settings *s, const char *path, tp_error *err) {
    NtBuilderContext *ctx = nt_builder_start_pack(path);
    if (!ctx) {
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "tp_pack: nt_builder_start_pack('%s') failed (bad work_dir?)", path);
    }

    /* §5 export-friendly profile + caller knobs. */
    nt_atlas_opts_t o = nt_atlas_opts_defaults();
    o.premultiplied = false; /* straight alpha (R3: expected NT_LOG_WARN, non-fatal) */
    o.compress = NULL;       /* RAW RGBA8 -- reader requires it (§2.3) */
    o.gen_mipmaps = false;
    o.format = NT_TEXTURE_FORMAT_RGBA8;
    o.debug_png = false;
    o.max_size = (uint32_t)s->max_size;
    o.padding = (uint32_t)s->padding;
    o.margin = (uint32_t)s->margin;
    o.extrude = (uint32_t)s->extrude;
    o.alpha_threshold = (uint8_t)s->alpha_threshold;
    o.max_vertices = (uint8_t)s->max_vertices;
    o.shape = (nt_atlas_shape_t)s->shape;
    o.allow_transform = s->allow_transform;
    o.power_of_two = s->power_of_two;
    o.pixels_per_unit = s->pixels_per_unit;

    nt_builder_begin_atlas(ctx, s->atlas_name, &o);
    for (int i = 0; i < s->sprite_count; i++) {
        const tp_pack_sprite_desc *sp = &s->sprites[i];
        nt_atlas_sprite_opts_t so = nt_atlas_sprite_opts_defaults();
        so.name = sp->name;
        so.origin_x = sp->origin_x;
        so.origin_y = sp->origin_y;
        so.slice9_left = sp->slice9_lrtb[0];
        so.slice9_right = sp->slice9_lrtb[1];
        so.slice9_top = sp->slice9_lrtb[2];
        so.slice9_bottom = sp->slice9_lrtb[3];
        if (sp->path) {
            nt_builder_atlas_add(ctx, sp->path, &so);
        } else {
            nt_builder_atlas_add_raw(ctx, sp->rgba, (uint32_t)sp->w, (uint32_t)sp->h, &so);
        }
    }
    nt_builder_end_atlas(ctx);

    nt_build_result_t br = nt_builder_finish_pack(ctx);
    nt_builder_free_pack(ctx); /* always, per nt_builder.h lifecycle contract */
    if (br != NT_BUILD_OK) {
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED, "tp_pack: nt_builder_finish_pack failed (code %d) for '%s'",
                            (int)br, s->atlas_name);
    }
    return TP_STATUS_OK;
}

tp_status tp_pack(const tp_pack_settings *settings, struct tp_arena *arena, struct tp_result **out_result,
                  tp_error *err) {
    if (out_result) {
        *out_result = NULL;
    }
    if (!arena || !out_result) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: arena and out_result are required");
    }

    tp_status st = validate_settings(settings, err);
    if (st != TP_STATUS_OK) {
        return st;
    }

    char path[512]; /* NtBuilderContext.output_path is char[512]; avoid silent truncation */
    int n = snprintf(path, sizeof path, "%s/%s.ntpack", settings->work_dir, settings->atlas_name);
    if (n < 0 || (size_t)n >= sizeof path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: work_dir + atlas_name path too long");
    }

    tp_name_map *names = NULL;
    st = build_name_map(settings, &names, err);
    if (st != TP_STATUS_OK) {
        return st;
    }

    st = run_builder(settings, path, err);
    if (st != TP_STATUS_OK) {
        tp_name_map_destroy(names);
        return st;
    }

    tp_result **results = NULL;
    int count = 0;
    st = tp_pack_read_file(path, names, arena, &results, &count, err);
    /* Reader copies names/pixels into `arena`; the map is no longer referenced. */
    tp_name_map_destroy(names);
    if (st != TP_STATUS_OK) {
        return st;
    }
    if (count != 1 || !results || !results[0]) {
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED, "tp_pack: session pack held %d atlases, expected 1", count);
    }

    /* tp_pack_read already sorts sprites ascending by name (tp_pack_read.c:502),
     * so the result is deterministically name-ordered -- do not re-sort. */
    *out_result = results[0];
    return TP_STATUS_OK;
}
