/* Degradation-prediction consistency (A5, review §3.4). Proves tp_export_predict_loss
 * is the single source of truth the GUI chip and CLI dry-run both read:
 *   - project-knowable axes (transform/polygon/slice9/pivot) are enumerated from the
 *     project with a NULL prep;
 *   - pack-dependent axes (alias/multipage) appear only with a prep;
 *   - every writer-emitted project-knowable notice field_id is ALSO predicted
 *     (narrowed superset) -- so predict never under-reports what a real export drops. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_build_worker.h"
#include "tp_project_mutation_internal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char *g_dir;
static uint8_t g_piv_px[30 * 20 * 4];
static uint8_t g_sl_px[30 * 20 * 4];

/* A test-only all-restricted exporter over the json writer: drops every axis. */
static tp_exporter g_restrict;
static char g_boundary_exporter_id[TP_EXPORTER_ID_MAX];
static tp_exporter g_boundary_exporter;

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

    tp_exporter oversized_exporter = g_boundary_exporter;
    oversized_exporter.id = oversized;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_exporter_register(&oversized_exporter));

    memset(g_boundary_exporter_id, 'b',
           sizeof g_boundary_exporter_id - 1U);
    g_boundary_exporter_id[sizeof g_boundary_exporter_id - 1U] = '\0';
    g_boundary_exporter.id = g_boundary_exporter_id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_exporter_id_validate(g_boundary_exporter_id,
                                                  &error));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_exporter_register(&g_boundary_exporter));
    TEST_ASSERT_EQUAL_PTR(&g_boundary_exporter,
                          tp_exporter_find(g_boundary_exporter_id));
}

void test_predict_alias_with_prep(void) {
    /* A prep whose sprite "b" dedups to "a" -- exactly what duplicate images pack to.
     * A caps.aliases=false target must predict the dropped alias, but only WITH a prep
     * (a NULL-prep predict cannot know aliases exist). */
    tp_project *p = tp_project_create();
    tp_export_sprite sprs[2] = {
        {.final_name = "a", .src = NULL, .alias_of = -1},
        {.final_name = "b", .src = NULL, .alias_of = 0},
    };
    tp_result r;
    memset(&r, 0, sizeof r);
    r.atlas_name = "t";
    r.pixels_per_unit = 1.0F;
    r.page_count = 1;
    tp_export_prepared prep;
    memset(&prep, 0, sizeof prep);
    prep.result = &r;
    prep.sprites = sprs;
    prep.sprite_count = 2;
    prep.scale = 1.0F;

    tp_export_caps caps = tp_export_caps_full();
    caps.aliases = false;
    tp_error e = {{0}};

    tp_export_notices with_prep;
    tp_export_notices_init(&with_prep);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_predict_loss(p, 0, &caps, "t", &prep, &with_prep, &e));
    TEST_ASSERT_TRUE_MESSAGE(has_field(&with_prep, TP_NOTICE_FIELD_ALIAS), "opt_prep predict covers the alias axis");

    tp_export_notices no_prep;
    tp_export_notices_init(&no_prep);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_predict_loss(p, 0, &caps, "t", NULL, &no_prep, &e));
    TEST_ASSERT_FALSE_MESSAGE(has_field(&no_prep, TP_NOTICE_FIELD_ALIAS), "alias axis excluded when prep == NULL");

    tp_export_notices_free(&with_prep);
    tp_export_notices_free(&no_prep);
    tp_project_destroy(p);
}

