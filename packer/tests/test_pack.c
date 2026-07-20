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
#include "tp_core/tp_build_worker.h"
#include "tp_fs_internal.h"
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

static bool write_tga_rgba(const char *path, const uint8_t *rgba, int width,
                           int height) {
    if (!path || !rgba || width < 1 || height < 1 || width > UINT16_MAX ||
        height > UINT16_MAX ||
        (size_t)width > (SIZE_MAX - 18U) / ((size_t)height * 4U)) {
        return false;
    }
    size_t pixel_bytes = (size_t)width * (size_t)height * 4U;
    size_t size = 18U + pixel_bytes;
    uint8_t *tga = (uint8_t *)calloc(size, 1U);
    if (!tga) {
        return false;
    }
    tga[2] = 2U;
    tga[12] = (uint8_t)((unsigned int)width & 0xffU);
    tga[13] = (uint8_t)((unsigned int)width >> 8U);
    tga[14] = (uint8_t)((unsigned int)height & 0xffU);
    tga[15] = (uint8_t)((unsigned int)height >> 8U);
    tga[16] = 32U;
    tga[17] = 0x28U; /* 8 alpha bits, top-left origin */
    for (size_t i = 0; i < pixel_bytes / 4U; i++) {
        tga[18U + i * 4U + 0U] = rgba[i * 4U + 2U];
        tga[18U + i * 4U + 1U] = rgba[i * 4U + 1U];
        tga[18U + i * 4U + 2U] = rgba[i * 4U + 0U];
        tga[18U + i * 4U + 3U] = rgba[i * 4U + 3U];
    }
    bool ok = tp_fs_write_file(path, tga, size);
    free(tga);
    return ok;
}

static void fill_path_fixture(uint8_t pixels[8 * 8 * 4]) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            uint8_t *pixel = &pixels[((size_t)y * 8U + (size_t)x) * 4U];
            pixel[0] = (uint8_t)(20 + x * 20);
            pixel[1] = (uint8_t)(30 + y * 18);
            pixel[2] = (uint8_t)(200 - x * 7 - y * 3);
            pixel[3] = (x == 0 && y == 0) ? 0U : 255U;
        }
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

void test_constraint_first_reject_contract(void) {
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_pack_settings settings;
    tp_result *result = NULL;
    tp_error error = {0};

    make_settings(&settings, g_dir);
    settings.max_size = 0;
    settings.padding = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_EQUAL_STRING("tp_pack: max_size 0 out of range [1..16384]",
                             error.msg);

    make_settings(&settings, g_dir);
    settings.padding = -1;
    settings.margin = settings.max_size + 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_EQUAL_STRING(
        "tp_pack: padding/margin/extrude must be >= 0", error.msg);

    make_settings(&settings, g_dir);
    settings.padding = settings.max_size + 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_EQUAL_STRING(
        "tp_pack: padding/margin/extrude must not exceed max_size 2048",
        error.msg);

    make_settings(&settings, g_dir);
    settings.shape = TP_PACK_SHAPE_MAX;
    settings.extrude = 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_EQUAL_STRING(
        "tp_pack: extrude > 0 is only valid for shape RECT (got shape 2)",
        error.msg);

    tp_pack_sprite_desc sprite = g_sprites[0];
    sprite.name = "bad_facts";
    sprite.ov_mask = TP_PACK_OV_SHAPE | TP_PACK_OV_ROTATE;
    sprite.ov_shape = 0;
    sprite.ov_allow_rotate = 0;
    make_settings(&settings, g_dir);
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_EQUAL_STRING(
        "tp_pack: sprite 'bad_facts' shape override 0 invalid", error.msg);

    sprite = g_sprites[0];
    sprite.name = "bad_spacing";
    sprite.ov_mask = TP_PACK_OV_MARGIN | TP_PACK_OV_EXTRUDE;
    sprite.ov_margin = 0;
    sprite.ov_extrude = 0;
    make_settings(&settings, g_dir);
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_EQUAL_STRING(
        "tp_pack: sprite 'bad_spacing' margin override 0 unrepresentable (omit to inherit, or use >= 1)",
        error.msg);

    sprite = g_sprites[0];
    sprite.name = "bad_slice9";
    sprite.ov_mask = TP_PACK_OV_SHAPE | TP_PACK_OV_EXTRUDE;
    sprite.ov_shape = TP_PACK_SPRITE_SHAPE_CONCAVE;
    sprite.ov_extrude = 1;
    sprite.slice9_lrtb[0] = 1;
    make_settings(&settings, g_dir);
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_EQUAL_STRING(
        "tp_pack: sprite 'bad_slice9' slice9 requires a RECT shape override",
        error.msg);

    tp_arena_destroy(arena);
}

