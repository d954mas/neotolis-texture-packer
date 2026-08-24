#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "tp_core/tp_export.h"
#include "tp_core/tp_format.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_core/tp_validate.h"
#include "tp_export_internal.h"
#include "tp_format_binding_proto_internal.h"
#include "tp_format_catalog_internal.h"
#include "tp_format_diagnostic_internal.h"

typedef struct deterministic_rng_state {
    uint8_t next;
} deterministic_rng_state;

static int deterministic_fill(void *context, uint8_t *out, size_t length) {
    deterministic_rng_state *state =
        (deterministic_rng_state *)context;
    for (size_t i = 0U; i < length; ++i) {
        out[i] = state->next++;
    }
    return (int)length;
}

static tp_status inert_serialize(const tp_export_serialize_ctx *context,
                                 tp_export_document *documents,
                                 int document_count, tp_error *error) {
    (void)context;
    (void)documents;
    (void)document_count;
    (void)error;
    return TP_STATUS_OK;
}

static const tp_format_descriptor k_format_a = {
    .id = "catalog-context-a",
    .display_name = "Catalog Context A",
    .caps = {.transform_mask = TP_EXPORT_TRANSFORMS_ALL,
             .polygons = true,
             .pivot = true,
             .slice9 = true,
             .multipage = true,
             .aliases = true,
             .animations = true},
};

static const tp_format_descriptor k_format_b = {
    .id = "catalog-context-b",
    .display_name = "Catalog Context B",
    .caps = {.transform_mask = TP_EXPORT_TRANSFORMS_ALL,
             .polygons = true,
             .pivot = true,
             .slice9 = true,
             .multipage = true,
             .aliases = true,
             .animations = true},
};

static const tp_exporter k_exporter_a = {
    .format = &k_format_a,
    .serialize = inert_serialize,
};

static const tp_exporter k_exporter_b = {
    .format = &k_format_b,
    .serialize = inert_serialize,
};

static tp_id128 id_with_byte(uint8_t value) {
    tp_id128 id;
    memset(id.bytes, value, sizeof id.bytes);
    return id;
}

static tp_id128 only_atlas_id(const tp_session *session) {
    tp_session_snapshot *snapshot = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &error), error.msg);
    TEST_ASSERT_NOT_NULL(snapshot);
    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(snapshot));
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 id = atlas->id;
    tp_session_snapshot_destroy(snapshot);
    return id;
}

static tp_status create_target(tp_session *session, tp_id128 atlas_id,
                               tp_id128 target_id, const char *format_id,
                               const char *out_path, const char *transaction_id,
                               tp_txn_result *result, tp_error *error) {
    tp_operation operation = {0};
    operation.kind = TP_OP_TARGET_CREATE;
    operation.atlas_id = atlas_id;
    operation.u.target_create.target_id = target_id;
    operation.u.target_create.exporter_id = (char *)format_id;
    operation.u.target_create.out_path = (char *)out_path;
    operation.u.target_create.enabled = true;

    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    (void)memcpy(request.id_hex, transaction_id,
                 sizeof request.id_hex);
    request.expected_revision = tp_session_revision(session);
    request.ops = &operation;
    request.op_count = 1;
    return tp_session_apply(session, &request, result, error);
}

static bool validation_has_code(const tp_validation_report *report,
                                const char *code) {
    for (size_t i = 0U; i < report->finding_count; ++i) {
        if (strcmp(report->findings[i].code, code) == 0) {
            return true;
        }
    }
    return false;
}

static void assert_preview_admission(tp_session *session,
                                     const char *format_id,
                                     tp_status expected_status,
                                     const char *expected_message) {
    const tp_pack_job_request request = {
        .atlas_id = id_with_byte(0xffU),
        .work_dir = "unused-catalog-context-work",
        .preview_exporter_id = format_id,
    };
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        expected_status,
        tp_session_pack_job_start(session, &request, &error));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, expected_message));
    TEST_ASSERT_FALSE(tp_session_job_active(session));
}

void setUp(void) {}

void tearDown(void) {}

void test_descriptor_id_wins_over_another_package_key(void) {
    const tp_exporter *const exporters[] = {&k_exporter_a, &k_exporter_b};
    const char *const keys[] = {k_format_b.id, "package-b"};
    tp_format_catalog *catalog = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog__test_create_with_keys(
            exporters, keys, 2U, &catalog, &error),
        error.msg);

    TEST_ASSERT_EQUAL_PTR(
        &k_exporter_b,
        tp_format_catalog_exporter_find(catalog, k_format_b.id));
    TEST_ASSERT_EQUAL_PTR(
        &k_exporter_a,
        tp_format_catalog_exporter_find(catalog, k_format_a.id));
    TEST_ASSERT_NULL(tp_format_catalog_exporter_find(catalog, "package-b"));
    tp_format_resolution resolution = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_catalog_resolve(
            catalog, "package-b", &resolution, &error));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_RESOLUTION_ABSENT, resolution.state);
    tp_format_catalog_release(catalog);
}

