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

#include "tinycthread.h"

#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_fs_internal.h" /* scratch teardown */
#include "tp_image_priv.h"  /* single-decode accounting seam */
#include "unity.h"

/* Cross-thread walk-gate seam (tp_scan.c): arms the next folder walk to PARK until
 * released, so a job test can prove the walk runs on the pack worker (start() returns
 * while the worker is parked here) and drive a mid-walk cancel deterministically. */
void tp_scan__test_arm_walk_gate(void);
bool tp_scan__test_walk_gate_entered(void);
void tp_scan__test_release_walk_gate(void);

/* Post-entry gate + visited-entry counter seam (tp_scan.c): trips after
 * the FIRST entry is visited INSIDE the walk and counts every visited entry, so the
 * mid-scan-cancel test can prove the scan STOPPED EARLY (visited < N) rather than only
 * that the job ended CANCELLED -- the shared cancel flag yields CANCELLED even if the
 * scan ran to completion and only the pack observed the cancel. */
void tp_scan__test_arm_post_entry_gate(void);
bool tp_scan__test_post_entry_gate_entered(void);
void tp_scan__test_release_post_entry_gate(void);
int tp_scan__test_visited_entries(void);

/* Parks a completed worker before terminal publication, making late cancellation
 * deterministic for Pack results and committed Export outputs. */
void tp_job__test_arm_before_terminal_gate(void);
bool tp_job__test_before_terminal_gate_entered(void);
void tp_job__test_release_before_terminal_gate(void);
void tp_job__test_arm_after_cancel_observation_gate(void);
bool tp_job__test_after_cancel_observation_gate_entered(void);
void tp_job__test_release_after_cancel_observation_gate(void);
void tp_job__test_arm_after_cancel_claim_gate(void);
bool tp_job__test_after_cancel_claim_gate_entered(void);
void tp_job__test_release_after_cancel_claim_gate(void);
void tp_export_run__test_arm_before_write_gate(void);
bool tp_export_run__test_before_write_gate_entered(void);
void tp_export_run__test_release_before_write_gate(void);

void setUp(void) {}
void tearDown(void) {}

/* Spins (bounded), yielding the CPU each iteration so the parked worker thread
 * always gets scheduled, until the worker parks in the armed walk. Returns true
 * once ENTERED; the large cap is a genuine timeout for a broken gate, not a
 * normal path -- the worker reaches the gate within microseconds. */
static bool wait_for_walk_gate(void) {
    for (long spins = 0; spins < 10000000L; ++spins) {
        if (tp_scan__test_walk_gate_entered()) {
            return true;
        }
        thrd_yield();
    }
    return tp_scan__test_walk_gate_entered();
}

/* Same bounded spin, for the post-entry gate: parks in scan_dir after the first
 * visited entry, yielding the CPU each iteration so the parked worker always
 * gets scheduled. The large cap is a genuine timeout for a broken gate, not a
 * normal path. */
static bool wait_for_post_entry_gate(void) {
    for (long spins = 0; spins < 10000000L; ++spins) {
        if (tp_scan__test_post_entry_gate_entered()) {
            return true;
        }
        thrd_yield();
    }
    return tp_scan__test_post_entry_gate_entered();
}

static bool wait_for_before_terminal_gate(void) {
    for (long spins = 0; spins < 10000000L; ++spins) {
        if (tp_job__test_before_terminal_gate_entered()) {
            return true;
        }
        thrd_yield();
    }
    return tp_job__test_before_terminal_gate_entered();
}

static bool wait_for_after_cancel_observation_gate(void) {
    for (long spins = 0; spins < 10000000L; ++spins) {
        if (tp_job__test_after_cancel_observation_gate_entered()) {
            return true;
        }
        thrd_yield();
    }
    return tp_job__test_after_cancel_observation_gate_entered();
}

static bool wait_for_after_cancel_claim_gate(void) {
    for (long spins = 0; spins < 10000000L; ++spins) {
        if (tp_job__test_after_cancel_claim_gate_entered()) {
            return true;
        }
        thrd_yield();
    }
    return tp_job__test_after_cancel_claim_gate_entered();
}

static bool wait_for_before_export_write_gate(void) {
    for (long spins = 0; spins < 10000000L; ++spins) {
        if (tp_export_run__test_before_write_gate_entered()) {
            return true;
        }
        thrd_yield();
    }
    return tp_export_run__test_before_write_gate_entered();
}

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

