/* Premise gate: is a serialized `.ntpack` blob materially
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
 *
 * ---------------------------------------------------------------------------
 * Codec sweep (appended below the premise lines)
 *
 * The owner wants a compressed COLD tier for the pack-result cache: inactive
 * atlases held as compressed pixel blobs in RAM, compressed in the background on
 * demotion, decompressed on atlas switch. Phase 1 only measured stb PNG at the
 * stb default effort (level 8). That is the slowest useful point on the curve
 * and says nothing about whether a cheaper codec buys the same ratio.
 *
 * The sweep runs on the two 4096-page fixtures only (synthetic incompressible
 * noise = the pessimistic floor, real Kenney CC0 art = the realistic case) and
 * measures, per codec configuration, over the FULL inflated page set:
 *   enc_p50_ms / dec_p50_ms -- median of `iterations` whole-set passes
 *   bytes / ratio           -- compressed size, raw_page_bytes / bytes
 *   enc_MBps / dec_MBps     -- throughput expressed in RAW MiB per second
 *   alloc_floor (one line per fixture) -- malloc + first-touch + free over the
 *                              same page set, i.e. the destination-allocation
 *                              cost that stb PNG unavoidably pays inside its
 *                              decode number and that the flate rows exclude by
 *                              inflating into a reused buffer
 * and then extrapolates the number that actually decides the design: the decode
 * cost of ONE atlas switch at two realistic atlas sizes (64 MiB = 4x2048^2,
 * 256 MiB = 4x4096^2), from the measured raw-bytes decode throughput.
 *
 * Configurations:
 *   png  L1/L3/L5/L8   stbi_write_png_compression_level, stb default filter
 *   png  L1 filter=0   stbi_write_force_png_filter=0 (no per-row filtering)
 *   flate L1/L3/L6     miniz mz_compress2 / mz_uncompress over raw RGBA8, no
 *                      PNG container and no filtering at all
 *
 * ANSWER (2026-07-29, native-release, Windows, iterations=7, median per number):
 *
 * 1. The stb PNG "level" knob is a trap. stb_image_write.h line 911 clamps the
 *    zlib quality with `if (quality < 5) quality = 5;`, so levels 1, 3 and 5 emit
 *    BYTE-IDENTICAL output at identical cost. There is no cheap PNG in stb --
 *    the sweep confirms it (9,637,670 coded bytes for L1 == L3 == L5 on Kenney,
 *    and encode within 1% across the three). The only real PNG lever is
 *    stbi_write_force_png_filter=0: on Kenney it gives up 2.4% ratio (62.7x ->
 *    61.2x) and buys 1.6x encode and 1.9x decode.
 *
 * 2. Raw miniz deflate beats PNG on every axis on both fixtures. On Kenney,
 *    flate L6 = 114.1x ratio / 478 MiB/s encode / 1839 MiB/s decode, versus PNG
 *    L8 at 62.7x / 130 MiB/s / 1070 MiB/s. PNG's filters do not pay for
 *    themselves on packed sprite pages: the alpha gutters and flat fills are long
 *    runs plain LZ77 already eats, and the filter step adds a full extra pass
 *    over the pixels on BOTH encode and decode. Even on the incompressible
 *    synthetic fixture flate wins (1.24x vs 1.03x, and 1.7x faster to decode).
 *
 * 3. Decode is the number that decides the tier, and it barely responds to the
 *    level. Across flate L1/L3/L6 decode moves ~7% (336 -> 313 ms) while the
 *    ratio moves 87.9x -> 114.1x. The level knob is close to free on the read
 *    path, so take the higher level.
 *
 * 4. Recommendation: miniz raw deflate, level 6. See the S27 packet notes.
 *
 * ---------------------------------------------------------------------------
 * The wired cold tier (`coldtier` rows)
 *
 * Phase 2 measured codecs in isolation. Phase 3 drives the shipped
 * tp_pack_result_cache: store a result, switch away (background encode), settle,
 * then time the atlas switch BACK -- the real promote, decompressing every page
 * in parallel into fresh buffers. Both realistic atlas scales are covered by
 * replicating the 4096^2 page record: 1 page = 64 MiB, 4 pages = 256 MiB.
 *
 * ANSWER (2026-07-29, native-release, Windows, iterations=5, Kenney fixture):
 *   pages=1  raw 64 MiB  -> coded 0.449 MiB (142.7x), meta 302.8 KiB,
 *            background encode 122 ms, promote p50 36.1 ms (1770 MiB/s)
 *   pages=4  raw 256 MiB -> coded 1.794 MiB (142.7x), meta 303.0 KiB,
 *            background encode 462 ms, promote p50 45.9 ms (5575 MiB/s)
 *
 * The 4-page row is the design's whole point: 4x the pixels for 1.27x the switch
 * latency, because the pages decode on separate threads. A single-page atlas has
 * nothing to fan out over and lands on the serial number (36 ms ~= the 33 ms the
 * phase-2 flate_L6 throughput predicts).
 *
 * The 143x ratio here is above the 114x of the phase-2 row because the phase-2
 * number averages nine DIFFERENT pages while this one replicates page 0; treat
 * 114x as the fixture's honest ratio and this as the per-page one.
 *
 * Geometry cost: 303 KiB of uncompressed metadata for 2000 sprites (~155 bytes
 * each) -- 0.5% of the raw pages, but ~40% of what the cold entry actually
 * occupies once the pages are compressed 143x. That is why the store's budget
 * counts it: a cold entry is charged its blobs AND its geometry, and the split is
 * still visible as tp_pack_result_cache_stats::cold_meta_bytes.
 *
 * 5. Caveats the numbers carry:
 *    - The sweep is single-threaded and serial over the pages of one result. A
 *      4-page atlas promoted on 4 threads divides switch_*_ms by ~the page count.
 *    - flate inflates into a reused destination (what a real promote into the
 *      result arena does); stb PNG allocates internally and cannot. The
 *      `alloc_floor` line measures that difference directly: 79.7 ms per 576 MiB
 *      (7.2 GiB/s) is inside every PNG decode number and outside every flate one.
 *    - Run-to-run spread on a loaded desktop is ~10-30% on the big fixture; treat
 *      the switch_*_ms column as an order-of-magnitude gate, not a budget.
 */