/* Per-sprite packing overrides (owner scope 2026-07-10): a RECT override in a
 * CONCAVE atlas packs that sprite as an exact 4-vert rect; a NO-rotate override
 * yields an identity/flip-only transform (no diagonal bit). The default disc stays
 * a many-vert concave hull -> proves the override actually changed the shape. */
void test_sprite_override_rect_and_rotate(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_pack_sprite_desc sp[2];
    sp[0] = g_sprites[1]; /* disc_green 96x48 -> RECT + no-rotate override */
    sp[0].name = "green_rect";
    sp[0].ov_mask |= TP_PACK_OV_SHAPE | TP_PACK_OV_ROTATE;
    sp[0].ov_shape = TP_PACK_SPRITE_SHAPE_RECT;
    sp[0].ov_allow_rotate = TP_PACK_SPRITE_ROTATE_NO;
    sp[1] = g_sprites[0]; /* disc_red 64x64 -> inherits concave atlas shape */
    sp[1].name = "red_concave";

    tp_pack_settings s;
    tp_pack_settings_defaults(&s); /* shape = CONCAVE_CONTOUR */
    s.atlas_name = "ov_shape_test";
    s.work_dir = g_dir;
    s.sprites = sp;
    s.sprite_count = 2;
    s.pixels_per_unit = 1.0f;
    tp_result *r = NULL;
    tp_error e;
    e.msg[0] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_pack(&s, ar, &r, &e), e.msg);

    const tp_sprite *rect = find_sprite(r, "green_rect");
    TEST_ASSERT_NOT_NULL(rect);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, rect->vert_count, "RECT override must pack as a 4-vert rect");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rect->transform & TP_TRANSFORM_DIAGONAL, "no-rotate override forbids diagonal");
    /* no-rotate -> unrotated frame equals the trimmed source bounds exactly */
    TEST_ASSERT_EQUAL_INT(rect->spriteSourceSize.w, rect->frame.w);
    TEST_ASSERT_EQUAL_INT(rect->spriteSourceSize.h, rect->frame.h);

    const tp_sprite *concave = find_sprite(r, "red_concave");
    TEST_ASSERT_NOT_NULL(concave);
    TEST_ASSERT_TRUE_MESSAGE(concave->vert_count > 4, "inherited concave disc keeps a many-vert hull");
    tp_arena_destroy(ar);
}

/* Per-sprite override validation: an extrude override on a sprite whose effective
 * shape is non-RECT, and an out-of-range max_vertices override, both return
 * TP_STATUS_INVALID_ARGUMENT with the sprite named. */
