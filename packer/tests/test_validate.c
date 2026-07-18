#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "tp_core/tp_validate.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_project_mutation_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_validate_internal.h"
#include "../src/tp_fs_internal.h"

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
    TEST_ASSERT_EQUAL_STRING("does_not_exist", report.findings[0].source);
    TEST_ASSERT_EQUAL_STRING("", report.findings[0].sprite);
    TEST_ASSERT_EQUAL_STRING("", report.findings[0].anim);
    TEST_ASSERT_EQUAL_STRING("", report.findings[0].frame);
    TEST_ASSERT_EQUAL_STRING("", report.findings[0].target);
    tp_id_kind id_kind = TP_ID_KIND_INVALID;
    tp_id128 expected_id = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_id_parse("atlas_00000000000000000000000000000701",
                    &id_kind, &expected_id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_ID_KIND_ATLAS, id_kind);
    TEST_ASSERT_TRUE(tp_id128_eq(expected_id, report.findings[0].atlas_id));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_id_parse("source_00000000000000000000000000000704",
                    &id_kind, &expected_id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_ID_KIND_SOURCE, id_kind);
    TEST_ASSERT_TRUE(tp_id128_eq(expected_id, report.findings[0].source_id));

    TEST_ASSERT_EQUAL_INT(TP_VALIDATION_ERROR, report.findings[1].severity);
    TEST_ASSERT_EQUAL_STRING(TP_VALIDATION_CODE_DANGLING_ANIM_FRAME, report.findings[1].code);
    TEST_ASSERT_EQUAL_STRING(
        "animation 'run' references frame 'ghost' which matches no canonical sprite",
        report.findings[1].message);
    TEST_ASSERT_EQUAL_STRING("problems", report.findings[1].atlas);
    TEST_ASSERT_EQUAL_STRING("", report.findings[1].source);
    TEST_ASSERT_EQUAL_STRING("", report.findings[1].sprite);
    TEST_ASSERT_EQUAL_STRING("run", report.findings[1].anim);
    TEST_ASSERT_EQUAL_STRING("ghost", report.findings[1].frame);
    TEST_ASSERT_EQUAL_STRING("", report.findings[1].target);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_id_parse("anim_00000000000000000000000000000702",
                    &id_kind, &expected_id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_ID_KIND_ANIM, id_kind);
    TEST_ASSERT_TRUE(
        tp_id128_eq(expected_id, report.findings[1].animation_id));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_id_parse("source_00000000000000000000000000000703",
                    &id_kind, &expected_id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_ID_KIND_SOURCE, id_kind);
    TEST_ASSERT_TRUE(tp_id128_eq(expected_id, report.findings[1].source_id));
    tp_validation_report_free(&report);
}

void test_long_shared_prefix_atlas_contexts_remain_exact_and_distinct(void) {
    char first[384];
    char second[384];
    char first_source[384];
    char second_source[384];
    memset(first, 'a', sizeof first);
    memset(second, 'a', sizeof second);
    memset(first_source, 's', sizeof first_source);
    memset(second_source, 's', sizeof second_source);
    first[sizeof first - 2U] = 'x';
    second[sizeof second - 2U] = 'y';
    first[sizeof first - 1U] = '\0';
    second[sizeof second - 1U] = '\0';
    first_source[sizeof first_source - 2U] = 'x';
    second_source[sizeof second_source - 2U] = 'y';
    first_source[sizeof first_source - 1U] = '\0';
    second_source[sizeof second_source - 1U] = '\0';
    const char *path =
        TP_VALIDATE_TEST_DIR "/long-finding-context.ntpacker_project";

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_set_atlas_name(&project->atlases[0], first));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_add_atlas(project, second, NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source(&project->atlases[0], first_source));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source(&project->atlases[1], second_source));
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &error),
        error.msg);
    const tp_id128 first_id = project->atlases[0].id;
    const tp_id128 second_id = project->atlases[1].id;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_save(project, path, &error), error.msg);
    tp_project_destroy(project);

    tp_validation_report report = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_validate_project_file(path, &report, &error),
        error.msg);
    const tp_validation_finding *first_empty = NULL;
    const tp_validation_finding *second_empty = NULL;
    const tp_validation_finding *first_missing = NULL;
    const tp_validation_finding *second_missing = NULL;
    for (size_t i = 0U; i < report.finding_count; ++i) {
        const tp_validation_finding *finding = &report.findings[i];
        if (strcmp(finding->code, TP_VALIDATION_CODE_EMPTY_ATLAS) == 0) {
            if (strcmp(finding->atlas, first) == 0) {
                first_empty = finding;
            } else if (strcmp(finding->atlas, second) == 0) {
                second_empty = finding;
            }
        } else if (strcmp(finding->code,
                          TP_VALIDATION_CODE_MISSING_SOURCE) == 0) {
            if (strcmp(finding->source, first_source) == 0) {
                first_missing = finding;
            } else if (strcmp(finding->source, second_source) == 0) {
                second_missing = finding;
            }
        }
    }
    TEST_ASSERT_NOT_NULL(first_empty);
    TEST_ASSERT_NOT_NULL(second_empty);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(first_empty->atlas, second_empty->atlas));
    TEST_ASSERT_TRUE(tp_id128_eq(first_id, first_empty->atlas_id));
    TEST_ASSERT_TRUE(tp_id128_eq(second_id, second_empty->atlas_id));
    TEST_ASSERT_NOT_NULL(first_missing);
    TEST_ASSERT_NOT_NULL(second_missing);
    TEST_ASSERT_GREATER_THAN_size_t(256U, strlen(first_missing->message));
    TEST_ASSERT_NOT_EQUAL(0,
                          strcmp(first_missing->message,
                                 second_missing->message));

    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

