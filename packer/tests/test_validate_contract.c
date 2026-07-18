#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "tp_core/tp_id.h"
#include "tp_core/tp_validate.h"

#ifndef TP_CLI_TESTDATA_DIR
#error "TP_CLI_TESTDATA_DIR is required"
#endif
#ifndef TP_VALIDATE_TEST_DIR
#error "TP_VALIDATE_TEST_DIR is required"
#endif

typedef struct {
    const char *atlas;
    const char *atlas_id;
    const char *target;
    const char *target_id;
    const char *out_path;
} expected_finding;

void setUp(void) {}
void tearDown(void) {}

static tp_id128 parse_id(const char *text, tp_id_kind expected_kind) {
    tp_id_kind kind = TP_ID_KIND_INVALID;
    tp_id128 id = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_parse(text, &kind, &id, NULL));
    TEST_ASSERT_EQUAL_INT(expected_kind, kind);
    return id;
}

static void assert_empty_non_target_context(const tp_validation_finding *finding) {
    TEST_ASSERT_EQUAL_STRING("", finding->source);
    TEST_ASSERT_TRUE(tp_id128_is_nil(finding->source_id));
    TEST_ASSERT_EQUAL_STRING("", finding->sprite);
    TEST_ASSERT_EQUAL_STRING("", finding->anim);
    TEST_ASSERT_TRUE(tp_id128_is_nil(finding->animation_id));
    TEST_ASSERT_EQUAL_STRING("", finding->frame);
}

static void assert_duplicate(const tp_validation_finding *finding,
                             const expected_finding *expected) {
    char message[256];
    (void)snprintf(message, sizeof message,
                   "two or more targets export to '%s' (they overwrite each other)",
                   expected->out_path);

    TEST_ASSERT_EQUAL_INT(TP_VALIDATION_WARNING, finding->severity);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_DUPLICATE_OUT_PATH,
                             finding->code);
    TEST_ASSERT_EQUAL_STRING(message, finding->message);
    TEST_ASSERT_EQUAL_STRING(expected->atlas, finding->atlas);
    TEST_ASSERT_TRUE(tp_id128_eq(parse_id(expected->atlas_id, TP_ID_KIND_ATLAS),
                                finding->atlas_id));
    assert_empty_non_target_context(finding);
    TEST_ASSERT_EQUAL_STRING(expected->target, finding->target);
    TEST_ASSERT_TRUE(tp_id128_eq(parse_id(expected->target_id, TP_ID_KIND_TARGET),
                                finding->target_id));
}

void test_multi_atlas_duplicate_groups_have_exact_order_and_context(void) {
    static const expected_finding expected[] = {
        {"a1", "atlas_00000000000000000000000000000301", "json-neotolis",
         "target_00000000000000000000000000000303", "out/shared"},
        {"a1", "atlas_00000000000000000000000000000301", "defold",
         "target_00000000000000000000000000000304", "out/secondary"},
        {"a2", "atlas_00000000000000000000000000000311", "defold",
         "target_00000000000000000000000000000313", "out/shared"},
        {"a2", "atlas_00000000000000000000000000000311", "json-neotolis",
         "target_00000000000000000000000000000314", "out/secondary"},
        {"a3", "atlas_00000000000000000000000000000321", "json-neotolis",
         "target_00000000000000000000000000000323", "out/shared"},
    };
    const char *path = TP_CLI_TESTDATA_DIR "/dup_target.ntpacker_project";
    tp_validation_report report = {0};
    tp_error err = {0};

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_validate_project_file(path, &report, &err));
    TEST_ASSERT_EQUAL_size_t(5U, report.finding_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.error_count);
    TEST_ASSERT_EQUAL_size_t(5U, report.warning_count);
    TEST_ASSERT_EQUAL_size_t(5U, report.total_finding_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.omitted_finding_count);
    TEST_ASSERT_FALSE(report.truncated);

    for (size_t i = 0U; i < sizeof expected / sizeof expected[0]; ++i) {
        assert_duplicate(&report.findings[i], &expected[i]);
    }

    tp_validation_report_free(&report);
    TEST_ASSERT_NULL(report.findings);
}