void test_predict_multipage_with_prep(void) {
    tp_project *p = tp_project_create();
    tp_result r;
    memset(&r, 0, sizeof r);
    r.atlas_name = "t";
    r.pixels_per_unit = 1.0F;
    r.page_count = 2; /* multi-page */
    tp_export_prepared prep;
    memset(&prep, 0, sizeof prep);
    prep.result = &r;
    prep.scale = 1.0F;

    tp_export_caps caps = tp_export_caps_full();
    caps.multipage = false;
    tp_error e = {{0}};

    tp_export_notices with_prep;
    tp_export_notices_init(&with_prep);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_predict_loss(p, 0, &caps, "t", &prep, &with_prep, &e));
    TEST_ASSERT_TRUE_MESSAGE(has_field(&with_prep, TP_NOTICE_FIELD_MULTIPAGE), "opt_prep predict covers multipage");

    tp_export_notices no_prep;
    tp_export_notices_init(&no_prep);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_predict_loss(p, 0, &caps, "t", NULL, &no_prep, &e));
    TEST_ASSERT_FALSE_MESSAGE(has_field(&no_prep, TP_NOTICE_FIELD_MULTIPAGE), "multipage excluded when prep == NULL");

    tp_export_notices_free(&with_prep);
    tp_export_notices_free(&no_prep);
    tp_project_destroy(p);
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
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_export_run(p, 0, descs, 2, g_dir, ar, &wn, NULL, &e), e.msg);

    const tp_exporter *rex = tp_exporter_find("test-restrict");
    TEST_ASSERT_NOT_NULL(rex);
    tp_export_notices pn;
    tp_export_notices_init(&pn);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_predict_loss(p, 0, &rex->caps, "test-restrict", NULL, &pn, &e));

    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_TRANSFORM), "predict must flag transform");
    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_POLYGON), "predict must flag polygon");
    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_SLICE9), "predict must flag slice9");
    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_PIVOT), "predict must flag pivot");
    assert_writer_subset_predict(&wn, &pn);
    /* NULL prep -> no pack-dependent axes. */
    TEST_ASSERT_FALSE(has_field(&pn, TP_NOTICE_FIELD_ALIAS));
    TEST_ASSERT_FALSE(has_field(&pn, TP_NOTICE_FIELD_MULTIPAGE));

    tp_export_notices_free(&wn);
    tp_export_notices_free(&pn);
    tp_arena_destroy(ar);
    tp_project_destroy(p);
}

void test_consistency_defold(void) {
    /* Task-literal case: pivot+slice9+polygon fixture against defold caps. Defold keeps
     * pivot/polygon, drops slice9, and packs identity (no transform) -- so the writer
     * emits {slice9} while predict adds the pack-level {transform}. Writer subset holds. */
    char base[600];
    (void)snprintf(base, sizeof base, "%s/pr_defold", g_dir);
    tp_project *p = make_fixture("defold", base);
    tp_pack_sprite_desc descs[2];
    build_descs(descs);

    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_export_notices wn;
    tp_export_notices_init(&wn);
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_export_run(p, 0, descs, 2, g_dir, ar, &wn, NULL, &e), e.msg);

    const tp_exporter *dex = tp_exporter_find("defold");
    TEST_ASSERT_NOT_NULL(dex);
    tp_export_notices pn;
    tp_export_notices_init(&pn);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_predict_loss(p, 0, &dex->caps, "defold", NULL, &pn, &e));

    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_TRANSFORM), "predict must flag transform for defold");
    TEST_ASSERT_TRUE_MESSAGE(has_field(&pn, TP_NOTICE_FIELD_SLICE9), "predict must flag slice9 for defold");
    assert_writer_subset_predict(&wn, &pn);

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

    g_restrict = (tp_exporter){.id = "test-restrict",
                               .display_name = "test restrict",
                               .extension = "json",
                               .caps = {.rotate90 = false,
                                        .flips = false,
                                        .polygons = false,
                                        .pivot = false,
                                        .slice9 = false,
                                        .multipage = false,
                                        .aliases = false},
                               .write = tp_export_json_neotolis_write};
    g_boundary_exporter = (tp_exporter){
        .display_name = "canonical id boundary",
        .extension = "json",
        .caps = {.rotate90 = true,
                 .flips = true,
                 .polygons = true,
                 .pivot = true,
                 .slice9 = true,
                 .multipage = true,
                 .aliases = true},
        .write = tp_export_json_neotolis_write};
    if (tp_exporter_register(&g_restrict) != TP_STATUS_OK) {
        (void)fprintf(stderr, "failed to register test-restrict exporter\n");
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_exporter_registry_enforces_exact_canonical_id_bound);
    RUN_TEST(test_predict_alias_with_prep);
    RUN_TEST(test_predict_multipage_with_prep);
    RUN_TEST(test_consistency_restrict);
    RUN_TEST(test_consistency_defold);
    return UNITY_END();
}
