/* Packet S27 phase-1 premise gate: is a serialized `.ntpack` blob materially
 * smaller than the raw RGBA8 page bytes it inflates into?
 *
 * The serialized pack-result cache lifecycle only pays for itself if an INACTIVE
 * entry (holding the encoded blob) costs much less than the inflated pages it
 * replaces. This bench measures, per scale:
 *   raw_page_bytes  -- sum(page.w * page.h * 4) of the inflated tp_result
 *   blob_bytes      -- size of the `.ntpack` the builder wrote
 *   ratio           -- raw_page_bytes / blob_bytes  (>= 1.5 = premise holds)
 *   inflate_ms      -- tp_pack_read_memory(blob) -> arena, median of N runs
 *   png_*           -- counterfactual: what a page costs, and what it costs to
 *                      encode/decode, IF the stored form were actually
 *                      compressed. The `.ntpack` never stores pixels this way.
 *
 * ANSWER (2026-07-29, all scales, synthetic AND real CC0 art): ratio ~= 0.999.
 * `.ntpack` mip0 is uncompressed RGBA8 (docs/formats/ntpack-binary.md, and
 * tp_pack_read.c rejects anything but RAW+RGBA8), so the serialized blob is
 * marginally LARGER than the pages it inflates into. The premise fails.
 *
 * NOT a ctest: it drives the real nt_builder and allocates hundreds of MiB.
 *
 *   tp_bench_pack_blob <scratch_dir> [iterations] [bench_assets_root]
 */

#include "tp_bench_support.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"
#include "tp_build_driver_internal.h"
#include "tp_name_map.h"
#include "tp_pack_read.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Counterfactual only: what a page would cost if the artifact format DID encode
 * pixels. The shipped `.ntpack` stores mip0 as uncompressed RGBA8, so this
 * number is never the blob size -- it is the answer to "what would a compressed
 * serialized mode buy?" that the phase-1 gate raises when the raw ratio fails. */
#include "stb_image.h"
#include "stb_image_write.h"

typedef struct scale_spec {
    const char *name;
    int page_dim;   /* max_size; sprites are laid out to fill exactly one page */
    int tile;       /* synthetic sprite edge in px; 0 = real Kenney assets */
    int asset_count;/* real-asset mode: how many manifest rows to pack */
} scale_spec;

/* Opaque, non-uniform pixels: nothing here may be trimmed away, and a codec (if
 * one ever appears in the format) must not see a degenerate constant image. */
static uint8_t *make_tile(int tile, uint32_t seed) {
    const size_t bytes = (size_t)tile * (size_t)tile * 4U;
    uint8_t *px = (uint8_t *)malloc(bytes);
    if (!px) {
        return NULL;
    }
    uint32_t state = seed * 2654435761U + 1U;
    for (size_t i = 0; i < bytes; i += 4U) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        px[i + 0U] = (uint8_t)(state);
        px[i + 1U] = (uint8_t)(state >> 8);
        px[i + 2U] = (uint8_t)(state >> 16);
        px[i + 3U] = 255U; /* fully opaque: no alpha trim, no shape shrink */
    }
    return px;
}

static int64_t file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0L, SEEK_END) != 0) {
        if (file) {
            (void)fclose(file);
        }
        return -1;
    }
    const long value = ftell(file);
    (void)fclose(file);
    return value < 0L ? -1 : (int64_t)value;
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    *out_size = 0U;
    const int64_t size = file_size(path);
    if (size <= 0) {
        return NULL;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    uint8_t *bytes = (uint8_t *)malloc((size_t)size);
    if (!bytes || fread(bytes, 1U, (size_t)size, file) != (size_t)size) {
        free(bytes);
        (void)fclose(file);
        return NULL;
    }
    (void)fclose(file);
    *out_size = (size_t)size;
    return bytes;
}

static uint64_t raw_page_bytes(const tp_result *result) {
    uint64_t total = 0U;
    for (int i = 0; i < result->page_count; ++i) {
        total += (uint64_t)result->pages[i].w * (uint64_t)result->pages[i].h * 4U;
    }
    return total;
}

typedef struct png_sink {
    uint8_t *bytes;
    size_t size;
    bool failed;
} png_sink;

static void png_collect(void *context, void *data, int size) {
    png_sink *sink = (png_sink *)context;
    if (sink->failed || size <= 0) {
        return;
    }
    uint8_t *grown = (uint8_t *)realloc(sink->bytes, sink->size + (size_t)size);
    if (!grown) {
        sink->failed = true;
        return;
    }
    memcpy(grown + sink->size, data, (size_t)size);
    sink->bytes = grown;
    sink->size += (size_t)size;
}

/* Encode every page to PNG and decode them all back. Pure counterfactual: the
 * `.ntpack` never stores pixels this way, but these are the numbers a genuinely
 * compressed cold-entry lifecycle would live or die by. */