void test_platform_absolute_source_report_is_exact_and_separate(void) {
#ifdef _WIN32
    const char *source_path = "C:/__ntpacker_validate_missing__/sprite.png";
#else
    const char *source_path = "/__ntpacker_validate_missing__/sprite.png";
#endif
    const char *project_path =
        TP_VALIDATE_TEST_DIR "/absolute-source.ntpacker_project";
    FILE *file = fopen(project_path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fprintf(
        file,
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000401\","
        "\"name\":\"absolute\",\"sources\":[{"
        "\"id\":\"source_00000000000000000000000000000402\","
        "\"path\":\"%s\"}]}]}\n",
        source_path) > 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    tp_validation_report report = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_validate_project_file(project_path, &report, &err));
    TEST_ASSERT_EQUAL_size_t(3U, report.finding_count);
    TEST_ASSERT_EQUAL_size_t(1U, report.error_count);
    TEST_ASSERT_EQUAL_size_t(2U, report.warning_count);
    TEST_ASSERT_EQUAL_size_t(3U, report.total_finding_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.omitted_finding_count);
    TEST_ASSERT_FALSE(report.truncated);

    char missing_message[256];
    char escape_message[320];
    (void)snprintf(missing_message, sizeof missing_message,
                   "source '%s' does not exist on disk", source_path);
    (void)snprintf(
        escape_message, sizeof escape_message,
        "source '%s' is absolute or escapes the project directory (not portable across machines)",
        source_path);
    const tp_id128 atlas_id = parse_id(
        "atlas_00000000000000000000000000000401", TP_ID_KIND_ATLAS);
    const tp_id128 source_id = parse_id(
        "source_00000000000000000000000000000402", TP_ID_KIND_SOURCE);

    TEST_ASSERT_EQUAL_INT(TP_VALIDATION_ERROR, report.findings[0].severity);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_MISSING_SOURCE,
                             report.findings[0].code);
    TEST_ASSERT_EQUAL_STRING(missing_message, report.findings[0].message);
    TEST_ASSERT_EQUAL_STRING("absolute", report.findings[0].atlas);
    TEST_ASSERT_TRUE(tp_id128_eq(atlas_id, report.findings[0].atlas_id));
    TEST_ASSERT_EQUAL_STRING(source_path, report.findings[0].source);
    TEST_ASSERT_TRUE(tp_id128_eq(source_id, report.findings[0].source_id));

    TEST_ASSERT_EQUAL_INT(TP_VALIDATION_WARNING, report.findings[1].severity);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_SOURCE_ESCAPES_ROOT,
                             report.findings[1].code);
    TEST_ASSERT_EQUAL_STRING(escape_message, report.findings[1].message);
    TEST_ASSERT_EQUAL_STRING("absolute", report.findings[1].atlas);
    TEST_ASSERT_TRUE(tp_id128_eq(atlas_id, report.findings[1].atlas_id));
    TEST_ASSERT_EQUAL_STRING(source_path, report.findings[1].source);
    TEST_ASSERT_TRUE(tp_id128_eq(source_id, report.findings[1].source_id));

    TEST_ASSERT_EQUAL_INT(TP_VALIDATION_WARNING, report.findings[2].severity);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_EMPTY_ATLAS,
                             report.findings[2].code);
    TEST_ASSERT_EQUAL_STRING(
        "atlas has no usable sprites (no images resolved from its sources)",
        report.findings[2].message);
    TEST_ASSERT_EQUAL_STRING("absolute", report.findings[2].atlas);
    TEST_ASSERT_TRUE(tp_id128_eq(atlas_id, report.findings[2].atlas_id));
    TEST_ASSERT_EQUAL_STRING("", report.findings[2].source);
    TEST_ASSERT_TRUE(tp_id128_is_nil(report.findings[2].source_id));

    for (size_t i = 0U; i < report.finding_count; ++i) {
        TEST_ASSERT_EQUAL_STRING("", report.findings[i].sprite);
        TEST_ASSERT_EQUAL_STRING("", report.findings[i].anim);
        TEST_ASSERT_TRUE(tp_id128_is_nil(report.findings[i].animation_id));
        TEST_ASSERT_EQUAL_STRING("", report.findings[i].frame);
        TEST_ASSERT_EQUAL_STRING("", report.findings[i].target);
        TEST_ASSERT_TRUE(tp_id128_is_nil(report.findings[i].target_id));
    }

    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(project_path));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_multi_atlas_duplicate_groups_have_exact_order_and_context);
    RUN_TEST(test_platform_absolute_source_report_is_exact_and_separate);
    return UNITY_END();
}
