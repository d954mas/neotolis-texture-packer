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
#include "tp_pack_constraints_internal.h"
#include "tp_pack_priv.h"
#include "tp_build_worker_internal.h"

/* nt_builder is confined to the driver TU (tp_build_driver.c), which now runs in
 * a private child process behind tp_build_worker_run (decision 0018, ROADMAP
 * H0.3-b): tp_pack keeps validate/preflight/name-map/read-back and hands decoded
 * pixels to the worker, so a builder abort/allocation/codec/write failure cannot
 * terminate the host. */

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
    const tp_pack_atlas_constraint_input atlas_input = {
        .max_size = s->max_size,
        .padding = s->padding,
        .margin = s->margin,
        .extrude = s->extrude,
        .alpha_threshold = s->alpha_threshold,
        .max_vertices = s->max_vertices,
        .shape = s->shape,
        .pixels_per_unit = s->pixels_per_unit,
    };
    const tp_pack_atlas_constraint_facts atlas_facts =
        tp_pack_atlas_constraint_facts_of(&atlas_input);
    if (atlas_facts.max_size_out_of_range) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: max_size %d out of range [1..%d]", s->max_size,
                            (int)TP_PACK_MAX_PAGE_DIM);
    }
    if (atlas_facts.padding_negative || atlas_facts.margin_negative ||
        atlas_facts.extrude_negative) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: padding/margin/extrude must be >= 0");
    }
    if (atlas_facts.padding_exceeds_max_size ||
        atlas_facts.margin_exceeds_max_size ||
        atlas_facts.extrude_exceeds_max_size) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: padding/margin/extrude must not exceed max_size %d",
                            s->max_size);
    }
    if (atlas_facts.alpha_threshold_out_of_range) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: alpha_threshold %d out of range [0..255]",
                            s->alpha_threshold);
    }
    if (atlas_facts.max_vertices_out_of_range) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: max_vertices %d out of range [1..16]",
                            s->max_vertices);
    }
    if (atlas_facts.shape_out_of_range) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: shape %d out of range [0..2]", s->shape);
    }
    if (atlas_facts.extrude_requires_rect) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack: extrude > 0 is only valid for shape RECT (got shape %d)", s->shape);
    }
    if (atlas_facts.pixels_per_unit_out_of_range) {
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
        const bool slice9 = sp->slice9_lrtb[0] || sp->slice9_lrtb[1] ||
                            sp->slice9_lrtb[2] || sp->slice9_lrtb[3];
        const tp_pack_sprite_constraint_input sprite_input = {
            .atlas_max_size = s->max_size,
            .atlas_shape = s->shape,
            .atlas_extrude = s->extrude,
            .has_slice9 = slice9,
            .has_shape = (sp->ov_mask & TP_PACK_OV_SHAPE) != 0U,
            .shape = (int)sp->ov_shape -
                     (int)TP_PACK_SPRITE_SHAPE_RECT + TP_PACK_SHAPE_MIN,
            .has_allow_rotate = (sp->ov_mask & TP_PACK_OV_ROTATE) != 0U,
            .allow_rotate =
                sp->ov_allow_rotate == TP_PACK_SPRITE_ROTATE_NO ? 0 : 1,
            .has_max_vertices = (sp->ov_mask & TP_PACK_OV_MAXVERT) != 0U,
            .max_vertices = sp->ov_max_vertices,
            .has_margin = (sp->ov_mask & TP_PACK_OV_MARGIN) != 0U,
            .margin = sp->ov_margin,
            .has_extrude = (sp->ov_mask & TP_PACK_OV_EXTRUDE) != 0U,
            .extrude = sp->ov_extrude,
        };
        const tp_pack_sprite_constraint_facts sprite_facts =
            tp_pack_sprite_constraint_facts_of(&sprite_input);
        /* Per-sprite override validation (owner scope 2026-07-10). Every check here
         * prevents a downstream NT_BUILD_ASSERT and names the sprite. Effective
         * shape = slice9 forces RECT, else the sprite shape override, else the atlas
         * shape; effective extrude = sprite override else the atlas extrude. */
        if (sprite_facts.shape_not_wire_representable) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack: sprite '%s' shape override %d invalid",
                                sp->name, (int)sp->ov_shape);
        }
        if (sprite_facts.allow_rotate_not_wire_representable) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' allow_rotate override %d invalid (only no-rotate)", sp->name,
                                (int)sp->ov_allow_rotate);
        }
        if (sprite_facts.max_vertices_not_wire_representable) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' max_vertices override %d out of range [1..16]", sp->name,
                                (int)sp->ov_max_vertices);
        }
        if (sprite_facts.margin_not_wire_representable) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' margin override 0 unrepresentable (omit to inherit, or use >= 1)",
                                sp->name);
        }
        if (sprite_facts.extrude_not_wire_representable) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_pack: sprite '%s' extrude override 0 unrepresentable (omit to inherit, or use >= 1)",
                                sp->name);
        }
        {
            if (slice9) {
                if (sprite_facts.slice9_shape_conflict) {
                    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                        "tp_pack: sprite '%s' slice9 requires a RECT shape override",
                                        sp->name);
                }
            }
            const int eff_extrude = (sp->ov_mask & TP_PACK_OV_EXTRUDE) ? (int)sp->ov_extrude : s->extrude;
            if (sprite_facts.effective_extrude_requires_rect) {
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
                                  tp_pack_image_observer observer,
                                  void *observer_ctx,
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
        /* Hand the just-resolved pixels to an in-process observer (pack_input_hash
         * folding) before the preflight -- the pack's single decode is the only one. */
        if (observer) {
            observer(observer_ctx, i, width, height, pixels);
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

tp_status tp_pack(const tp_pack_settings *settings, struct tp_arena *arena, struct tp_result **out_result,
                  tp_error *err) {
    return tp_pack_cancellable(settings, arena, out_result, NULL, NULL, err);
}

tp_status tp_pack_cancellable(const tp_pack_settings *settings, struct tp_arena *arena,
                              struct tp_result **out_result, tp_pack_cancel_poll cancel_poll,
                              void *cancel_ctx, tp_error *err) {
    return tp_pack_cancellable_observed(settings, arena, out_result, cancel_poll,
                                        cancel_ctx, NULL, NULL, err);
}

tp_status tp_pack_cancellable_observed(const tp_pack_settings *settings,
                                       struct tp_arena *arena,
                                       struct tp_result **out_result,
                                       tp_pack_cancel_poll cancel_poll,
                                       void *cancel_ctx,
                                       tp_pack_image_observer observer,
                                       void *observer_ctx, tp_error *err) {
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

    /* Decode path sources to canonical RGBA8 and run the page-area preflight in
     * the parent (the worker boundary owns UTF-8 reads + validation, decision
     * 0018); the driver then packs from raw pixels only. */
    tp_image_rgba8 *path_images = NULL;
    st = load_path_images(settings, observer, observer_ctx, &path_images, err);
    if (st != TP_STATUS_OK) {
        tp_name_map_destroy(names);
        return st;
    }

    /* The worker consumes `path_images` (frees them on every path). It packs in a
     * private child process, stages an ASCII artifact, and atomically publishes to
     * `path` (Unicode / long paths included). Cancellation kills the worker, cleans
     * staging, publishes nothing, and returns OK with no artifact -- there is
     * nothing to read back, so short-circuit to a benign cancelled result. */
    tp_build_worker_opts wopts;
    memset(&wopts, 0, sizeof wopts);
    bool cancelled = false;
    wopts.cancel_poll = cancel_poll;
    wopts.cancel_ctx = cancel_ctx;
    wopts.out_cancelled = &cancelled;
    st = tp_build_worker_run_opts(settings, path_images, path, &wopts, err);
    if (st != TP_STATUS_OK || cancelled) {
        tp_name_map_destroy(names);
        return st; /* on cancel st is TP_STATUS_OK and *out_result stays NULL */
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
