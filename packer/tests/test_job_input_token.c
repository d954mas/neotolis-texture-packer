#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* A minimal, valid 4x4 fully-opaque RGBA PNG (stb_image decodes it). A real pack
 * job needs a decodable source on disk, so the end-to-end token test below lays
 * this down and adds it as a file source before packing. */
static const uint8_t k_png_4x4[] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
    0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0xA9U, 0xF1U, 0x9EU, 0x7EU, 0x00U, 0x00U, 0x00U,
    0x33U, 0x49U, 0x44U, 0x41U, 0x54U, 0x78U, 0xDAU, 0x15U, 0xC8U, 0xA1U, 0x11U, 0x00U,
    0x30U, 0x08U, 0x04U, 0x41U, 0x34U, 0x3AU, 0x95U, 0x44U, 0x53U, 0x09U, 0x9AU, 0x42U,
    0x98U, 0xD7U, 0xF4U, 0x4CU, 0x2EU, 0x62U, 0xCDU, 0x9AU, 0x9FU, 0xDEU, 0x8BU, 0x84U,
    0x60U, 0x1EU, 0x04U, 0x12U, 0x8AU, 0x1FU, 0x45U, 0x20U, 0xA1U, 0xFAU, 0x31U, 0x04U,
    0x12U, 0x9AU, 0xDEU, 0x07U, 0x18U, 0xA8U, 0x21U, 0x51U, 0x4CU, 0xC2U, 0x35U, 0x74U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4EU, 0x44U, 0xAEU, 0x42U, 0x60U, 0x82U};

/* Absolute, '/'-normalized scratch dir under the test's working directory. An
 * untitled session resolves only ABSOLUTE source paths, so the fixture path must
 * be absolute. */
static void job_scratch_dir(char *out, size_t cap) {
    char cwd[1024];
#ifdef _WIN32
    TEST_ASSERT_NOT_NULL(_getcwd(cwd, (int)sizeof cwd));
#else
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
#endif
    for (char *p = cwd; *p; ++p) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    const int n = snprintf(out, cap, "%s/tp_job_input_token_scratch", cwd);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < cap);
}

static void write_png_fixture(const char *path) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    TEST_ASSERT_EQUAL_size_t(sizeof k_png_4x4,
                             fwrite(k_png_4x4, 1U, sizeof k_png_4x4, f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

static int deterministic_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *seed = (uint8_t *)ctx;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(*seed + (uint8_t)i);
    }
    *seed = (uint8_t)(*seed + 17U);
    return (int)len;
}

static tp_session *make_session(void) {
    uint8_t seed = 1U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create(&rng, &session, &err));
    TEST_ASSERT_NOT_NULL(session);
    return session;
}

static tp_session_input_token snapshot_input_token(tp_session *session) {
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_session_input_token token =
        tp_session_snapshot_input_token(snapshot);
    tp_session_snapshot_destroy(snapshot);
    return token;
}

static void rename_default_atlas(tp_session *session) {
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas->id;
    operation.u.atlas_rename.name = (char *)malloc(sizeof "renamed");
    TEST_ASSERT_NOT_NULL(operation.u.atlas_rename.name);
    memcpy(operation.u.atlas_rename.name, "renamed", sizeof "renamed");

    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "abababababababababababababababab",
           sizeof request.id_hex);
    request.expected_revision = tp_session_snapshot_revision(snapshot);
    request.ops = &operation;
    request.op_count = 1U;

    tp_txn_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    TEST_ASSERT_TRUE(result.committed);

    tp_txn_result_free(&result);
    tp_operation_free(&operation);
    tp_session_snapshot_destroy(snapshot);
}

void test_zero_generations_are_a_valid_fresh_job_token(void) {
    tp_session *session = make_session();
    const tp_session_input_token at_start = snapshot_input_token(session);
    TEST_ASSERT_EQUAL_UINT64(0U, at_start.model_generation);
    TEST_ASSERT_EQUAL_UINT64(0U, at_start.source_generation);

    tp_session_pack_job_result job_result;
    memset(&job_result, 0, sizeof job_result);
    job_result.input_token_at_start = at_start;

    const tp_session_input_token current = snapshot_input_token(session);
    TEST_ASSERT_TRUE(tp_session_input_token_equal(
        job_result.input_token_at_start, current));
    tp_session_destroy(session);
}

void test_model_mutation_invalidates_the_captured_job_token(void) {
    tp_session *session = make_session();
    const tp_session_input_token at_start = snapshot_input_token(session);

    rename_default_atlas(session);
    const tp_session_input_token current = snapshot_input_token(session);

    TEST_ASSERT_EQUAL_UINT64(at_start.model_generation + 1U,
                             current.model_generation);
    TEST_ASSERT_EQUAL_UINT64(at_start.source_generation,
                             current.source_generation);
    TEST_ASSERT_FALSE(tp_session_input_token_equal(at_start, current));
    tp_session_destroy(session);
}

static tp_id128 default_atlas_id(tp_session *session) {
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 id = atlas->id;
    tp_session_snapshot_destroy(snapshot);
    return id;
}

/* Lays down the PNG fixture in `work_dir` and adds it as a FILE source on the
 * default atlas so the atlas has a decodable image a pack job can consume. */
