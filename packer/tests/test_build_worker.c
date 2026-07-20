/* Private build-worker outcome + oracle suite (decision 0018, ROADMAP H0.3-b).
 *
 * Proves the process boundary end to end:
 *  - a normal pack routed THROUGH the worker process is byte-identical to a
 *    direct nt_builder invocation (the worker child runs the same driver);
 *  - a crashing worker returns BUILDER_CRASHED, the host survives, and a prior
 *    artifact is not overwritten (no publish);
 *  - a malformed reply after a clean exit fails closed as BUILDER_FAILED;
 *  - a non-zero exit paired with a VALID error reply maps to BUILDER_FAILED and
 *    carries the worker's structured message.
 *
 * The test executable is BOTH the parent and the normal worker: main() dispatches
 * the hidden __build-worker argv the same way every shipping/pack exe must. The
 * fault outcomes use dedicated fault-worker binaries passed on argv. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"
#include "tp_build_worker_internal.h"
#include "unity.h"

#include "nt_builder.h"

static const char *g_dir;
static const char *g_worker_crash;
static const char *g_worker_malformed;
static const char *g_worker_nonzero;

void setUp(void) {}
void tearDown(void) {}

static void fill_gradient(uint8_t *rgba, uint32_t w, uint32_t h, uint8_t seed) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *px = &rgba[((size_t)y * w + x) * 4U];
            px[0] = (uint8_t)(seed + x * 3U);
            px[1] = (uint8_t)(seed + y * 5U);
            px[2] = (uint8_t)(x + y);
            px[3] = (x == 0U && y == 0U) ? 0U : 255U;
        }
    }
}

static uint8_t g_a[48 * 32 * 4];
static uint8_t g_b[16 * 40 * 4];
static uint8_t g_c[24 * 24 * 4];

static void make_settings(tp_pack_settings *s) {
    static tp_pack_sprite_desc sprites[3];
    fill_gradient(g_a, 48, 32, 20);
    fill_gradient(g_b, 16, 40, 90);
    fill_gradient(g_c, 24, 24, 150);
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "alpha", .rgba = g_a, .w = 48, .h = 32, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[1] = (tp_pack_sprite_desc){.name = "beta", .rgba = g_b, .w = 16, .h = 40, .origin_x = 0.25F, .origin_y = 0.75F};
    sprites[2] = (tp_pack_sprite_desc){.name = "gamma", .rgba = g_c, .w = 24, .h = 24, .origin_x = 0.5F, .origin_y = 0.5F};
    tp_pack_settings_defaults(s);
    s->atlas_name = "wrk";
    s->work_dir = g_dir;
    s->sprites = sprites;
    s->sprite_count = 3;
    s->pixels_per_unit = 8.0F;
}

/* Independent reference: the export-friendly nt_builder sequence (literal mirror
 * of the driver, same as the H0.3-a oracle). */
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

/* THE process oracle: a pack routed through the worker CHILD PROCESS is byte-for-
 * byte identical to a direct nt_builder invocation. */
void test_worker_process_matches_direct_nt_builder(void) {
    char worker_path[1024];
    char direct_path[1024];
    TEST_ASSERT_TRUE(snprintf(worker_path, sizeof worker_path, "%s/wrk_worker.ntpack", g_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(direct_path, sizeof direct_path, "%s/wrk_direct.ntpack", g_dir) > 0);

    tp_pack_settings s;
    make_settings(&s);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_build_worker_run(&s, NULL, worker_path, &err), err.msg);
    direct_nt_build(&s, direct_path);

    size_t ln = 0, rn = 0;
    uint8_t *lb = read_file(worker_path, &ln);
    uint8_t *rb = read_file(direct_path, &rn);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(rn, ln, "worker oracle: pack sizes differ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(lb, rb, ln), "worker oracle: pack bytes differ");
    free(lb);
    free(rb);
}

/* A crashing worker -> BUILDER_CRASHED, the host survives, and a prior artifact
 * at the same path is untouched (no publish). */
void test_crashing_worker_reports_crashed_and_preserves_prior_artifact(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_crash.ntpack", g_dir) > 0);

    /* Establish a good prior artifact and snapshot its bytes. */
    tp_pack_settings s;
    make_settings(&s);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_build_worker_run(&s, NULL, path, &err), err.msg);
    size_t before_n = 0;
    uint8_t *before = read_file(path, &before_n);

    /* Now a crashing worker aimed at the SAME path. */
    make_settings(&s);
    err = (tp_error){{0}};
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_crash, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_CRASHED, st, err.msg);

    /* No publish: the prior artifact is byte-for-byte unchanged. */
    size_t after_n = 0;
    uint8_t *after = read_file(path, &after_n);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(before_n, after_n, "crash overwrote the prior artifact size");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(before, after, before_n), "crash overwrote the prior artifact bytes");
    free(before);
    free(after);
}

/* A malformed reply after a clean exit fails closed as BUILDER_FAILED. */
void test_malformed_reply_is_builder_failed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_malformed.ntpack", g_dir) > 0);
    tp_pack_settings s;
    make_settings(&s);
    tp_error err = {{0}};
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_malformed, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_FAILED, st, err.msg);
}

/* A non-zero exit paired with a valid error reply -> BUILDER_FAILED with the
 * worker's structured message carried through. */
void test_nonzero_exit_with_error_reply_is_builder_failed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_nonzero.ntpack", g_dir) > 0);
    tp_pack_settings s;
    make_settings(&s);
    tp_error err = {{0}};
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_nonzero, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_FAILED, st, err.msg);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "worker fault seam"));
}

int main(int argc, char **argv) {
    /* Same first-thing dispatch every pack-capable exe must have: as the worker
     * child, service the request and return -- never fall through to the tests. */
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    g_dir = (argc > 1) ? argv[1] : ".";
    g_worker_crash = (argc > 2) ? argv[2] : "";
    g_worker_malformed = (argc > 3) ? argv[3] : "";
    g_worker_nonzero = (argc > 4) ? argv[4] : "";

    UNITY_BEGIN();
    RUN_TEST(test_worker_process_matches_direct_nt_builder);
    RUN_TEST(test_crashing_worker_reports_crashed_and_preserves_prior_artifact);
    RUN_TEST(test_malformed_reply_is_builder_failed);
    RUN_TEST(test_nonzero_exit_with_error_reply_is_builder_failed);
    return UNITY_END();
}
