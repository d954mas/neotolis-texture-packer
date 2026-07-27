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
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_fs_internal.h"
#include "tp_test_seams.h"
#include "unity.h"

static const uint8_t k_png_4x4[] = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
    0x00U, 0x00U, 0x00U, 0x0DU, 0x49U, 0x48U, 0x44U, 0x52U,
    0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0xA9U, 0xF1U, 0x9EU,
    0x7EU, 0x00U, 0x00U, 0x00U, 0x33U, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xDAU, 0x15U, 0xC8U, 0xA1U, 0x11U, 0x00U,
    0x30U, 0x08U, 0x04U, 0x41U, 0x34U, 0x3AU, 0x95U, 0x44U,
    0x53U, 0x09U, 0x9AU, 0x42U, 0x98U, 0xD7U, 0xF4U, 0x4CU,
    0x2EU, 0x62U, 0xCDU, 0x9AU, 0x9FU, 0xDEU, 0x8BU, 0x84U,
    0x60U, 0x1EU, 0x04U, 0x12U, 0x8AU, 0x1FU, 0x45U, 0x20U,
    0xA1U, 0xFAU, 0x31U, 0x04U, 0x12U, 0x9AU, 0xDEU, 0x07U,
    0x18U, 0xA8U, 0x21U, 0x51U, 0x4CU, 0xC2U, 0x35U, 0x74U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4EU, 0x44U,
    0xAEU, 0x42U, 0x60U, 0x82U,
};

void setUp(void) { tp_job__test_reset_all(); }
void tearDown(void) { tp_job__test_reset_all(); }

static int deterministic_fill(void *context, uint8_t *out, size_t length) {
    uint8_t *seed = context;
    for (size_t i = 0U; i < length; ++i) {
        out[i] = (uint8_t)(*seed + (uint8_t)i);
    }
    *seed = (uint8_t)(*seed + 17U);
    return (int)length;
}

static tp_session *make_session(void) {
    uint8_t seed = 1U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_create(&rng, &session, &error), error.msg);
    return session;
}

static tp_id128 default_atlas_id(tp_session *session) {
    tp_session_snapshot *snapshot = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &error),
        error.msg);
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 id = atlas->id;
    tp_session_snapshot_destroy(snapshot);
    return id;
}

static void scratch_dir(char *out, size_t capacity) {
    char cwd[1024];
#ifdef _WIN32
    TEST_ASSERT_NOT_NULL(_getcwd(cwd, (int)sizeof cwd));
#else
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
#endif
    for (char *cursor = cwd; *cursor; ++cursor) {
        if (*cursor == '\\') {
            *cursor = '/';
        }
    }
    const int length = snprintf(
        out, capacity, "%s/tp_job_process_scratch", cwd);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < capacity);
}

static void remove_tree(const char *path) {
    tp_fs_dir *dir = tp_fs_dir_open(path);
    if (dir) {
        tp_fs_dir_entry entry;
        while (tp_fs_dir_next(dir, &entry) == TP_FS_DIR_ENTRY) {
            char child[2048];
            const int length = snprintf(
                child, sizeof child, "%s/%s", path, entry.name);
            if (length < 0 || (size_t)length >= sizeof child) {
                continue;
            }
            if (entry.info.kind == TP_FS_KIND_DIRECTORY &&
                !entry.info.reparse) {
                remove_tree(child);
            } else {
                (void)tp_fs_remove_file(child);
            }
        }
        tp_fs_dir_close(dir);
    }
    (void)tp_fs_remove_dir(path);
}

static void add_file_source(tp_session *session, tp_id128 atlas_id,
                            const char *work_dir) {
    char path[1400];
    const int length = snprintf(
        path, sizeof path, "%s/job_src.png", work_dir);
    TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(
        sizeof k_png_4x4,
        fwrite(k_png_4x4, 1U, sizeof k_png_4x4, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    tp_operation operation = {0};
    operation.kind = TP_OP_SOURCE_ADD;
    operation.atlas_id = atlas_id;
    operation.u.source_add.kind = TP_SOURCE_KIND_FILE;
    operation.u.source_add.source_id =
        (tp_id128){{0x11U, 0x22U, 0x33U, 0x44U,
                    0x55U, 0x66U, 0x77U, 0x88U,
                    0x99U, 0xAAU, 0xBBU, 0xCCU,
                    0xDDU, 0xEEU, 0xF0U, 0x0FU}};
    const size_t path_size = strlen(path) + 1U;
    operation.u.source_add.key = malloc(path_size);
    TEST_ASSERT_NOT_NULL(operation.u.source_add.key);
    memcpy(operation.u.source_add.key, path, path_size);

    tp_session_snapshot *snapshot = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &error));
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
           sizeof request.id_hex);
    request.expected_revision =
        tp_session_snapshot_revision(snapshot);
    request.ops = &operation;
    request.op_count = 1U;
    tp_txn_result result = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_apply(session, &request, &result, &error),
        error.msg);
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
    tp_operation_free(&operation);
    tp_session_snapshot_destroy(snapshot);
}

