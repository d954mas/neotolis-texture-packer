/* Private build-worker outcome + oracle + containment suite (decision 0018,
 * ROADMAP H0.3-b / H0.4).
 *
 * Proves the process boundary end to end:
 *  - a normal pack routed THROUGH the worker process is byte-identical to a
 *    direct nt_builder invocation (the worker child runs the same driver);
 *  - a Unicode + long (>260) destination packs through an ASCII staging dir and
 *    is published byte-identical to the direct driver;
 *  - a crashing worker returns BUILDER_CRASHED, the host survives, a prior
 *    artifact is not overwritten (no publish), and staging is gone;
 *  - cancellation mid-pack kills the worker, publishes nothing, removes staging,
 *    and surfaces no builder error;
 *  - the safety timeout kills a wedged worker as BUILDER_CRASHED and removes
 *    staging;
 *  - a malformed reply after a clean exit fails closed as BUILDER_FAILED;
 *  - a non-zero exit paired with a VALID error reply maps to BUILDER_FAILED;
 *  - a sink/full-disk write failure (partial staging file + a BUILDER_FAILED reply)
 *    publishes nothing and cleans the partial file (H0.5);
 *  - exit 0 + a valid OK reply but NO staged artifact maps to BUILDER_CRASHED (H0.5);
 *  - a reply frame declaring a giant payload length, and one truncated on the wire,
 *    each fail closed as BUILDER_FAILED THROUGH the process with no publish (H0.5).
 *
 * The test executable is BOTH the parent and the normal worker: main() dispatches
 * the hidden __build-worker argv the same way every shipping/pack exe must. The
 * fault outcomes use dedicated fault-worker binaries passed on argv. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define tp_test_getpid _getpid
#else
#include <time.h>
#include <unistd.h>
#define tp_test_getpid getpid
#endif

#include "tp_core/tp_arena.h"
#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_scan.h"
#include "tp_build_worker_internal.h"
#include "tp_fs_internal.h"
#include "unity.h"

#include "nt_builder.h"

#ifndef TP_TEST_WORKER_TIMEOUT_MS
#define TP_TEST_WORKER_TIMEOUT_MS 400
#endif

#define LONG_PATH_CAP 4096

static const char *g_dir;
static const char *g_worker_crash;
static const char *g_worker_malformed;
static const char *g_worker_nonzero;
static const char *g_worker_hang;
static const char *g_worker_nowrite;
static const char *g_worker_oknoart;
static const char *g_worker_biglen;
static const char *g_worker_trunc;
static const char *g_worker_flood;

void setUp(void) {}
void tearDown(void) {}

static double wall_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f;
    LARGE_INTEGER c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
#endif
}

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

static void make_settings(tp_pack_settings *s, const char *work_dir) {
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
    s->work_dir = work_dir;
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

/* Plain-fopen read for short ASCII paths. */
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

/* UTF-8 / long-path aware read via the shared filesystem backend (a plain fopen
 * cannot open a >260 Unicode path on Windows). */
static uint8_t *read_file_fs(const char *path, size_t *out_size) {
    tp_fs_info info;
    TEST_ASSERT_TRUE_MESSAGE(tp_fs_stat(path, &info), path);
    TEST_ASSERT_TRUE(info.size > 0U);
    FILE *f = tp_fs_fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    uint8_t *b = (uint8_t *)malloc((size_t)info.size);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(tp_fs_read_all(f, b, (size_t)info.size));
    (void)tp_fs_close(f);
    *out_size = (size_t)info.size;
    return b;
}

/* How many of THIS process's private staging dirs remain directly under `dir`. A
 * worker run must leave this UNCHANGED (delta 0). Scoped to our own pid prefix
 * (pkw-<pid>-...): an absolute "pkw-" count is fragile -- the persistent ctest work
 * dir can hold stale dirs from earlier broken runs, and the cross-run reaper now
 * sweeps dead-owner leftovers during a pack. Only our own dirs answer "did the pack
 * leak its staging". Returns -1 if the dir cannot be opened. */
static int count_staging_dirs(const char *dir) {
    char prefix[32];
    int pn = snprintf(prefix, sizeof prefix, "pkw-%08lx-",
                      (unsigned long)tp_test_getpid() & 0xffffffffUL);
    if (pn <= 0 || (size_t)pn >= sizeof prefix) {
        return -1;
    }
    tp_fs_dir *d = tp_fs_dir_open(dir);
    if (!d) {
        return -1;
    }
    int count = 0;
    tp_fs_dir_entry e;
    for (;;) {
        tp_fs_dir_result r = tp_fs_dir_next(d, &e);
        if (r != TP_FS_DIR_ENTRY) {
            break;
        }
        if (strncmp(e.name, prefix, (size_t)pn) == 0) {
            count++;
        }
    }
    tp_fs_dir_close(d);
    return count;
}

