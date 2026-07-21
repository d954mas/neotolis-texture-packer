/* F3-03 T1: canonical semantic pack_input_hash + per-file image-hash cache.
 *
 * Proves the spec §10.2 / §59 contract: the hash is derived from normalized
 * pixels + settings + ordered sprite identity, NEVER from raw bytes / mtime /
 * container metadata (those only decide whether to re-decode). Named packet
 * cases: identical pixels re-saved with a changed mtime keep the same hash;
 * changed RGBA changes it; a semantic sprite reorder changes it; caching is a
 * pure optimization (cached == uncached); a settings / override / target change
 * changes it; the version byte is pinned. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif

#include "stb_image_write.h"

#include "tp_core/tp_id.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_pack_hash.h"
#include "unity.h"

static const char *g_dir;

void setUp(void) {}
void tearDown(void) {}

static void set_mtime(const char *path, time_t when) {
#ifdef _WIN32
    struct _utimbuf tb;
    tb.actime = when;
    tb.modtime = when;
    (void)_utime(path, &tb);
#else
    struct utimbuf tb;
    tb.actime = when;
    tb.modtime = when;
    (void)utime(path, &tb);
#endif
}

/* An 8x8 RGBA gradient; `salt` perturbs one channel so callers can produce a
 * distinct-but-same-size image. */
static void fill_pixels(uint8_t px[8 * 8 * 4], uint8_t salt) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            uint8_t *p = &px[((size_t)y * 8U + (size_t)x) * 4U];
            p[0] = (uint8_t)(x * 16 + salt);
            p[1] = (uint8_t)(y * 16);
            p[2] = (uint8_t)(200 - x * 4);
            p[3] = 255U;
        }
    }
}

static void base_desc(tp_pack_sprite_desc *d, const char *name, uint8_t *rgba) {
    memset(d, 0, sizeof *d);
    d->name = name;
    d->rgba = rgba;
    d->w = 8;
    d->h = 8;
    d->origin_x = 0.5F;
    d->origin_y = 0.5F;
}

static void base_settings(tp_pack_settings *s, tp_pack_sprite_desc *descs,
                          int count) {
    tp_pack_settings_defaults(s);
    s->atlas_name = "hash_atlas";
    s->work_dir = g_dir; /* excluded from the hash by contract */
    s->sprites = descs;
    s->sprite_count = count;
    s->pixels_per_unit = 1.0F;
}

static tp_id128 compute(const tp_pack_settings *s, const char *exporter,
                        tp_pack_image_hash_cache *cache) {
    tp_id128 hash = tp_id128_nil();
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_pack_input_hash_compute(s, exporter, cache, &hash, &err), err.msg);
    TEST_ASSERT_FALSE(tp_id128_is_nil(hash));
    return hash;
}

void test_version_byte_pinned(void) {
    /* A change to either constant is a visible cache-invalidating change and
     * must be a deliberate edit here. */
    TEST_ASSERT_EQUAL_UINT8(1U, TP_PACK_INPUT_HASH_VERSION);
    TEST_ASSERT_EQUAL_UINT32(1U, TP_PACK_ALGO_VERSION);
}

void test_recompute_and_caching_are_stable(void) {
    uint8_t px[8 * 8 * 4];
    fill_pixels(px, 0U);
    tp_pack_sprite_desc d;
    base_desc(&d, "sprite", px);
    tp_pack_settings s;
    base_settings(&s, &d, 1);

    const tp_id128 a = compute(&s, NULL, NULL);
    const tp_id128 b = compute(&s, NULL, NULL);
    TEST_ASSERT_TRUE_MESSAGE(tp_id128_eq(a, b), "recompute must be stable");

    /* Cache is a pure optimization: cached == uncached. */
    tp_pack_image_hash_cache *cache = tp_pack_image_hash_cache_create();
    TEST_ASSERT_NOT_NULL(cache);
    const tp_id128 c = compute(&s, NULL, cache);
    TEST_ASSERT_TRUE(tp_id128_eq(a, c));
    tp_pack_image_hash_cache_destroy(cache);
}

void test_changed_rgba_changes_hash(void) {
    uint8_t px_a[8 * 8 * 4];
    uint8_t px_b[8 * 8 * 4];
    fill_pixels(px_a, 0U);
    fill_pixels(px_b, 0U);
    px_b[0] = (uint8_t)(px_b[0] ^ 0xFFU); /* one pixel differs */

    tp_pack_sprite_desc da;
    tp_pack_sprite_desc db;
    base_desc(&da, "sprite", px_a);
    base_desc(&db, "sprite", px_b);
    tp_pack_settings sa;
    tp_pack_settings sb;
    base_settings(&sa, &da, 1);
    base_settings(&sb, &db, 1);
    TEST_ASSERT_FALSE(tp_id128_eq(compute(&sa, NULL, NULL),
                                  compute(&sb, NULL, NULL)));
}

void test_sprite_reorder_changes_hash(void) {
    uint8_t red[8 * 8 * 4];
    uint8_t blue[8 * 8 * 4];
    fill_pixels(red, 10U);
    fill_pixels(blue, 200U);

    tp_pack_sprite_desc fwd[2];
    tp_pack_sprite_desc rev[2];
    base_desc(&fwd[0], "a", red);
    base_desc(&fwd[1], "b", blue);
    base_desc(&rev[0], "b", blue);
    base_desc(&rev[1], "a", red);

    tp_pack_settings sf;
    tp_pack_settings sr;
    base_settings(&sf, fwd, 2);
    base_settings(&sr, rev, 2);
    /* Packing layout depends on input order, so a reorder IS semantic. */
    TEST_ASSERT_FALSE(tp_id128_eq(compute(&sf, NULL, NULL),
                                  compute(&sr, NULL, NULL)));
}

