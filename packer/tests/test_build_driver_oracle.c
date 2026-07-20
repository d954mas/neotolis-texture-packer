/* Byte-identical normal-result oracle (decision 0018, ROADMAP H0.3/H0.5).
 *
 * The in-process driver (and tp_pack, which now calls it) must produce a
 * .ntpack byte-for-byte equal to a direct nt_builder invocation with the same
 * validated settings and pixels. This pins the normal result across the H0.3-a
 * extraction and stays as the permanent regression when later slices route the
 * same driver body through the private worker process + protocol. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"
#include "tp_build_driver_internal.h"
#include "unity.h"

#include "nt_builder.h"

static const char *g_dir;

void setUp(void) {}
void tearDown(void) {}

static void fill_gradient(uint8_t *rgba, uint32_t w, uint32_t h, uint8_t seed) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *px = &rgba[((size_t)y * w + x) * 4U];
            px[0] = (uint8_t)(seed + x * 3U);
            px[1] = (uint8_t)(seed + y * 5U);
            px[2] = (uint8_t)(x + y);
            px[3] = (x == 0U && y == 0U) ? 0U : 255U; /* one transparent corner -> a real trim */
        }
    }
}

static uint8_t g_a[48 * 32 * 4];
static uint8_t g_b[16 * 40 * 4];
static uint8_t g_c[24 * 24 * 4];

static void make_settings(tp_pack_settings *s, const char *atlas_name) {
    static tp_pack_sprite_desc sprites[3];
    fill_gradient(g_a, 48, 32, 20);
    fill_gradient(g_b, 16, 40, 90);
    fill_gradient(g_c, 24, 24, 150);
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "alpha", .rgba = g_a, .w = 48, .h = 32, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[1] = (tp_pack_sprite_desc){.name = "beta", .rgba = g_b, .w = 16, .h = 40, .origin_x = 0.25F, .origin_y = 0.75F};
    sprites[2] = (tp_pack_sprite_desc){.name = "gamma", .rgba = g_c, .w = 24, .h = 24, .origin_x = 0.5F, .origin_y = 0.5F};

    tp_pack_settings_defaults(s);
    s->atlas_name = atlas_name;
    s->work_dir = g_dir;
    s->sprites = sprites;
    s->sprite_count = 3;
    s->pixels_per_unit = 8.0F;
}

/* Independent reference: the export-friendly nt_builder sequence, kept a literal
 * mirror of the driver so it pins the exact call profile rather than reusing it. */
static void direct_nt_build(const tp_pack_settings *s, const char *out_path) {
    NtBuilderContext *ctx = nt_builder_start_pack(out_path);
    TEST_ASSERT_NOT_NULL(ctx);
    nt_builder_set_threads_auto(ctx);
    nt_atlas_opts_t o = nt_atlas_opts_defaults();
    o.premultiplied = false;
    o.compress = NULL;
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
        nt_builder_atlas_add_raw(ctx, sp->rgba, (uint32_t)sp->w, (uint32_t)sp->h, &so);
    }
    nt_builder_end_atlas(ctx);
    nt_build_result_t br = nt_builder_finish_pack(ctx);
    nt_builder_free_pack(ctx);
    TEST_ASSERT_EQUAL_INT(NT_BUILD_OK, br);
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    long sz = ftell(f);
    TEST_ASSERT_TRUE(sz > 0);
    rewind(f);
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_size_t((size_t)sz, fread(b, 1, (size_t)sz, f));
    (void)fclose(f);
    *out_size = (size_t)sz;
    return b;
}

static void assert_files_identical(const char *lhs, const char *rhs) {
    size_t ln = 0, rn = 0;
    uint8_t *lb = read_file(lhs, &ln);
    uint8_t *rb = read_file(rhs, &rn);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(rn, ln, "oracle: pack sizes differ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(lb, rb, ln), "oracle: pack bytes differ");
    free(lb);
    free(rb);
}

/* The extracted driver == a direct nt_builder invocation, byte for byte. */
void test_driver_matches_direct_nt_builder(void) {
    char driver_path[1024];
    char direct_path[1024];
    TEST_ASSERT_TRUE(snprintf(driver_path, sizeof driver_path, "%s/oracle_driver.ntpack", g_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(direct_path, sizeof direct_path, "%s/oracle_direct.ntpack", g_dir) > 0);

    tp_pack_settings s;
    make_settings(&s, "oracle");
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_build_driver_run(&s, NULL, driver_path, &err), err.msg);
    direct_nt_build(&s, direct_path);
    assert_files_identical(driver_path, direct_path);
}

/* The full tp_pack path (validate/preflight/name-map + driver) matches too. */
void test_tp_pack_matches_direct_nt_builder(void) {
    char direct_path[1024];
    char pack_path[1024];
    TEST_ASSERT_TRUE(snprintf(direct_path, sizeof direct_path, "%s/oracle_direct2.ntpack", g_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(pack_path, sizeof pack_path, "%s/oracle.ntpack", g_dir) > 0);

    tp_pack_settings s;
    make_settings(&s, "oracle"); /* tp_pack writes <work_dir>/oracle.ntpack */
    direct_nt_build(&s, direct_path);

    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *result = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_pack(&s, arena, &result, &err), err.msg);
    TEST_ASSERT_NOT_NULL(result);
    assert_files_identical(pack_path, direct_path);
    tp_arena_destroy(arena);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    UNITY_BEGIN();
    RUN_TEST(test_driver_matches_direct_nt_builder);
    RUN_TEST(test_tp_pack_matches_direct_nt_builder);
    return UNITY_END();
}