static uint64_t png_page_bytes(const tp_result *result, double *out_encode_ms,
                               double *out_decode_ms) {
    uint64_t total = 0U;
    double encode_ms = 0.0;
    double decode_ms = 0.0;
    for (int i = 0; i < result->page_count; ++i) {
        png_sink sink = {NULL, 0U, false};
        const double encode_start = tp_bench_now_ms();
        const int written = stbi_write_png_to_func(
            png_collect, &sink, result->pages[i].w, result->pages[i].h, 4,
            result->pages[i].rgba, result->pages[i].w * 4);
        encode_ms += tp_bench_now_ms() - encode_start;
        if (!written || sink.failed || sink.size == 0U) {
            free(sink.bytes);
            return 0U;
        }
        total += (uint64_t)sink.size;
        int w = 0;
        int h = 0;
        int comp = 0;
        const double decode_start = tp_bench_now_ms();
        stbi_uc *decoded = stbi_load_from_memory(sink.bytes, (int)sink.size, &w,
                                                 &h, &comp, 4);
        decode_ms += tp_bench_now_ms() - decode_start;
        free(sink.bytes);
        if (!decoded || w != result->pages[i].w || h != result->pages[i].h) {
            stbi_image_free(decoded);
            return 0U;
        }
        stbi_image_free(decoded);
    }
    *out_encode_ms = encode_ms;
    *out_decode_ms = decode_ms;
    return total;
}

/* Fill `descs` from real Kenney bench assets. Returns the number of sprites
 * actually loaded, 0 on failure. */
static int load_manifest_sprites(const char *assets_root, int wanted,
                                 tp_pack_sprite_desc *descs, char (*names)[32],
                                 uint8_t **tiles, int *widths, int *heights) {
    char manifest_path[1024];
    if (snprintf(manifest_path, sizeof manifest_path, "%s/manifest.tsv",
                 assets_root) < 0) {
        return 0;
    }
    FILE *manifest = fopen(manifest_path, "rb");
    if (!manifest) {
        (void)fprintf(stderr, "bench assets missing: %s\n", manifest_path);
        return 0;
    }
    char line[2048];
    int loaded = 0;
    while (loaded < wanted && fgets(line, sizeof line, manifest)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        char *tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        char asset_path[1400];
        if (snprintf(asset_path, sizeof asset_path, "%s/kenney/%s", assets_root,
                     line) < 0) {
            continue;
        }
        tp_image_rgba8 image = {0};
        tp_error err = {{0}};
        if (tp_image_load_file(asset_path, &image, &err) != TP_STATUS_OK) {
            continue;
        }
        tiles[loaded] = image.pixels; /* freed by the caller, not tp_image_free */
        widths[loaded] = image.width;
        heights[loaded] = image.height;
        (void)snprintf(names[loaded], sizeof names[loaded], "s%05d", loaded);
        descs[loaded].name = names[loaded];
        descs[loaded].rgba = tiles[loaded];
        descs[loaded].w = image.width;
        descs[loaded].h = image.height;
        descs[loaded].origin_x = 0.5F;
        descs[loaded].origin_y = 0.5F;
        loaded++;
    }
    (void)fclose(manifest);
    return loaded;
}