void test_settings_override_target_changes(void) {
    uint8_t px[8 * 8 * 4];
    fill_pixels(px, 0U);
    tp_pack_sprite_desc d;
    base_desc(&d, "sprite", px);
    tp_pack_settings base;
    base_settings(&base, &d, 1);
    const tp_id128 ref = compute(&base, NULL, NULL);

    /* settings change */
    tp_pack_settings s = base;
    s.padding = base.padding + 1;
    TEST_ASSERT_FALSE_MESSAGE(tp_id128_eq(ref, compute(&s, NULL, NULL)),
                              "a settings change must change the hash");

    /* per-sprite override change */
    tp_pack_sprite_desc od = d;
    od.ov_mask = TP_PACK_OV_MARGIN;
    od.ov_margin = 3U;
    tp_pack_settings so;
    base_settings(&so, &od, 1);
    TEST_ASSERT_FALSE_MESSAGE(tp_id128_eq(ref, compute(&so, NULL, NULL)),
                              "a per-sprite override change must change the hash");

    /* pivot change */
    tp_pack_sprite_desc pd = d;
    pd.origin_x = 0.25F;
    tp_pack_settings sp;
    base_settings(&sp, &pd, 1);
    TEST_ASSERT_FALSE(tp_id128_eq(ref, compute(&sp, NULL, NULL)));

    /* target (preview exporter) change */
    TEST_ASSERT_FALSE_MESSAGE(
        tp_id128_eq(ref, compute(&base, "some-exporter", NULL)),
        "a target-adaptation change must change the hash");
}

/* A path sprite and a raw-pixel sprite with identical NAME + identical decoded
 * pixels produce the SAME hash: proof that the file bytes/mtime/path do not leak
 * into the semantic hash. */
void test_path_and_raw_pixels_hash_identically(void) {
    uint8_t px[8 * 8 * 4];
    fill_pixels(px, 0U);
    char png[1024];
    TEST_ASSERT_TRUE(snprintf(png, sizeof png, "%s/equiv.png", g_dir) > 0);
    TEST_ASSERT_NOT_EQUAL(0, stbi_write_png(png, 8, 8, 4, px, 8 * 4));

    tp_pack_sprite_desc raw;
    base_desc(&raw, "sprite", px);
    tp_pack_settings sraw;
    base_settings(&sraw, &raw, 1);

    tp_pack_sprite_desc pathd;
    memset(&pathd, 0, sizeof pathd);
    pathd.name = "sprite";
    pathd.path = png;
    pathd.w = 0;
    pathd.h = 0;
    pathd.origin_x = 0.5F;
    pathd.origin_y = 0.5F;
    tp_pack_settings spath;
    base_settings(&spath, &pathd, 1);

    TEST_ASSERT_TRUE(tp_id128_eq(compute(&sraw, NULL, NULL),
                                 compute(&spath, NULL, NULL)));
}

void test_identical_pixels_changed_mtime_same_hash(void) {
    uint8_t px[8 * 8 * 4];
    fill_pixels(px, 7U);
    char png[1024];
    TEST_ASSERT_TRUE(snprintf(png, sizeof png, "%s/mtime.png", g_dir) > 0);
    TEST_ASSERT_NOT_EQUAL(0, stbi_write_png(png, 8, 8, 4, px, 8 * 4));

    tp_pack_sprite_desc d;
    memset(&d, 0, sizeof d);
    d.name = "sprite";
    d.path = png;
    d.origin_x = 0.5F;
    d.origin_y = 0.5F;
    tp_pack_settings s;
    base_settings(&s, &d, 1);

    tp_pack_image_hash_cache *cache = tp_pack_image_hash_cache_create();
    TEST_ASSERT_NOT_NULL(cache);

    const tp_id128 h1 = compute(&s, NULL, cache); /* miss -> decode */
    tp_pack_image_hash_cache_stats stats;
    tp_pack_image_hash_cache_stats_get(cache, &stats);
    TEST_ASSERT_EQUAL_UINT64(1U, stats.decodes);
    TEST_ASSERT_EQUAL_UINT64(1U, stats.misses);

    /* Same file, same bytes -> fingerprint hit, no re-decode. */
    const tp_id128 h_hit = compute(&s, NULL, cache);
    tp_pack_image_hash_cache_stats_get(cache, &stats);
    TEST_ASSERT_EQUAL_UINT64(1U, stats.decodes);
    TEST_ASSERT_EQUAL_UINT64(1U, stats.hits);
    TEST_ASSERT_TRUE(tp_id128_eq(h1, h_hit));

    /* Change ONLY the mtime (bytes untouched): fingerprint changes -> forced
     * re-decode -> identical pixels -> identical semantic hash. */
    set_mtime(png, (time_t)1234567890);
    const tp_id128 h2 = compute(&s, NULL, cache);
    tp_pack_image_hash_cache_stats_get(cache, &stats);
    TEST_ASSERT_EQUAL_UINT64(2U, stats.decodes); /* re-decoded */
    TEST_ASSERT_TRUE_MESSAGE(tp_id128_eq(h1, h2),
                             "changed mtime must not change the semantic hash");

    tp_pack_image_hash_cache_destroy(cache);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    UNITY_BEGIN();
    RUN_TEST(test_version_byte_pinned);
    RUN_TEST(test_recompute_and_caching_are_stable);
    RUN_TEST(test_changed_rgba_changes_hash);
    RUN_TEST(test_sprite_reorder_changes_hash);
    RUN_TEST(test_settings_override_target_changes);
    RUN_TEST(test_path_and_raw_pixels_hash_identically);
    RUN_TEST(test_identical_pixels_changed_mtime_same_hash);
    return UNITY_END();
}
