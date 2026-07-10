/* Interim golden round-trip for tp_pack_read (plan §3.2, task 4-5 Done criteria).
 *
 * Builds a small .ntpack IN this test via nt_builder (like apps/smoke), then parses
 * it back and asserts the recovered canonical model. The full fixture-driven golden
 * test (tp_fixtures) is tasks 6-7 and deliberately NOT built here.
 *
 * Fixtures exercise the reader's core branches:
 *   - opaque_rect    : untrimmed; distinct corner-marker pixels pin frame.x/y + atlas_v
 *                      orientation via page sampling.
 *   - trimmed_sprite : asymmetric transparent margins (left=5,top=8) -> exercises the
 *                      source_h - trim_offset_y - trim_h y-flip.
 *   - alias_a/alias_b: byte-identical pixels -> builder dedups -> reader recovers alias_of.
 *
 * Profile = nt_atlas_opts_defaults() (premultiplied=true, so this proves the reader
 * SURFACES the flag rather than rejecting it) with debug_png=false. allow_transform is
 * forced off so placements are identity and page sampling needs no transform accounting,
 * and pixels_per_unit is set non-default to prove the meta round-trip. */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nt_builder.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_core/tp_pack_read.h"
#include "unity.h"

/* --- fixture colors (all opaque so premultiply is a no-op and pixel asserts are exact) --- */
static const uint8_t C_RECT_BASE[4] = {200, 50, 100, 255};
static const uint8_t C_TL[4] = {255, 255, 0, 255};   /* top-left marker */
static const uint8_t C_TR[4] = {0, 255, 255, 255};   /* top-right marker */
static const uint8_t C_BL[4] = {255, 0, 255, 255};   /* bottom-left marker */
static const uint8_t C_BR[4] = {10, 20, 30, 255};    /* bottom-right marker */
static const uint8_t C_TRIM[4] = {30, 180, 90, 255}; /* trimmed sprite content */
static const uint8_t C_ALIAS[4] = {40, 60, 220, 255};

static char g_pack_path[1024];
static tp_arena *g_arena;
static tp_name_map *g_names;
static tp_result **g_results;
static int g_count;
static uint8_t *g_bytes;
static size_t g_size;

void setUp(void) {}
void tearDown(void) {}

static void set_px(uint8_t *buf, int w, int x, int y, const uint8_t c[4]) {
    uint8_t *p = &buf[((size_t)y * (size_t)w + (size_t)x) * 4];
    p[0] = c[0];
    p[1] = c[1];
    p[2] = c[2];
    p[3] = c[3];
}

static void fill_solid(uint8_t *buf, int w, int h, const uint8_t c[4]) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            set_px(buf, w, x, y, c);
        }
    }
}

static bool build_interim_pack(const char *path) {
    NtBuilderContext *ctx = nt_builder_start_pack(path);
    if (!ctx) {
        return false;
    }
    nt_atlas_opts_t opts = nt_atlas_opts_defaults();
    opts.debug_png = false;
    opts.allow_transform = false;           /* identity placement -> simple pixel sampling */
    opts.shape = NT_ATLAS_SHAPE_RECT;       /* no clipper2 hull inflation -> exact trim/frame recovery */
    opts.pixels_per_unit = 32.0f;           /* non-default: assert meta round-trip */
    nt_builder_begin_atlas(ctx, "interim_atlas", &opts);

    static uint8_t buf[40 * 30 * 4];

    /* 1. opaque rect (untrimmed) with 4 distinct corner markers */
    fill_solid(buf, 20, 12, C_RECT_BASE);
    set_px(buf, 20, 0, 0, C_TL);
    set_px(buf, 20, 19, 0, C_TR);
    set_px(buf, 20, 0, 11, C_BL);
    set_px(buf, 20, 19, 11, C_BR);
    {
        nt_atlas_sprite_opts_t so = nt_atlas_sprite_opts_defaults();
        so.name = "opaque_rect";
        nt_builder_atlas_add_raw(ctx, buf, 20, 12, &so);
    }

    /* 2. trimmed sprite: 40x30 with opaque sub-rect x[5,30) y[8,25) */
    memset(buf, 0, sizeof buf);
    for (int y = 8; y < 25; y++) {
        for (int x = 5; x < 30; x++) {
            set_px(buf, 40, x, y, C_TRIM);
        }
    }
    {
        nt_atlas_sprite_opts_t so = nt_atlas_sprite_opts_defaults();
        so.name = "trimmed_sprite";
        nt_builder_atlas_add_raw(ctx, buf, 40, 30, &so);
    }

    /* 3+4. alias pair: byte-identical 16x16 solid */
    fill_solid(buf, 16, 16, C_ALIAS);
    {
        nt_atlas_sprite_opts_t so = nt_atlas_sprite_opts_defaults();
        so.name = "alias_a";
        nt_builder_atlas_add_raw(ctx, buf, 16, 16, &so);
    }
    {
        nt_atlas_sprite_opts_t so = nt_atlas_sprite_opts_defaults();
        so.name = "alias_b";
        nt_builder_atlas_add_raw(ctx, buf, 16, 16, &so);
    }

    nt_builder_end_atlas(ctx);
    nt_build_result_t r = nt_builder_finish_pack(ctx);
    nt_builder_free_pack(ctx);
    return r == NT_BUILD_OK;
}

