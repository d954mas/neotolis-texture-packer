/* _CRT_SECURE_NO_WARNINGS comes from the target (the seam-enabled worker TUs
 * compiled into this executable need it too). */

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
    TEST_ASSERT_NULL(result._owner);
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    tp_session_job_result_destroy(&result);
    tp_session_observation_destroy(observation);
    remove_tree(work_dir);
}

static void set_worker_env(const char *name, const char *value) {
#ifdef _WIN32
    TEST_ASSERT_EQUAL_INT(0, _putenv_s(name, value ? value : ""));
#else
    if (value) {
        TEST_ASSERT_EQUAL_INT(0, setenv(name, value, 1));
    } else {
        (void)unsetenv(name);
    }
#endif
}

/* Count the worker's private per-request directories left under work_dir. */
static int count_request_dirs(const char *work_dir) {
    tp_fs_dir *dir = tp_fs_dir_open(work_dir);
    if (!dir) {
        return 0;
    }
    int count = 0;
    tp_fs_dir_entry entry;
    while (tp_fs_dir_next(dir, &entry) == TP_FS_DIR_ENTRY) {
        if (entry.info.kind == TP_FS_KIND_DIRECTORY &&
            strncmp(entry.name, "req-", 4U) == 0) {
            count++;
        }
    }
    tp_fs_dir_close(dir);
    return count;
}

/* Count `.ntpack` artifacts anywhere under work_dir (they live one level down,
 * inside the worker's private request directory). */
static int count_pack_artifacts(const char *path) {
    tp_fs_dir *dir = tp_fs_dir_open(path);
    if (!dir) {
        return 0;
    }
    int count = 0;
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
            count += count_pack_artifacts(child);
            continue;
        }
        const size_t name_length = strlen(entry.name);
        if (name_length > 7U &&
            strcmp(entry.name + name_length - 7U, ".ntpack") == 0) {
            count++;
        }
    }
    tp_fs_dir_close(dir);
    return count;
}

/* Cancel linearization without a race: the worker is held in its pre-work block
 * for far longer than the host's cancellation grace, so the cancel is provably
 * issued while the worker has done nothing. The host does not wait out the block,
 * and because the worker never answers, the terminal is SYNTHESIZED -- which is
 * exactly the terminal that used to report 0 ms to the UI, since only the host
 * ever saw this request's clock. */
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
    set_worker_env("TP_TEST_JOB_WORKER_BLOCK_MS", "8000");
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_pack_job_start(session, &request, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_job_cancel(session, &error));
    tp_session_job_result result = wait_for_result(session);
    set_worker_env("TP_TEST_JOB_WORKER_BLOCK_MS", "");
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, result.status);
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    TEST_ASSERT_TRUE_MESSAGE(
        result.elapsed_ms > 0.0,
        "a synthesized terminal must carry the host's own elapsed measurement");
    TEST_ASSERT_TRUE_MESSAGE(
        result.elapsed_ms < 6000.0,
        "cancel waited out the blocked worker instead of linearizing");
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_tree(work_dir);
}

/* The cancel that actually costs something: the worker is parked INSIDE run_pack
 * with its private request directory already created, so "nothing was left
 * behind" is a real claim about a real directory. The worker observes the cancel
 * there, so the terminal is its own CANCELLED frame -- not a crash report from a
 * killed process -- and neither the directory nor an artifact survives. */
void test_cancel_of_a_running_pack_leaves_no_artifact_behind(void) {
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
    /* The worker waits for the cancel rather than racing it; the grace is widened
     * so the outcome is the worker's own terminal even on a slow machine. */
    set_worker_env("TP_TEST_JOB_WORKER_BLOCK_IN_PACK_MS", "20000");
    tp_job__test_set_worker_cancel_grace_ms(20000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_pack_job_start(session, &request, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_job_cancel(session, &error));
    tp_session_job_result result = wait_for_result(session);
    set_worker_env("TP_TEST_JOB_WORKER_BLOCK_IN_PACK_MS", "");
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, result.status);
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, count_request_dirs(work_dir),
        "a cancelled Pack must leave no private request directory");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, count_pack_artifacts(work_dir),
        "a cancelled Pack must leave no artifact");
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_tree(work_dir);
}

/* Cancel racing a SUCCEEDED terminal. The worker is deaf to the cancel byte, so
 * it publishes a genuine artifact; the host accepted the cancellation first, so
 * it adopts nothing. The only handle left on that file is the path in the
 * terminal frame -- if the host does not take it, a `.ntpack` and its private
 * directory outlive the run for good. */
