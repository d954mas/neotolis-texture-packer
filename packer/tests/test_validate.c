#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "tp_core/tp_validate.h"
#include "tp_core/tp_project.h"
#include "tp_project_mutation_internal.h"
#include "tp_core/tp_project_migrate.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_validate_internal.h"

#ifndef TP_CLI_TESTDATA_DIR
#error "TP_CLI_TESTDATA_DIR is required"
#endif
#ifndef TP_VALIDATE_TEST_DIR
#error "TP_VALIDATE_TEST_DIR is required"
#endif

static void fixture_path(char *out, size_t cap, const char *name) {
    (void)snprintf(out, cap, "%s/%s", TP_CLI_TESTDATA_DIR, name);
}

void setUp(void) {
    tp_validate__test_set_alloc_fail(-1);
    tp_validate__test_fail_sprite_index(false);
}
void tearDown(void) {
    tp_validate__test_set_alloc_fail(-1);
    tp_validate__test_fail_sprite_index(false);
}

void test_clean_file_returns_owned_empty_report(void) {
    char path[512];
    fixture_path(path, sizeof path, "clean.ntpacker_project");
    tp_validation_report report = {0};
    tp_error err = {0};

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_validate_project_file(path, &report, &err));
    TEST_ASSERT_EQUAL_size_t(0U, report.finding_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.error_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.warning_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.total_finding_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.omitted_finding_count);
    TEST_ASSERT_FALSE(report.truncated);
    tp_validation_report_free(&report);
    TEST_ASSERT_NULL(report.findings);
}

void test_problem_file_report_is_exact_and_stably_ordered(void) {
    char path[512];
    fixture_path(path, sizeof path, "problems.ntpacker_project");
    tp_validation_report report = {0};
    tp_error err = {0};

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_validate_project_file(path, &report, &err));
    TEST_ASSERT_EQUAL_size_t(2U, report.finding_count);
    TEST_ASSERT_EQUAL_size_t(2U, report.error_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.warning_count);
    TEST_ASSERT_EQUAL_size_t(2U, report.total_finding_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.omitted_finding_count);
    TEST_ASSERT_FALSE(report.truncated);

    TEST_ASSERT_EQUAL_INT(TP_VALIDATION_ERROR, report.findings[0].severity);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_MISSING_SOURCE, report.findings[0].code);
    TEST_ASSERT_EQUAL_STRING("source 'does_not_exist' does not exist on disk",
                             report.findings[0].message);
    TEST_ASSERT_EQUAL_STRING("problems", report.findings[0].atlas);
    TEST_ASSERT_EQUAL_STRING("", report.findings[0].sprite);
    TEST_ASSERT_EQUAL_STRING("", report.findings[0].anim);
    TEST_ASSERT_EQUAL_STRING("", report.findings[0].frame);
    TEST_ASSERT_EQUAL_STRING("", report.findings[0].target);

    TEST_ASSERT_EQUAL_INT(TP_VALIDATION_ERROR, report.findings[1].severity);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_DANGLING_ANIM_FRAME, report.findings[1].code);
    TEST_ASSERT_EQUAL_STRING(
        "animation 'run' references frame 'ghost' which matches no sprite export key",
        report.findings[1].message);
    TEST_ASSERT_EQUAL_STRING("problems", report.findings[1].atlas);
    TEST_ASSERT_EQUAL_STRING("", report.findings[1].sprite);
    TEST_ASSERT_EQUAL_STRING("run", report.findings[1].anim);
    TEST_ASSERT_EQUAL_STRING("ghost", report.findings[1].frame);
    TEST_ASSERT_EQUAL_STRING("", report.findings[1].target);
    tp_validation_report_free(&report);
}