/* THE process oracle: a pack routed through the worker CHILD PROCESS is byte-for-
 * byte identical to a direct nt_builder invocation. Also prints the measured
 * worker round-trip (U-01 budget seam, measurement only). */
/* Global long-path work dir, prepared by test_direct_reference_builds. */
static char g_long_dir[LONG_PATH_CAP];

/* Runs FIRST, before any worker spawn: both byte-identity reference artifacts
 * are built in-process here. The engine's threaded builder can deadlock under
 * Windows clang-ASan when invoked after a child spawn/reap cycle (pre-existing
 * engine/env issue, not the containment path), so no direct_nt_build call may
 * follow a tp_build_worker_run/tp_build_worker_run_exe call in this suite. */
void test_direct_reference_builds(void) {
    char direct_path[1024];
    TEST_ASSERT_TRUE(snprintf(direct_path, sizeof direct_path, "%s/wrk_direct.ntpack", g_dir) > 0);
    tp_pack_settings s;
    make_settings(&s, g_dir);
    direct_nt_build(&s, direct_path);

    /* Unicode + length: build the >300 char destination directory used by
     * test_worker_publishes_unicode_long_path. */
    int used = snprintf(g_long_dir, sizeof g_long_dir, "%s", g_dir);
    TEST_ASSERT_TRUE(used > 0 && used < (int)sizeof g_long_dir);
    for (unsigned int seg = 0U; strlen(g_long_dir) <= 300U; seg++) {
        const size_t at = strlen(g_long_dir);
        const int n = snprintf(g_long_dir + at, sizeof g_long_dir - at,
                               "/\xD0\xBF\xD0\xB0\xD0\xBA-t\xC3\xAF\x6C\xC3\xA9-seg%02u", seg);
        TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof g_long_dir - at);
    }
    tp_mkdirs(g_long_dir);
    TEST_ASSERT_TRUE_MESSAGE(tp_scan_is_dir(g_long_dir), g_long_dir);

    char long_direct_path[1024];
    TEST_ASSERT_TRUE(snprintf(long_direct_path, sizeof long_direct_path, "%s/lp_direct.ntpack", g_dir) > 0);
    tp_pack_settings ls;
    make_settings(&ls, g_long_dir);
    direct_nt_build(&ls, long_direct_path);
}

void test_worker_process_matches_direct_nt_builder(void) {
    char worker_path[1024];
    char direct_path[1024];
    TEST_ASSERT_TRUE(snprintf(worker_path, sizeof worker_path, "%s/wrk_worker.ntpack", g_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(direct_path, sizeof direct_path, "%s/wrk_direct.ntpack", g_dir) > 0);

    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    /* The reference artifact was built by test_direct_reference_builds (which
     * runs first): the in-process nt_builder can deadlock under Windows
     * clang-ASan when it runs after a child spawn/reap cycle (pre-existing
     * engine/env issue), so every direct build precedes the first spawn. */
    const double t0 = wall_ms();
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_build_worker_run(&s, NULL, worker_path, &err), err.msg);
    const double rt = wall_ms() - t0;
    printf("[perf] worker round-trip = %.2f ms (U-01 budget seam)\n", rt);
    fflush(stdout);

    size_t ln = 0, rn = 0;
    uint8_t *lb = read_file(worker_path, &ln);
    uint8_t *rb = read_file(direct_path, &rn);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(rn, ln, "worker oracle: pack sizes differ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(lb, rb, ln), "worker oracle: pack bytes differ");
    free(lb);
    free(rb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir), "worker left staging behind");
}

/* Unicode + long (>260) destination: the child packs into an ASCII staging dir,
 * the parent publishes to the Unicode long path, byte-identical to direct. */
