#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_operation.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_project_internal.h"
#include "unity.h"

static const char *g_dir;

typedef struct session_save_fault {
    tp_file_io_phase phase;
    bool create_only;
} session_save_fault;

static const session_save_fault SAVE_FAULTS[] = {
    {TP_FILE_IO_PHASE_TEMP_OPEN, false},
    {TP_FILE_IO_PHASE_TEMP_WRITE, false},
    {TP_FILE_IO_PHASE_FILE_SYNC, false},
    {TP_FILE_IO_PHASE_TEMP_CLOSE, false},
    {TP_FILE_IO_PHASE_ATOMIC_REPLACE, false},
    {TP_FILE_IO_PHASE_ATOMIC_CREATE, true},
};

void setUp(void) {}
void tearDown(void) {}

static int deterministic_fill(void *context, uint8_t *out, size_t length) {
    uint8_t *next = (uint8_t *)context;
    for (size_t index = 0; index < length; index++) {
        out[index] = (uint8_t)(*next + (uint8_t)index);
    }
    *next = (uint8_t)(*next + 17U);
    return (int)length;
}

static void make_path(char *out, size_t capacity, const char *name) {
    const int written = snprintf(out, capacity, "%s/%s", g_dir, name);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t)written < capacity);
}

static void apply_rename(tp_session *session, tp_id128 atlas_id,
                         const char *transaction_id) {
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas_id;
    const char *name = "after";
    operation.u.atlas_rename.name = (char *)malloc(strlen(name) + 1U);
    TEST_ASSERT_NOT_NULL(operation.u.atlas_rename.name);
    (void)snprintf(operation.u.atlas_rename.name, strlen(name) + 1U, "%s",
                   name);

    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%s",
                   transaction_id);
    request.expected_revision = tp_session_revision(session);
    request.ops = &operation;
    request.op_count = 1;
    tp_txn_result result;
    memset(&result, 0, sizeof result);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_apply(session, &request, &result, &error));
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
    tp_operation_free(&operation);
}

static void assert_disk_name(const char *path, const char *expected) {
    tp_session_snapshot *snapshot = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_snapshot_load(path, &snapshot, &error));
    TEST_ASSERT_EQUAL_STRING(
        expected, tp_session_snapshot_atlas_at(snapshot, 0)->name);
    tp_session_snapshot_destroy(snapshot);
}

