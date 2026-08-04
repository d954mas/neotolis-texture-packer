#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_format.h"
#include "tp_format_catalog_internal.h"
#include "tp_format_compile_proto_internal.h"
#include "tp_format_compile_worker_internal.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_fs_internal.h"
#include "unity.h"

#ifndef TP_FORMAT_COMPILE_TEST_DIR
#error "TP_FORMAT_COMPILE_TEST_DIR is required"
#endif

#define TEST_PATH_CAP (TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U)
#define TEST_ACTION_ENV "TP_TEST_FORMAT_COMPILE_ACTION"
#define TEST_INDEX_ENV "TP_TEST_FORMAT_COMPILE_INDEX"

static bool set_env_value(const char *name, const char *value) {
#if defined(_WIN32)
    return _putenv_s(name, value ? value : "") == 0;
#else
    return value ? setenv(name, value, 1) == 0 : unsetenv(name) == 0;
#endif
}

static bool join_path(char *out, size_t capacity, const char *left,
                      const char *right) {
    const int written = snprintf(out, capacity, "%s/%s", left, right);
    return written >= 0 && (size_t)written < capacity;
}

static bool write_package(unsigned int index, const char *source) {
    char package_name[32];
    char format_id[32];
    char package_path[TEST_PATH_CAP];
    char descriptor_path[TEST_PATH_CAP];
    char source_path[TEST_PATH_CAP];
    char descriptor[2048];
    (void)snprintf(package_name, sizeof package_name, "package-%03u", index);
    (void)snprintf(format_id, sizeof format_id, "compile-%03u", index);
    if (!join_path(package_path, sizeof package_path,
                   TP_FORMAT_COMPILE_TEST_DIR, package_name) ||
        !join_path(descriptor_path, sizeof descriptor_path, package_path,
                   "format.json") ||
        !join_path(source_path, sizeof source_path, package_path,
                   "export.lua") ||
        !tp_fs_create_dir(package_path)) {
        return false;
    }
    const int length = snprintf(
        descriptor, sizeof descriptor,
        "{\n"
        "  \"api_version\": 1,\n"
        "  \"id\": \"%s\",\n"
        "  \"display_name\": \"Compile %03u\",\n"
        "  \"capabilities\": {\n"
        "    \"transforms\": [\"identity\"],\n"
        "    \"polygons\": false, \"pivot\": false,\n"
        "    \"slice9\": false, \"multipage\": false,\n"
        "    \"aliases\": false, \"animations\": false\n"
        "  },\n"
        "  \"outputs\": [{\"id\": \"metadata\", \"suffix\": \".txt\"}]\n"
        "}\n",
        format_id, index);
    return length > 0 && (size_t)length < sizeof descriptor &&
           tp_fs_write_file(descriptor_path, descriptor, (size_t)length) &&
           tp_fs_write_file(source_path, source, strlen(source));
}

static bool prepare_packages(size_t count, size_t invalid_index) {
    tp_fs_remove_tree(TP_FORMAT_COMPILE_TEST_DIR);
    if (!tp_fs_create_dir(TP_FORMAT_COMPILE_TEST_DIR)) {
        return false;
    }
    for (size_t i = 0U; i < count; ++i) {
        const char *source = i == invalid_index ? "return function("
                                                 : "return function() end\n";
        if (!write_package((unsigned int)i, source)) {
            return false;
        }
    }
    return true;
}

static tp_format_catalog_scan *scan_packages(size_t expected_count) {
    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog_scan_root(TP_FORMAT_COMPILE_TEST_DIR, &scan,
                                    &failure, &error),
        error.msg);
    TEST_ASSERT_NULL(failure);
    TEST_ASSERT_NOT_NULL(scan);
    TEST_ASSERT_EQUAL_size_t(expected_count,
                             tp_format_catalog_scan_compile_count(scan));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_COMPILE_BATCH_PENDING,
                          tp_format_catalog_scan_compile_state_internal(scan));
    return scan;
}

