#include "tp_core/tp_pack.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_image.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_core/tp_pack_read.h"

#include "nt_builder.h"

/* Reader recovers exact frame rects only for page dims <= 4096 (plan §2.5), and
 * that is also the builder's own texture-size cap (nt_builder.h:43). */
/* The desc override values (tp_pack.h) mirror the engine encoding 1:1. */
_Static_assert(TP_PACK_SPRITE_SHAPE_RECT == NT_ATLAS_SPRITE_SHAPE_RECT, "sprite shape RECT encoding");
_Static_assert(TP_PACK_SPRITE_SHAPE_CONVEX == NT_ATLAS_SPRITE_SHAPE_CONVEX, "sprite shape CONVEX encoding");
_Static_assert(TP_PACK_SPRITE_SHAPE_CONCAVE == NT_ATLAS_SPRITE_SHAPE_CONCAVE, "sprite shape CONCAVE encoding");
_Static_assert(TP_PACK_SPRITE_ROTATE_NO == NT_ATLAS_SPRITE_ROTATE_NO, "sprite allow_rotate NO encoding");

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
    if ((unsigned int)s->sprite_count > UINT16_MAX) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: sprite_count %d exceeds %u regions",
                            s->sprite_count, (unsigned int)UINT16_MAX);
    }
    if (!tp_pack_max_size_valid(s->max_size)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: max_size %d out of range [1..%d]", s->max_size,
                            (int)TP_PACK_MAX_PAGE_DIM);
    }
    if (!tp_pack_nonnegative_valid(s->padding) ||
        !tp_pack_nonnegative_valid(s->margin) ||
        !tp_pack_nonnegative_valid(s->extrude)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: padding/margin/extrude must be >= 0");
    }
    if (s->padding > s->max_size || s->margin > s->max_size ||
        s->extrude > s->max_size) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: padding/margin/extrude must not exceed max_size %d",
                            s->max_size);
    }
    if (!tp_pack_alpha_threshold_valid(s->alpha_threshold)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: alpha_threshold %d out of range [0..255]",
                            s->alpha_threshold);
    }
    if (!tp_pack_max_vertices_valid(s->max_vertices)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: max_vertices %d out of range [1..16]",
                            s->max_vertices);
    }
    if (!tp_pack_shape_valid(s->shape)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: shape %d out of range [0..2]", s->shape);
    }
    if (!tp_pack_extrude_shape_valid(s->extrude, s->shape)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: extrude > 0 is only valid for shape RECT (got shape %d)", s->shape);
    }
    if (!tp_pack_pixels_per_unit_valid(s->pixels_per_unit)) {
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
        if (((uint32_t)sp->ov_mask & ~(uint32_t)TP_PACK_OV_ALL) != 0U) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' ov_mask contains unknown bits 0x%02x",
                                sp->name, (unsigned int)sp->ov_mask);
        }
        /* Per-sprite override validation (owner scope 2026-07-10). Every check here
         * prevents a downstream NT_BUILD_ASSERT and names the sprite. Effective
         * shape = slice9 forces RECT, else the sprite shape override, else the atlas
         * shape; effective extrude = sprite override else the atlas extrude. */
        if ((sp->ov_mask & TP_PACK_OV_SHAPE) &&
            (sp->ov_shape < TP_PACK_SPRITE_SHAPE_RECT || sp->ov_shape > TP_PACK_SPRITE_SHAPE_CONCAVE)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: sprite '%s' shape override %d invalid",
                                sp->name, (int)sp->ov_shape);
        }
        if ((sp->ov_mask & TP_PACK_OV_ROTATE) && sp->ov_allow_rotate != TP_PACK_SPRITE_ROTATE_NO) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' allow_rotate override %d invalid (only no-rotate)", sp->name,
                                (int)sp->ov_allow_rotate);
        }
        if ((sp->ov_mask & TP_PACK_OV_MAXVERT) &&
            !tp_pack_max_vertices_valid(sp->ov_max_vertices)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' max_vertices override %d out of range [1..16]", sp->name,
                                (int)sp->ov_max_vertices);
        }
        if ((sp->ov_mask & TP_PACK_OV_MARGIN) && sp->ov_margin == 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' margin override 0 unrepresentable (omit to inherit, or use >= 1)",
                                sp->name);
        }
        if ((sp->ov_mask & TP_PACK_OV_EXTRUDE) && sp->ov_extrude == 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' extrude override 0 unrepresentable (omit to inherit, or use >= 1)",
                                sp->name);
        }
        {
            const bool slice9 = sp->slice9_lrtb[0] || sp->slice9_lrtb[1] || sp->slice9_lrtb[2] || sp->slice9_lrtb[3];
            bool eff_rect;
            if (slice9) {
                if ((sp->ov_mask & TP_PACK_OV_SHAPE) &&
                    sp->ov_shape != TP_PACK_SPRITE_SHAPE_RECT) {
                    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                        "tp_pack: sprite '%s' slice9 requires a RECT shape override",
                                        sp->name);
                }
                eff_rect = true; /* engine auto-forces RECT for slice9 sprites */
            } else if (sp->ov_mask & TP_PACK_OV_SHAPE) {
                eff_rect = (sp->ov_shape == TP_PACK_SPRITE_SHAPE_RECT);
            } else {
                eff_rect = (s->shape == NT_ATLAS_SHAPE_RECT);
            }
            const int eff_extrude = (sp->ov_mask & TP_PACK_OV_EXTRUDE) ? (int)sp->ov_extrude : s->extrude;
            if (eff_extrude > 0 && !eff_rect) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                    "tp_pack: sprite '%s' effective extrude %d requires effective RECT shape",
                                    sp->name, eff_extrude);
            }
        }
        if (!sp->path) {
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

static void free_loaded_images(tp_image_rgba8 *images, int count) {
    if (!images) {
        return;
    }
    for (int i = 0; i < count; i++) {
        tp_image_free(&images[i]);
    }
    free(images);
}

typedef struct tp_pack_area_entry {
    uint64_t content_key;
    uint64_t opaque_pixels;
} tp_pack_area_entry;

/* FNV-1a is only an admission bucketing key, never an identity.  A collision
 * deliberately merges buckets and therefore under-counts the lower bound; it
 * cannot make this guard reject a pack that the builder could accept. */
static uint64_t area_key_add(uint64_t key, const void *bytes, size_t size) {
    const uint8_t *p = (const uint8_t *)bytes;
    for (size_t i = 0; i < size; i++) {
        key ^= p[i];
        key *= UINT64_C(1099511628211);
    }
    return key;
}

static int area_entry_compare(const void *lhs, const void *rhs) {
    const tp_pack_area_entry *a = (const tp_pack_area_entry *)lhs;
    const tp_pack_area_entry *b = (const tp_pack_area_entry *)rhs;
    if (a->content_key < b->content_key) {
        return -1;
    }
    if (a->content_key > b->content_key) {
        return 1;
    }
    return 0;
}

static tp_status validate_page_area_lower_bound(const tp_pack_settings *settings,
                                                tp_pack_area_entry *entries,
                                                tp_error *err) {
    qsort(entries, (size_t)settings->sprite_count, sizeof *entries,
          area_entry_compare);

    uint64_t minimum_opaque_pixels = 0;
    for (int first = 0; first < settings->sprite_count;) {
        int end = first + 1;
        uint64_t group_minimum = entries[first].opaque_pixels;
        while (end < settings->sprite_count &&
               entries[end].content_key == entries[first].content_key) {
            if (entries[end].opaque_pixels < group_minimum) {
                group_minimum = entries[end].opaque_pixels;
            }
            end++;
        }
        minimum_opaque_pixels += group_minimum;
        first = end;
    }

    const uint64_t page_area =
        (uint64_t)(unsigned int)settings->max_size *
        (uint64_t)(unsigned int)settings->max_size;
    const uint64_t total_capacity = page_area * TP_PACK_MAX_PAGES;
    if (minimum_opaque_pixels > total_capacity) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "tp_pack: unique opaque-pixel lower bound %llu exceeds %d pages of %dx%d",
            (unsigned long long)minimum_opaque_pixels, TP_PACK_MAX_PAGES,
            settings->max_size, settings->max_size);
    }
    return TP_STATUS_OK;
}