void test_worker_publishes_unicode_long_path(void) {
    /* g_long_dir and the lp_direct.ntpack reference were prepared by
     * test_direct_reference_builds (before any spawn -- see its comment). */
    TEST_ASSERT_TRUE_MESSAGE(tp_scan_is_dir(g_long_dir), g_long_dir);

    char worker_path[LONG_PATH_CAP];
    char direct_path[1024];
    TEST_ASSERT_TRUE(snprintf(worker_path, sizeof worker_path, "%s/lp.ntpack", g_long_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(direct_path, sizeof direct_path, "%s/lp_direct.ntpack", g_dir) > 0);
    TEST_ASSERT_TRUE(strlen(worker_path) > 260U);

    tp_pack_settings s;
    make_settings(&s, g_long_dir);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_build_worker_run(&s, NULL, worker_path, &err), err.msg);

    size_t ln = 0, rn = 0;
    uint8_t *lb = read_file_fs(worker_path, &ln);   /* long Unicode: tp_fs read */
    uint8_t *rb = read_file(direct_path, &rn);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(rn, ln, "unicode-long: pack sizes differ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(lb, rb, ln), "unicode-long: pack bytes differ");
    free(lb);
    free(rb);
}

/* A crashing worker -> BUILDER_CRASHED, the host survives, a prior artifact at the
 * same path is untouched (no publish), and staging is gone.
 *
 * This pins the on-disk half of decision 0018's "the last successful preview
 * remains authoritative": a crash publishes nothing, so the prior artifact bytes
 * survive. The in-memory half is a structural property of the take_result
 * contract, not something this worker-layer test can observe: tp_pack returns
 * *out_result == NULL on any non-OK worker status (tp_pack.c:577-580), and
 * tp_session_job_take_result transfers a pack arena/result ONLY when the job
 * state is SUCCEEDED (tp_job.c:473-478) -- a FAILED/CRASHED job hands back no
 * result, so a consumer's last successful preview cannot be replaced. The
 * positive direction (SUCCEEDED -> non-NULL result) is pinned by
 * apps/gui/test_client_parity.c:703-713. */
void test_crashing_worker_reports_crashed_and_preserves_prior_artifact(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_crash.ntpack", g_dir) > 0);

    /* Establish a good prior artifact and snapshot its bytes. */
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_build_worker_run(&s, NULL, path, &err), err.msg);
    size_t before_n = 0;
    uint8_t *before = read_file(path, &before_n);
    const int staging_before = count_staging_dirs(g_dir);

    /* Now a crashing worker aimed at the SAME path. */
    make_settings(&s, g_dir);
    err = (tp_error){{0}};
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_crash, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_CRASHED, st, err.msg);

    /* No publish: the prior artifact is byte-for-byte unchanged. */
    size_t after_n = 0;
    uint8_t *after = read_file(path, &after_n);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(before_n, after_n, "crash overwrote the prior artifact size");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(before, after, before_n), "crash overwrote the prior artifact bytes");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir), "crash left staging behind");
    free(before);
    free(after);
}

/* Cancel counter: the worker wait loop polls this; report cancelled after a few
 * polls so the hung child is killed mid-pack. */
static bool cancel_after_a_few_polls(void *ctx) {
    int *polls = (int *)ctx;
    (*polls)++;
    return *polls > 3;
}

/* Cancellation mid-pack: hang worker + a cancel poll -> the child is killed,
 * nothing is published, staging is gone, and NO builder error is surfaced. */
void test_cancel_kills_worker_and_cleans_staging(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_cancel.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));

    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    int polls = 0;
    bool cancelled = false;
    tp_build_worker_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.worker_exe = g_worker_hang;
    opts.cancel_poll = cancel_after_a_few_polls;
    opts.cancel_ctx = &polls;
    opts.out_cancelled = &cancelled;

    tp_status st = tp_build_worker_run_opts(&s, NULL, path, &opts, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, "cancel must not surface a builder error");
    TEST_ASSERT_TRUE_MESSAGE(cancelled, "cancel was not observed");
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "cancel published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir), "cancel left staging behind");
}

/* Safety timeout: hang worker + a tiny compile-def timeout -> BUILDER_CRASHED with
 * a distinct "timed out" diagnostic (same status enum), staging gone, no publish. */
void test_timeout_kills_worker_and_cleans_staging(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_timeout.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));

    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_build_worker_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.worker_exe = g_worker_hang;
    opts.timeout_ms = TP_TEST_WORKER_TIMEOUT_MS;

    tp_status st = tp_build_worker_run_opts(&s, NULL, path, &opts, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_CRASHED, st, err.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err.msg, "timed out"), err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "timeout published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir), "timeout left staging behind");
}

/* A malformed reply after a clean exit fails closed as BUILDER_FAILED. */
void test_malformed_reply_is_builder_failed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_malformed.ntpack", g_dir) > 0);
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_malformed, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_FAILED, st, err.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir), "malformed left staging behind");
}

/* A non-zero exit paired with a valid error reply -> BUILDER_FAILED with the
 * worker's structured message carried through. */
