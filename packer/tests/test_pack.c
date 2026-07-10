/* Phase 1b tp_pack invariants + determinism (plan §3.4, task 10).
 *
 * Packs the smoke disc set (reusing apps/smoke's fill_disc) plus one INJECTED
 * DUPLICATE (identical pixels, different name) through tp_pack into a scratch
 * dir (argv[1]), then asserts the recovered tp_result:
 *   - TP_STATUS_OK, non-NULL result, page_count >= 1 with sane dims.
 *   - stable ordering: sprites strictly ascending by strcmp(name).
 *   - alias_of: the duplicate resolves to its original's index; original == -1.
 *   - premultiplied == false on every page (export-profile flag, §2.3/§5).
 *   - pixels_per_unit round-trips the settings value.
 *   - frame inside page + untrimmed => spriteSourceSize full (§2.6).
 * Determinism: two runs (separate arenas) produce a byte-identical session
 * .ntpack AND field-identical results. Negatives: invalid atlas_name and a
 * duplicate sprite NAME both return an error rather than crashing. */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"
#include "unity.h"

/* --- sprite pixel generation (apps/smoke fill_disc, verbatim) --- */
static void fill_disc(uint8_t *rgba, uint32_t w, uint32_t h, const uint8_t color[4]) {
    const float cx = (float)w * 0.5F;
    const float cy = (float)h * 0.5F;
    const float r = ((float)(w < h ? w : h)) * 0.5F - 1.0F;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const float dx = ((float)x + 0.5F) - cx;
            const float dy = ((float)y + 0.5F) - cy;
            uint8_t *px = &rgba[(y * w + x) * 4];
            if (dx * dx + dy * dy <= r * r) {
                px[0] = color[0];
                px[1] = color[1];
                px[2] = color[2];
                px[3] = color[3];
            } else {
                px[0] = px[1] = px[2] = px[3] = 0;
            }
        }
    }
}

/* red and red_dup share the same buffer -> byte-identical pixels -> the builder
 * dedups them, and the reader recovers the alias relationship. */
static uint8_t g_red[64 * 64 * 4];
static uint8_t g_green[96 * 48 * 4];
static uint8_t g_blue[33 * 90 * 4];
static tp_pack_sprite_desc g_sprites[4];

static const char *g_dir;
static tp_arena *g_arena;
static tp_result *g_res;

static void init_sprites(void) {
    static const uint8_t red[4] = {230, 80, 60, 255};
    static const uint8_t green[4] = {70, 180, 90, 255};
    static const uint8_t blue[4] = {60, 110, 220, 255};
    fill_disc(g_red, 64u, 64u, red);
    fill_disc(g_green, 96u, 48u, green);
    fill_disc(g_blue, 33u, 90u, blue);

    memset(g_sprites, 0, sizeof g_sprites);
    g_sprites[0] = (tp_pack_sprite_desc){.name = "disc_red", .rgba = g_red, .w = 64, .h = 64, .origin_x = 0.5f, .origin_y = 0.5f};
    g_sprites[1] = (tp_pack_sprite_desc){.name = "disc_green", .rgba = g_green, .w = 96, .h = 48, .origin_x = 0.5f, .origin_y = 0.5f};
    g_sprites[2] = (tp_pack_sprite_desc){.name = "disc_blue", .rgba = g_blue, .w = 33, .h = 90, .origin_x = 0.5f, .origin_y = 0.5f};
    /* injected duplicate: identical pixels to disc_red, different (later-sorting) name. */
    g_sprites[3] = (tp_pack_sprite_desc){.name = "disc_red_dup", .rgba = g_red, .w = 64, .h = 64, .origin_x = 0.5f, .origin_y = 0.5f};
}

#define TEST_PPU 16.0f

static void make_settings(tp_pack_settings *s, const char *work_dir) {
    tp_pack_settings_defaults(s);
    s->atlas_name = "pack_test";
    s->work_dir = work_dir;
    s->sprites = g_sprites;
    s->sprite_count = 4;
    s->pixels_per_unit = TEST_PPU;
}

void setUp(void) {}
void tearDown(void) {}

// #region helpers
static const tp_sprite *find_sprite(const tp_result *res, const char *name) {
    for (int i = 0; i < res->sprite_count; i++) {
        if (strcmp(res->sprites[i].name, name) == 0) {
            return &res->sprites[i];
        }
    }
    return NULL;
}