static tp_status preflight_sprite_pixels(const tp_pack_settings *settings,
                                         const tp_pack_sprite_desc *sprite,
                                         const uint8_t *pixels, int width,
                                         int height,
                                         tp_pack_area_entry *area_entry,
                                         tp_error *err) {
    bool found = false;
    uint64_t opaque_pixels = 0;
    int min_x = width;
    int min_y = height;
    int max_x = 0;
    int max_y = 0;
    const uint8_t threshold = (uint8_t)settings->alpha_threshold;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            size_t offset =
                ((size_t)y * (size_t)width + (size_t)x) * 4U + 3U;
            if (pixels[offset] >= threshold) {
                opaque_pixels++;
                if (x < min_x) {
                    min_x = x;
                }
                if (x > max_x) {
                    max_x = x;
                }
                if (y < min_y) {
                    min_y = y;
                }
                if (y > max_y) {
                    max_y = y;
                }
                found = true;
            }
        }
    }
    if (!found) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: sprite '%s' is fully transparent at alpha_threshold %u",
                            sprite->name, (unsigned int)threshold);
    }

    const bool slice9 = sprite->slice9_lrtb[0] || sprite->slice9_lrtb[1] ||
                        sprite->slice9_lrtb[2] || sprite->slice9_lrtb[3];
    if ((uint32_t)sprite->slice9_lrtb[0] +
            (uint32_t)sprite->slice9_lrtb[1] >=
        (uint32_t)width) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: sprite '%s' slice9 left+right must be less than width %d",
                            sprite->name, width);
    }
    if ((uint32_t)sprite->slice9_lrtb[2] +
            (uint32_t)sprite->slice9_lrtb[3] >=
        (uint32_t)height) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: sprite '%s' slice9 top+bottom must be less than height %d",
                            sprite->name, height);
    }

    uint64_t trim_width =
        slice9 ? (uint64_t)(unsigned int)width
               : (uint64_t)(unsigned int)(max_x - min_x + 1);
    uint64_t trim_height =
        slice9 ? (uint64_t)(unsigned int)height
               : (uint64_t)(unsigned int)(max_y - min_y + 1);

    uint64_t content_key = UINT64_C(14695981039346656037);
    const uint32_t key_width = (uint32_t)trim_width;
    const uint32_t key_height = (uint32_t)trim_height;
    content_key = area_key_add(content_key, &key_width, sizeof key_width);
    content_key = area_key_add(content_key, &key_height, sizeof key_height);
    for (uint32_t row = 0; row < key_height; row++) {
        const size_t offset =
            (((size_t)(slice9 ? 0 : min_y) + row) * (size_t)width +
             (size_t)(slice9 ? 0 : min_x)) *
            4U;
        content_key = area_key_add(content_key, pixels + offset,
                                   (size_t)key_width * 4U);
    }
    area_entry->content_key = content_key;
    area_entry->opaque_pixels = opaque_pixels;

    const uint64_t sprite_margin =
        (sprite->ov_mask & TP_PACK_OV_MARGIN) ? sprite->ov_margin
                                              : (uint64_t)settings->margin;
    const uint64_t sprite_extrude =
        (sprite->ov_mask & TP_PACK_OV_EXTRUDE) ? sprite->ov_extrude
                                               : (uint64_t)settings->extrude;
    const uint64_t extra_margin =
        sprite_margin > (uint64_t)settings->margin
            ? sprite_margin - (uint64_t)settings->margin
            : 0U;
    const uint64_t extra_extrude =
        sprite_extrude > (uint64_t)settings->extrude
            ? sprite_extrude - (uint64_t)settings->extrude
            : 0U;
    const uint64_t extra = 2U * (extra_margin + extra_extrude);
    trim_width += extra;
    trim_height += extra;

    /* Mirror the builder's empty-page candidate bounds before invoking it.
     * vpack inflates every polygon by atlas extrude + padding/2, starts at
     * max(margin, extrude), reserves the far-side margin, and treats polygon
     * coordinates as inclusive (`... - aabb_max - 1`).  Ceil the half-padding
     * to fail closed at integer raster coordinates. Per-sprite overrides above
     * expand the trim rectangle only by the amount beyond the atlas baseline,
     * exactly as pipeline_tile_pack does. */
    const uint64_t inflate =
        (uint64_t)settings->extrude +
        ((uint64_t)settings->padding + 1U) / 2U;
    const uint64_t min_edge =
        settings->margin > settings->extrude
            ? (uint64_t)settings->margin
            : (uint64_t)settings->extrude;
    const uint64_t fixed_edges =
        2U * inflate + min_edge + (uint64_t)settings->margin + 1U;
    const uint64_t required_width = trim_width + fixed_edges;
    const uint64_t required_height = trim_height + fixed_edges;
    if (required_width > (uint64_t)settings->max_size ||
        required_height > (uint64_t)settings->max_size) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: sprite '%s' footprint %llux%llu cannot fit max_size %d",
                            sprite->name, (unsigned long long)required_width,
                            (unsigned long long)required_height,
                            settings->max_size);
    }
    return TP_STATUS_OK;
}

