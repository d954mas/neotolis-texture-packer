/* Executable-relative runtime format-root and missing-root catalog contract. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "tp_core/tp_format.h"
#include "tp_fs_internal.h"
#include "unity.h"

static const char *g_scratch;

void setUp(void) {}
void tearDown(void) {}

static bool join_path(char *out, size_t out_cap, const char *parent,
                      const char *child) {
    const int written = snprintf(out, out_cap, "%s/%s", parent, child);
    return written >= 0 && (size_t)written < out_cap;
}

static char *current_directory(char *out, size_t out_cap) {
#ifdef _WIN32
    return _getcwd(out, (int)out_cap);
#else
    return getcwd(out, out_cap);
#endif
}

static int change_directory(const char *path) {
#ifdef _WIN32
    return _chdir(path);
#else
    return chdir(path);
#endif
}

static void test_executable_root_is_independent_of_process_cwd(void) {
    char saved_cwd[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char executable[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char directory[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char root_before[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char root_after[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    tp_error error = {{0}};

    TEST_ASSERT_NOT_NULL(current_directory(saved_cwd, sizeof saved_cwd));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_executable_path(executable, sizeof executable, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_executable_directory(directory, sizeof directory, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_root_from_executable(root_before, sizeof root_before,
                                       &error));

    const int changed = change_directory(g_scratch);
    const tp_status after_status =
        changed == 0
            ? tp_format_root_from_executable(root_after, sizeof root_after,
                                             &error)
            : TP_STATUS_PATH_RESOLVE_FAILED;
    const int restored = change_directory(saved_cwd);

    TEST_ASSERT_EQUAL_INT(0, changed);
    TEST_ASSERT_EQUAL_INT(0, restored);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, after_status);
    TEST_ASSERT_EQUAL_STRING(root_before, root_after);

    const size_t directory_length = strlen(directory);
    TEST_ASSERT_GREATER_THAN_size_t(directory_length, strlen(executable));
    TEST_ASSERT_EQUAL_MEMORY(directory, executable, directory_length);
    TEST_ASSERT_EQUAL_CHAR('/', executable[directory_length]);

    char expected_root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    TEST_ASSERT_TRUE(join_path(expected_root, sizeof expected_root, directory,
                               "formats"));
    TEST_ASSERT_EQUAL_STRING(expected_root, root_before);
}

static void test_executable_root_reports_bounded_output_failure(void) {
    char tiny[1] = {'x'};
    tp_error error = {{0}};

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_root_from_executable(tiny, sizeof tiny, &error));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);
    TEST_ASSERT_NOT_EQUAL('\0', error.msg[0]);
}

static void test_missing_root_finishes_as_native_only_catalog(void) {
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    TEST_ASSERT_TRUE(
        join_path(root, sizeof root, g_scratch, "missing-format-root"));
    tp_fs_remove_tree(root);
    (void)tp_fs_remove_file(root);

    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_catalog_scan_root(root, &scan, &failure, &error));
    TEST_ASSERT_NOT_NULL(scan);
    TEST_ASSERT_NULL(failure);
    TEST_ASSERT_EQUAL_size_t(0U, tp_format_catalog_scan_compile_count(scan));

    tp_format_catalog *catalog = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_catalog_scan_finish_without_compile(&scan, &catalog,
                                                      &error));
    TEST_ASSERT_NULL(scan);
    TEST_ASSERT_NOT_NULL(catalog);
    TEST_ASSERT_TRUE(tp_format_catalog_root_missing(catalog));
    TEST_ASSERT_FALSE(tp_format_catalog_limit_fail_closed(catalog));
    TEST_ASSERT_NULL(tp_format_catalog_root_diagnostics(catalog));
    TEST_ASSERT_EQUAL_STRING(root, tp_format_catalog_root(catalog));

    tp_format_catalog *native = tp_format_catalog_native();
    const size_t native_count = tp_format_catalog_row_count(native);
    TEST_ASSERT_EQUAL_size_t(native_count,
                             tp_format_catalog_row_count(catalog));
    if (native_count > 0U) {
        tp_format_catalog_row row;
        tp_format_resolution resolution;
        TEST_ASSERT_TRUE(tp_format_catalog_row_at(catalog, 0U, &row));
        TEST_ASSERT_EQUAL_INT(TP_FORMAT_IMPLEMENTATION_NATIVE,
                              row.implementation);
        TEST_ASSERT_TRUE(row.available);
        TEST_ASSERT_NOT_NULL(row.key);
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            tp_format_catalog_resolve(catalog, row.key, &resolution, &error));
        TEST_ASSERT_EQUAL_INT(TP_FORMAT_RESOLUTION_AVAILABLE,
                              resolution.state);
        TEST_ASSERT_EQUAL_INT(TP_FORMAT_IMPLEMENTATION_NATIVE,
                              resolution.implementation);
        TEST_ASSERT_NOT_NULL(resolution.descriptor);
    }
    tp_format_catalog_release(catalog);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_executable_root_is_independent_of_process_cwd);
    RUN_TEST(test_executable_root_reports_bounded_output_failure);
    RUN_TEST(test_missing_root_finishes_as_native_only_catalog);
    return UNITY_END();
}