/* Best-effort recursive teardown of the runtime scratch dir this suite lays down
 * under the cwd (dir + job_src.png + the pack .ntpack), so a run leaves no litter.
 * Mirrors the build worker's staging cleanup; skips "."/".." like tp_fs_dir_next. */
static void remove_scratch_tree(const char *path) {
    tp_fs_dir *dir = tp_fs_dir_open(path);
    if (dir) {
        tp_fs_dir_entry entry;
        while (tp_fs_dir_next(dir, &entry) == TP_FS_DIR_ENTRY) {
            char child[2048];
            if ((size_t)snprintf(child, sizeof child, "%s/%s", path,
                                 entry.name) >= sizeof child) {
                continue;
            }
            if (entry.info.kind == TP_FS_KIND_DIRECTORY && !entry.info.reparse) {
                remove_scratch_tree(child);
            } else {
                (void)tp_fs_remove_file(child);
            }
        }
        tp_fs_dir_close(dir);
    }
    (void)tp_fs_remove_dir(path);
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

static tp_session *make_default_project_session(void) {
    uint8_t seed = 1U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_create_default_project(&rng, &session, &err));
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

/* Adds `folder` (an existing directory) as a FOLDER source on `atlas_id`. The
 * caller writes the images inside it; the recursive enumeration happens later, on
 * the pack worker (U-01a). */
static void add_folder_source(tp_session *session, tp_id128 atlas_id,
                              const char *folder) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SOURCE_ADD;
    operation.atlas_id = atlas_id;
    operation.u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    operation.u.source_add.source_id = (tp_id128){{0x22U, 0x33U, 0x44U, 0x55U,
                                                   0x66U, 0x77U, 0x88U, 0x99U,
                                                   0xAAU, 0xBBU, 0xCCU, 0xDDU,
                                                   0xEEU, 0xF0U, 0x0FU, 0x11U}};
    const size_t key_size = strlen(folder) + 1U;
    operation.u.source_add.key = malloc(key_size);
    TEST_ASSERT_NOT_NULL(operation.u.source_add.key);
    memcpy(operation.u.source_add.key, folder, key_size);

    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));

    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "efefefefefefefefefefefefefefefef",
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

static tp_session_job_result wait_for_job_result(tp_session *session) {
    tp_error err = {{0}};
    tp_session_job_progress progress;
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    return result;
}

static tp_status partial_failure_write(
    const tp_export_prepared *prepared, const tp_export_caps *caps,
    const char *out_path_base, tp_export_notices *notices, tp_error *err) {
    (void)prepared;
    (void)caps;
    (void)out_path_base;
    const tp_status notice_status = tp_export_notice_addf(
        notices, "intentional partial-export notice");
    if (notice_status != TP_STATUS_OK) {
        return notice_status;
    }
    return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                        "intentional partial-export failure");
}

static void add_partial_failure_target(tp_session *session, tp_id128 atlas_id) {
    const tp_exporter *base = tp_exporter_find("json-neotolis");
    TEST_ASSERT_NOT_NULL(base);
    static tp_exporter failing;
    failing = *base;
    failing.id = "test-partial-failure";
    failing.display_name = "Test partial failure";
    failing.write = partial_failure_write;
    const tp_status registration = tp_exporter_register(&failing);
    TEST_ASSERT_TRUE(registration == TP_STATUS_OK ||
                     tp_exporter_find(failing.id) != NULL);

    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_TARGET_CREATE;
    operation.atlas_id = atlas_id;
    operation.u.target_create.target_id =
        (tp_id128){{0x43U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U, 0x99U,
                    0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xF0U, 0x0FU, 0x11U}};
    operation.u.target_create.exporter_id =
        malloc(sizeof "test-partial-failure");
    operation.u.target_create.out_path = malloc(sizeof "out/partial");
    TEST_ASSERT_NOT_NULL(operation.u.target_create.exporter_id);
    TEST_ASSERT_NOT_NULL(operation.u.target_create.out_path);
    memcpy(operation.u.target_create.exporter_id, "test-partial-failure",
           sizeof "test-partial-failure");
    memcpy(operation.u.target_create.out_path, "out/partial",
           sizeof "out/partial");
    operation.u.target_create.enabled = true;

    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "43434343434343434343434343434343",
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

typedef struct cancel_thread_ctx {
    tp_session *session;
    tp_status status;
    tp_error error;
} cancel_thread_ctx;

static int request_cancel_thread(void *context) {
    cancel_thread_ctx *ctx = context;
    ctx->status = tp_session_job_cancel(ctx->session, &ctx->error);
    return 0;
}

void test_source_less_export_succeeds_as_skipped(void) {
    tp_session *session = make_default_project_session();
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);

    tp_error err = {{0}};
    const tp_export_command_request request = {
        .work_dir = work_dir,
        .atlas_id = default_atlas_id(session),
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_session_export_start(session, &request, &err),
        "a source-less atlas must start and publish a skipped result");

    tp_session_job_result result = wait_for_job_result(session);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_EXPORT, result.kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.status);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.targets);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.atlases_ok);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.atlases_failed);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.atlases_skipped);

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

