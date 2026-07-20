#define _CRT_SECURE_NO_WARNINGS

#include "tp_core/tp_id.h"
#include "tp_core/tp_project.h"
#include "tp_fs_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_internal.h"
#include "tp_project_mutation_internal.h"
#include "unity.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *g_dir;

typedef struct save_fault_case {
    tp_file_io_phase phase;
    const char *phase_id;
    int native_code;
    bool create_only;
} save_fault_case;

static const save_fault_case SAVE_FAULTS[] = {
    {TP_FILE_IO_PHASE_TEMP_OPEN, "temp_open", EACCES, false},
    {TP_FILE_IO_PHASE_TEMP_WRITE, "temp_write", ENOSPC, false},
    {TP_FILE_IO_PHASE_FILE_SYNC, "file_sync", EIO, false},
    {TP_FILE_IO_PHASE_TEMP_CLOSE, "temp_close", EIO, false},
    {TP_FILE_IO_PHASE_ATOMIC_REPLACE, "atomic_replace", EACCES, false},
    {TP_FILE_IO_PHASE_ATOMIC_CREATE, "atomic_create", EACCES, true},
};

void setUp(void) {}
void tearDown(void) {}

static void make_path(char *out, size_t capacity, const char *name) {
    const int written = snprintf(out, capacity, "%s/%s", g_dir, name);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t)written < capacity);
}

static tp_project *make_project(const char *name) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_set_atlas_name(tp_project_get_atlas(project, 0), name));
    tp_rng rng = tp_rng_os();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, NULL));
    return project;
}

static void assert_saved_name(const char *path, const char *expected) {
    tp_project *loaded = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_load(path, &loaded, &error));
    TEST_ASSERT_EQUAL_STRING(expected,
                             tp_project_get_atlas(loaded, 0)->name);
    tp_project_destroy(loaded);
}

static tp_status save_for_case(tp_project *project, const char *path,
                               bool create_only, tp_id128 *fingerprint,
                               tp_error *error) {
    if (create_only) {
        return tp_project_save_new_with_fingerprint(project, path,
                                                    fingerprint, error);
    }
    return tp_project_save_with_fingerprint(project, path, fingerprint,
                                            error);
}

static void assert_fault_outcome(const save_fault_case *fault, size_t index) {
    char baseline_path[512];
    char destination_path[512];
    char baseline_name[64];
    char destination_name[64];
    (void)snprintf(baseline_name, sizeof baseline_name,
                   "save-io-%zu-baseline.ntpacker_project", index);
    (void)snprintf(destination_name, sizeof destination_name,
                   "save-io-%zu-destination.ntpacker_project", index);
    make_path(baseline_path, sizeof baseline_path, baseline_name);
    make_path(destination_path, sizeof destination_path, destination_name);
    (void)tp_fs_remove_file(baseline_path);
    (void)tp_fs_remove_file(destination_path);

    tp_project *project = make_project("before");
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_save(project, baseline_path, &error));
    const char *attempted_path = baseline_path;
    if (fault->create_only) {
        attempted_path = destination_path;
    }
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_set_atlas_name(tp_project_get_atlas(project, 0), "after"));

    tp_id128 fingerprint = {{0xFF}};
    tp_project__test_fail_next_save_io(fault->phase);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_FILE_IO_FAILED,
        save_for_case(project, attempted_path, fault->create_only,
                      &fingerprint, &error));
    TEST_ASSERT_TRUE(tp_id128_is_nil(fingerprint));
    TEST_ASSERT_EQUAL_INT(fault->phase, error.file_io.phase);
    TEST_ASSERT_EQUAL_STRING(fault->phase_id,
                             tp_file_io_phase_id(error.file_io.phase));
    TEST_ASSERT_EQUAL_STRING(attempted_path, error.file_io.path);
    TEST_ASSERT_EQUAL_INT(fault->native_code, error.file_io.native_code);

    assert_saved_name(baseline_path, "before");
    if (fault->create_only) {
        TEST_ASSERT_FALSE(tp_fs_exists(destination_path));
    }

    /* The product performs no hidden retry. A caller may explicitly retry. */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        save_for_case(project, attempted_path, fault->create_only,
                      &fingerprint, &error));
    TEST_ASSERT_FALSE(tp_id128_is_nil(fingerprint));
    assert_saved_name(attempted_path, "after");

    tp_project_destroy(project);
    TEST_ASSERT_TRUE(tp_fs_remove_file(baseline_path));
    if (fault->create_only) {
        TEST_ASSERT_TRUE(tp_fs_remove_file(destination_path));
    }
}

void test_save_io_failure_matrix(void) {
    for (size_t index = 0;
         index < sizeof SAVE_FAULTS / sizeof SAVE_FAULTS[0]; index++) {
        assert_fault_outcome(&SAVE_FAULTS[index], index);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    g_dir = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_save_io_failure_matrix);
    return UNITY_END();
}