static tp_id128 add_enabled_export_target(
    tp_session *session, tp_id128 atlas_id) {
    const tp_id128 target_id = {{
        0x21U, 0x32U, 0x43U, 0x54U,
        0x65U, 0x76U, 0x87U, 0x98U,
        0xA9U, 0xBAU, 0xCBU, 0xDCU,
        0xEDU, 0xFEU, 0x10U, 0x20U,
    }};
    tp_operation operation = {0};
    operation.kind = TP_OP_TARGET_CREATE;
    operation.atlas_id = atlas_id;
    operation.u.target_create.target_id = target_id;
    operation.u.target_create.exporter_id =
        (char *)TP_EXPORTER_ID_JSON_NEOTOLIS;
    operation.u.target_create.out_path = (char *)"out/job-oom";
    operation.u.target_create.enabled = true;

    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "dededededededededededededededede",
           sizeof request.id_hex);
    request.expected_revision = tp_session_revision(session);
    request.ops = &operation;
    request.op_count = 1U;
    tp_txn_result result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_apply(session, &request, &result, &error),
        error.msg);
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
    return target_id;
}

static tp_session_job_result wait_for_result(tp_session *session) {
    tp_error error = {{0}};
    tp_session_job_progress progress = {0};
    bool terminal = false;
    for (long spin = 0; spin < 10000000L; ++spin) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            TP_STATUS_OK,
            tp_session_job_poll(session, &progress, &error),
            error.msg);
        if (progress.state != TP_SESSION_JOB_RUNNING) {
            terminal = true;
            break;
        }
        thrd_yield();
    }
    TEST_ASSERT_TRUE_MESSAGE(terminal,
                             "worker process did not terminate");
    tp_session_job_result result = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_job_take_result(session, &result, &error),
        error.msg);
    return result;
}

void test_process_pack_terminal_is_published_after_host_poll(void) {
    tp_session *session = make_session();
    char work_dir[1024];
    scratch_dir(work_dir, sizeof work_dir);
    remove_tree(work_dir);
    tp_mkdirs(work_dir);
    const tp_id128 atlas_id = default_atlas_id(session);
    add_file_source(session, atlas_id, work_dir);

    const tp_pack_job_request request = {
        .atlas_id = atlas_id,
        .work_dir = work_dir,
        .session_instance_generation = 41U,
    };
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_pack_job_start(session, &request, &error),
        error.msg);

    tp_session_observation *observation = NULL;
    tp_session_observation_token token = {0};
    bool terminal = false;
    for (long spin = 0; spin < 10000000L; ++spin) {
        tp_session_job_progress progress = {0};
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            TP_STATUS_OK,
            tp_session_job_poll(session, &progress, &error),
            error.msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            TP_STATUS_OK,
            tp_session_observe(session, observation ? &token : NULL,
                               &observation, &error),
            error.msg);
        if (!observation) {
            thrd_yield();
            continue;
        }
        token = tp_session_observation_token_query(observation);
        const tp_session_job_observed_state state =
            tp_session_observation_job_state(observation);
        if (state.terminal) {
            if (state.state != TP_SESSION_JOB_SUCCEEDED) {
                TEST_MESSAGE(state.terminal_error.msg);
            }
            TEST_ASSERT_EQUAL_UINT64(41U,
                                     state.session_instance_generation);
            TEST_ASSERT_TRUE(state.request_id > 0U);
            TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED,
                                  state.state);
            const tp_session_job_observed_result *observed =
                tp_session_observation_job_result(observation);
            TEST_ASSERT_NOT_NULL(observed);
            TEST_ASSERT_TRUE(observed->present);
            TEST_ASSERT_NOT_NULL(observed->result);
            TEST_ASSERT_NOT_NULL(observed->result->pack.arena);
            TEST_ASSERT_NOT_NULL(observed->result->pack.result);
            TEST_ASSERT_EQUAL_STRING(
                "atlas1",
                observed->result->pack.result->atlas_name);
            terminal = true;
            break;
        }
        tp_session_observation_destroy(observation);
        observation = NULL;
        thrd_yield();
    }
    TEST_ASSERT_TRUE_MESSAGE(terminal,
                             "terminal observation was not admitted");

    tp_session_job_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_take_result(session, &result, &error));
    TEST_ASSERT_EQUAL_PTR(
        result.pack.arena,
        tp_session_observation_job_result(observation)
            ->result->pack.arena);
    TEST_ASSERT_EQUAL_STRING(
        "atlas1", result.pack.result->atlas_name);
    tp_session_destroy(session);
    TEST_ASSERT_NOT_NULL(
        tp_session_observation_job_result(observation)
            ->result->pack.result);
    tp_session_job_result_destroy(&result);
    tp_session_observation_destroy(observation);
    remove_tree(work_dir);
}