static tp_status load_path_images(const tp_pack_settings *settings,
                                  tp_image_rgba8 **out_images,
                                  tp_error *err) {
    *out_images = NULL;
    bool has_path = false;
    for (int i = 0; i < settings->sprite_count; i++) {
        if (settings->sprites[i].path) {
            has_path = true;
            break;
        }
    }
    tp_image_rgba8 *images = NULL;
    tp_pack_area_entry *area_entries =
        (tp_pack_area_entry *)malloc((size_t)settings->sprite_count *
                                     sizeof *area_entries);
    if (!area_entries) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "tp_pack: page preflight allocation failed");
    }
    if (has_path) {
        if ((size_t)settings->sprite_count >
            SIZE_MAX / sizeof(tp_image_rgba8)) {
            free(area_entries);
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "tp_pack: path image table size overflows");
        }
        images = (tp_image_rgba8 *)calloc((size_t)settings->sprite_count,
                                          sizeof *images);
        if (!images) {
            free(area_entries);
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_pack: path image table allocation failed");
        }
    }
    for (int i = 0; i < settings->sprite_count; i++) {
        const tp_pack_sprite_desc *sprite = &settings->sprites[i];
        const uint8_t *pixels = sprite->rgba;
        int width = sprite->w;
        int height = sprite->h;
        if (sprite->path) {
            tp_error cause = {{0}};
            tp_status status =
                tp_image_load_file(sprite->path, &images[i], &cause);
            if (status != TP_STATUS_OK) {
                free(area_entries);
                free_loaded_images(images, settings->sprite_count);
                return tp_error_set(err, status, "tp_pack: sprite '%s': %s",
                                    sprite->name, cause.msg);
            }
            pixels = images[i].pixels;
            width = images[i].width;
            height = images[i].height;
        }
        tp_status status = preflight_sprite_pixels(
            settings, sprite, pixels, width, height, &area_entries[i], err);
        if (status != TP_STATUS_OK) {
            free(area_entries);
            free_loaded_images(images, settings->sprite_count);
            return status;
        }
    }
    tp_status area_status =
        validate_page_area_lower_bound(settings, area_entries, err);
    free(area_entries);
    if (area_status != TP_STATUS_OK) {
        free_loaded_images(images, settings->sprite_count);
        return area_status;
    }
    *out_images = images;
    return TP_STATUS_OK;
}