void test_sprite_override_validation(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_pack_settings s;
    tp_result *r = NULL;
    tp_error e;

    /* extrude override on a concave-shaped sprite */
    tp_pack_sprite_desc bad_ex = g_sprites[0];
    bad_ex.name = "bad_extrude";
    bad_ex.ov_mask |= TP_PACK_OV_EXTRUDE;
    bad_ex.ov_extrude = 3; /* effective shape CONCAVE -> invalid */
    tp_pack_settings_defaults(&s);
    s.atlas_name = "ov_bad_ex";
    s.work_dir = g_dir;
    s.sprites = &bad_ex;
    s.sprite_count = 1;
    s.pixels_per_unit = 1.0f;
    e.msg[0] = '\0';
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_pack(&s, ar, &r, &e));
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.msg, "bad_extrude"), "extrude error must name the sprite");

    /* max_vertices override out of range */
    tp_pack_sprite_desc bad_mv = g_sprites[0];
    bad_mv.name = "bad_maxv";
    bad_mv.ov_mask |= TP_PACK_OV_MAXVERT;
    bad_mv.ov_max_vertices = 99;
    tp_pack_settings_defaults(&s);
    s.atlas_name = "ov_bad_mv";
    s.work_dir = g_dir;
    s.sprites = &bad_mv;
    s.sprite_count = 1;
    s.pixels_per_unit = 1.0f;
    r = NULL;
    e.msg[0] = '\0';
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_pack(&s, ar, &r, &e));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.msg, "bad_maxv"), "max_vertices error must name the sprite");

    /* Unknown presence bits are rejected instead of being silently ignored. */
    tp_pack_sprite_desc bad_mask = g_sprites[0];
    bad_mask.name = "bad_override_mask";
    bad_mask.ov_mask = UINT8_C(0x80);
    tp_pack_settings_defaults(&s);
    s.atlas_name = "ov_bad_mask";
    s.work_dir = g_dir;
    s.sprites = &bad_mask;
    s.sprite_count = 1;
    s.pixels_per_unit = 1.0F;
    r = NULL;
    e.msg[0] = '\0';
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_pack(&s, ar, &r, &e));
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.msg, "ov_mask"),
                                 "unknown override-mask error must name the field");
    tp_arena_destroy(ar);
}

void test_ascii_path_and_equivalent_raw_pixels_pack_byte_identically(void) {
    uint8_t pixels[8 * 8 * 4];
    fill_path_fixture(pixels);
    char image_path[1024];
    char pack_path[1024];
    TEST_ASSERT_TRUE(snprintf(image_path, sizeof image_path, "%s/path-source.tga",
                              g_dir) > 0);
    TEST_ASSERT_TRUE(write_tga_rgba(image_path, pixels, 8, 8));

    tp_pack_sprite_desc sprite = {
        .name = "path_equivalence",
        .path = image_path,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "path_equivalence";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;

    tp_arena *path_arena = tp_arena_create(0);
    tp_arena *raw_arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(path_arena);
    TEST_ASSERT_NOT_NULL(raw_arena);
    tp_result *path_result = NULL;
    tp_result *raw_result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_pack(&settings, path_arena, &path_result,
                                          &error),
                                  error.msg);
    TEST_ASSERT_TRUE(snprintf(pack_path, sizeof pack_path,
                              "%s/path_equivalence.ntpack", g_dir) > 0);
    size_t path_size = 0U;
    uint8_t *path_bytes = read_whole_file(pack_path, &path_size);
    TEST_ASSERT_NOT_NULL(path_bytes);

    sprite.path = NULL;
    sprite.rgba = pixels;
    sprite.w = 8;
    sprite.h = 8;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_pack(&settings, raw_arena, &raw_result,
                                          &error),
                                  error.msg);
    size_t raw_size = 0U;
    uint8_t *raw_bytes = read_whole_file(pack_path, &raw_size);
    TEST_ASSERT_NOT_NULL(raw_bytes);
    TEST_ASSERT_EQUAL_size_t(path_size, raw_size);
    TEST_ASSERT_EQUAL_INT(0, memcmp(path_bytes, raw_bytes, path_size));
    compare_results(path_result, raw_result);

    free(path_bytes);
    free(raw_bytes);
    tp_arena_destroy(path_arena);
    tp_arena_destroy(raw_arena);
}