void test_empty_folder_export_succeeds_as_skipped(void) {
    tp_session *session = make_default_project_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    char folder[1200];
    TEST_ASSERT_TRUE(
        snprintf(folder, sizeof folder, "%s/empty", work_dir) > 0);
    tp_mkdirs(folder);
    add_folder_source(session, atlas, folder);

    tp_error err = {{0}};
    const tp_export_command_request request = {
        .work_dir = work_dir,
        .atlas_id = atlas,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_export_start(session, &request, &err));

    tp_session_job_result result = wait_for_job_result(session);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_EXPORT, result.kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.status);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.targets);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.atlases_ok);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.atlases_failed);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.atlases_skipped);

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

/* The job/pack surface carries a pack_input_hash, recomputed from the
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
    remove_scratch_tree(work_dir);
}

/* Single-decode guard: a native Pack job decodes each source EXACTLY once. The
 * pack_input_hash is folded from the pack's own decode pass, so the former
 * hash-compute pre-decode (which doubled every source's decode) is gone. */
void test_pack_job_decodes_each_source_once(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    add_file_source(session, atlas, work_dir); /* one FILE source */

    tp_error err = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas,
        .work_dir = work_dir,
        .preview_exporter_id = NULL,
    };
    tp_image__test_reset_decode_count();
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
    /* take_result joined the worker thread, so the decode count is settled. The
     * build worker runs in a CHILD process that never decodes (it is fed raw
     * pixels), so this process's only decode is the pack's single load pass. */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        1U, tp_image__test_decode_count(),
        "a pack must decode each source exactly once (no hash-compute re-decode)");
    TEST_ASSERT_FALSE(tp_id128_is_nil(result.pack.pack_input_hash));

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
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

/* U-01a: the pack INPUT (incl. the folder walk for folder sources) is now built on
 * the WORKER, not on the caller/UI thread at pack-start. An atlas with no usable
 * sources therefore no longer fails SYNCHRONOUSLY at start -- tp_session_pack_job_start
 * returns OK and the job completes FAILED, the exact async-failure path export already
 * uses. This pins that the empty-input check moved off the caller thread. */
void test_empty_atlas_pack_fails_async_not_at_start(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session); /* fresh default atlas: no sources */
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);

    tp_error err = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas,
        .work_dir = work_dir,
        .preview_exporter_id = NULL,
    };
    /* Starts OK now (the "no usable images" check moved to the worker). */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_session_pack_job_start(session, &request, &err),
        "empty-atlas pack must START ok (input build is on the worker now)");

    tp_session_job_progress progress;
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);
    /* ...and fails ASYNC, never producing a usable result. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_SESSION_JOB_FAILED, progress.state,
                                  "empty atlas pack must fail async, not at start");

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    (void)tp_session_job_take_result(session, &result, &err); /* release the handle */
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

/* A folder source's recursive enumeration runs on the pack worker,
 * not at pack-start on the UI thread. The walk gate parks the worker INSIDE the folder
 * walk; while it is parked we prove start() has already returned with the job still
 * RUNNING (the walk had NOT completed on the caller thread) -- a deterministic pin of
 * the non-blocking-walk property, not a timing race. Releasing the gate lets the walk
 * finish and the folder packs (all three images enumerated). */