static void assert_session_outcome(const session_save_fault *fault,
                                   size_t index) {
    char baseline_path[512];
    char destination_path[512];
    char name[80];
    (void)snprintf(name, sizeof name,
                   "session-save-io-%zu.ntpacker_project", index);
    make_path(baseline_path, sizeof baseline_path, name);
    (void)snprintf(name, sizeof name,
                   "session-save-io-%zu-new.ntpacker_project", index);
    make_path(destination_path, sizeof destination_path, name);
    (void)remove(baseline_path);
    (void)remove(destination_path);

    uint8_t seed = (uint8_t)(index * 23U + 1U);
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_create_default_project(&rng, &session, &error));
    tp_session_save_result save_result;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_new(session, baseline_path, &save_result, &error));

    tp_session_snapshot *initial = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &initial, &error));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(initial, 0);
    char transaction_id[33];
    (void)snprintf(transaction_id, sizeof transaction_id, "%032zx",
                   index + 1U);
    apply_rename(session, atlas->id, transaction_id);
    tp_session_snapshot_destroy(initial);

    tp_session_snapshot *before = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &before, &error));
    tp_id128 baseline_fingerprint = {{0}};
    TEST_ASSERT_TRUE(tp_session_snapshot_saved_file_fingerprint(
        before, &baseline_fingerprint));
    const tp_session_identity baseline_identity =
        tp_session_snapshot_identity(before);
    const int64_t revision = tp_session_snapshot_revision(before);
    const uint64_t event_sequence =
        tp_session_snapshot_event_sequence(before);
    const uint64_t model_generation =
        tp_session_snapshot_model_generation(before);
    const int undo_depth = tp_session_undo_depth(session);
    TEST_ASSERT_TRUE(tp_session_snapshot_dirty(before));

    memset(&save_result, 0xA5, sizeof save_result);
    tp_project__test_fail_next_save_io(fault->phase);
    const char *attempted_path = fault->create_only
                                     ? destination_path
                                     : baseline_path;
    tp_status status = fault->create_only
                           ? tp_session_save_new(session, attempted_path,
                                                 &save_result, &error)
                           : tp_session_save(session, &save_result, &error);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_FILE_IO_FAILED, status);
    TEST_ASSERT_FALSE(save_result.saved);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          save_result.file_durability_status);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, save_result.recovery_status);
    TEST_ASSERT_EQUAL_INT(0, save_result.target_path[0]);
    TEST_ASSERT_TRUE(tp_id128_is_nil(save_result.file_fingerprint));
    TEST_ASSERT_EQUAL_INT(fault->phase, error.file_io.phase);
    TEST_ASSERT_EQUAL_STRING(attempted_path, error.file_io.path);
    if (fault->create_only) {
        TEST_ASSERT_EQUAL_PTR(attempted_path, error.file_io.path);
    }

    tp_session_snapshot *after_failure = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &after_failure, &error));
    tp_id128 after_fingerprint = {{0}};
    TEST_ASSERT_TRUE(tp_session_snapshot_saved_file_fingerprint(
        after_failure, &after_fingerprint));
    TEST_ASSERT_TRUE(tp_id128_eq(baseline_fingerprint, after_fingerprint));
    TEST_ASSERT_EQUAL_STRING(
        baseline_identity.canonical_path,
        tp_session_snapshot_identity(after_failure).canonical_path);
    TEST_ASSERT_EQUAL_INT64(revision,
                            tp_session_snapshot_revision(after_failure));
    TEST_ASSERT_EQUAL_UINT64(
        event_sequence,
        tp_session_snapshot_event_sequence(after_failure));
    TEST_ASSERT_EQUAL_UINT64(
        model_generation,
        tp_session_snapshot_model_generation(after_failure));
    TEST_ASSERT_TRUE(tp_session_snapshot_dirty(after_failure));
    TEST_ASSERT_EQUAL_INT(undo_depth, tp_session_undo_depth(session));
    assert_disk_name(baseline_path, "atlas1");
    if (fault->create_only) {
        tp_session_snapshot *absent = NULL;
        TEST_ASSERT_NOT_EQUAL(
            TP_STATUS_OK,
            tp_session_snapshot_load(destination_path, &absent, &error));
        TEST_ASSERT_NULL(absent);
    }

    /* A retry is an explicit new session command, never hidden in Save. */
    status = fault->create_only
                 ? tp_session_save_new(session, attempted_path, &save_result,
                                       &error)
                 : tp_session_save(session, &save_result, &error);
    char retry_diagnostic[768];
    (void)snprintf(retry_diagnostic, sizeof retry_diagnostic,
                   "explicit retry after %s failed: status=%s phase=%s native=%d msg=%s",
                   tp_file_io_phase_id(fault->phase), tp_status_id(status),
                   tp_file_io_phase_id(error.file_io.phase),
                   error.file_io.native_code, error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, status, retry_diagnostic);
    TEST_ASSERT_TRUE(save_result.saved);
    assert_disk_name(attempted_path, "after");

    tp_session_snapshot_destroy(after_failure);
    tp_session_snapshot_destroy(before);
    tp_session_destroy(session);
    TEST_ASSERT_EQUAL_INT(0, remove(baseline_path));
    if (fault->create_only) {
        TEST_ASSERT_EQUAL_INT(0, remove(destination_path));
    }
}

void test_session_save_io_outcome_matrix(void) {
    for (size_t index = 0;
         index < sizeof SAVE_FAULTS / sizeof SAVE_FAULTS[0]; index++) {
        assert_session_outcome(&SAVE_FAULTS[index], index);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    g_dir = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_session_save_io_outcome_matrix);
    return UNITY_END();
}
