#include "app_format_catalog.h"

#include "unity.h"

#ifndef APP_FORMAT_CATALOG_EMPTY_ROOT
#error "APP_FORMAT_CATALOG_EMPTY_ROOT is required"
#endif
#ifndef APP_FORMAT_CATALOG_MISSING_ROOT
#error "APP_FORMAT_CATALOG_MISSING_ROOT is required"
#endif
#ifndef APP_FORMAT_CATALOG_FILE_ROOT
#error "APP_FORMAT_CATALOG_FILE_ROOT is required"
#endif
#ifndef APP_FORMAT_CATALOG_FIXTURE_ROOT
#error "APP_FORMAT_CATALOG_FIXTURE_ROOT is required"
#endif

void setUp(void) {}
void tearDown(void) {}

static app_format_catalog open_root(const char *root) {
    app_format_catalog result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        app_format_catalog_open_startup_at_root(root, &result, &error));
    TEST_ASSERT_NOT_NULL(result.catalog);
    return result;
}

void test_empty_root_installs_active_native_only_generation(void) {
    app_format_catalog result = open_root(APP_FORMAT_CATALOG_EMPTY_ROOT);
    TEST_ASSERT_EQUAL_INT(APP_FORMAT_CATALOG_ACTIVE, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.reason_status);
    TEST_ASSERT_FALSE(tp_format_catalog_root_missing(result.catalog));
    TEST_ASSERT_EQUAL_size_t(
        tp_format_catalog_row_count(tp_format_catalog_native()),
        tp_format_catalog_row_count(result.catalog));
    app_format_catalog_close(&result);
    TEST_ASSERT_NULL(result.catalog);
}

void test_missing_root_installs_active_missing_generation(void) {
    app_format_catalog result = open_root(APP_FORMAT_CATALOG_MISSING_ROOT);
    TEST_ASSERT_EQUAL_INT(APP_FORMAT_CATALOG_ACTIVE, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.reason_status);
    TEST_ASSERT_TRUE(tp_format_catalog_root_missing(result.catalog));
    app_format_catalog_close(&result);
}

void test_non_directory_root_preserves_fallback_diagnostics(void) {
    app_format_catalog result = open_root(APP_FORMAT_CATALOG_FILE_ROOT);
    TEST_ASSERT_EQUAL_INT(APP_FORMAT_CATALOG_NATIVE_FALLBACK, result.state);
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, result.reason_status);
    TEST_ASSERT_EQUAL_PTR(tp_format_catalog_native(), result.catalog);
    TEST_ASSERT_NOT_NULL(result.failure_diagnostics);
    TEST_ASSERT_GREATER_THAN_size_t(
        0U, tp_format_diagnostic_report_count(result.failure_diagnostics));
    app_format_catalog_close(&result);
}

void test_compile_candidates_have_one_explicit_pending_state(void) {
    app_format_catalog result = open_root(APP_FORMAT_CATALOG_FIXTURE_ROOT);
    TEST_ASSERT_EQUAL_INT(APP_FORMAT_CATALOG_PENDING_COMPILER, result.state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNIMPLEMENTED, result.reason_status);
    TEST_ASSERT_EQUAL_PTR(tp_format_catalog_native(), result.catalog);
    TEST_ASSERT_NOT_EQUAL('\0', result.reason.msg[0]);
    app_format_catalog_close(&result);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_root_installs_active_native_only_generation);
    RUN_TEST(test_missing_root_installs_active_missing_generation);
    RUN_TEST(test_non_directory_root_preserves_fallback_diagnostics);
    RUN_TEST(test_compile_candidates_have_one_explicit_pending_state);
    return UNITY_END();
}