static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        (void)fclose(f);
        return NULL;
    }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) {
        (void)fclose(f);
        return NULL;
    }
    size_t rd = fread(b, 1, (size_t)sz, f);
    (void)fclose(f);
    if (rd != (size_t)sz) {
        free(b);
        return NULL;
    }
    *out_size = (size_t)sz;
    return b;
}

static const tp_sprite *find_sprite(const tp_result *res, const char *name) {
    for (int i = 0; i < res->sprite_count; i++) {
        if (strcmp(res->sprites[i].name, name) == 0) {
            return &res->sprites[i];
        }
    }
    return NULL;
}

static void sample_and_check(const tp_page *pg, int x, int y, const uint8_t c[4]) {
    TEST_ASSERT_TRUE(x >= 0 && y >= 0 && x < pg->w && y < pg->h);
    const uint8_t *p = &pg->rgba[((size_t)y * (size_t)pg->w + (size_t)x) * 4];
    TEST_ASSERT_EQUAL_UINT8(c[0], p[0]);
    TEST_ASSERT_EQUAL_UINT8(c[1], p[1]);
    TEST_ASSERT_EQUAL_UINT8(c[2], p[2]);
    TEST_ASSERT_EQUAL_UINT8(c[3], p[3]);
}

static void require_parsed(void) {
    TEST_ASSERT_NOT_NULL(g_results);
    TEST_ASSERT_TRUE(g_count >= 1);
    TEST_ASSERT_NOT_NULL(g_results[0]);
}

void test_parse_file_ok(void) {
    tp_error err;
    err.msg[0] = '\0';
    tp_status st = tp_pack_read_file(g_pack_path, g_names, g_arena, &g_results, &g_count, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, err.msg);
    TEST_ASSERT_EQUAL_INT(1, g_count);
    require_parsed();
    TEST_ASSERT_EQUAL_INT(4, g_results[0]->sprite_count);
}

void test_page(void) {
    require_parsed();
    const tp_result *res = g_results[0];
    TEST_ASSERT_EQUAL_STRING("interim_atlas", res->atlas_name);
    TEST_ASSERT_EQUAL_INT(1, res->page_count);
    TEST_ASSERT_NOT_NULL(res->pages);
    TEST_ASSERT_TRUE(res->pages[0].w > 0);
    TEST_ASSERT_TRUE(res->pages[0].h > 0);
    TEST_ASSERT_NOT_NULL(res->pages[0].rgba);
    /* default profile is premultiplied=true; reader must surface (not reject) it */
    TEST_ASSERT_TRUE(res->pages[0].premultiplied);
}

void test_pixels_per_unit(void) {
    require_parsed();
    TEST_ASSERT_TRUE(fabsf(g_results[0]->pixels_per_unit - 32.0f) < 1e-5f);
}

void test_opaque_rect(void) {
    require_parsed();
    const tp_result *res = g_results[0];
    const tp_sprite *s = find_sprite(res, "opaque_rect");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(s->trimmed);
    TEST_ASSERT_EQUAL_INT(0, s->spriteSourceSize.x);
    TEST_ASSERT_EQUAL_INT(0, s->spriteSourceSize.y);
    TEST_ASSERT_EQUAL_INT(20, s->spriteSourceSize.w);
    TEST_ASSERT_EQUAL_INT(12, s->spriteSourceSize.h);
    TEST_ASSERT_EQUAL_INT(20, s->sourceSize.w);
    TEST_ASSERT_EQUAL_INT(12, s->sourceSize.h);
    TEST_ASSERT_EQUAL_INT(20, s->frame.w);
    TEST_ASSERT_EQUAL_INT(12, s->frame.h);
    TEST_ASSERT_EQUAL_UINT8(0, s->transform);
    TEST_ASSERT_TRUE(fabsf(s->pivot.x - 0.5f) < 1e-5f);
    TEST_ASSERT_TRUE(fabsf(s->pivot.y - 0.5f) < 1e-5f);

    /* Independent check of frame.x/y AND both-axis orientation via known corner colors.
     * A y-flip in atlas_v handling would swap top/bottom and fail these. */
    const tp_page *pg = &res->pages[0];
    sample_and_check(pg, s->frame.x + 0, s->frame.y + 0, C_TL);
    sample_and_check(pg, s->frame.x + 19, s->frame.y + 0, C_TR);
    sample_and_check(pg, s->frame.x + 0, s->frame.y + 11, C_BL);
    sample_and_check(pg, s->frame.x + 19, s->frame.y + 11, C_BR);
}