static void write_many_empty_atlases(const char *path, size_t count) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs("{\"version\":1,\"atlases\":[", f) >= 0);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(fprintf(f, "%s{\"name\":\"atlas_%zu\",\"max_size\":0}", i == 0U ? "" : ",", i) > 0);
    }
    TEST_ASSERT_TRUE(fputs("]}\n", f) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

static void write_v3_sources(const char *path, const char *first,
                             const char *second) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs(
        "{\"version\":3,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"source-keys\",\"sources\":[{"
        "\"id\":\"source_00000000000000000000000000000010\","
        "\"path\":\"", f) >= 0);
    TEST_ASSERT_TRUE(fputs(first, f) >= 0);
    TEST_ASSERT_TRUE(fputs("\"},{\"id\":\"source_00000000000000000000000000000011\","
                           "\"path\":\"", f) >= 0);
    TEST_ASSERT_TRUE(fputs(second, f) >= 0);
    TEST_ASSERT_TRUE(fputs("\"}]}]}\n", f) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

static bool report_has_code(const tp_validation_report *report, const char *code) {
    for (size_t i = 0U; i < report->finding_count; ++i) {
        if (strcmp(report->findings[i].code, code) == 0) {
            return true;
        }
    }
    return false;
}

void test_canonical_animation_frame_does_not_match_same_key_in_other_source(void) {
    char source_a[512];
    char source_b[512];
    char sprite_b[640];
    char project_path[640];
    (void)snprintf(source_a, sizeof source_a, "%s/canonical-frame-a",
                   TP_VALIDATE_TEST_DIR);
    (void)snprintf(source_b, sizeof source_b, "%s/canonical-frame-b",
                   TP_VALIDATE_TEST_DIR);
    (void)snprintf(sprite_b, sizeof sprite_b, "%s/shared.png", source_b);
    (void)snprintf(project_path, sizeof project_path, "%s/canonical-frame.ntpacker_project",
                   TP_VALIDATE_TEST_DIR);
    tp_mkdirs(source_a);
    tp_mkdirs(source_b);
    FILE *sprite = fopen(sprite_b, "wb");
    TEST_ASSERT_NOT_NULL(sprite);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("x", 1U, 1U, sprite));
    TEST_ASSERT_EQUAL_INT(0, fclose(sprite));

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, source_a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, source_b));
    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_animation(atlas, "walk",
                                                         &animation));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(animation, "shared"));
    tp_rng rng = tp_rng_os();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_promote_ids(project, &rng, &error));
    animation->frames[0].source_ref = atlas->sources[0].id;
    const char *key = "shared.png";
    animation->frames[0].src_key = malloc(strlen(key) + 1U);
    TEST_ASSERT_NOT_NULL(animation->frames[0].src_key);
    memcpy(animation->frames[0].src_key, key, strlen(key) + 1U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, project_path, &error));
    tp_project_destroy(project);

    tp_validation_report report = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_validate_project_file(project_path, &report,
                                                   &error));
    TEST_ASSERT_TRUE(report_has_code(
        &report, TP_VALIDATION_CODE_DANGLING_ANIM_FRAME));
    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(sprite_b));
    TEST_ASSERT_EQUAL_INT(0, remove(project_path));
}

void test_persisted_canonical_keys_report_invalid_normalization_before_dangling(void) {
    char source_dir[512];
    char project_path[640];
    (void)snprintf(source_dir, sizeof source_dir, "%s/invalid-canonical-keys",
                   TP_VALIDATE_TEST_DIR);
    (void)snprintf(project_path, sizeof project_path,
                   "%s/invalid-canonical-keys.ntpacker_project",
                   TP_VALIDATE_TEST_DIR);
    tp_mkdirs(source_dir);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, source_dir));
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_pending_sprite(atlas, "bad",
                                                              &sprite));
    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_animation(atlas, "walk",
                                                         &animation));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(animation, "hero"));
    tp_rng rng = tp_rng_os();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_promote_ids(project, &rng, &error));
    const tp_id128 source_id = atlas->sources[0].id;
    sprite->source_ref = source_id;
    const char *bad_sprite_key = "../escape.png";
    sprite->src_key = malloc(strlen(bad_sprite_key) + 1U);
    TEST_ASSERT_NOT_NULL(sprite->src_key);
    memcpy(sprite->src_key, bad_sprite_key, strlen(bad_sprite_key) + 1U);
    animation->frames[0].source_ref = source_id;
    const char *bad_frame_key = "dir//hero.png";
    animation->frames[0].src_key = malloc(strlen(bad_frame_key) + 1U);
    TEST_ASSERT_NOT_NULL(animation->frames[0].src_key);
    memcpy(animation->frames[0].src_key, bad_frame_key,
           strlen(bad_frame_key) + 1U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, project_path, &error));
    tp_project_destroy(project);

    tp_validation_report report = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_validate_project_file(project_path, &report,
                                                   &error));
    TEST_ASSERT_TRUE(report_has_code(&report,
                                     TP_VALIDATION_CODE_INVALID_SPRITE_KEY));
    TEST_ASSERT_TRUE(report_has_code(&report,
                                     TP_VALIDATION_CODE_INVALID_FRAME_KEY));
    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(project_path));
}