static bool run_scale(const scale_spec *spec, const char *scratch,
                      const char *assets_root, int iterations) {
    const bool synthetic = spec->tile > 0;
    const int per_row = synthetic ? spec->page_dim / spec->tile : 0;
    int sprite_count = synthetic ? per_row * per_row : spec->asset_count;
    tp_pack_sprite_desc *descs =
        (tp_pack_sprite_desc *)calloc((size_t)sprite_count, sizeof *descs);
    char (*names)[32] = (char (*)[32])calloc((size_t)sprite_count, sizeof *names);
    uint8_t **tiles = (uint8_t **)calloc((size_t)sprite_count, sizeof *tiles);
    int *widths = (int *)calloc((size_t)sprite_count, sizeof *widths);
    int *heights = (int *)calloc((size_t)sprite_count, sizeof *heights);
    if (!descs || !names || !tiles || !widths || !heights) {
        free(heights);
        free(widths);
        free(tiles);
        free(names);
        free(descs);
        return false;
    }
    bool ok = true;
    if (synthetic) {
        for (int i = 0; i < sprite_count && ok; ++i) {
            tiles[i] = make_tile(spec->tile, (uint32_t)i + 1U);
            ok = tiles[i] != NULL;
            (void)snprintf(names[i], sizeof names[i], "s%05d", i);
            descs[i].name = names[i];
            descs[i].rgba = tiles[i];
            descs[i].w = spec->tile;
            descs[i].h = spec->tile;
            descs[i].origin_x = 0.5F;
            descs[i].origin_y = 0.5F;
        }
    } else {
        sprite_count = load_manifest_sprites(assets_root, sprite_count, descs,
                                             names, tiles, widths, heights);
        ok = sprite_count > 0;
    }

    tp_pack_settings settings;
    tp_error err = {{0}};
    if (ok && tp_pack_settings_defaults(&settings) != TP_STATUS_OK) {
        ok = false;
    }
    settings.atlas_name = spec->name;
    settings.work_dir = scratch;
    settings.sprites = descs;
    settings.sprite_count = sprite_count;
    settings.max_size = spec->page_dim;
    settings.padding = 0;
    settings.margin = 0;
    settings.extrude = 0;
    settings.shape = TP_PACK_SHAPE_RECT;
    settings.allow_transform = false;
    settings.power_of_two = true;

    char path[1024];
    if (ok && snprintf(path, sizeof path, "%s/%s.ntpack", scratch,
                       spec->name) < 0) {
        ok = false;
    }

    double build_ms = 0.0;
    if (ok) {
        const double start = tp_bench_now_ms();
        const tp_status status =
            tp_build_driver_run(&settings, NULL, path, &err);
        build_ms = tp_bench_now_ms() - start;
        /* the driver consumes loaded_images only; raw tile buffers stay ours */
        if (status != TP_STATUS_OK) {
            (void)fprintf(stderr, "scale=%s build failed: %s\n", spec->name,
                          err.msg);
            ok = false;
        }
    }

    size_t blob_size = 0U;
    uint8_t *blob = ok ? read_file(path, &blob_size) : NULL;
    if (ok && !blob) {
        (void)fprintf(stderr, "scale=%s could not read artifact %s\n",
                      spec->name, path);
        ok = false;
    }

    tp_name_map *map = ok ? tp_name_map_create() : NULL;
    if (ok && (!map || tp_name_map_insert(map, spec->name) != TP_STATUS_OK)) {
        ok = false;
    }
    for (int i = 0; ok && i < sprite_count; ++i) {
        ok = tp_name_map_insert(map, names[i]) == TP_STATUS_OK;
    }

    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    uint64_t pages_bytes = 0U;
    uint64_t png_bytes = 0U;
    double png_encode_ms = 0.0;
    double png_decode_ms = 0.0;
    int page_count = 0;
    for (int i = 0; ok && i < iterations; ++i) {
        tp_arena *arena = tp_arena_create(0);
        tp_result **results = NULL;
        int count = 0;
        if (!arena) {
            ok = false;
            break;
        }
        const double start = tp_bench_now_ms();
        const tp_status status = tp_pack_read_memory(blob, blob_size, map,
                                                     arena, &results, &count,
                                                     &err);
        const double elapsed = tp_bench_now_ms() - start;
        if (status != TP_STATUS_OK || count != 1) {
            (void)fprintf(stderr, "scale=%s inflate failed: %s\n", spec->name,
                          err.msg);
            ok = false;
        } else {
            pages_bytes = raw_page_bytes(results[0]);
            page_count = results[0]->page_count;
            if (png_bytes == 0U) {
                png_bytes = png_page_bytes(results[0], &png_encode_ms,
                                           &png_decode_ms);
            }
            ok = tp_bench_samples_record(&samples, true, elapsed);
        }
        tp_arena_destroy(arena);
    }

    if (ok && tp_bench_samples_valid(&samples)) {
        const double median = tp_bench_samples_percentile(&samples, 50U);
        const double ratio =
            blob_size > 0U ? (double)pages_bytes / (double)blob_size : 0.0;
        const double png_ratio =
            png_bytes > 0U ? (double)pages_bytes / (double)png_bytes : 0.0;
        (void)printf(
            "scale=%s page_dim=%d sprites=%d pages=%d "
            "raw_page_bytes=%" PRIu64 " blob_bytes=%zu ratio=%.4f "
            "inflate_p50_ms=%.3f build_ms=%.1f samples=%zu "
            "png_page_bytes=%" PRIu64 " png_ratio=%.2f "
            "png_encode_ms=%.1f png_decode_ms=%.1f\n",
            spec->name, spec->page_dim, sprite_count, page_count, pages_bytes,
            blob_size, ratio, median, build_ms, samples.count, png_bytes,
            png_ratio, png_encode_ms, png_decode_ms);
    } else {
        ok = false;
    }

    tp_name_map_destroy(map);
    free(blob);
    (void)remove(path);
    for (int i = 0; i < sprite_count; ++i) {
        free(tiles[i]);
    }
    free(heights);
    free(widths);
    free(tiles);
    free(names);
    free(descs);
    return ok;
}

int main(int argc, char **argv) {
    const char *scratch = argc > 1 ? argv[1] : ".";
    int iterations = 5;
    if (argc > 2) {
        const long value = strtol(argv[2], NULL, 10);
        if (value >= 1L && value <= 64L) {
            iterations = (int)value;
        }
    }
    const char *assets_root = argc > 3 ? argv[3] : "examples/bench-assets";
    const scale_spec scales[] = {
        {"blob_1024", 1024, 128, 0},
        {"blob_2048", 2048, 128, 0},
        {"blob_4096", 4096, 128, 0},
        /* Real CC0 sprite art: the synthetic tiles are incompressible noise, so
         * only this case makes the png_ratio counterfactual meaningful. */
        {"blob_kenney_4096", 4096, 0, 2000},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; ++i) {
        if (!run_scale(&scales[i], scratch, assets_root, iterations)) {
            failures++;
        }
    }
    return failures == 0 ? 0 : 1;
}