void test_host_cancel_owns_process_terminal_result(void) {
    tp_session *session = make_session();
    char work_dir[1024];
    scratch_dir(work_dir, sizeof work_dir);
    remove_tree(work_dir);
    tp_mkdirs(work_dir);
    const tp_id128 atlas_id = default_atlas_id(session);
    add_file_source(session, atlas_id, work_dir);

    const tp_pack_job_request request = {
        .atlas_id = atlas_id,
        .work_dir = work_dir,
    };
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_pack_job_start(session, &request, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_job_cancel(session, &error));
    tp_session_job_result result = wait_for_result(session);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, result.status);
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_tree(work_dir);
}

void test_worker_timeout_is_a_structured_terminal_failure(void) {
    tp_session *session = make_session();
    char work_dir[1024];
    scratch_dir(work_dir, sizeof work_dir);
    remove_tree(work_dir);
    tp_mkdirs(work_dir);
    const tp_id128 atlas_id = default_atlas_id(session);
    add_file_source(session, atlas_id, work_dir);

    tp_job__test_set_worker_timeout_ms(1);
    const tp_pack_job_request request = {
        .atlas_id = atlas_id,
        .work_dir = work_dir,
    };
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_pack_job_start(session, &request, &error),
        error.msg);
    tp_session_job_result result = wait_for_result(session);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BUILDER_CRASHED, result.status);
    TEST_ASSERT_TRUE(result.error.msg[0] != '\0');
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_tree(work_dir);
}

void test_export_target_allocation_failure_is_fail_atomic(void) {
    tp_session *session = make_session();
    char work_dir[1024];
    scratch_dir(work_dir, sizeof work_dir);
    remove_tree(work_dir);
    tp_mkdirs(work_dir);
    const tp_id128 atlas_id = default_atlas_id(session);
    add_file_source(session, atlas_id, work_dir);
    const tp_id128 target_id =
        add_enabled_export_target(session, atlas_id);

    const tp_export_command_request request = {
        .work_dir = work_dir,
        .atlas_id = atlas_id,
    };
    tp_error error = {{0}};
    tp_job__test_fail_next_observation_target_allocation();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        tp_session_export_start(session, &request, &error));
    TEST_ASSERT_TRUE(error.msg[0] != '\0');
    TEST_ASSERT_FALSE(tp_session_job_active(session));

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &error),
        error.msg);
    const tp_snapshot_target *target =
        tp_session_snapshot_target_by_id(
            snapshot, atlas_id, target_id);
    TEST_ASSERT_NOT_NULL(target);
    TEST_ASSERT_TRUE(target->enabled);
    tp_session_snapshot_destroy(snapshot);

    memset(&error, 0, sizeof error);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_export_start(session, &request, &error),
        error.msg);
    tp_session_job_result result = wait_for_result(session);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_SUCCEEDED, result.state, result.error.msg);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.status);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.targets);
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_tree(work_dir);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(
        test_process_pack_terminal_is_published_after_host_poll);
    RUN_TEST(test_host_cancel_owns_process_terminal_result);
    RUN_TEST(test_worker_timeout_is_a_structured_terminal_failure);
    RUN_TEST(test_export_target_allocation_failure_is_fail_atomic);
    return UNITY_END();
}