void test_unavailable_package_binding_keeps_no_binding_sentinel(void) {
    tp_format_catalog_owned_row *rows = calloc(1U, sizeof *rows);
    TEST_ASSERT_NOT_NULL(rows);
    static const char key[] = "broken-package";
    rows[0].key = malloc(sizeof key);
    TEST_ASSERT_NOT_NULL(rows[0].key);
    memcpy(rows[0].key, key, sizeof key);
    rows[0].implementation = TP_FORMAT_IMPLEMENTATION_LUA;
    rows[0].available = false;

    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_diagnostic_report_create_internal(
            &rows[0].diagnostics, &error), error.msg);
    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
        .phase = TP_FORMAT_PHASE_COMPILE,
        .format_id = key,
        .package_path = "formats/broken-package",
        .message = "package compile failed",
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_diagnostic_report_append_internal(
            rows[0].diagnostics, &diagnostic, &error), error.msg);
    tp_format_catalog *catalog = tp_format_catalog_create_owned_internal(
        NULL, rows, 1U, NULL, false, false, &error);
    TEST_ASSERT_NOT_NULL_MESSAGE(catalog, error.msg);
    const tp_format_binding_capture_target target = {
        .atlas_id = id_with_byte(0x31U),
        .target_id = id_with_byte(0x42U),
        .format_id = key,
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog_encode_bindings_internal(
            catalog, NULL, &target, 1U, &bytes, &length, &error),
        error.msg);
    tp_format_binding_proto_value decoded = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(0U, decoded.binding_count);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE,
                          decoded.targets[0].resolution.kind);
    TEST_ASSERT_EQUAL_UINT32(
        UINT32_MAX, decoded.targets[0].resolution.binding_index);
    tp_format_binding_proto_value_free(&decoded);
    free(bytes);
    tp_format_catalog_release(catalog);
}