void test_folder_source_walk_runs_on_worker(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);

    char folder[1200];
    TEST_ASSERT_TRUE(snprintf(folder, sizeof folder, "%s/sprites", work_dir) > 0);
    tp_mkdirs(folder);
    for (int i = 0; i < 3; ++i) {
        char png[1320];
        TEST_ASSERT_TRUE(snprintf(png, sizeof png, "%s/s%d.png", folder, i) > 0);
        write_png_fixture(png);
    }
    add_folder_source(session, atlas, folder);

    tp_error err = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas,
        .work_dir = work_dir,
        .preview_exporter_id = NULL,
    };
    tp_scan__test_arm_walk_gate(); /* park the worker inside the folder walk */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_pack_job_start(session, &request, &err));

    /* The walk is executing on the worker (it reached the gate). start() returned
     * while the worker is parked mid-walk, so the job is still RUNNING -- proving the
     * walk did NOT run synchronously on the caller thread. */
    TEST_ASSERT_TRUE_MESSAGE(wait_for_walk_gate(),
                             "the folder walk must run on the worker (gate never entered)");
    tp_session_job_progress progress;
    memset(&progress, 0, sizeof progress);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_poll(session, &progress, &err));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_RUNNING, progress.state,
        "start() must return while the worker is still mid-walk (non-blocking)");

    tp_scan__test_release_walk_gate(); /* let the walk finish */
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_SESSION_JOB_SUCCEEDED, progress.state,
                                  "folder-source pack must succeed");

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_PACK, result.kind);
    TEST_ASSERT_NOT_NULL(result.pack.result);
    /* The worker-side walk enumerated all three folder images. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, result.pack.result->sprite_count,
                                  "the worker-side folder walk must find all 3 images");

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

/* A cancel raised while the worker is mid-folder-walk is
 * observed cooperatively INSIDE the scan -- the walk aborts EARLY and the job ends
 * CANCELLED (never SUCCEEDED). The POST-entry gate parks the worker right after its
 * FIRST visited entry; we request cancel while it is parked, then release so the walk
 * resumes and polls the cancel at the loop top BEFORE reading a second entry.
 *
 * The load-bearing assertion is the visited-entry counter: it stays at 1 because the
 * scan stopped early. This pins the SCAN-level poll specifically -- delete the loop-top
 * cancel poll (tp_scan.c) and the resumed walk runs to completion, visiting all N
 * entries; the job still ends CANCELLED (the shared cancel flag is seen later by the
 * pack), so ONLY the counter catches the regression. */
void test_folder_walk_cancels_mid_scan(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);

    char folder[1200];
    TEST_ASSERT_TRUE(snprintf(folder, sizeof folder, "%s/sprites", work_dir) > 0);
    tp_mkdirs(folder);
    enum { k_folder_images = 5 };
    for (int i = 0; i < k_folder_images; ++i) {
        char png[1320];
        TEST_ASSERT_TRUE(snprintf(png, sizeof png, "%s/s%d.png", folder, i) > 0);
        write_png_fixture(png);
    }
    add_folder_source(session, atlas, folder);

    tp_error err = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas,
        .work_dir = work_dir,
        .preview_exporter_id = NULL,
    };
    tp_scan__test_arm_post_entry_gate(); /* count every entry; park after the first */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_pack_job_start(session, &request, &err));
    TEST_ASSERT_TRUE_MESSAGE(
        wait_for_post_entry_gate(),
        "the folder walk must reach the worker gate after its first entry");
    /* Parked after exactly one visited entry. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, tp_scan__test_visited_entries(),
        "the walk must park after visiting exactly one entry");

    /* Cancel while parked mid-walk, THEN release: the walk resumes, polls the cancel at
     * the loop top, and aborts before reading a second entry -- so the job ends
     * CANCELLED and the visited counter never climbs past 1. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_job_cancel(session, &err));
    tp_scan__test_release_post_entry_gate();

    tp_session_job_progress progress;
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);
    /* (a) terminal CANCELLED. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_CANCELLED, progress.state,
        "a cancel raised mid-walk must end the job CANCELLED, not SUCCEEDED");
    /* (b) the SCAN stopped early -- it did not visit every entry. This is what fails if
     * the loop-top cancel poll is removed (the walk then visits all k_folder_images). */
    const int visited = tp_scan__test_visited_entries();
    TEST_ASSERT_TRUE_MESSAGE(
        visited < k_folder_images,
        "the scan must abort mid-walk (visited < total), not run to completion");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, visited,
        "the scan must stop right after the first entry once cancel is observed");

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, result.status);
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    TEST_ASSERT_TRUE(tp_id128_is_nil(result.pack.pack_input_hash));
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

/* A cancel requested after a private Pack result exists but before terminal
 * publication must produce one coherent CANCELLED outcome: cancelled status,
 * no transferable arena/result/hash, and no accidental success publication. */