void test_unicode_path_source_packs_without_engine_path_io(void) {
    static const char leaf[] =
        "\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82.tga";
    uint8_t pixels[8 * 8 * 4];
    fill_path_fixture(pixels);
    char image_path[1024];
    TEST_ASSERT_TRUE(snprintf(image_path, sizeof image_path, "%s/%s", g_dir,
                              leaf) > 0);
    TEST_ASSERT_TRUE(write_tga_rgba(image_path, pixels, 8, 8));

    tp_pack_sprite_desc sprite = {
        .name = "unicode_path",
        .path = image_path,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "unicode_path_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_pack(&settings, arena, &result, &error),
                                  error.msg);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(1, result->sprite_count);
    TEST_ASSERT_EQUAL_INT(8, result->sprites[0].sourceSize.w);
    TEST_ASSERT_EQUAL_INT(8, result->sprites[0].sourceSize.h);
    tp_arena_destroy(arena);
}

void test_corrupt_path_source_returns_structured_error(void) {
    char image_path[1024];
    TEST_ASSERT_TRUE(snprintf(image_path, sizeof image_path, "%s/corrupt-source.img",
                              g_dir) > 0);
    static const uint8_t corrupt[] = {1U, 2U, 3U, 4U};
    TEST_ASSERT_TRUE(tp_fs_write_file(image_path, corrupt, sizeof corrupt));
    tp_pack_sprite_desc sprite = {
        .name = "corrupt_path",
        .path = image_path,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "corrupt_path_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNSUPPORTED_TEXTURE,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "corrupt_path"));
    tp_arena_destroy(arena);
}

void test_fully_transparent_sprite_returns_structured_error(void) {
    uint8_t transparent[8 * 8 * 4] = {0};
    tp_pack_sprite_desc sprite = {
        .name = "transparent",
        .rgba = transparent,
        .w = 8,
        .h = 8,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "transparent_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    settings.alpha_threshold = 1;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "transparent"));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "fully transparent"));
    tp_arena_destroy(arena);
}

void test_invalid_slice9_borders_return_structured_error(void) {
    uint8_t pixels[8 * 8 * 4];
    fill_path_fixture(pixels);
    tp_pack_sprite_desc sprite = {
        .name = "bad_slice9",
        .rgba = pixels,
        .w = 8,
        .h = 8,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
        .slice9_lrtb = {4U, 4U, 0U, 0U},
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "bad_slice9_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "bad_slice9"));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "left+right"));
    tp_arena_destroy(arena);
}

void test_nonrect_slice9_override_returns_structured_error(void) {
    uint8_t pixels[8 * 8 * 4];
    fill_path_fixture(pixels);
    tp_pack_sprite_desc sprite = {
        .name = "bad_slice9_shape",
        .rgba = pixels,
        .w = 8,
        .h = 8,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
        .slice9_lrtb = {1U, 1U, 1U, 1U},
        .ov_mask = TP_PACK_OV_SHAPE,
        .ov_shape = TP_PACK_SPRITE_SHAPE_CONCAVE,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "bad_slice9_shape_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "bad_slice9_shape"));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "RECT"));
    tp_arena_destroy(arena);
}

void test_sprite_larger_than_page_returns_structured_error(void) {
    uint8_t pixels[8 * 8 * 4];
    fill_path_fixture(pixels);
    tp_pack_sprite_desc sprite = {
        .name = "too_large",
        .rgba = pixels,
        .w = 8,
        .h = 8,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "too_large_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    settings.max_size = 7;
    settings.padding = 0;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "too_large"));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "cannot fit"));
    tp_arena_destroy(arena);
}

void test_atlas_spacing_larger_than_page_returns_structured_error(void) {
    tp_pack_settings settings;
    make_settings(&settings, g_dir);
    settings.max_size = 16;
    settings.padding = 17;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "padding"));
    tp_arena_destroy(arena);
}

