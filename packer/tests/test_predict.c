/* Degradation-prediction consistency. Proves tp_export_predict_loss
 * is the single source of truth the GUI chip and CLI dry-run both read:
 *   - project-only GUI preview enumerates transform/polygon/slice9/pivot;
 *   - actual execution takes pivot/slice9/alias from the packed IR;
 *   - dry and wet orchestration share this one capability-loss owner. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_pack_result.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_build_worker.h"
#include "tp_export_internal.h"
#include "tp_format_catalog_internal.h"
#include "tp_export_job_internal.h"
#include "tp_project_mutation_internal.h"
#include "unity.h"


void setUp(void) {}
void tearDown(void) {}

static const char *g_dir;
static uint8_t g_piv_px[30 * 20 * 4];
static uint8_t g_sl_px[30 * 20 * 4];

/* A test-only all-restricted exporter over the json writer: drops every axis. */
static tp_exporter g_restrict;
static tp_format_descriptor g_restrict_format;
static char g_boundary_exporter_id[TP_EXPORTER_ID_MAX];
static tp_exporter g_boundary_exporter;
static tp_format_descriptor g_boundary_format;
static tp_format_catalog *g_catalog;
static const tp_format_artifact_decl g_json_artifact[] = {
    {.id = "metadata", .suffix = ".json"},
};

static void fill(uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        p[i * 4 + 0] = 80;
        p[i * 4 + 1] = 120;
        p[i * 4 + 2] = 160;
        p[i * 4 + 3] = 255;
    }
}

static bool has_field(const tp_export_notices *n, int field_id) {
    for (int i = 0; i < n->count; i++) {
        if (n->items[i].field_id == field_id) {
            return true;
        }
    }
    return false;
}

static int count_field(const tp_export_notices *n, int field_id) {
    int count = 0;
    for (int i = 0; i < n->count; ++i) {
        if (n->items[i].field_id == field_id) {
            ++count;
        }
    }
    return count;
}

/* Every writer-emitted PROJECT-KNOWABLE axis must be covered by predict. */
static void assert_writer_subset_predict(const tp_export_notices *writer, const tp_export_notices *predict) {
    static const int axes[] = {TP_NOTICE_FIELD_TRANSFORM, TP_NOTICE_FIELD_POLYGON, TP_NOTICE_FIELD_SLICE9,
                               TP_NOTICE_FIELD_PIVOT};
    for (size_t k = 0; k < sizeof axes / sizeof axes[0]; k++) {
        if (has_field(writer, axes[k])) {
            TEST_ASSERT_TRUE_MESSAGE(has_field(predict, axes[k]),
                                     "predict must cover every writer-emitted project-knowable axis");
        }
    }
}

/* Fresh project whose atlas carries pivot + slice9 overrides and a polygon shape,
 * with one target `exporter_id` at `outbase`. Descs (below) mirror the same pivot +
 * slice9 so the real export drops what predict predicts. */
static tp_project *make_fixture(const char *exporter_id, const char *outbase) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    a->shape = 2; /* CONCAVE_CONTOUR -> the polygon axis is live vs a rect-only target */
    a->allow_transform = true;
    a->power_of_two = false;
    a->padding = 0;
    a->margin = 0;
    a->extrude = 0;
    a->alpha_threshold = 1;
    a->max_size = 1024;
    a->pixels_per_unit = 1.0F;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(a, "sprites"));
    a->sources[0].id.bytes[0] = 1U;
    const tp_id128 source_id = a->sources[0].id;
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(a, source_id, "piv.png",
                                                  &sp));
    sp->origin_x = 1.5F;
    sp->origin_y = -0.25F;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(a, source_id, "sl.png",
                                                  &sp));
    sp->slice9_lrtb[0] = sp->slice9_lrtb[1] = sp->slice9_lrtb[2] = sp->slice9_lrtb[3] = 4;
    (void)tp_project_atlas_add_target(a, exporter_id, outbase, NULL);
    return p;
}

static void build_descs(tp_pack_sprite_desc descs[2]) {
    memset(descs, 0, 2 * sizeof descs[0]);
    descs[0] = (tp_pack_sprite_desc){.name = "piv", .rgba = g_piv_px, .w = 30, .h = 20, .origin_x = 1.5F,
                                     .origin_y = -0.25F};
    descs[1] = (tp_pack_sprite_desc){
        .name = "sl", .rgba = g_sl_px, .w = 30, .h = 20, .origin_x = 0.5F, .origin_y = 0.5F, .slice9_lrtb = {4, 4, 4, 4}};
}