static void add_file_source(tp_session *session, tp_id128 atlas_id,
                            const char *work_dir) {
    char png_path[1200];
    const int n = snprintf(png_path, sizeof png_path, "%s/job_src.png", work_dir);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof png_path);
    write_png_fixture(png_path);

    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SOURCE_ADD;
    operation.atlas_id = atlas_id;
    operation.u.source_add.kind = TP_SOURCE_KIND_FILE;
    operation.u.source_add.source_id = (tp_id128){{0x11U, 0x22U, 0x33U, 0x44U,
                                                   0x55U, 0x66U, 0x77U, 0x88U,
                                                   0x99U, 0xAAU, 0xBBU, 0xCCU,
                                                   0xDDU, 0xEEU, 0xF0U, 0x0FU}};
    const size_t key_size = strlen(png_path) + 1U;
    operation.u.source_add.key = malloc(key_size);
    TEST_ASSERT_NOT_NULL(operation.u.source_add.key);
    memcpy(operation.u.source_add.key, png_path, key_size);

    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));

    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
           sizeof request.id_hex);
    request.expected_revision = tp_session_snapshot_revision(snapshot);
    request.ops = &operation;
    request.op_count = 1U;

    tp_txn_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    TEST_ASSERT_TRUE(result.committed);

    tp_txn_result_free(&result);
    tp_operation_free(&operation);
    tp_session_snapshot_destroy(snapshot);
}

/* F3-03 T2: the job/pack surface carries a pack_input_hash, recomputed from the
 * session's immutable snapshot, stable for the same snapshot -- and a REAL native
 * async pack carries that same hash on its completed result. This drives a job to
 * completion (worker-thread hash compute, tp_job.c) and asserts the taken result's
 * pack_input_hash equals tp_session_pack_input_hash on the unmutated session. */
void test_pack_input_hash_present_and_stable_for_same_snapshot(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    add_file_source(session, atlas, work_dir);

    tp_error err = {{0}};
    tp_id128 first = tp_id128_nil();
    tp_id128 second = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_pack_input_hash(
                                            session, atlas, NULL, &first, &err));
    TEST_ASSERT_FALSE(tp_id128_is_nil(first));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_pack_input_hash(
                                            session, atlas, NULL, &second, &err));
    TEST_ASSERT_TRUE(tp_id128_eq(first, second));

    /* Drive a real native pack job (no preview exporter) to completion. */
    const tp_pack_job_request request = {
        .atlas_id = atlas,
        .work_dir = work_dir,
        .preview_exporter_id = NULL,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_pack_job_start(session, &request, &err));
    tp_session_job_progress progress;
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_SESSION_JOB_SUCCEEDED, progress.state,
                                  "native pack job did not succeed");

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_PACK, result.kind);

    /* (a) the worker computed and carried a real (non-nil) hash out. */
    TEST_ASSERT_FALSE(tp_id128_is_nil(result.pack.pack_input_hash));
    /* (b) it matches the hash recomputed on the same UNMUTATED session (native,
     * no preview clamp) -- tp_session_pack_input_hash matches a native pack job. */
    tp_id128 recomputed = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_pack_input_hash(
                                            session, atlas, NULL, &recomputed,
                                            &err));
    TEST_ASSERT_FALSE(tp_id128_is_nil(recomputed));
    TEST_ASSERT_TRUE_MESSAGE(
        tp_id128_eq(result.pack.pack_input_hash, recomputed),
        "completed job hash must equal the recomputed input hash");
    /* No mutation occurred, so it is also the earlier stable snapshot hash. */
    TEST_ASSERT_TRUE(tp_id128_eq(result.pack.pack_input_hash, first));

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
}

void test_pack_input_hash_changes_on_semantic_mutation(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    tp_error err = {{0}};
    tp_id128 before = tp_id128_nil();
    tp_id128 after = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_pack_input_hash(
                                            session, atlas, NULL, &before, &err));
    rename_default_atlas(session);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_pack_input_hash(
                                            session, atlas, NULL, &after, &err));
    TEST_ASSERT_FALSE(tp_id128_eq(before, after));
    tp_session_destroy(session);
}

void test_source_runtime_change_invalidates_without_model_mutation(void) {
    tp_session *session = make_session();
    const tp_session_input_token at_start = snapshot_input_token(session);
    tp_error err = {{0}};

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_invalidate_sources(session, &err));
    const tp_session_input_token current = snapshot_input_token(session);

    TEST_ASSERT_EQUAL_UINT64(at_start.model_generation,
                             current.model_generation);
    TEST_ASSERT_EQUAL_UINT64(at_start.source_generation + 1U,
                             current.source_generation);
    TEST_ASSERT_FALSE(tp_session_input_token_equal(at_start, current));
    tp_session_destroy(session);
}

int main(int argc, char **argv) {
    /* The real pack job spawns THIS executable as its build-worker child; service
     * that dispatch first, exactly like every pack-capable exe (and sibling test). */
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_zero_generations_are_a_valid_fresh_job_token);
    RUN_TEST(test_model_mutation_invalidates_the_captured_job_token);
    RUN_TEST(test_source_runtime_change_invalidates_without_model_mutation);
    RUN_TEST(test_pack_input_hash_present_and_stable_for_same_snapshot);
    RUN_TEST(test_pack_input_hash_changes_on_semantic_mutation);
    return UNITY_END();
}