void test_atlas_margin_larger_than_page_returns_structured_error(void) {
    tp_pack_settings settings;
    make_settings(&settings, g_dir);
    settings.max_size = 16;
    settings.padding = 0;
    settings.margin = 17;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "margin"));
    tp_arena_destroy(arena);
}

void test_atlas_extrude_larger_than_page_returns_structured_error(void) {
    tp_pack_settings settings;
    make_settings(&settings, g_dir);
    settings.max_size = 16;
    settings.padding = 0;
    settings.margin = 0;
    settings.extrude = 17;
    settings.shape = TP_PACK_SHAPE_MIN;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "extrude"));
    tp_arena_destroy(arena);
}

void test_sprite_equal_to_page_boundary_returns_structured_error(void) {
    uint8_t pixels[8 * 8 * 4];
    fill_path_fixture(pixels);
    tp_pack_sprite_desc sprite = {
        .name = "equal_page",
        .rgba = pixels,
        .w = 8,
        .h = 8,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "equal_page_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    settings.max_size = 8;
    settings.padding = 0;
    settings.margin = 0;
    settings.shape = TP_PACK_SHAPE_MIN;
    settings.power_of_two = false;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "equal_page"));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "cannot fit"));
    tp_arena_destroy(arena);
}

void test_base_margin_footprint_returns_structured_error(void) {
    uint8_t pixels[8 * 8 * 4];
    fill_path_fixture(pixels);
    tp_pack_sprite_desc sprite = {
        .name = "margin_footprint",
        .rgba = pixels,
        .w = 8,
        .h = 8,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "margin_footprint_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    settings.max_size = 10;
    settings.padding = 0;
    settings.margin = 1;
    settings.shape = TP_PACK_SHAPE_MIN;
    settings.power_of_two = false;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "margin_footprint"));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "cannot fit"));
    tp_arena_destroy(arena);
}

void test_base_padding_footprint_returns_structured_error(void) {
    uint8_t pixels[8 * 8 * 4];
    fill_path_fixture(pixels);
    tp_pack_sprite_desc sprite = {
        .name = "padding_footprint",
        .rgba = pixels,
        .w = 8,
        .h = 8,
        .origin_x = 0.5F,
        .origin_y = 0.5F,
    };
    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "padding_footprint_pack";
    settings.work_dir = g_dir;
    settings.sprites = &sprite;
    settings.sprite_count = 1;
    settings.max_size = 10;
    settings.padding = 2;
    settings.margin = 0;
    settings.shape = TP_PACK_SHAPE_MIN;
    settings.power_of_two = false;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "padding_footprint"));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "cannot fit"));
    tp_arena_destroy(arena);
}

void test_provable_more_than_page_limit_returns_structured_error(void) {
    enum { SPRITE_COUNT = 74, WIDTH = 15, HEIGHT = 15 };
    uint8_t pixels[SPRITE_COUNT][WIDTH * HEIGHT * 4];
    char names[SPRITE_COUNT][32];
    tp_pack_sprite_desc sprites[SPRITE_COUNT];
    memset(sprites, 0, sizeof sprites);
    for (int i = 0; i < SPRITE_COUNT; i++) {
        (void)snprintf(names[i], sizeof names[i], "page_limit_%02d", i);
        for (int p = 0; p < WIDTH * HEIGHT; p++) {
            pixels[i][p * 4 + 0] = (uint8_t)(i + 1);
            pixels[i][p * 4 + 1] = (uint8_t)(i * 3 + 7);
            pixels[i][p * 4 + 2] = (uint8_t)(i * 5 + 11);
            pixels[i][p * 4 + 3] = 255U;
        }
        sprites[i] = (tp_pack_sprite_desc){
            .name = names[i],
            .rgba = pixels[i],
            .w = WIDTH,
            .h = HEIGHT,
            .origin_x = 0.5F,
            .origin_y = 0.5F,
        };
    }

    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "provable_page_limit";
    settings.work_dir = g_dir;
    settings.sprites = sprites;
    settings.sprite_count = SPRITE_COUNT;
    settings.max_size = 16;
    settings.padding = 0;
    settings.margin = 0;
    settings.extrude = 0;
    settings.shape = TP_PACK_SHAPE_MIN;
    settings.power_of_two = false;

    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_pack(&settings, arena, &result, &error));
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "64"));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "opaque"));
    tp_arena_destroy(arena);
}