void test_oversized_source_fallback_never_truncates_distinct_paths(void) {
    char first[5002];
    char second[5002];
    memset(first, 'a', sizeof first - 2U);
    memset(second, 'a', sizeof second - 2U);
    first[sizeof first - 2U] = 'x';
    second[sizeof second - 2U] = 'y';
    first[sizeof first - 1U] = '\0';
    second[sizeof second - 1U] = '\0';
    const char *path = TP_VALIDATE_TEST_DIR "/long-source-keys.ntpacker_project";
    write_v3_sources(path, first, second);

    tp_validation_report report = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_validate_project_file(path, &report, &err));
    TEST_ASSERT_FALSE(report_has_code(&report,
                                      TP_VALIDATION_CODE_DUPLICATE_SOURCE));
    TEST_ASSERT_FALSE(report_has_code(&report,
                                      TP_VALIDATION_CODE_SOURCE_COLLISION));
    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

void test_expanding_unicode_casefold_detects_full_length_collision(void) {
    enum { REPEATS = 1400 };
    char first[REPEATS * 2 + 1];
    char second[REPEATS * 3 + 1];
    for (int i = 0; i < REPEATS; ++i) {
        first[i * 2] = (char)0xC4;
        first[i * 2 + 1] = (char)0xB0; /* U+0130 */
        second[i * 3] = 'i';
        second[i * 3 + 1] = (char)0xCC;
        second[i * 3 + 2] = (char)0x87; /* i + COMBINING DOT */
    }
    first[sizeof first - 1U] = '\0';
    second[sizeof second - 1U] = '\0';
    const char *path = TP_VALIDATE_TEST_DIR "/fold-source-keys.ntpacker_project";
    write_v3_sources(path, first, second);

    tp_validation_report report = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_validate_project_file(path, &report, &err));
    TEST_ASSERT_TRUE(report_has_code(&report,
                                     TP_VALIDATION_CODE_SOURCE_COLLISION));
    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

void test_large_report_is_bounded_and_ends_with_deterministic_summary(void) {
    const size_t actual_count = TP_VALIDATION_REPORT_MAX_FINDINGS + 52U;
    const char *path = TP_VALIDATE_TEST_DIR "/many-findings.ntpacker_project";
    write_many_empty_atlases(path, actual_count);

    tp_validation_report report = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_validate_project_file(path, &report, &err));
    TEST_ASSERT_TRUE(report.truncated);
    TEST_ASSERT_EQUAL_size_t(actual_count * 2U, report.total_finding_count);
    TEST_ASSERT_EQUAL_size_t(actual_count * 2U - (TP_VALIDATION_REPORT_MAX_FINDINGS - 1U),
                             report.omitted_finding_count);
    TEST_ASSERT_EQUAL_size_t(actual_count, report.warning_count);
    TEST_ASSERT_EQUAL_size_t(actual_count, report.error_count);
    TEST_ASSERT_EQUAL_size_t(TP_VALIDATION_REPORT_MAX_FINDINGS, report.finding_count);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(TP_VALIDATION_REPORT_MAX_BYTES,
                                     report.finding_count * sizeof(tp_validation_finding));

    const tp_validation_finding *summary = &report.findings[report.finding_count - 1U];
    TEST_ASSERT_EQUAL_INT(TP_VALIDATION_ERROR, summary->severity);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_TRUNCATED, summary->code);
    TEST_ASSERT_EQUAL_STRING(
        "validation report truncated: omitted 2153 of 4200 findings (1077 errors, 1076 warnings); limits are 2048 findings and 4194304 bytes",
        summary->message);
    TEST_ASSERT_EQUAL_STRING("", summary->atlas);
    TEST_ASSERT_EQUAL_STRING("", summary->sprite);
    TEST_ASSERT_EQUAL_STRING("", summary->anim);
    TEST_ASSERT_EQUAL_STRING("", summary->frame);
    TEST_ASSERT_EQUAL_STRING("", summary->target);

    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