// #region tests
void test_exporter_registry_enforces_exact_canonical_id_bound(void) {
    char oversized[TP_EXPORTER_ID_MAX + 1U];
    memset(oversized, 'x', sizeof oversized - 1U);
    oversized[sizeof oversized - 1U] = '\0';
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_exporter_id_validate(oversized, &error));

    tp_format_descriptor oversized_format = g_boundary_format;
    oversized_format.id = oversized;
    tp_exporter oversized_exporter = {
        .format = &oversized_format,
        .serialize = tp_export_json_neotolis_serialize,
    };
    const tp_exporter *oversized_exporters[] = {&oversized_exporter};
    tp_format_catalog *boundary_catalog = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_format_catalog__test_create(
            oversized_exporters, 1U, &boundary_catalog, &error));
    TEST_ASSERT_NULL(boundary_catalog);

    memset(g_boundary_exporter_id, 'b',
           sizeof g_boundary_exporter_id - 1U);
    g_boundary_exporter_id[sizeof g_boundary_exporter_id - 1U] = '\0';
    g_boundary_format.id = g_boundary_exporter_id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_exporter_id_validate(g_boundary_exporter_id,
                                                  &error));
    const tp_exporter *boundary_exporters[] = {&g_boundary_exporter};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_catalog__test_create(
            boundary_exporters, 1U, &boundary_catalog, &error));
    TEST_ASSERT_EQUAL_PTR(
        &g_boundary_exporter,
        tp_format_catalog_exporter_find(
            boundary_catalog, g_boundary_exporter_id));
    tp_format_catalog_release(boundary_catalog);
}

void test_predict_alias_with_ir(void) {
    /* An IR whose sprite "b" dedups to "a" -- exactly what duplicate images
     * pack to. An aliases=false target can predict that loss only from an IR. */
    tp_project *p = tp_project_create();
    tp_export_sprite sprs[2] = {
        {.final_name = "a", .data = {.alias_of = -1}},
        {.final_name = "b", .data = {.alias_of = 0}},
    };
    tp_export_ir ir = {.sprites = sprs, .sprite_count = 2};

    tp_export_caps caps = tp_export_caps_full();
    caps.aliases = false;
    tp_error e = {{0}};

    tp_export_notices with_ir;
    tp_export_notices_init(&with_ir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_predict_loss(p, 0, &caps, "t", &ir, &with_ir, &e));
    TEST_ASSERT_TRUE_MESSAGE(has_field(&with_ir, TP_NOTICE_FIELD_ALIAS),
                             "IR-aware prediction covers the alias axis");

    tp_export_notices no_ir;
    tp_export_notices_init(&no_ir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_predict_loss(p, 0, &caps, "t", NULL, &no_ir, &e));
    TEST_ASSERT_FALSE_MESSAGE(has_field(&no_ir, TP_NOTICE_FIELD_ALIAS),
                              "alias axis is unavailable without an IR");

    tp_export_notices_free(&with_ir);
    tp_export_notices_free(&no_ir);
    tp_project_destroy(p);
}

void test_animation_capability_projects_ir_and_emits_one_notice(void) {
    TEST_ASSERT_EQUAL_INT(7, TP_NOTICE_FIELD_ANIMATION);

    tp_export_page page = {.artifact_id = 0, .w = 1, .h = 1};
    tp_export_anim animation = {
        .id = "idle",
        .frames = NULL,
        .frame_count = 0,
        .fps = 12.0F,
    };
    tp_export_ir source = {
        .version = TP_EXPORT_IR_VERSION,
        .atlas_name = "animations",
        .pixels_per_unit = 1.0F,
        .pages = &page,
        .page_count = 1,
        .animations = &animation,
        .animation_count = 1,
    };
    tp_export_caps caps = tp_export_caps_full();
    caps.animations = false;
    tp_export_ir projected = {0};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_export_ir_project_for_caps(&source, &caps, &projected, &error));
    TEST_ASSERT_EQUAL_INT(0, projected.animation_count);
    TEST_ASSERT_NULL(projected.animations);
    TEST_ASSERT_EQUAL_PTR(source.pages, projected.pages);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = tp_project_get_atlas(project, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_animation(atlas, "idle", NULL));
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_export_predict_loss(project, 0, &caps, "test-restrict", NULL,
                               &notices, &error));
    TEST_ASSERT_EQUAL_INT(
        1, count_field(&notices, TP_NOTICE_FIELD_ANIMATION));
    TEST_ASSERT_EQUAL_INT(
        TP_NOTICE_REASON_CAPS_UNSUPPORTED,
        notices.items[0].reason_id);
    TEST_ASSERT_NULL(notices.items[0].sprite);

    tp_export_notices_free(&notices);
    tp_project_destroy(project);
}