void test_nonzero_exit_with_error_reply_is_builder_failed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_nonzero.ntpack", g_dir) > 0);
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_nonzero, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_FAILED, st, err.msg);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "worker fault seam"));
}

/* Full-disk / sink write failure via the seam: the child leaves a PARTIAL artifact
 * in staging then reports a structured builder/sink failure -> BUILDER_FAILED,
 * NOTHING is published at the destination, and the partial staging file is cleaned
 * (staging delta 0). The contract under test is the PARENT's behavior. */
void test_sink_write_failure_publishes_nothing_and_cleans_staging(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_nowrite.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_nowrite, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_FAILED, st, err.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err.msg, "sink write failed"), err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "sink failure published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "sink failure left a partial staging dir behind");
}

/* Exit 0 + a valid OK reply but NO staged artifact -> BUILDER_CRASHED ("reported
 * success but produced no readable artifact"): a lying success cannot publish. */
void test_ok_reply_missing_artifact_is_builder_crashed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_oknoart.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_oknoart, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_CRASHED, st, err.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err.msg, "no readable artifact"), err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "missing-artifact OK published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "missing-artifact OK left staging behind");
}

/* A reply frame declaring a GIANT payload length (header says far more than the
 * bytes on the wire) fails closed THROUGH the process -> BUILDER_FAILED, no
 * publish. Codec-level declared-length rejection is #43; this pins the parent. */
void test_oversized_declared_length_reply_is_builder_failed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_biglen.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_biglen, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_FAILED, st, err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "oversized-length reply published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "oversized-length reply left staging behind");
}

/* A reply frame truncated on the wire (fewer payload bytes than its header
 * declares) fails closed THROUGH the process -> BUILDER_FAILED, no publish. */
void test_truncated_reply_frame_is_builder_failed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_trunc.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, g_worker_trunc, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_FAILED, st, err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "truncated reply published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "truncated reply left staging behind");
}

/* A missing/renamed worker exe: on Windows CreateProcessW fails (spawn NULL); on
 * POSIX fork succeeds but execv fails -> _exit(127). Both are parent-owned
 * BUILDER_CRASHED branches -- fail closed, publish nothing, staging delta 0. */
void test_missing_worker_exe_is_builder_crashed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_nospawn.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run_exe(&s, NULL, path, "ntpacker-no-such-worker", &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_CRASHED, st, err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "spawn/exec failure published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "spawn/exec failure left staging behind");
}

/* A valid artifact the host cannot publish (destination parent dir absent) -> the
 * publish rename fails AFTER a good build. Kept as BUILDER_CRASHED (no trustworthy
 * PUBLISHED artifact exists; host survives, nothing written over the destination). */
void test_publish_failure_maps_to_builder_crashed(void) {
    char missing_dir[1024];
    char path[1200];
    TEST_ASSERT_TRUE(snprintf(missing_dir, sizeof missing_dir, "%s/no_such_pub_dir", g_dir) > 0);
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/out.ntpack", missing_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(missing_dir));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run(&s, NULL, path, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_CRASHED, st, err.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err.msg, "could not be published"), err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "publish failure created the artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "publish failure left staging behind");
}

/* An over-cap reply THROUGH the process: the flood worker writes more than the
 * lowered reply_cap (but under the pipe buffer) and exits, so the parent's read
 * fills the cap without EOF -> the "oversized" fail-closed branch -> BUILDER_FAILED,
 * no publish. Exercises the reply-cap overflow defense the comment describes. */
void test_oversized_reply_is_builder_failed(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_flood.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_build_worker_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.worker_exe = g_worker_flood;
    opts.reply_cap = 1024; /* below the flood (8 KiB) and the 64 KiB pipe buffer */
    tp_status st = tp_build_worker_run_opts(&s, NULL, path, &opts, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_BUILDER_FAILED, st, err.msg);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err.msg, "oversized"), err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "oversized reply published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "oversized reply left staging behind");
}

/* A staging dir owned by a dead pid (pkw-<hexpid>-<serial>) is a cross-run leftover
 * from a host killed mid-pack; the next pack's best-effort reaper must sweep it
 * (contents and all), while our own live staging is untouched. */