void test_load_failure_leaves_report_freeable(void) {
    tp_validation_report report = {0};
    tp_error err = {0};
    tp_status status = tp_validate_project_file("does-not-exist.ntpacker_project", &report, &err);

    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, status);
    TEST_ASSERT_NULL(report.findings);
    TEST_ASSERT_EQUAL_size_t(0U, report.finding_count);
    tp_validation_report_free(&report);
}

void test_findings_allocation_failure_cleans_partial_report(void) {
    char path[512];
    fixture_path(path, sizeof path, "problems.ntpacker_project");
    tp_validation_report report = {0};
    tp_error err = {0};
    tp_validate__test_set_alloc_fail(0);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, tp_validate_project_file(path, &report, &err));
    TEST_ASSERT_NULL(report.findings);
    TEST_ASSERT_EQUAL_size_t(0U, report.finding_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.error_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.warning_count);
}

void test_sprite_index_oom_fails_validation_and_cleans_report(void) {
    char path[512];
    fixture_path(path, sizeof path, "clean.ntpacker_project");
    tp_validation_report report = {0};
    tp_error err = {0};
    tp_validate__test_fail_sprite_index(true);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM,
                          tp_validate_project_file(path, &report, &err));
    TEST_ASSERT_NULL(report.findings);
    TEST_ASSERT_EQUAL_size_t(0U, report.finding_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.error_count);
    TEST_ASSERT_EQUAL_size_t(0U, report.warning_count);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "out of memory"));
}

void test_target_row_diagnostics_reuse_full_validation_predicates(void) {
    const char *path = TP_VALIDATE_TEST_DIR "/target-row.ntpacker_project";
    (void)remove(path);
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_seed_default_target(project, 0));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_target(&project->atlases[0],
                                                      "missing-exporter",
                                                      "out/atlas1", NULL));
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_promote_ids(project, &rng, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, path, &err));
    tp_project_destroy(project);

    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_open(path, &rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot,
                                                     &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_snapshot_target *target = tp_session_snapshot_target_at(
        snapshot, atlas->id, 1);
    TEST_ASSERT_NOT_NULL(target);
    tp_target_validation_report report;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_validate_session_snapshot_target(
                              snapshot, atlas->id, target->id, &report, &err));
    TEST_ASSERT_EQUAL_size_t(2U, report.issue_count);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_UNKNOWN_EXPORTER,
                             report.issues[0].code);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_DUPLICATE_OUT_PATH,
                             report.issues[1].code);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_file_returns_owned_empty_report);
    RUN_TEST(test_problem_file_report_is_exact_and_stably_ordered);
    RUN_TEST(test_load_failure_leaves_report_freeable);
    RUN_TEST(test_findings_allocation_failure_cleans_partial_report);
    RUN_TEST(test_sprite_index_oom_fails_validation_and_cleans_report);
    RUN_TEST(test_large_report_is_bounded_and_ends_with_deterministic_summary);
    RUN_TEST(test_oversized_source_fallback_never_truncates_distinct_paths);
    RUN_TEST(test_expanding_unicode_casefold_detects_full_length_collision);
    RUN_TEST(test_canonical_animation_frame_does_not_match_same_key_in_other_source);
    RUN_TEST(test_persisted_canonical_keys_report_invalid_normalization_before_dangling);
    RUN_TEST(test_target_row_diagnostics_reuse_full_validation_predicates);
    return UNITY_END();
}