static tp_format_catalog *finish_scan(tp_format_catalog_scan **scan) {
    tp_format_catalog *catalog = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog_scan_finish_compiled_internal(scan, &catalog,
                                                        &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(catalog);
    return catalog;
}

static const tp_format_diagnostic *first_resolution_diagnostic(
    const tp_format_catalog *catalog, const char *id,
    tp_format_resolution_state expected_state) {
    tp_format_resolution resolution = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog_resolve(catalog, id, &resolution, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(expected_state, resolution.state);
    if (expected_state == TP_FORMAT_RESOLUTION_AVAILABLE) {
        TEST_ASSERT_NOT_NULL(resolution.descriptor);
        TEST_ASSERT_NULL(resolution.diagnostics);
        return NULL;
    }
    TEST_ASSERT_NOT_NULL(resolution.diagnostics);
    TEST_ASSERT_GREATER_THAN_size_t(
        0U, tp_format_diagnostic_report_count(resolution.diagnostics));
    return tp_format_diagnostic_report_at(resolution.diagnostics, 0U);
}

static tp_status run_with_options(tp_format_catalog_scan *scan,
                                  int timeout_ms, tp_error *error) {
    const tp_format_compile_worker_options options = {
        .worker_exe = NULL,
        .timeout_ms = timeout_ms,
    };
    return tp_format_compile_worker_run(scan, &options, error);
}

void setUp(void) {
    TEST_ASSERT_TRUE(set_env_value(TEST_ACTION_ENV, NULL));
    TEST_ASSERT_TRUE(set_env_value(TEST_INDEX_ENV, NULL));
    tp_fs_remove_tree(TP_FORMAT_COMPILE_TEST_DIR);
    TEST_ASSERT_TRUE(tp_fs_create_dir(TP_FORMAT_COMPILE_TEST_DIR));
}

void tearDown(void) {
    (void)set_env_value(TEST_ACTION_ENV, NULL);
    (void)set_env_value(TEST_INDEX_ENV, NULL);
    tp_fs_remove_tree(TP_FORMAT_COMPILE_TEST_DIR);
}

static void write_u16le(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void write_u32le(uint8_t *bytes, uint32_t value) {
    for (unsigned int i = 0U; i < 4U; ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
}

void test_compile_protocol_round_trips_candidate_and_diagnostics(void) {
    static const unsigned char descriptor[] = "{}";
    static const unsigned char source[] = "return function() end";
    const tp_format_compile_proto_request request = {
        .candidate_index = 7U,
        .format_id = "compile-roundtrip",
        .package_path = "formats/compile-roundtrip/export.lua",
        .descriptor_bytes = descriptor,
        .descriptor_byte_count = sizeof descriptor - 1U,
        .source_bytes = source,
        .source_byte_count = sizeof source - 1U,
    };
    uint8_t *encoded = NULL;
    size_t encoded_length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_compile_proto_encode_request(
            &request, &encoded, &encoded_length, &error),
        error.msg);
    tp_format_compile_proto_message decoded = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_compile_proto_decode_request_message(
            encoded, encoded_length, &decoded, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_COMPILE_PROTO_REQUEST, decoded.kind);
    TEST_ASSERT_EQUAL_UINT32(7U, decoded.request.candidate_index);
    TEST_ASSERT_EQUAL_STRING(request.format_id, decoded.request.format_id);
    TEST_ASSERT_EQUAL_STRING(request.package_path,
                             decoded.request.package_path);
    TEST_ASSERT_EQUAL_size_t(request.descriptor_byte_count,
                             decoded.request.descriptor_byte_count);
    TEST_ASSERT_EQUAL_MEMORY(request.source_bytes, decoded.request.source_bytes,
                             request.source_byte_count);
    tp_format_compile_proto_message_free(&decoded);
    free(encoded);

    tp_format_diagnostic_report *report = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_diagnostic_report_create_internal(&report, &error));
    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
        .phase = TP_FORMAT_PHASE_COMPILE,
        .format_id = request.format_id,
        .package_path = request.package_path,
        .line = 3U,
        .column = 4U,
        .message = "syntax error",
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_diagnostic_report_append_internal(report, &diagnostic,
                                                    &error));
    const tp_format_compile_proto_result result = {
        .candidate_index = 7U,
        .status = TP_STATUS_INVALID_ARGUMENT,
        .available = false,
        .diagnostics = report,
    };
    encoded = NULL;
    encoded_length = 0U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_compile_proto_encode_result(
            &result, &encoded, &encoded_length, &error),
        error.msg);
    memset(&decoded, 0, sizeof decoded);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_compile_proto_decode_response_message(
            encoded, encoded_length, &decoded, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_COMPILE_PROTO_RESULT, decoded.kind);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, decoded.result.status);
    TEST_ASSERT_FALSE(decoded.result.available);
    TEST_ASSERT_EQUAL_size_t(
        1U, tp_format_diagnostic_report_count(decoded.result.diagnostics));
    const tp_format_diagnostic *decoded_diagnostic =
        tp_format_diagnostic_report_at(decoded.result.diagnostics, 0U);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
                          decoded_diagnostic->code);
    TEST_ASSERT_EQUAL_UINT32(3U, decoded_diagnostic->line);
    TEST_ASSERT_EQUAL_STRING("syntax error", decoded_diagnostic->message);
    tp_format_compile_proto_message_free(&decoded);
    tp_format_diagnostic_report_destroy(report);
    free(encoded);
}