void test_single_page_format_rejects_multipage_ir_before_planning(void) {
    tp_export_page pages[2] = {
        {.artifact_id = 0, .w = 8, .h = 8},
        {.artifact_id = 1, .w = 8, .h = 8},
    };
    tp_export_ir ir = {
        .version = TP_EXPORT_IR_VERSION,
        .atlas_name = "two-pages",
        .pixels_per_unit = 1.0F,
        .pages = pages,
        .page_count = 2,
    };
    char format_id[] = "test-single-page";
    tp_format_descriptor format = {
        .id = format_id,
        .display_name = "single page",
        .caps = tp_export_caps_full(),
        .artifacts = g_json_artifact,
        .artifact_count = 1,
    };
    format.caps.multipage = false;
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_export_artifact_plan plan;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_export_artifact_plan_build(&format, &ir, "two-pages", arena,
                                      &plan, &e));
    TEST_ASSERT_NOT_NULL(strstr(e.msg, "single-page"));

    format.caps.multipage = true;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_export_artifact_plan_build(&format, &ir, "two-pages", arena,
                                      &plan, &e));
    TEST_ASSERT_EQUAL_STRING("test-single-page", plan.format_id);
    format_id[0] = 'X';
    TEST_ASSERT_EQUAL_STRING("test-single-page", plan.format_id);
    tp_arena_destroy(arena);
}

void test_consistency_restrict(void) {
    /* All-restricted target: the real export drops pivot + slice9 (metadata survives the
     * effective-settings clamp), and predict covers ALL four project-knowable axes. */
    char base[600];
    (void)snprintf(base, sizeof base, "%s/pr_restrict", g_dir);
    tp_project *p = make_fixture("test-restrict", base);
    tp_pack_sprite_desc descs[2];
    build_descs(descs);

    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_export_notices wn;
    tp_export_notices_init(&wn);
    tp_error e = {{0}};
    const tp_export_run_opts run_opts = {.catalog = g_catalog};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_export_run_ex(p, 0, descs, 2, g_dir, ar, &wn, NULL,
                         &run_opts, &e),
        e.msg);

    const tp_exporter *rex =
        tp_format_catalog_exporter_find(g_catalog, "test-restrict");
    TEST_ASSERT_NOT_NULL(rex);
    tp_export_notices pn;
    tp_export_notices_init(&pn);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_export_predict_loss(p, 0, &rex->format->caps,
                                                 "test-restrict", NULL, &pn, &e));

    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_TRANSFORM), "predict must flag transform");
    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_POLYGON), "predict must flag polygon");
    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_SLICE9), "predict must flag slice9");
    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_PIVOT), "predict must flag pivot");
    assert_writer_subset_predict(&wn, &pn);
    /* NULL IR -> no pack-dependent axes. */
    TEST_ASSERT_FALSE(has_field(&pn, TP_NOTICE_FIELD_ALIAS));
    TEST_ASSERT_FALSE(has_field(&pn, TP_NOTICE_FIELD_MULTIPAGE));

    tp_export_notices_free(&wn);
    tp_export_notices_free(&pn);
    tp_arena_destroy(ar);
    tp_project_destroy(p);
}

// #endregion

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    g_dir = (argc > 1) ? argv[1] : ".";
    fill(g_piv_px, 30 * 20);
    fill(g_sl_px, 30 * 20);

    g_restrict_format = (tp_format_descriptor){
        .id = "test-restrict", .display_name = "test restrict",
        .caps = {.transform_mask = TP_EXPORT_TRANSFORMS_IDENTITY},
        .artifacts = g_json_artifact, .artifact_count = 1};
    g_restrict = (tp_exporter){
        .format = &g_restrict_format,
        .serialize = tp_export_json_neotolis_serialize};
    g_boundary_format = (tp_format_descriptor){
        .id = "boundary-placeholder", .display_name = "canonical id boundary",
        .caps = tp_export_caps_full(), .artifacts = g_json_artifact,
        .artifact_count = 1};
    g_boundary_exporter = (tp_exporter){
        .format = &g_boundary_format,
        .serialize = tp_export_json_neotolis_serialize};
    const tp_exporter *const exporters[] = {&g_restrict};
    if (tp_format_catalog__test_create(
            exporters, 1U, &g_catalog, NULL) != TP_STATUS_OK) {
        (void)fprintf(stderr, "failed to register test-restrict exporter\n");
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_exporter_registry_enforces_exact_canonical_id_bound);
    RUN_TEST(test_predict_alias_with_ir);
    RUN_TEST(test_animation_capability_projects_ir_and_emits_one_notice);
    RUN_TEST(test_single_page_format_rejects_multipage_ir_before_planning);
    RUN_TEST(test_consistency_restrict);
    const int result = UNITY_END();
    tp_format_catalog_release(g_catalog);
    return result;
}