void test_trimmed_sprite(void) {
    require_parsed();
    const tp_result *res = g_results[0];
    const tp_sprite *s = find_sprite(res, "trimmed_sprite");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->trimmed);
    TEST_ASSERT_EQUAL_INT(40, s->sourceSize.w);
    TEST_ASSERT_EQUAL_INT(30, s->sourceSize.h);
    TEST_ASSERT_EQUAL_INT(5, s->spriteSourceSize.x);  /* left strip */
    TEST_ASSERT_EQUAL_INT(8, s->spriteSourceSize.y);  /* top strip (source_h - trim_offset_y - trim_h) */
    TEST_ASSERT_EQUAL_INT(25, s->spriteSourceSize.w); /* trim_w */
    TEST_ASSERT_EQUAL_INT(17, s->spriteSourceSize.h); /* trim_h */
    TEST_ASSERT_EQUAL_INT(25, s->frame.w);
    TEST_ASSERT_EQUAL_INT(17, s->frame.h);

    /* content top-left = source(5,8); bottom-right = source(29,24). Both are C_TRIM,
     * but at the recovered frame rect -> pins frame origin + atlas_v y-down. */
    const tp_page *pg = &res->pages[0];
    sample_and_check(pg, s->frame.x, s->frame.y, C_TRIM);
    sample_and_check(pg, s->frame.x + 24, s->frame.y + 16, C_TRIM);
}

void test_alias(void) {
    require_parsed();
    const tp_result *res = g_results[0];
    const tp_sprite *a = find_sprite(res, "alias_a");
    const tp_sprite *b = find_sprite(res, "alias_b");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    int ia = -1;
    int ib = -1;
    for (int i = 0; i < res->sprite_count; i++) {
        if (&res->sprites[i] == a) {
            ia = i;
        }
        if (&res->sprites[i] == b) {
            ib = i;
        }
    }
    TEST_ASSERT_TRUE(ia >= 0 && ib >= 0);
    /* reader assigns original by sorted-by-name order: alias_a < alias_b */
    TEST_ASSERT_EQUAL_INT(-1, a->alias_of);
    TEST_ASSERT_EQUAL_INT(ia, b->alias_of);
    /* dedup -> shared placement */
    TEST_ASSERT_EQUAL_INT(a->page, b->page);
    TEST_ASSERT_EQUAL_INT(a->frame.x, b->frame.x);
    TEST_ASSERT_EQUAL_INT(a->frame.y, b->frame.y);
}

void test_neg_truncated(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_result **r = NULL;
    int c = -1;
    tp_error err;
    err.msg[0] = '\0';
    tp_status st = tp_pack_read_memory(g_bytes, g_size / 2, g_names, ar, &r, &c, &err);
    TEST_ASSERT_TRUE(st != TP_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(0, c);
    TEST_ASSERT_NULL(r);
    tp_arena_destroy(ar);
}

void test_neg_bad_magic(void) {
    uint8_t *copy = (uint8_t *)malloc(g_size);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, g_bytes, g_size);
    copy[0] ^= 0xFFu; /* corrupt magic */
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_result **r = NULL;
    int c = -1;
    tp_error err;
    err.msg[0] = '\0';
    tp_status st = tp_pack_read_memory(copy, g_size, g_names, ar, &r, &c, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_MAGIC, st);
    tp_arena_destroy(ar);
    free(copy);
}

void test_neg_unknown_region(void) {
    tp_name_map *empty = tp_name_map_create();
    TEST_ASSERT_NOT_NULL(empty);
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_result **r = NULL;
    int c = -1;
    tp_error err;
    err.msg[0] = '\0';
    /* empty map: atlas name falls back to hex (ok) but the first region name misses */
    tp_status st = tp_pack_read_memory(g_bytes, g_size, empty, ar, &r, &c, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNKNOWN_REGION, st);
    tp_arena_destroy(ar);
    tp_name_map_destroy(empty);
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    (void)snprintf(g_pack_path, sizeof g_pack_path, "%s/interim.ntpack", dir);

    if (!build_interim_pack(g_pack_path)) {
        (void)fprintf(stderr, "test_pack_read: failed to build %s\n", g_pack_path);
        return 1;
    }
    g_bytes = read_whole_file(g_pack_path, &g_size);
    if (!g_bytes) {
        (void)fprintf(stderr, "test_pack_read: failed to read %s\n", g_pack_path);
        return 1;
    }
    g_arena = tp_arena_create(0);
    g_names = tp_name_map_create();
    if (!g_arena || !g_names) {
        (void)fprintf(stderr, "test_pack_read: alloc failed\n");
        return 1;
    }
    (void)tp_name_map_insert(g_names, "interim_atlas");
    (void)tp_name_map_insert(g_names, "opaque_rect");
    (void)tp_name_map_insert(g_names, "trimmed_sprite");
    (void)tp_name_map_insert(g_names, "alias_a");
    (void)tp_name_map_insert(g_names, "alias_b");

    UNITY_BEGIN();
    RUN_TEST(test_parse_file_ok);
    RUN_TEST(test_page);
    RUN_TEST(test_pixels_per_unit);
    RUN_TEST(test_opaque_rect);
    RUN_TEST(test_trimmed_sprite);
    RUN_TEST(test_alias);
    RUN_TEST(test_neg_truncated);
    RUN_TEST(test_neg_bad_magic);
    RUN_TEST(test_neg_unknown_region);
    int rc = UNITY_END();

    tp_arena_destroy(g_arena);
    tp_name_map_destroy(g_names);
    free(g_bytes);
    return rc;
}