void test_compile_protocol_rejects_bad_headers_caps_and_trailing_bytes(void) {
    uint8_t header[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES] = {0};
    write_u32le(header, TP_FORMAT_COMPILE_PROTO_REQUEST_MAGIC);
    write_u16le(header + 4U, TP_FORMAT_COMPILE_PROTO_VERSION);
    write_u16le(header + 6U, TP_FORMAT_COMPILE_PROTO_REQUEST);
    write_u32le(header + 8U,
                TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_PAYLOAD_BYTES + 1U);
    size_t frame_size = 0U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_proto_frame_size(header, true, &frame_size, NULL));
    write_u32le(header + 8U, TP_FORMAT_COMPILE_PROTO_REQUEST_FIXED_BYTES);
    header[4] = (uint8_t)(TP_FORMAT_COMPILE_PROTO_VERSION + 1U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_BAD_VERSION,
        tp_format_compile_proto_frame_size(header, true, &frame_size, NULL));

    uint8_t *complete = NULL;
    size_t complete_length = 0U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_format_compile_proto_encode_complete(
                          &complete, &complete_length, NULL));
    uint8_t with_trailing[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES + 1U];
    memcpy(with_trailing, complete, complete_length);
    with_trailing[complete_length] = 0x7fU;
    tp_format_compile_proto_message message = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_proto_decode_response_message(
            with_trailing, sizeof with_trailing, &message, NULL));
    free(complete);

    TEST_ASSERT_EQUAL_UINT32(1024U, TP_FORMAT_COMPILE_PROTO_MAX_FRAMES);
    TEST_ASSERT_EQUAL_UINT32(83886080U,
                             TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_STREAM_BYTES);
    TEST_ASSERT_EQUAL_UINT32(16777216U,
                             TP_FORMAT_COMPILE_PROTO_MAX_RESPONSE_STREAM_BYTES);
    TEST_ASSERT_EQUAL_UINT32(65U, TP_FORMAT_COMPILE_PROTO_MAX_PROCESSES);
}