static void write_many_empty_atlases(const char *path, size_t count) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs("{\"version\":5,\"atlases\":[", f) >= 0);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(
            fprintf(f,
                    "%s{\"id\":\"atlas_%032llx\","
                    "\"name\":\"atlas_%zu\",\"max_size\":0}",
                    i == 0U ? "" : ",", (unsigned long long)(i + 1U),
                    i) > 0);
    }
    TEST_ASSERT_TRUE(fputs("]}\n", f) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

static void write_canonical_sources(const char *path, const char *first,
                                    const char *second) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs(
        "{\"version\":5,\"atlases\":[{"
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

static size_t collect_setting_findings(
    const tp_validation_report *report,
    const tp_validation_finding **out, size_t capacity) {
    size_t count = 0U;
    for (size_t i = 0U; i < report->finding_count; ++i) {
        if (strcmp(report->findings[i].code,
                   TP_VALIDATION_CODE_SETTING_OUT_OF_RANGE) != 0) {
            continue;
        }
        if (count < capacity) {
            out[count] = &report->findings[i];
        }
        count++;
    }
    return count;
}

void test_validation_reports_raw_atlas_and_sprite_pack_constraints(void) {
    const char *source_path = TP_VALIDATE_TEST_DIR "/constraint-source";
    const char *project_path =
        TP_VALIDATE_TEST_DIR "/raw-pack-constraints.ntpacker_project";
    tp_mkdirs(source_path);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = &project->atlases[0];
    atlas->max_size = 32;
    atlas->padding = 64;
    atlas->margin = 33;
    atlas->extrude = 1;
    atlas->shape = TP_PACK_SHAPE_MAX;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, source_path));
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &error),
        error.msg);
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(
            atlas, atlas->sources[0].id, "ghost.png", &sprite));
    TEST_ASSERT_NOT_NULL(sprite);
    sprite->ov_margin = 33;
    sprite->ov_extrude = 33;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_save(project, project_path, &error),
        error.msg);
    tp_project_destroy(project);

    tp_validation_report report = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_validate_project_file(project_path, &report, &error), error.msg);
    const tp_validation_finding *settings[8] = {0};
    TEST_ASSERT_EQUAL_size_t(
        6U, collect_setting_findings(&report, settings,
                                     sizeof settings / sizeof settings[0]));
    TEST_ASSERT_EQUAL_STRING("padding = 64 must be in [0..32]",
                             settings[0]->message);
    TEST_ASSERT_EQUAL_STRING("margin = 33 must be in [0..32]",
                             settings[1]->message);
    TEST_ASSERT_EQUAL_STRING("extrude > 0 requires shape RECT",
                             settings[2]->message);
    TEST_ASSERT_EQUAL_STRING(
        "sprite ov_margin = 33 must not exceed atlas max_size 32",
        settings[3]->message);
    TEST_ASSERT_EQUAL_STRING(
        "sprite ov_extrude = 33 must not exceed atlas max_size 32",
        settings[4]->message);
    TEST_ASSERT_EQUAL_STRING(
        "sprite effective extrude 33 requires effective shape RECT",
        settings[5]->message);
    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(project_path));
}