static int sprite_index(const tp_result *res, const char *name) {
    for (int i = 0; i < res->sprite_count; i++) {
        if (strcmp(res->sprites[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int expected_src(const char *name, bool want_h) {
    if (strcmp(name, "disc_green") == 0) {
        return want_h ? 48 : 96;
    }
    if (strcmp(name, "disc_blue") == 0) {
        return want_h ? 90 : 33;
    }
    return 64; /* disc_red, disc_red_dup */
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

static void compare_results(const tp_result *a, const tp_result *b) {
    TEST_ASSERT_EQUAL_STRING(a->atlas_name, b->atlas_name);
    TEST_ASSERT_TRUE(fabsf(a->pixels_per_unit - b->pixels_per_unit) < 1e-6f);
    TEST_ASSERT_EQUAL_INT(a->page_count, b->page_count);
    for (int p = 0; p < a->page_count; p++) {
        const tp_page *pa = &a->pages[p];
        const tp_page *pb = &b->pages[p];
        TEST_ASSERT_EQUAL_INT(pa->w, pb->w);
        TEST_ASSERT_EQUAL_INT(pa->h, pb->h);
        TEST_ASSERT_EQUAL_INT(pa->premultiplied ? 1 : 0, pb->premultiplied ? 1 : 0);
        TEST_ASSERT_EQUAL_INT(0, memcmp(pa->rgba, pb->rgba, (size_t)pa->w * (size_t)pa->h * 4u));
    }
    TEST_ASSERT_EQUAL_INT(a->sprite_count, b->sprite_count);
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_sprite *sa = &a->sprites[i];
        const tp_sprite *sb = &b->sprites[i];
        TEST_ASSERT_EQUAL_STRING(sa->name, sb->name);
        TEST_ASSERT_EQUAL_INT(sa->page, sb->page);
        TEST_ASSERT_EQUAL_INT(sa->frame.x, sb->frame.x);
        TEST_ASSERT_EQUAL_INT(sa->frame.y, sb->frame.y);
        TEST_ASSERT_EQUAL_INT(sa->frame.w, sb->frame.w);
        TEST_ASSERT_EQUAL_INT(sa->frame.h, sb->frame.h);
        TEST_ASSERT_EQUAL_INT(sa->transform, sb->transform);
        TEST_ASSERT_EQUAL_INT(sa->alias_of, sb->alias_of);
    }
}
// #endregion

// #region tests
void test_ok_pages_and_flag(void) {
    TEST_ASSERT_NOT_NULL(g_res);
    TEST_ASSERT_EQUAL_STRING("pack_test", g_res->atlas_name);
    TEST_ASSERT_TRUE(g_res->page_count >= 1);
    TEST_ASSERT_EQUAL_INT(4, g_res->sprite_count);
    for (int p = 0; p < g_res->page_count; p++) {
        const tp_page *pg = &g_res->pages[p];
        TEST_ASSERT_TRUE(pg->w > 0 && pg->h > 0);
        TEST_ASSERT_NOT_NULL(pg->rgba);
        /* export-profile: straight alpha (the ONLY place this flag is asserted). */
        TEST_ASSERT_FALSE_MESSAGE(pg->premultiplied, "export profile must yield straight-alpha pages");
    }
}

void test_stable_ordering(void) {
    for (int i = 1; i < g_res->sprite_count; i++) {
        TEST_ASSERT_TRUE_MESSAGE(strcmp(g_res->sprites[i - 1].name, g_res->sprites[i].name) < 0,
                                 "sprites are not strictly ascending by name");
    }
}

void test_sprites_present_and_inside_page(void) {
    static const char *names[4] = {"disc_blue", "disc_green", "disc_red", "disc_red_dup"};
    for (int k = 0; k < 4; k++) {
        const tp_sprite *s = find_sprite(g_res, names[k]);
        TEST_ASSERT_NOT_NULL_MESSAGE(s, names[k]);
        TEST_ASSERT_EQUAL_INT(expected_src(names[k], false), s->sourceSize.w);
        TEST_ASSERT_EQUAL_INT(expected_src(names[k], true), s->sourceSize.h);

        TEST_ASSERT_TRUE(s->page >= 0 && s->page < g_res->page_count);
        const tp_page *pg = &g_res->pages[s->page];
        int aabb_w = (s->transform & 4u) ? s->frame.h : s->frame.w;
        int aabb_h = (s->transform & 4u) ? s->frame.w : s->frame.h;
        TEST_ASSERT_TRUE(s->frame.x >= 0 && s->frame.y >= 0);
        TEST_ASSERT_TRUE(s->frame.x + aabb_w <= pg->w && s->frame.y + aabb_h <= pg->h);

        if (!s->trimmed) {
            TEST_ASSERT_EQUAL_INT(0, s->spriteSourceSize.x);
            TEST_ASSERT_EQUAL_INT(0, s->spriteSourceSize.y);
            TEST_ASSERT_EQUAL_INT(s->sourceSize.w, s->spriteSourceSize.w);
            TEST_ASSERT_EQUAL_INT(s->sourceSize.h, s->spriteSourceSize.h);
        }
    }
}

void test_alias_relationship(void) {
    const tp_sprite *red = find_sprite(g_res, "disc_red");
    const tp_sprite *dup = find_sprite(g_res, "disc_red_dup");
    TEST_ASSERT_NOT_NULL(red);
    TEST_ASSERT_NOT_NULL(dup);
    int ired = sprite_index(g_res, "disc_red");
    /* "disc_red" sorts before "disc_red_dup" -> red is the original. */
    TEST_ASSERT_EQUAL_INT(-1, red->alias_of);
    TEST_ASSERT_EQUAL_INT(ired, dup->alias_of);
    TEST_ASSERT_EQUAL_INT(red->page, dup->page);
    TEST_ASSERT_EQUAL_INT(red->frame.x, dup->frame.x);
    TEST_ASSERT_EQUAL_INT(red->frame.y, dup->frame.y);
    TEST_ASSERT_EQUAL_INT(red->transform, dup->transform);
}

void test_pixels_per_unit(void) {
    TEST_ASSERT_TRUE(fabsf(g_res->pixels_per_unit - TEST_PPU) < 1e-5f);
}

void test_determinism(void) {
    tp_arena *a1 = tp_arena_create(0);
    tp_arena *a2 = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(a1);
    TEST_ASSERT_NOT_NULL(a2);
    tp_pack_settings s1;
    tp_pack_settings s2;
    make_settings(&s1, g_dir);
    make_settings(&s2, g_dir);
    tp_result *r1 = NULL;
    tp_result *r2 = NULL;
    tp_error e;
    e.msg[0] = '\0';

    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_pack(&s1, a1, &r1, &e), e.msg);
    char path[600];
    (void)snprintf(path, sizeof path, "%s/pack_test.ntpack", g_dir);
    size_t n1 = 0;
    uint8_t *b1 = read_whole_file(path, &n1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_pack(&s2, a2, &r2, &e), e.msg); /* overwrites file */
    size_t n2 = 0;
    uint8_t *b2 = read_whole_file(path, &n2);

    TEST_ASSERT_NOT_NULL(b1);
    TEST_ASSERT_NOT_NULL(b2);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n1, (uint32_t)n2, "determinism: session pack sizes differ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(b1, b2, n1), "determinism: session pack bytes differ");
    compare_results(r1, r2);

    free(b1);
    free(b2);
    tp_arena_destroy(a1);
    tp_arena_destroy(a2);
}

void test_neg_invalid_atlas_name(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_pack_settings s;
    make_settings(&s, g_dir);
    s.atlas_name = "a/../b";
    tp_result *r = NULL;
    tp_error e;
    e.msg[0] = '\0';
    tp_status st = tp_pack(&s, ar, &r, &e);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, st);
    TEST_ASSERT_NULL(r);
    tp_arena_destroy(ar);
}

void test_neg_duplicate_sprite_name(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_pack_sprite_desc dup[2];
    dup[0] = g_sprites[0]; /* disc_red */
    dup[1] = g_sprites[0]; /* same name "disc_red" again */
    tp_pack_settings s;
    tp_pack_settings_defaults(&s);
    s.atlas_name = "dup_test";
    s.work_dir = g_dir;
    s.sprites = dup;
    s.sprite_count = 2;
    s.pixels_per_unit = 1.0f;
    tp_result *r = NULL;
    tp_error e;
    e.msg[0] = '\0';
    tp_status st = tp_pack(&s, ar, &r, &e);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, st);
    TEST_ASSERT_NULL(r);
    tp_arena_destroy(ar);
}
// #endregion

static bool setup_all(const char *dir) {
    g_dir = dir;
    init_sprites();
    g_arena = tp_arena_create(0);
    if (!g_arena) {
        (void)fprintf(stderr, "test_pack: arena alloc failed\n");
        return false;
    }
    tp_pack_settings s;
    make_settings(&s, dir);
    tp_error e;
    e.msg[0] = '\0';
    tp_status st = tp_pack(&s, g_arena, &g_res, &e);
    if (st != TP_STATUS_OK) {
        (void)fprintf(stderr, "test_pack: tp_pack failed: %s (%s)\n", tp_status_str(st), e.msg);
        return false;
    }
    if (!g_res) {
        (void)fprintf(stderr, "test_pack: tp_pack returned NULL result\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    if (!setup_all(dir)) {
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_ok_pages_and_flag);
    RUN_TEST(test_stable_ordering);
    RUN_TEST(test_sprites_present_and_inside_page);
    RUN_TEST(test_alias_relationship);
    RUN_TEST(test_pixels_per_unit);
    RUN_TEST(test_determinism);
    RUN_TEST(test_neg_invalid_atlas_name);
    RUN_TEST(test_neg_duplicate_sprite_name);
    int rc = UNITY_END();

    tp_arena_destroy(g_arena);
    return rc;
}