void test_compile_attempt_global_budgets_accept_boundary_and_reject_plus_one(void) {
    tp_error error = {{0}};
    tp_format_compile_worker_test_budget budget = {
        .frame_count = TP_FORMAT_COMPILE_PROTO_MAX_FRAMES - 1U,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_compile_worker__test_reserve_request(&budget, 0U, 0U,
                                                       &error));
    TEST_ASSERT_EQUAL_size_t(TP_FORMAT_COMPILE_PROTO_MAX_FRAMES,
                             budget.frame_count);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_worker__test_reserve_request(&budget, 0U, 0U,
                                                       &error));

    budget = (tp_format_compile_worker_test_budget){
        .request_bytes =
            TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_STREAM_BYTES - 1U,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_compile_worker__test_reserve_request(&budget, 1U, 0U,
                                                       &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_worker__test_reserve_request(&budget, 1U, 0U,
                                                       &error));

    budget = (tp_format_compile_worker_test_budget){
        .source_bytes = TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX - 1U,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_compile_worker__test_reserve_request(&budget, 0U, 1U,
                                                       &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_worker__test_reserve_request(&budget, 0U, 1U,
                                                       &error));

    budget = (tp_format_compile_worker_test_budget){
        .response_bytes =
            TP_FORMAT_COMPILE_PROTO_MAX_RESPONSE_STREAM_BYTES - 1U,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_compile_worker__test_charge_response_bytes(&budget, 1U,
                                                             &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_worker__test_charge_response_bytes(&budget, 1U,
                                                             &error));

    budget = (tp_format_compile_worker_test_budget){
        .frame_count = TP_FORMAT_COMPILE_PROTO_MAX_FRAMES - 1U,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_compile_worker__test_reserve_response_frame(&budget,
                                                              &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_worker__test_reserve_response_frame(&budget,
                                                              &error));

    budget = (tp_format_compile_worker_test_budget){
        .request_bytes = SIZE_MAX,
        .response_bytes = SIZE_MAX,
        .source_bytes = SIZE_MAX,
    };
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_worker__test_reserve_request(&budget, SIZE_MAX,
                                                       SIZE_MAX, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_compile_worker__test_charge_response_bytes(&budget,
                                                             SIZE_MAX,
                                                             &error));
}

void test_compile_worker_commits_only_after_complete_batch(void) {
    TEST_ASSERT_TRUE(prepare_packages(2U, 1U));
    tp_format_catalog_scan *scan = scan_packages(2U);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, run_with_options(scan, 5000, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_COMPILE_BATCH_COMPLETE,
                          tp_format_catalog_scan_compile_state_internal(scan));
    tp_format_catalog *catalog = finish_scan(&scan);
    (void)first_resolution_diagnostic(
        catalog, "compile-000", TP_FORMAT_RESOLUTION_AVAILABLE);
    const tp_format_diagnostic *diagnostic = first_resolution_diagnostic(
        catalog, "compile-001", TP_FORMAT_RESOLUTION_UNAVAILABLE);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
                          diagnostic->code);
    tp_format_catalog_release(catalog);
}

void test_empty_source_is_a_valid_text_chunk_at_compile_admission(void) {
    tp_fs_remove_tree(TP_FORMAT_COMPILE_TEST_DIR);
    TEST_ASSERT_TRUE(tp_fs_create_dir(TP_FORMAT_COMPILE_TEST_DIR));
    TEST_ASSERT_TRUE(write_package(0U, ""));
    tp_format_catalog_scan *scan = scan_packages(1U);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, run_with_options(scan, 5000, &error), error.msg);
    tp_format_catalog *catalog = finish_scan(&scan);
    (void)first_resolution_diagnostic(
        catalog, "compile-000", TP_FORMAT_RESOLUTION_AVAILABLE);
    tp_format_catalog_release(catalog);
}

void test_announced_crash_is_row_local_and_later_row_compiles(void) {
    TEST_ASSERT_TRUE(prepare_packages(2U, SIZE_MAX));
    TEST_ASSERT_TRUE(set_env_value(TEST_ACTION_ENV, "crash_after_announce"));
    TEST_ASSERT_TRUE(set_env_value(TEST_INDEX_ENV, "0"));
    tp_format_catalog_scan *scan = scan_packages(2U);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, run_with_options(scan, 5000, &error), error.msg);
    tp_format_catalog *catalog = finish_scan(&scan);
    const tp_format_diagnostic *diagnostic = first_resolution_diagnostic(
        catalog, "compile-000", TP_FORMAT_RESOLUTION_UNAVAILABLE);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_COMPILE_WORKER_FAILED,
                          diagnostic->code);
    (void)first_resolution_diagnostic(
        catalog, "compile-001", TP_FORMAT_RESOLUTION_AVAILABLE);
    tp_format_catalog_release(catalog);
}

void test_announced_timeout_is_row_local(void) {
    TEST_ASSERT_TRUE(prepare_packages(2U, SIZE_MAX));
    TEST_ASSERT_TRUE(set_env_value(TEST_ACTION_ENV, "hang_after_announce"));
    TEST_ASSERT_TRUE(set_env_value(TEST_INDEX_ENV, "0"));
    tp_format_catalog_scan *scan = scan_packages(2U);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, run_with_options(scan, 75, &error), error.msg);
    tp_format_catalog *catalog = finish_scan(&scan);
    const tp_format_diagnostic *diagnostic = first_resolution_diagnostic(
        catalog, "compile-000", TP_FORMAT_RESOLUTION_UNAVAILABLE);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_COMPILE_WORKER_FAILED,
                          diagnostic->code);
    (void)first_resolution_diagnostic(
        catalog, "compile-001", TP_FORMAT_RESOLUTION_AVAILABLE);
    tp_format_catalog_release(catalog);
}

static void assert_action_invalidates(const char *action, size_t count,
                                      const char *index_text) {
    TEST_ASSERT_TRUE(prepare_packages(count, SIZE_MAX));
    TEST_ASSERT_TRUE(set_env_value(TEST_ACTION_ENV, action));
    TEST_ASSERT_TRUE(set_env_value(TEST_INDEX_ENV, index_text));
    tp_format_catalog_scan *scan = scan_packages(count);
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK,
                          run_with_options(scan, 1000, &error));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_COMPILE_BATCH_INELIGIBLE,
                          tp_format_catalog_scan_compile_state_internal(scan));
    tp_format_catalog_scan_destroy(scan);
    TEST_ASSERT_TRUE(set_env_value(TEST_ACTION_ENV, NULL));
    TEST_ASSERT_TRUE(set_env_value(TEST_INDEX_ENV, NULL));
}