void test_validation_accepts_spacing_at_effective_max_size(void) {
    const char *source_path = TP_VALIDATE_TEST_DIR "/constraint-source";
    const char *project_path =
        TP_VALIDATE_TEST_DIR "/pack-constraint-boundary.ntpacker_project";
    tp_mkdirs(source_path);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = &project->atlases[0];
    atlas->max_size = 32;
    atlas->padding = 32;
    atlas->margin = 32;
    atlas->extrude = 32;
    atlas->shape = TP_PACK_SHAPE_MIN;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, source_path));
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &error),
        error.msg);
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(
            atlas, atlas->sources[0].id, "ghost.png", &sprite));
    TEST_ASSERT_NOT_NULL(sprite);
    sprite->ov_margin = 32;
    sprite->ov_extrude = 32;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_save(project, project_path, &error),
        error.msg);
    tp_project_destroy(project);

    tp_validation_report report = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_validate_project_file(project_path, &report, &error), error.msg);
    const tp_validation_finding *settings[1] = {0};
    TEST_ASSERT_EQUAL_size_t(0U, collect_setting_findings(&report, settings, 1U));
    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(project_path));
}

void test_validation_reports_loaded_animation_domains(void) {
    const char *project_path =
        TP_VALIDATE_TEST_DIR "/animation-domains.ntpacker_project";
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_anim *low = NULL;
    tp_project_anim *high = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_animation(&project->atlases[0], "low", &low));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_animation(&project->atlases[0], "high", &high));
    TEST_ASSERT_NOT_NULL(low);
    TEST_ASSERT_NOT_NULL(high);
    low->fps = 0.0F;
    low->playback = -1;
    high->fps = -1.0F;
    high->playback = 7;
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_save(project, project_path, &error),
        error.msg);
    const tp_id128 low_id = low->id;
    const tp_id128 high_id = high->id;
    tp_project_destroy(project);

    tp_validation_report report = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_validate_project_file(project_path, &report, &error), error.msg);
    const tp_validation_finding *settings[4] = {0};
    TEST_ASSERT_EQUAL_size_t(
        4U, collect_setting_findings(&report, settings,
                                     sizeof settings / sizeof settings[0]));
    TEST_ASSERT_EQUAL_STRING("animation 'low' fps must be positive and finite",
                             settings[0]->message);
    TEST_ASSERT_EQUAL_STRING(
        "animation 'low' playback = -1 is out of range [0..6]",
        settings[1]->message);
    TEST_ASSERT_EQUAL_STRING("animation 'high' fps must be positive and finite",
                             settings[2]->message);
    TEST_ASSERT_EQUAL_STRING(
        "animation 'high' playback = 7 is out of range [0..6]",
        settings[3]->message);
    TEST_ASSERT_EQUAL_STRING("low", settings[0]->anim);
    TEST_ASSERT_TRUE(tp_id128_eq(low_id, settings[0]->animation_id));
    TEST_ASSERT_EQUAL_STRING("high", settings[2]->anim);
    TEST_ASSERT_TRUE(tp_id128_eq(high_id, settings[2]->animation_id));
    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(project_path));
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
    tp_rng rng = tp_rng_os();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_anim_add_frame(animation, atlas->sources[0].id,
                                  "shared.png"));
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

void test_save_rejects_invalid_canonical_key_normalization(void) {
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
    tp_rng rng = tp_rng_os();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &error));
    const tp_id128 source_id = atlas->sources[0].id;
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(
            atlas, source_id, "bad.png", &sprite));
    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_animation(atlas, "walk",
                                                         &animation));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(animation, source_id,
                                                    "dir/hero.png"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &error));
    const char *bad_sprite_key = "../escape.png";
    free(sprite->src_key);
    sprite->src_key = malloc(strlen(bad_sprite_key) + 1U);
    TEST_ASSERT_NOT_NULL(sprite->src_key);
    memcpy(sprite->src_key, bad_sprite_key, strlen(bad_sprite_key) + 1U);
    free(animation->frames[0].src_key);
    const char *bad_frame_key = "dir//hero.png";
    animation->frames[0].src_key = malloc(strlen(bad_frame_key) + 1U);
    TEST_ASSERT_NOT_NULL(animation->frames[0].src_key);
    memcpy(animation->frames[0].src_key, bad_frame_key,
           strlen(bad_frame_key) + 1U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_project_save(project, project_path, &error));
    tp_project_destroy(project);
    TEST_ASSERT_NOT_EQUAL(0, remove(project_path));
}

void test_oversized_source_path_is_rejected(void) {
    char first[5002];
    char second[5002];
    memset(first, 'a', sizeof first - 2U);
    memset(second, 'a', sizeof second - 2U);
    first[sizeof first - 2U] = 'x';
    second[sizeof second - 2U] = 'y';
    first[sizeof first - 1U] = '\0';
    second[sizeof second - 1U] = '\0';
    const char *path = TP_VALIDATE_TEST_DIR "/long-source-keys.ntpacker_project";
    write_canonical_sources(path, first, second);

    tp_validation_report report = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_validate_project_file(path, &report, &err));
    tp_validation_report_free(&report);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