void test_late_pack_cancel_has_coherent_terminal_result(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    add_file_source(session, atlas, work_dir);

    tp_error err = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas,
        .work_dir = work_dir,
        .preview_exporter_id = NULL,
    };
    tp_job__test_arm_before_terminal_gate();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_pack_job_start(session, &request, &err));
    TEST_ASSERT_TRUE_MESSAGE(
        wait_for_before_terminal_gate(),
        "pack worker did not reach the pre-terminal publication gate");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_job_cancel(session, &err));
    tp_job__test_release_before_terminal_gate();

    tp_session_job_progress progress;
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, progress.state);

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, result.status);
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    TEST_ASSERT_TRUE(tp_id128_is_nil(result.pack.pack_input_hash));

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

void test_cancel_after_export_commit_is_rejected(void) {
    tp_session *session = make_default_project_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    add_file_source(session, atlas, work_dir);

    char project_path[1200];
    TEST_ASSERT_TRUE(snprintf(project_path, sizeof project_path,
                              "%s/job.ntpacker_project", work_dir) > 0);
    tp_session_save_result save_result;
    memset(&save_result, 0, sizeof save_result);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_new(session, project_path, &save_result, &err));

    const tp_export_command_request request = {
        .work_dir = work_dir,
        .atlas_id = atlas,
    };
    tp_job__test_arm_before_terminal_gate();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_export_start(session, &request, &err));
    const bool gate_entered = wait_for_before_terminal_gate();
    if (!gate_entered) {
        tp_job__test_release_before_terminal_gate();
    }
    TEST_ASSERT_TRUE_MESSAGE(
        gate_entered,
        "export worker did not reach the post-commit publication gate");
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT, tp_session_job_cancel(session, &err));
    tp_job__test_release_before_terminal_gate();

    tp_session_job_result result = wait_for_job_result(session);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_EXPORT, result.kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.status);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.atlases_ok);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.atlases_failed);

    char json_path[1200];
    TEST_ASSERT_TRUE(
        snprintf(json_path, sizeof json_path, "%s/out/atlas1.json", work_dir) >
        0);
    TEST_ASSERT_TRUE_MESSAGE(tp_fs_exists(json_path),
                             "successful terminal result must match committed output");

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

void test_export_cancel_accepted_before_final_commit_owns_terminal(void) {
    tp_session *session = make_default_project_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    add_file_source(session, atlas, work_dir);

    char project_path[1200];
    TEST_ASSERT_TRUE(snprintf(project_path, sizeof project_path,
                              "%s/job.ntpacker_project", work_dir) > 0);
    tp_session_save_result save_result;
    memset(&save_result, 0, sizeof save_result);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_new(session, project_path, &save_result, &err));

    char json_path[1200];
    TEST_ASSERT_TRUE(
        snprintf(json_path, sizeof json_path, "%s/out/atlas1.json", work_dir) >
        0);
    const tp_export_command_request request = {
        .work_dir = work_dir,
        .atlas_id = atlas,
    };
    tp_export_run__test_arm_before_write_gate();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_export_start(session, &request, &err));
    const bool gate_entered = wait_for_before_export_write_gate();
    if (!gate_entered) {
        tp_export_run__test_release_before_write_gate();
    }
    TEST_ASSERT_TRUE_MESSAGE(
        gate_entered,
        "export did not park before the target's irreversible write");
    TEST_ASSERT_FALSE_MESSAGE(
        tp_fs_exists(json_path),
        "the target must not commit before the pre-write cancellation boundary");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_cancel(session, &err));
    tp_export_run__test_release_before_write_gate();

    tp_session_job_result result = wait_for_job_result(session);
    const tp_session_job_kind kind = result.kind;
    const tp_session_job_state state = result.state;
    const tp_status status = result.status;
    const bool output_exists = tp_fs_exists(json_path);

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_EXPORT, kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, status);
    TEST_ASSERT_FALSE_MESSAGE(
        output_exists,
        "an accepted pre-write cancellation must prevent target publication");
}