void test_unannounced_and_unattributed_exits_invalidate_batch(void) {
    assert_action_invalidates("crash_before_announce", 1U, "0");
    assert_action_invalidates("clean_exit_after_announce", 1U, "0");
    assert_action_invalidates("crash_after_result", 2U, "0");
    assert_action_invalidates("clean_exit_after_result", 2U, "0");
    assert_action_invalidates("hang_after_result", 2U, "0");
}

void test_unannounced_worker_oom_is_a_global_oom(void) {
    TEST_ASSERT_TRUE(prepare_packages(1U, SIZE_MAX));
    TEST_ASSERT_TRUE(set_env_value(TEST_ACTION_ENV,
                                   "global_oom_before_announce"));
    TEST_ASSERT_TRUE(set_env_value(TEST_INDEX_ENV, "0"));
    tp_format_catalog_scan *scan = scan_packages(1U);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM,
                          run_with_options(scan, 1000, &error));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_COMPILE_BATCH_INELIGIBLE,
                          tp_format_catalog_scan_compile_state_internal(scan));
    tp_format_catalog_scan_destroy(scan);
    TEST_ASSERT_TRUE(set_env_value(TEST_ACTION_ENV, NULL));
    TEST_ASSERT_TRUE(set_env_value(TEST_INDEX_ENV, NULL));
}

void test_malformed_wrong_duplicate_and_partial_frames_invalidate_batch(void) {
    assert_action_invalidates("wrong_announce", 1U, "0");
    assert_action_invalidates("wrong_result", 1U, "0");
    assert_action_invalidates("duplicate_announce", 1U, "0");
    assert_action_invalidates("duplicate_result", 2U, "0");
    assert_action_invalidates("malformed_after_announce", 1U, "0");
    assert_action_invalidates("partial_after_announce", 1U, "0");
}

void test_missing_complete_and_trailing_response_invalidate_batch(void) {
    assert_action_invalidates("missing_complete", 1U, NULL);
    assert_action_invalidates("trailing_after_complete", 1U, NULL);
}

void test_restart_process_budget_accepts_boundary_and_rejects_plus_one(void) {
    TEST_ASSERT_TRUE(prepare_packages(64U, SIZE_MAX));
    TEST_ASSERT_TRUE(set_env_value(TEST_ACTION_ENV, "crash_after_announce"));
    tp_format_catalog_scan *scan = scan_packages(64U);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, run_with_options(scan, 5000, &error), error.msg);
    tp_format_catalog *catalog = finish_scan(&scan);
    const tp_format_diagnostic *diagnostic = first_resolution_diagnostic(
        catalog, "compile-063", TP_FORMAT_RESOLUTION_UNAVAILABLE);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_COMPILE_WORKER_FAILED,
                          diagnostic->code);
    tp_format_catalog_release(catalog);

    TEST_ASSERT_TRUE(prepare_packages(65U, SIZE_MAX));
    scan = scan_packages(65U);
    memset(&error, 0, sizeof error);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          run_with_options(scan, 5000, &error));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_COMPILE_BATCH_INELIGIBLE,
                          tp_format_catalog_scan_compile_state_internal(scan));
    tp_format_catalog_scan_destroy(scan);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_compile_protocol_round_trips_candidate_and_diagnostics);
    RUN_TEST(test_compile_protocol_rejects_bad_headers_caps_and_trailing_bytes);
    RUN_TEST(
        test_compile_attempt_global_budgets_accept_boundary_and_reject_plus_one);
    RUN_TEST(test_compile_worker_commits_only_after_complete_batch);
    RUN_TEST(test_empty_source_is_a_valid_text_chunk_at_compile_admission);
    RUN_TEST(test_announced_crash_is_row_local_and_later_row_compiles);
    RUN_TEST(test_announced_timeout_is_row_local);
    RUN_TEST(test_unannounced_and_unattributed_exits_invalidate_batch);
    RUN_TEST(test_unannounced_worker_oom_is_a_global_oom);
    RUN_TEST(test_malformed_wrong_duplicate_and_partial_frames_invalidate_batch);
    RUN_TEST(test_missing_complete_and_trailing_response_invalidate_batch);
    RUN_TEST(
        test_restart_process_budget_accepts_boundary_and_rejects_plus_one);
    return UNITY_END();
}