void test_exact_duplicates_do_not_consume_page_lower_bound_twice(void) {
    enum { SPRITE_COUNT = 74, WIDTH = 15, HEIGHT = 15 };
    uint8_t pixels[WIDTH * HEIGHT * 4];
    char names[SPRITE_COUNT][32];
    tp_pack_sprite_desc sprites[SPRITE_COUNT];
    for (int p = 0; p < WIDTH * HEIGHT; p++) {
        pixels[p * 4 + 0] = 31U;
        pixels[p * 4 + 1] = 47U;
        pixels[p * 4 + 2] = 59U;
        pixels[p * 4 + 3] = 255U;
    }
    memset(sprites, 0, sizeof sprites);
    for (int i = 0; i < SPRITE_COUNT; i++) {
        (void)snprintf(names[i], sizeof names[i], "page_duplicate_%02d", i);
        sprites[i] = (tp_pack_sprite_desc){
            .name = names[i],
            .rgba = pixels,
            .w = WIDTH,
            .h = HEIGHT,
            .origin_x = 0.5F,
            .origin_y = 0.5F,
        };
    }

    tp_pack_settings settings;
    tp_pack_settings_defaults(&settings);
    settings.atlas_name = "duplicate_page_lower_bound";
    settings.work_dir = g_dir;
    settings.sprites = sprites;
    settings.sprite_count = SPRITE_COUNT;
    settings.max_size = 16;
    settings.padding = 0;
    settings.margin = 0;
    settings.extrude = 0;
    settings.shape = TP_PACK_SHAPE_MIN;
    settings.power_of_two = false;

    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_pack(&settings, arena, &result, &error),
                                  error.msg);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(SPRITE_COUNT, result->sprite_count);
    TEST_ASSERT_EQUAL_INT(1, result->page_count);
    tp_arena_destroy(arena);
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
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
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
    RUN_TEST(test_constraint_first_reject_contract);
    RUN_TEST(test_sprite_override_rect_and_rotate);
    RUN_TEST(test_sprite_override_validation);
    RUN_TEST(test_ascii_path_and_equivalent_raw_pixels_pack_byte_identically);
    RUN_TEST(test_unicode_path_source_packs_without_engine_path_io);
    RUN_TEST(test_corrupt_path_source_returns_structured_error);
    RUN_TEST(test_fully_transparent_sprite_returns_structured_error);
    RUN_TEST(test_invalid_slice9_borders_return_structured_error);
    RUN_TEST(test_nonrect_slice9_override_returns_structured_error);
    RUN_TEST(test_sprite_larger_than_page_returns_structured_error);
    RUN_TEST(test_atlas_spacing_larger_than_page_returns_structured_error);
    RUN_TEST(test_atlas_margin_larger_than_page_returns_structured_error);
    RUN_TEST(test_atlas_extrude_larger_than_page_returns_structured_error);
    RUN_TEST(test_sprite_equal_to_page_boundary_returns_structured_error);
    RUN_TEST(test_base_margin_footprint_returns_structured_error);
    RUN_TEST(test_base_padding_footprint_returns_structured_error);
    RUN_TEST(test_provable_more_than_page_limit_returns_structured_error);
    RUN_TEST(test_exact_duplicates_do_not_consume_page_lower_bound_twice);
    int rc = UNITY_END();

    tp_arena_destroy(g_arena);
    return rc;
}