void test_catalog_generation_is_threaded_through_sessions_and_snapshots(void) {
    const tp_exporter *const exporters_a[] = {&k_exporter_a};
    const tp_exporter *const exporters_b[] = {&k_exporter_b};
    tp_format_catalog *catalog_a = NULL;
    tp_format_catalog *catalog_b = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog__test_create(exporters_a, 1U, &catalog_a, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog__test_create(exporters_b, 1U, &catalog_b, &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(catalog_a);
    TEST_ASSERT_NOT_NULL(catalog_b);
    TEST_ASSERT_TRUE(catalog_a != catalog_b);
    TEST_ASSERT_NOT_NULL(
        tp_format_catalog_find_available(catalog_a, k_format_a.id));
    TEST_ASSERT_NULL(
        tp_format_catalog_find_available(catalog_a, k_format_b.id));
    TEST_ASSERT_NOT_NULL(
        tp_format_catalog_find_available(catalog_b, k_format_b.id));
    TEST_ASSERT_NULL(
        tp_format_catalog_find_available(catalog_b, k_format_a.id));

    deterministic_rng_state state_a = {.next = 1U};
    deterministic_rng_state state_b = {.next = 101U};
    const tp_rng rng_a = {.fill = deterministic_fill, .ctx = &state_a};
    const tp_rng rng_b = {.fill = deterministic_fill, .ctx = &state_b};
    tp_session *session_a = NULL;
    tp_session *session_b = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_create_with_catalog(catalog_a, &rng_a, &session_a,
                                       &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_create_with_catalog(catalog_b, &rng_b, &session_b,
                                       &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(session_a);
    TEST_ASSERT_NOT_NULL(session_b);
    TEST_ASSERT_TRUE(tp_session_format_catalog(session_a) == catalog_a);
    TEST_ASSERT_TRUE(tp_session_format_catalog(session_b) == catalog_b);

    /* Sessions retain their immutable catalog generation. */
    tp_format_catalog_release(catalog_a);
    tp_format_catalog_release(catalog_b);

    const tp_id128 atlas_a = only_atlas_id(session_a);
    const tp_id128 atlas_b = only_atlas_id(session_b);
    const tp_id128 target_a = id_with_byte(0xa1U);
    const tp_id128 target_b = id_with_byte(0xb2U);

    tp_txn_result result = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        create_target(session_a, atlas_a, target_a, k_format_a.id,
                      "out/context-a", "11111111111111111111111111111111",
                      &result, &error),
        error.msg);
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);

    const int64_t revision_b_before = tp_session_revision(session_b);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_NOT_FOUND,
        create_target(session_b, atlas_b, id_with_byte(0xc3U), k_format_a.id,
                      "out/context-cross-a",
                      "22222222222222222222222222222222", &result,
                      &error));
    TEST_ASSERT_FALSE(result.committed);
    TEST_ASSERT_EQUAL_INT64(revision_b_before,
                            tp_session_revision(session_b));
    TEST_ASSERT_EQUAL_INT(1, result.error_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, result.errors[0].code);
    tp_txn_result_free(&result);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        create_target(session_b, atlas_b, target_b, k_format_b.id,
                      "out/context-b", "33333333333333333333333333333333",
                      &result, &error),
        error.msg);
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);

    const int64_t revision_a_before = tp_session_revision(session_a);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_NOT_FOUND,
        create_target(session_a, atlas_a, id_with_byte(0xd4U), k_format_b.id,
                      "out/context-cross-b",
                      "44444444444444444444444444444444", &result,
                      &error));
    TEST_ASSERT_FALSE(result.committed);
    TEST_ASSERT_EQUAL_INT64(revision_a_before,
                            tp_session_revision(session_a));
    TEST_ASSERT_EQUAL_INT(1, result.error_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, result.errors[0].code);
    tp_txn_result_free(&result);

    assert_preview_admission(session_a, k_format_a.id, TP_STATUS_NOT_FOUND,
                             "Pack job atlas was not found");
    assert_preview_admission(session_a, k_format_b.id, TP_STATUS_NOT_FOUND,
                             "unknown preview exporter");
    assert_preview_admission(session_b, k_format_b.id, TP_STATUS_NOT_FOUND,
                             "Pack job atlas was not found");
    assert_preview_admission(session_b, k_format_a.id, TP_STATUS_NOT_FOUND,
                             "unknown preview exporter");

    tp_session_snapshot *snapshot_a = NULL;
    tp_session_snapshot *snapshot_b = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_snapshot_create(session_a, &snapshot_a, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_snapshot_create(session_b, &snapshot_b, &error),
        error.msg);
    tp_format_catalog *snapshot_catalog_a =
        tp_session_snapshot_format_catalog(snapshot_a);
    tp_format_catalog *snapshot_catalog_b =
        tp_session_snapshot_format_catalog(snapshot_b);
    TEST_ASSERT_TRUE(snapshot_catalog_a ==
                     tp_session_format_catalog(session_a));
    TEST_ASSERT_TRUE(snapshot_catalog_b ==
                     tp_session_format_catalog(session_b));
    TEST_ASSERT_TRUE(snapshot_catalog_a != snapshot_catalog_b);

    /* Snapshots retain the same generations after their sessions are gone. */
    tp_session_destroy(session_a);
    tp_session_destroy(session_b);
    TEST_ASSERT_NOT_NULL(tp_format_catalog_find_available(snapshot_catalog_a,
                                                          k_format_a.id));
    TEST_ASSERT_NULL(tp_format_catalog_find_available(snapshot_catalog_a,
                                                      k_format_b.id));
    TEST_ASSERT_NOT_NULL(tp_format_catalog_find_available(snapshot_catalog_b,
                                                          k_format_b.id));
    TEST_ASSERT_NULL(tp_format_catalog_find_available(snapshot_catalog_b,
                                                      k_format_a.id));

    tp_validation_report validation = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_validate_session_snapshot(snapshot_a, &validation, &error),
        error.msg);
    TEST_ASSERT_FALSE(validation_has_code(
        &validation, TP_VALIDATION_CODE_UNKNOWN_EXPORTER));
    tp_validation_report_free(&validation);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_validate_session_snapshot(snapshot_b, &validation, &error),
        error.msg);
    TEST_ASSERT_FALSE(validation_has_code(
        &validation, TP_VALIDATION_CODE_UNKNOWN_EXPORTER));
    tp_validation_report_free(&validation);

    tp_target_validation_report target_validation = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_validate_session_snapshot_target(snapshot_a, atlas_a, target_a,
                                            &target_validation, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(0U, target_validation.issue_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_validate_session_snapshot_target(snapshot_b, atlas_b, target_b,
                                            &target_validation, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(0U, target_validation.issue_count);

    tp_session_snapshot_destroy(snapshot_a);
    tp_session_snapshot_destroy(snapshot_b);

    /* Catalog-local test rows never mutate the process-global native catalog. */
    TEST_ASSERT_NULL(tp_format_catalog_find_available(
        tp_format_catalog_native(), k_format_a.id));
    TEST_ASSERT_NULL(tp_format_catalog_find_available(
        tp_format_catalog_native(), k_format_b.id));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_descriptor_id_wins_over_another_package_key);
    RUN_TEST(test_unavailable_package_binding_keeps_no_binding_sentinel);
    RUN_TEST(
        test_catalog_generation_is_threaded_through_sessions_and_snapshots);
    return UNITY_END();
}