/* Drive nt_builder for one atlas into `path`. Threaded encode, sequential assembly
 * (nt_builder_set_threads_auto): determinism is covered by the engine's own threaded
 * tests plus our byte-identical determinism suite (test_pack / test_export_*). */
static tp_status run_builder(const tp_pack_settings *s, const char *path, tp_error *err) {
    tp_image_rgba8 *path_images = NULL;
    tp_status load_status = load_path_images(s, &path_images, err);
    if (load_status != TP_STATUS_OK) {
        return load_status;
    }

    NtBuilderContext *ctx = nt_builder_start_pack(path);
    if (!ctx) {
        free_loaded_images(path_images, s->sprite_count);
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "tp_pack: nt_builder_start_pack('%s') failed (bad work_dir?)", path);
    }
    nt_builder_set_threads_auto(ctx); /* parallel encode; assembly stays sequential */

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
        /* Per-sprite overrides: 0 stays "inherit atlas" (engine encoding); we only
         * write a field when its mask bit is set (validated above). */
        if (sp->ov_mask & TP_PACK_OV_SHAPE) {
            so.shape = sp->ov_shape;
        }
        if (sp->ov_mask & TP_PACK_OV_ROTATE) {
            so.allow_rotate = sp->ov_allow_rotate;
        }
        if (sp->ov_mask & TP_PACK_OV_MAXVERT) {
            so.max_vertices = sp->ov_max_vertices;
        }
        if (sp->ov_mask & TP_PACK_OV_MARGIN) {
            so.margin = sp->ov_margin;
        }
        if (sp->ov_mask & TP_PACK_OV_EXTRUDE) {
            so.extrude = sp->ov_extrude;
        }
        if (sp->path) {
            nt_builder_atlas_add_raw(ctx, path_images[i].pixels,
                                     (uint32_t)path_images[i].width,
                                     (uint32_t)path_images[i].height, &so);
        } else {
            nt_builder_atlas_add_raw(ctx, sp->rgba, (uint32_t)sp->w, (uint32_t)sp->h, &so);
        }
    }
    nt_builder_end_atlas(ctx);

    /* nt_builder_atlas_add_raw deep-copies every image before returning, so the
     * pack-job-owned decode buffers can be released before encode/assembly. */
    free_loaded_images(path_images, s->sprite_count);

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