void test_export_scan_observes_the_accepted_cancel_claim(void) {
    tp_session *session = make_default_project_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    char folder[1200];
    TEST_ASSERT_TRUE(snprintf(folder, sizeof folder, "%s/folder", work_dir) > 0);
    tp_mkdirs(folder);
    char png_path[1400];
    TEST_ASSERT_TRUE(
        snprintf(png_path, sizeof png_path, "%s/export.png", folder) > 0);
    write_png_fixture(png_path);
    add_folder_source(session, atlas, folder);

    char project_path[1200];
    TEST_ASSERT_TRUE(snprintf(project_path, sizeof project_path,
                              "%s/job.ntpacker_project", work_dir) > 0);
    tp_session_save_result save_result;
    memset(&save_result, 0, sizeof save_result);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_new(session, project_path, &save_result, &err));

    const tp_export_command_request request = {
        .work_dir = work_dir,
        .atlas_id = atlas,
    };
    tp_scan__test_arm_walk_gate();
    tp_job__test_arm_after_cancel_claim_gate();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_export_start(session, &request, &err));
    TEST_ASSERT_TRUE_MESSAGE(wait_for_walk_gate(),
                             "export input scan did not enter the walk gate");

    cancel_thread_ctx cancel = {.session = session};
    thrd_t cancel_thread;
    TEST_ASSERT_EQUAL_INT(thrd_success,
                          thrd_create(&cancel_thread, request_cancel_thread,
                                      &cancel));
    TEST_ASSERT_TRUE_MESSAGE(
        wait_for_after_cancel_claim_gate(),
        "cancel request did not reach its accepted-claim gate");
    tp_scan__test_release_walk_gate();

    tp_session_job_result result = wait_for_job_result(session);
    tp_job__test_release_after_cancel_claim_gate();
    TEST_ASSERT_EQUAL_INT(thrd_success, thrd_join(cancel_thread, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, cancel.status);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, result.status);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.targets);

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

void test_partial_export_counts_successful_targets_and_notices(void) {
    tp_session *session = make_default_project_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    add_file_source(session, atlas, work_dir);
    add_partial_failure_target(session, atlas);

    char project_path[1200];
    TEST_ASSERT_TRUE(snprintf(project_path, sizeof project_path,
                              "%s/job.ntpacker_project", work_dir) > 0);
    tp_session_save_result save_result;
    memset(&save_result, 0, sizeof save_result);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_new(session, project_path, &save_result, &err));
    const tp_export_command_request request = {
        .work_dir = work_dir,
        .atlas_id = atlas,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_export_start(session, &request, &err));

    tp_session_job_result result = wait_for_job_result(session);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, result.state);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.targets);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.atlases_failed);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.notices);

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

/* The worker's last cancellation observation and terminal publication must be
 * one handshake. A cancel accepted inside that former race window must still
 * own the terminal outcome. */
void test_cancel_after_last_observation_wins_terminal_publication(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    add_file_source(session, atlas, work_dir);

    tp_error err = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas,
        .work_dir = work_dir,
        .preview_exporter_id = NULL,
    };
    tp_job__test_arm_after_cancel_observation_gate();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_pack_job_start(session, &request, &err));
    TEST_ASSERT_TRUE_MESSAGE(
        wait_for_after_cancel_observation_gate(),
        "pack worker did not reach the post-observation terminal gate");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_job_cancel(session, &err));
    tp_job__test_release_after_cancel_observation_gate();

    tp_session_job_progress progress;
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, progress.state);

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, result.status);
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    TEST_ASSERT_TRUE(tp_id128_is_nil(result.pack.pack_input_hash));
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
}

/* Once terminal publication has claimed the outcome, cancellation must report
 * that it was not accepted instead of returning a misleading success. */
void test_cancel_after_terminal_publication_is_rejected(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    char work_dir[1024];
    job_scratch_dir(work_dir, sizeof work_dir);
    tp_mkdirs(work_dir);
    add_file_source(session, atlas, work_dir);

    tp_error err = {{0}};
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
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, progress.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_session_job_cancel(session, &err));

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.status);
    TEST_ASSERT_NOT_NULL(result.pack.arena);
    TEST_ASSERT_NOT_NULL(result.pack.result);
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_scratch_tree(work_dir);
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
    RUN_TEST(test_pack_job_decodes_each_source_once);
    RUN_TEST(test_pack_input_hash_changes_on_semantic_mutation);
    RUN_TEST(test_source_less_export_succeeds_as_skipped);
    RUN_TEST(test_empty_folder_export_succeeds_as_skipped);
    RUN_TEST(test_empty_atlas_pack_fails_async_not_at_start);
    RUN_TEST(test_folder_source_walk_runs_on_worker);
    RUN_TEST(test_folder_walk_cancels_mid_scan);
    RUN_TEST(test_late_pack_cancel_has_coherent_terminal_result);
    RUN_TEST(test_cancel_after_export_commit_is_rejected);
    RUN_TEST(test_export_cancel_accepted_before_final_commit_owns_terminal);
    RUN_TEST(test_export_scan_observes_the_accepted_cancel_claim);
    RUN_TEST(test_partial_export_counts_successful_targets_and_notices);
    RUN_TEST(test_cancel_after_last_observation_wins_terminal_publication);
    RUN_TEST(test_cancel_after_terminal_publication_is_rejected);
    return UNITY_END();
}