void test_cancel_racing_a_successful_pack_deletes_the_orphan(void) {
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
    set_worker_env("TP_TEST_JOB_WORKER_IGNORE_CANCEL", "1");
    tp_job__test_set_worker_cancel_grace_ms(20000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_pack_job_start(session, &request, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_job_cancel(session, &error));
    tp_session_job_result result = wait_for_result(session);
    set_worker_env("TP_TEST_JOB_WORKER_IGNORE_CANCEL", "");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_CANCELLED, result.state,
        "an accepted host cancellation outranks the terminal that raced it");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, result.status);
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, count_pack_artifacts(work_dir),
        "the artifact of a cancelled-but-successful Pack must be deleted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, count_request_dirs(work_dir),
        "and so must its private request directory");
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_tree(work_dir);
}

/* The UTF-8 name table is load-bearing on the wire: `.ntpack` stores hash-only
 * names, so a host that rebuilt the map from the LIVE model would relabel every
 * sprite of a Pack that was renamed while it ran. Rename the atlas after the job
 * snapshot is taken and assert the adopted result still carries the packed name. */
void test_rename_during_pack_keeps_the_packed_labels(void) {
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
    set_worker_env("TP_TEST_JOB_WORKER_BLOCK_MS", "250");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_pack_job_start(session, &request, &error),
        error.msg);

    tp_operation operation = {0};
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas_id;
    operation.u.atlas_rename.name = (char *)"renamed-mid-pack";
    tp_txn_request rename_request = {0};
    rename_request.schema = TP_TXN_SCHEMA;
    memcpy(rename_request.id_hex, "efefefefefefefefefefefefefefefef",
           sizeof rename_request.id_hex);
    rename_request.expected_revision = tp_session_revision(session);
    rename_request.ops = &operation;
    rename_request.op_count = 1U;
    tp_txn_result rename_result = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_apply(session, &rename_request, &rename_result, &error),
        error.msg);
    TEST_ASSERT_TRUE(rename_result.committed);
    tp_txn_result_free(&rename_result);

    tp_session_job_result result = wait_for_result(session);
    set_worker_env("TP_TEST_JOB_WORKER_BLOCK_MS", "");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_SESSION_JOB_SUCCEEDED, result.state, result.error.msg);
    TEST_ASSERT_NOT_NULL(result.pack.result);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "atlas1", result.pack.result->atlas_name,
        "the adopted result must carry the names the worker packed");
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_tree(work_dir);
}

/* Digestless artifact validation. The host trusts an exact size + the pack magic
 * (never a full-file hash), so both failures must surface as structured terminal
 * errors that leave no result behind -- the caller's previous Pack stands. */
static void assert_artifact_fault_is_structured(const char *fault,
                                                tp_status expected) {
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
    set_worker_env("TP_TEST_JOB_WORKER_ARTIFACT_FAULT", fault);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_pack_job_start(session, &request, &error),
        error.msg);
    tp_session_job_result result = wait_for_result(session);
    set_worker_env("TP_TEST_JOB_WORKER_ARTIFACT_FAULT", "");
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, result.state);
    TEST_ASSERT_EQUAL_INT(expected, result.status);
    TEST_ASSERT_TRUE(result.error.msg[0] != '\0');
    TEST_ASSERT_NULL(result.pack.arena);
    TEST_ASSERT_NULL(result.pack.result);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, count_request_dirs(work_dir),
        "a rejected artifact must not leave its private directory behind");
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    remove_tree(work_dir);
}

void test_damaged_pack_artifact_is_a_structured_terminal_failure(void) {
    assert_artifact_fault_is_structured("truncate",
                                        TP_STATUS_BUILDER_FAILED);
    assert_artifact_fault_is_structured("magic", TP_STATUS_BAD_MAGIC);
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
    RUN_TEST(test_cancel_of_a_running_pack_leaves_no_artifact_behind);
    RUN_TEST(test_cancel_racing_a_successful_pack_deletes_the_orphan);
    RUN_TEST(test_rename_during_pack_keeps_the_packed_labels);
    RUN_TEST(test_damaged_pack_artifact_is_a_structured_terminal_failure);
    RUN_TEST(test_worker_timeout_is_a_structured_terminal_failure);
    RUN_TEST(test_export_target_allocation_failure_is_fail_atomic);
    return UNITY_END();
}