void test_stale_staging_dir_is_reaped(void) {
    char fake[1024];
    char fakefile[1200];
    TEST_ASSERT_TRUE(snprintf(fake, sizeof fake, "%s/pkw-999999-0", g_dir) > 0);
    TEST_ASSERT_TRUE(tp_fs_create_dir(fake));
    TEST_ASSERT_TRUE(snprintf(fakefile, sizeof fakefile, "%s/out.ntpack", fake) > 0);
    static const uint8_t junk[4] = {1U, 2U, 3U, 4U};
    TEST_ASSERT_TRUE(tp_fs_write_file(fakefile, junk, sizeof junk));
    TEST_ASSERT_TRUE(tp_fs_exists(fake));

    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_reap.ntpack", g_dir) > 0);
    tp_pack_settings s;
    make_settings(&s, g_dir);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_build_worker_run(&s, NULL, path, &err), err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(fake), "dead-owner staging dir was not reaped");
}

/* A well-encoded but out-of-range request (extrude > 0 with the default non-RECT
 * shape) bypasses tp_pack's validate_settings and trips an always-on NT_BUILD_ASSERT
 * in the builder. The abort is CONTAINED to the child (mapped BUILDER_CRASHED here);
 * the host survives, nothing is published, staging is clean. Defense-in-depth: the
 * parent validates first in production, so this proves the worker's own robustness. */
void test_out_of_range_knob_is_contained(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_badknob.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    s.extrude = 5; /* extrude > 0 requires shape RECT; default shape is CONCAVE */
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run(&s, NULL, path, &err);
    TEST_ASSERT_TRUE_MESSAGE(st == TP_STATUS_BUILDER_FAILED || st == TP_STATUS_BUILDER_CRASHED, err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "out-of-range knob published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "out-of-range knob left staging behind");
}

/* A zero-sprite request through the real worker: the builder's end_atlas asserts
 * "atlas has no sprites" and aborts the child -> contained, mapped to a structured
 * status here (never a host crash/hang). Nothing published, staging clean. */
void test_zero_sprite_request_is_contained(void) {
    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/wrk_nosprite.ntpack", g_dir) > 0);
    TEST_ASSERT_FALSE(tp_fs_exists(path));
    tp_pack_settings s;
    make_settings(&s, g_dir);
    s.sprite_count = 0; /* codec allows it; the builder rejects an empty atlas */
    tp_error err = {{0}};
    const int staging_before = count_staging_dirs(g_dir);
    tp_status st = tp_build_worker_run(&s, NULL, path, &err);
    TEST_ASSERT_TRUE_MESSAGE(st == TP_STATUS_BUILDER_FAILED || st == TP_STATUS_BUILDER_CRASHED, err.msg);
    TEST_ASSERT_FALSE_MESSAGE(tp_fs_exists(path), "zero-sprite request published an artifact");
    TEST_ASSERT_EQUAL_INT_MESSAGE(staging_before, count_staging_dirs(g_dir),
                                  "zero-sprite request left staging behind");
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
    g_worker_hang = (argc > 5) ? argv[5] : "";
    g_worker_nowrite = (argc > 6) ? argv[6] : "";
    g_worker_oknoart = (argc > 7) ? argv[7] : "";
    g_worker_biglen = (argc > 8) ? argv[8] : "";
    g_worker_trunc = (argc > 9) ? argv[9] : "";
    g_worker_flood = (argc > 10) ? argv[10] : "";

    UNITY_BEGIN();
    RUN_TEST(test_direct_reference_builds);
    RUN_TEST(test_worker_process_matches_direct_nt_builder);
    RUN_TEST(test_worker_publishes_unicode_long_path);
    RUN_TEST(test_crashing_worker_reports_crashed_and_preserves_prior_artifact);
    RUN_TEST(test_cancel_kills_worker_and_cleans_staging);
    RUN_TEST(test_timeout_kills_worker_and_cleans_staging);
    RUN_TEST(test_malformed_reply_is_builder_failed);
    RUN_TEST(test_nonzero_exit_with_error_reply_is_builder_failed);
    RUN_TEST(test_sink_write_failure_publishes_nothing_and_cleans_staging);
    RUN_TEST(test_ok_reply_missing_artifact_is_builder_crashed);
    RUN_TEST(test_oversized_declared_length_reply_is_builder_failed);
    RUN_TEST(test_truncated_reply_frame_is_builder_failed);
    RUN_TEST(test_missing_worker_exe_is_builder_crashed);
    RUN_TEST(test_publish_failure_maps_to_builder_crashed);
    RUN_TEST(test_oversized_reply_is_builder_failed);
    RUN_TEST(test_stale_staging_dir_is_reaped);
    RUN_TEST(test_out_of_range_knob_is_contained);
    RUN_TEST(test_zero_sprite_request_is_contained);
    return UNITY_END();
}