#include "tp_bench_support.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_pack_result.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_pack_result_cache.h"
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

/* Raw deflate, no PNG container: the phase-2 alternative to stb PNG. Vendored at
 * external/neotolis-engine/deps/miniz and already PUBLIC-linked by nt_builder. */
#include "miniz.h"

typedef struct scale_spec {
    const char *name;
    int page_dim;   /* max_size; sprites are laid out to fill exactly one page */
    int tile;       /* synthetic sprite edge in px; 0 = real Kenney assets */
    int asset_count;/* real-asset mode: how many manifest rows to pack */
    bool codec_sweep;/* run the phase-2 codec/level sweep on this fixture */
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

/* ------------------------------------------------------------------------- */
/* Phase-2 codec sweep                                                        */
/* ------------------------------------------------------------------------- */

#define TP_BENCH_MIB 1048576.0

typedef struct codec_cfg {
    const char *codec;  /* "png" | "flate" */
    int level;          /* png: stbi_write_png_compression_level; flate: mz level */
    int png_filter;     /* png only: -1 = stb default (adaptive), 0 = filter none */
} codec_cfg;

/* Ordered cheap -> expensive within each codec so the printed table reads as a
 * curve. flate has no filtering stage at all, which is the whole point. */
static const codec_cfg k_codec_cfgs[] = {
    {"png", 1, -1},
    {"png", 3, -1},
    {"png", 5, -1},
    {"png", 8, -1}, /* stb default: the phase-1 number, re-measured here */
    {"png", 1, 0},  /* filters cost encode time and buy little at low effort */
    {"flate", 1, -1},
    {"flate", 3, -1},
    {"flate", 6, -1},
};

static void codec_cfg_label(const codec_cfg *cfg, char *out, size_t out_size) {
    if (cfg->png_filter >= 0) {
        (void)snprintf(out, out_size, "%s_L%d_filter%d", cfg->codec, cfg->level,
                       cfg->png_filter);
    } else {
        (void)snprintf(out, out_size, "%s_L%d", cfg->codec, cfg->level);
    }
}

/* One page in, one owned compressed buffer out. */
static bool codec_encode_page(const codec_cfg *cfg, const tp_page *page,
                              uint8_t **out_bytes, size_t *out_size) {
    *out_bytes = NULL;
    *out_size = 0U;
    const size_t raw = (size_t)page->w * (size_t)page->h * 4U;
    if (strcmp(cfg->codec, "png") == 0) {
        stbi_write_png_compression_level = cfg->level;
        stbi_write_force_png_filter = cfg->png_filter;
        png_sink sink = {NULL, 0U, false};
        const int written =
            stbi_write_png_to_func(png_collect, &sink, page->w, page->h, 4,
                                   page->rgba, page->w * 4);
        if (!written || sink.failed || sink.size == 0U) {
            free(sink.bytes);
            return false;
        }
        *out_bytes = sink.bytes;
        *out_size = sink.size;
        return true;
    }
    const mz_ulong bound = mz_compressBound((mz_ulong)raw);
    uint8_t *dst = (uint8_t *)malloc((size_t)bound);
    if (!dst) {
        return false;
    }
    mz_ulong dst_len = bound;
    if (mz_compress2(dst, &dst_len, page->rgba, (mz_ulong)raw, cfg->level) !=
        MZ_OK) {
        free(dst);
        return false;
    }
    *out_bytes = dst;
    *out_size = (size_t)dst_len;
    return true;
}

/* Decode one compressed page back to RGBA8 and verify the dimensions/length, so
 * a codec that silently truncates cannot post a fast time.
 *
 * `scratch` is a caller-owned destination big enough for the largest page, held
 * across runs. This matters more than it looks: a fresh 64 MiB malloc per page
 * costs a kernel zero-fill on first touch, which on this machine is the same
 * order as the whole inflate. A real cold tier promotes into the result arena or
 * a pooled buffer, so reusing the destination is the honest model. stb PNG
 * allocates internally and cannot be given a destination -- the `alloc_floor`
 * line printed per fixture quantifies exactly that difference. */
static bool codec_decode_page(const codec_cfg *cfg, const tp_page *page,
                              const uint8_t *bytes, size_t size,
                              uint8_t *scratch) {
    const size_t raw = (size_t)page->w * (size_t)page->h * 4U;
    if (strcmp(cfg->codec, "png") == 0) {
        int w = 0;
        int h = 0;
        int comp = 0;
        stbi_uc *decoded =
            stbi_load_from_memory(bytes, (int)size, &w, &h, &comp, 4);
        const bool ok = decoded && w == page->w && h == page->h;
        stbi_image_free(decoded);
        return ok;
    }
    mz_ulong dst_len = (mz_ulong)raw;
    const int status = mz_uncompress(scratch, &dst_len, bytes, (mz_ulong)size);
    return status == MZ_OK && (size_t)dst_len == raw;
}

/* Volatile so the touch loop below survives -O2: an unread malloc/memset/free
 * triple is dead code clang deletes outright, which is exactly what made the
 * first version of this measurement print 0.0 ms. */
static volatile uint8_t g_alloc_floor_sink;

/* Median cost of malloc + first-touch + free over the whole page set: the
 * allocation floor that is unavoidably inside every stb PNG decode number above,
 * and that the flate rows deliberately exclude. One volatile store per 4 KiB is
 * what actually costs -- it forces the kernel to commit and zero each page. */
static void run_alloc_floor(const scale_spec *spec, const tp_result *result,
                            int runs) {
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    bool ok = true;
    for (int run = 0; ok && run < runs; ++run) {
        const double start = tp_bench_now_ms();
        for (int p = 0; p < result->page_count; ++p) {
            const size_t raw =
                (size_t)result->pages[p].w * (size_t)result->pages[p].h * 4U;
            uint8_t *dst = (uint8_t *)malloc(raw);
            if (!dst) {
                ok = false;
                break;
            }
            for (size_t off = 0U; off < raw; off += 4096U) {
                ((volatile uint8_t *)dst)[off] = (uint8_t)run;
            }
            g_alloc_floor_sink = ((volatile uint8_t *)dst)[raw - 1U];
            free(dst);
        }
        const double elapsed = tp_bench_now_ms() - start;
        ok = ok && tp_bench_samples_record(&samples, true, elapsed);
    }
    if (!ok || !tp_bench_samples_valid(&samples)) {
        (void)fprintf(stderr, "alloc_floor scale=%s FAILED\n", spec->name);
        return;
    }
    const double ms = tp_bench_samples_percentile(&samples, 50U);
    const double raw_mib = (double)raw_page_bytes(result) / TP_BENCH_MIB;
    (void)printf("alloc_floor scale=%s raw_MiB=%.1f p50_ms=%.1f MiBps=%.1f "
                 "runs=%d\n",
                 spec->name, raw_mib, ms,
                 ms > 0.0 ? raw_mib / (ms / 1000.0) : 0.0, runs);
}

static void codec_free_blobs(uint8_t **blobs, int count) {
    for (int i = 0; i < count; ++i) {
        free(blobs[i]);
        blobs[i] = NULL;
    }
}

/* Whole-page-set encode + decode, `runs` passes each, median reported. Times are
 * per FULL result (all pages), which is what a cold-tier demote/promote of one
 * atlas would actually pay. */
static bool run_codec_cfg(const codec_cfg *cfg, const scale_spec *spec,
                          const tp_result *result, int runs) {
    const int page_count = result->page_count;
    const uint64_t raw_total = raw_page_bytes(result);
    uint8_t **blobs = (uint8_t **)calloc((size_t)page_count, sizeof *blobs);
    size_t *sizes = (size_t *)calloc((size_t)page_count, sizeof *sizes);
    size_t widest = 0U;
    for (int p = 0; p < page_count; ++p) {
        const size_t raw =
            (size_t)result->pages[p].w * (size_t)result->pages[p].h * 4U;
        if (raw > widest) {
            widest = raw;
        }
    }
    /* Pre-faulted, reused decode destination -- see codec_decode_page(). */
    uint8_t *scratch = (uint8_t *)malloc(widest);
    if (!blobs || !sizes || !scratch) {
        free(scratch);
        free(sizes);
        free(blobs);
        return false;
    }
    memset(scratch, 0, widest);
    bool ok = true;
    tp_bench_samples encode;
    tp_bench_samples decode;
    tp_bench_samples_init(&encode);
    tp_bench_samples_init(&decode);

    for (int run = 0; ok && run < runs; ++run) {
        codec_free_blobs(blobs, page_count);
        const double start = tp_bench_now_ms();
        for (int p = 0; p < page_count; ++p) {
            if (!codec_encode_page(cfg, &result->pages[p], &blobs[p],
                                   &sizes[p])) {
                ok = false;
                break;
            }
        }
        const double elapsed = tp_bench_now_ms() - start;
        ok = ok && tp_bench_samples_record(&encode, true, elapsed);
    }

    uint64_t coded_total = 0U;
    for (int p = 0; ok && p < page_count; ++p) {
        coded_total += (uint64_t)sizes[p];
    }

    for (int run = 0; ok && run < runs; ++run) {
        const double start = tp_bench_now_ms();
        for (int p = 0; p < page_count; ++p) {
            if (!codec_decode_page(cfg, &result->pages[p], blobs[p], sizes[p],
                                   scratch)) {
                ok = false;
                break;
            }
        }
        const double elapsed = tp_bench_now_ms() - start;
        ok = ok && tp_bench_samples_record(&decode, true, elapsed);
    }

    if (ok && tp_bench_samples_valid(&encode) &&
        tp_bench_samples_valid(&decode)) {
        char label[64];
        codec_cfg_label(cfg, label, sizeof label);
        const double enc_ms = tp_bench_samples_percentile(&encode, 50U);
        const double dec_ms = tp_bench_samples_percentile(&decode, 50U);
        const double raw_mib = (double)raw_total / TP_BENCH_MIB;
        const double ratio =
            coded_total > 0U ? (double)raw_total / (double)coded_total : 0.0;
        const double enc_mibps = enc_ms > 0.0 ? raw_mib / (enc_ms / 1000.0) : 0.0;
        const double dec_mibps = dec_ms > 0.0 ? raw_mib / (dec_ms / 1000.0) : 0.0;
        /* Atlas-switch cost: how long ONE atlas of the given raw footprint takes
         * to decode at the measured throughput. 64 MiB = 4x2048^2 RGBA8,
         * 256 MiB = 4x4096^2 RGBA8. */
        const double switch_64_ms =
            dec_mibps > 0.0 ? 64.0 / dec_mibps * 1000.0 : 0.0;
        const double switch_256_ms =
            dec_mibps > 0.0 ? 256.0 / dec_mibps * 1000.0 : 0.0;
        /* Cold-tier capacity: raw page MiB retained per 512 MiB RAM budget. */
        const double budget512_raw_mib = ratio * 512.0;
        (void)printf(
            "codec scale=%s cfg=%-16s pages=%d raw_bytes=%" PRIu64
            " coded_bytes=%" PRIu64 " ratio=%.2f enc_p50_ms=%.1f "
            "dec_p50_ms=%.1f enc_MiBps=%.1f dec_MiBps=%.1f "
            "switch_64MiB_ms=%.1f switch_256MiB_ms=%.1f "
            "budget512_raw_MiB=%.0f runs=%d\n",
            spec->name, label, page_count, raw_total, coded_total, ratio, enc_ms,
            dec_ms, enc_mibps, dec_mibps, switch_64_ms, switch_256_ms,
            budget512_raw_mib, runs);
    } else {
        char label[64];
        codec_cfg_label(cfg, label, sizeof label);
        (void)fprintf(stderr, "codec scale=%s cfg=%s FAILED\n", spec->name,
                      label);
        ok = false;
    }

    codec_free_blobs(blobs, page_count);
    free(scratch);
    free(sizes);
    free(blobs);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Phase-3: the WIRED cold tier                                               */
/*                                                                            */
/* Phase 2 measured a codec in isolation. This phase measures the shipped      */
/* tp_pack_result_cache doing the thing the packet exists for: an atlas switch */
/* onto a cold entry, which is a metadata-copy-free promote plus a per-page    */
/* parallel decode into fresh buffers. It also reports what the entry costs    */
/* while cold, including the uncompressed geometry the store keeps beside the  */
/* blobs.                                                                      */
/* ------------------------------------------------------------------------- */

/* Neither pin owns anything here: the arena belongs to run_scale, which outlives
 * the store. The hook only has to be non-NULL and idempotent-safe. */
static void bench_pin_release(void *owner) { (void)owner; }

static uint64_t bench_raw_bytes(const tp_result *result) {
    return raw_page_bytes(result);
}

/* A view of `source` with `pages` page records, all pointing at the same real
 * page pixels. 4x a 4096^2 page is the 256 MiB atlas the design targets, and
 * duplicating the record is the honest way to get one without packing 256 MiB of
 * sprites: the codec and the fork-join both see `pages` independent pages. */
static tp_result *bench_widen(const tp_result *source, int pages,
                              tp_arena *arena) {
    tp_result *wide = (tp_result *)tp_arena_alloc(arena, sizeof *wide);
    if (!wide) {
        return NULL;
    }
    *wide = *source;
    wide->page_count = pages;
    wide->pages = (tp_page *)tp_arena_alloc(arena, (size_t)pages * sizeof *wide->pages);
    if (!wide->pages) {
        return NULL;
    }
    for (int i = 0; i < pages; ++i) {
        wide->pages[i] = source->pages[0];
    }
    return wide;
}

static void run_cold_tier(const scale_spec *spec, const tp_result *result,
                          int runs) {
    static const int k_page_counts[] = {1, 4}; /* 64 MiB and 256 MiB at 4096^2 */
    if (result->page_count < 1 || result->pages[0].w <= 0) {
        return;
    }
    for (size_t k = 0; k < sizeof k_page_counts / sizeof k_page_counts[0]; ++k) {
        const int pages = k_page_counts[k];
        tp_arena *arena = tp_arena_create(0);
        if (!arena) {
            return;
        }
        tp_result *wide = bench_widen(result, pages, arena);
        /* A second entry to switch to, so the first one is demoted and cold. */
        tp_result *other = bench_widen(result, 1, arena);
        tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 40);
        if (!wide || !other || !cache) {
            tp_pack_result_cache_destroy(cache);
            tp_arena_destroy(arena);
            return;
        }
        tp_id128 hot = tp_id128_nil();
        tp_id128 spare = tp_id128_nil();
        hot.bytes[0] = 1U;
        spare.bytes[0] = 2U;
        tp_error err = {{0}};
        const uint64_t raw = bench_raw_bytes(wide);
        bool ok = tp_pack_result_cache_store_retained(
                      cache, hot, 1U, wide, raw, wide, bench_pin_release,
                      &err) == TP_STATUS_OK &&
                  tp_pack_result_cache_store_retained(
                      cache, spare, 2U, other, bench_raw_bytes(other), other,
                      bench_pin_release, &err) == TP_STATUS_OK;

        /* Encode: settle drains the background queue, so its wall time IS the
         * single-threaded background encode of the demoted entry. */
        const double encode_start = tp_bench_now_ms();
        if (ok) {
            tp_pack_result_cache_settle(cache);
        }
        const double encode_ms = tp_bench_now_ms() - encode_start;

        tp_pack_result_cache_stats stats;
        tp_pack_result_cache_stats_get(cache, &stats);
        ok = ok && stats.cold_entries == 1;

        /* Promote: select the cold entry, resolve it (this is the atlas switch),
         * then switch away again so the next run starts cold once more. */
        tp_bench_samples promote;
        tp_bench_samples_init(&promote);
        for (int run = 0; ok && run < runs; ++run) {
            tp_pack_result_cache_select(cache, hot);
            const double start = tp_bench_now_ms();
            const tp_result *promoted = NULL;
            ok = tp_pack_result_cache_authoritative(cache, NULL, &promoted, NULL,
                                                    &err) == TP_STATUS_OK;
            const double elapsed = tp_bench_now_ms() - start;
            ok = ok && promoted && promoted->pages[pages - 1].rgba != NULL;
            ok = ok && tp_bench_samples_record(&promote, true, elapsed);
            tp_pack_result_cache_select(cache, spare);
            ok = ok && tp_pack_result_cache_authoritative(cache, NULL, NULL,
                                                          NULL,
                                                          &err) == TP_STATUS_OK;
        }

        if (ok && tp_bench_samples_valid(&promote)) {
            const double raw_mib = (double)raw / TP_BENCH_MIB;
            const double promote_ms = tp_bench_samples_percentile(&promote, 50U);
            (void)printf(
                "coldtier scale=%s pages=%d raw_MiB=%.1f coded_MiB=%.3f "
                "meta_KiB=%.1f ratio=%.2f encode_ms=%.1f promote_p50_ms=%.1f "
                "promote_MiBps=%.1f runs=%d\n",
                spec->name, pages, raw_mib,
                (double)stats.cold_coded_bytes / TP_BENCH_MIB,
                (double)stats.cold_meta_bytes / 1024.0,
                stats.cold_coded_bytes > 0U
                    ? (double)stats.cold_raw_bytes / (double)stats.cold_coded_bytes
                    : 0.0,
                encode_ms, promote_ms,
                promote_ms > 0.0 ? raw_mib / (promote_ms / 1000.0) : 0.0, runs);
        } else {
            (void)fprintf(stderr, "coldtier scale=%s pages=%d FAILED (%s)\n",
                          spec->name, pages, err.msg);
        }
        tp_pack_result_cache_destroy(cache);
        tp_arena_destroy(arena);
    }
}

static bool run_codec_sweep(const scale_spec *spec, const tp_result *result,
                            int runs) {
    bool ok = true;
    run_alloc_floor(spec, result, runs);
    for (size_t i = 0; i < sizeof k_codec_cfgs / sizeof k_codec_cfgs[0]; ++i) {
        if (!run_codec_cfg(&k_codec_cfgs[i], spec, result, runs)) {
            ok = false;
        }
    }
    /* Leave the globals where the rest of the process expects them. */
    stbi_write_png_compression_level = 8;
    stbi_write_force_png_filter = -1;
    run_cold_tier(spec, result, runs);
    return ok;
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

    /* Phase-2 sweep runs on a fresh inflate so the phase-1 lines above stay
     * exactly the measurement they were, untouched by codec work. */
    if (ok && spec->codec_sweep) {
        tp_arena *arena = tp_arena_create(0);
        tp_result **results = NULL;
        int count = 0;
        if (!arena) {
            ok = false;
        } else if (tp_pack_read_memory(blob, blob_size, map, arena, &results,
                                       &count, &err) != TP_STATUS_OK ||
                   count != 1) {
            (void)fprintf(stderr, "scale=%s codec-sweep inflate failed: %s\n",
                          spec->name, err.msg);
            ok = false;
        } else {
            ok = run_codec_sweep(spec, results[0], iterations);
        }
        tp_arena_destroy(arena);
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
        {"blob_1024", 1024, 128, 0, false},
        {"blob_2048", 2048, 128, 0, false},
        /* The two codec-sweep fixtures: 4096^2 synthetic noise (pessimistic
         * floor -- nothing compresses) and 4096^2 real art (the real case). */
        {"blob_4096", 4096, 128, 0, true},
        /* Real CC0 sprite art: the synthetic tiles are incompressible noise, so
         * only this case makes the png_ratio counterfactual meaningful. */
        {"blob_kenney_4096", 4096, 0, 2000, true},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; ++i) {
        if (!run_scale(&scales[i], scratch, assets_root, iterations)) {
            failures++;
        }
    }
    return failures == 0 ? 0 : 1;
}