void test_expanding_unicode_casefold_detects_full_length_collision(void) {
    enum { REPEATS = 1000 };
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
    write_canonical_sources(path, first, second);

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
    TEST_ASSERT_EQUAL_STRING("", summary->source);
    TEST_ASSERT_EQUAL_STRING("", summary->sprite);
    TEST_ASSERT_EQUAL_STRING("", summary->anim);
    TEST_ASSERT_EQUAL_STRING("", summary->frame);
    TEST_ASSERT_EQUAL_STRING("", summary->target);
    TEST_ASSERT_TRUE(tp_id128_is_nil(summary->atlas_id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(summary->source_id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(summary->animation_id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(summary->target_id));

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
                          tp_project_assign_missing_ids(project, &rng, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, path, &err));
    const tp_id128 atlas_id = project->atlases[0].id;
    const tp_id128 target_id = project->atlases[0].targets[1].id;
    tp_project_destroy(project);

    tp_validation_report full_report = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_validate_project_file(path, &full_report, &err));
    const tp_validation_finding *unknown_exporter = NULL;
    for (size_t i = 0U; i < full_report.finding_count; ++i) {
        if (strcmp(full_report.findings[i].code,
                   TP_VALIDATION_CODE_UNKNOWN_EXPORTER) == 0) {
            unknown_exporter = &full_report.findings[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(unknown_exporter);
    TEST_ASSERT_TRUE(tp_id128_eq(atlas_id, unknown_exporter->atlas_id));
    TEST_ASSERT_TRUE(tp_id128_eq(target_id, unknown_exporter->target_id));
    TEST_ASSERT_EQUAL_STRING("missing-exporter", unknown_exporter->target);
    tp_validation_report_free(&full_report);

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

void test_validation_does_not_misreport_existing_long_source_as_missing(void) {
    char root[TP_IDENTITY_PATH_MAX];
    char dir[TP_IDENTITY_PATH_MAX];
    char image[TP_IDENTITY_PATH_MAX];
    const char *project_path =
        TP_VALIDATE_TEST_DIR "/long-existing-source.ntpacker_project";
    (void)snprintf(root, sizeof root, "%s/long-existing-source",
                   TP_VALIDATE_TEST_DIR);
    (void)snprintf(dir, sizeof dir, "%s", root);
    for (int i = 0; i < 24; ++i) {
        char segment[64];
        (void)snprintf(segment, sizeof segment,
                       "/segment_%02d_abcdefghijklmnop", i);
        size_t used = strlen(dir);
        TEST_ASSERT_TRUE(used + strlen(segment) + 1U < sizeof dir);
        memcpy(dir + used, segment, strlen(segment) + 1U);
    }
    (void)snprintf(image, sizeof image,
                   "%s/\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82.png",
                   dir);
    TEST_ASSERT_GREATER_THAN_size_t(511U, strlen(image));
    tp_mkdirs(dir);
    FILE *file = tp_fs_fopen(image, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(tp_fs_write_all(file, "X", 1U));
    TEST_ASSERT_TRUE(tp_fs_close(file));

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source(&project->atlases[0], dir));
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_save(project, project_path, &error),
        error.msg);
    tp_project_destroy(project);

    tp_validation_report report = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_validate_project_file(project_path, &report, &error), error.msg);
    for (size_t i = 0; i < report.finding_count; ++i) {
        TEST_ASSERT_NOT_EQUAL(
            0, strcmp(report.findings[i].code,
                      TP_VALIDATION_CODE_MISSING_SOURCE));
    }
    tp_validation_report_free(&report);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_file_returns_owned_empty_report);
    RUN_TEST(test_problem_file_report_is_exact_and_stably_ordered);
    RUN_TEST(test_long_shared_prefix_atlas_contexts_remain_exact_and_distinct);
    RUN_TEST(test_load_failure_leaves_report_freeable);
    RUN_TEST(test_findings_allocation_failure_cleans_partial_report);
    RUN_TEST(test_sprite_index_oom_fails_validation_and_cleans_report);
    RUN_TEST(test_large_report_is_bounded_and_ends_with_deterministic_summary);
    RUN_TEST(test_oversized_source_path_is_rejected);
    RUN_TEST(test_expanding_unicode_casefold_detects_full_length_collision);
    RUN_TEST(test_canonical_animation_frame_does_not_match_same_key_in_other_source);
    RUN_TEST(test_validation_reports_raw_atlas_and_sprite_pack_constraints);
    RUN_TEST(test_validation_accepts_spacing_at_effective_max_size);
    RUN_TEST(test_validation_reports_loaded_animation_domains);
    RUN_TEST(test_save_rejects_invalid_canonical_key_normalization);
    RUN_TEST(test_target_row_diagnostics_reuse_full_validation_predicates);
    RUN_TEST(test_validation_does_not_misreport_existing_long_source_as_missing);
    return UNITY_END();
}
