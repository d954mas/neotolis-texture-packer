/* Per-target packing proof (ROADMAP Phase 2 acceptance, SUMMARY.md §5h).
 *
 * One project, one atlas (full D4 allowed), three targets over TEST-ONLY
 * capability-restricted descriptors that reuse the json-neotolis writer:
 *   - "json-neotolis": full caps           -> packs full D4  (>=1 diagonal)
 *   - "test-nopivot" : full EXCEPT pivot    -> SAME layout, drops pivot + notice
 *   - "test-norot"   : no rotate/flip       -> identity repack (zero transforms)
 * Proves: effective-settings REPACK (not post-filter); identical effective
 * settings share ONE pack run; metadata-loss raises a notice, never an error. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "unity.h"

/* wide + tall force the packer to rotate the tall sprite (diagonal bit). */
static uint8_t g_wide[120 * 24 * 4];
static uint8_t g_tall[24 * 100 * 4];
static uint8_t g_piv[30 * 20 * 4];

static const char *g_dir;
static tp_project *g_proj;
static tp_arena *g_arena;
static tp_export_notices g_notices;
static int g_pack_runs;
static char g_A[1024]; /* json-neotolis base */
static char g_B[1024]; /* test-nopivot base  */
static char g_C[1024]; /* test-norot base    */

/* test-only descriptors (borrowed by the registry; must outlive the run). */
static tp_exporter g_nopivot;
static tp_exporter g_norot;

void setUp(void) {}
void tearDown(void) {}

static void fill(uint8_t *p, int n, uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < n; i++) {
        p[i * 4 + 0] = r;
        p[i * 4 + 1] = g;
        p[i * 4 + 2] = b;
        p[i * 4 + 3] = 255;
    }
}

static cJSON *load_json(const char *base) {
    char path[1088];
    (void)snprintf(path, sizeof path, "%s.json", base);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (sz > 0) ? (char *)malloc((size_t)sz + 1) : NULL;
    size_t rd = buf ? fread(buf, 1, (size_t)sz, f) : 0;
    (void)fclose(f);
    if (!buf) {
        return NULL;
    }
    buf[rd] = '\0';
    cJSON *root = cJSON_ParseWithLength(buf, rd);
    free(buf);
    return root;
}

static int count_nonidentity_transforms(cJSON *root) {
    int n = 0;
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    cJSON *s = NULL;
    cJSON_ArrayForEach(s, sprites) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(s, "transform");
        if (cJSON_IsNumber(t) && t->valueint != 0) {
            n++;
        }
    }
    return n;
}

static cJSON *sprite_by_name(cJSON *root, const char *name) {
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    cJSON *s = NULL;
    cJSON_ArrayForEach(s, sprites) {
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(s, "name");
        if (cJSON_IsString(nm) && strcmp(nm->valuestring, name) == 0) {
            return s;
        }
    }
    return NULL;
}

// #region tests
void test_shared_run_count(void) {
    /* json-neotolis and test-nopivot have identical effective settings (both
     * full D4) -> ONE pack run; test-norot differs -> a second. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_pack_runs, "identical effective settings must share one pack run");
}

void test_full_target_has_diagonal(void) {
    cJSON *root = load_json(g_A);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE_MESSAGE(count_nonidentity_transforms(root) >= 1,
                             "full-D4 target must produce >=1 non-identity transform");
    cJSON_Delete(root);
}

void test_norot_target_is_identity(void) {
    cJSON *root = load_json(g_C);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_nonidentity_transforms(root),
                                  "no-rotation target must repack to an all-identity layout");
    cJSON_Delete(root);
}

void test_nopivot_drops_pivot_with_notice(void) {
    /* pivot present in the full target... */
    cJSON *ra = load_json(g_A);
    TEST_ASSERT_NOT_NULL(ra);
    cJSON *pa = sprite_by_name(ra, "piv");
    TEST_ASSERT_NOT_NULL(pa);
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(pa, "pivot")));
    cJSON_Delete(ra);

    /* ...but dropped in the pivot-less target. */
    cJSON *rb = load_json(g_B);
    TEST_ASSERT_NOT_NULL(rb);
    cJSON *pb = sprite_by_name(rb, "piv");
    TEST_ASSERT_NOT_NULL(pb);
    TEST_ASSERT_NULL_MESSAGE(cJSON_GetObjectItemCaseSensitive(pb, "pivot"), "pivot must be dropped for a pivot-less target");
    cJSON_Delete(rb);

    /* and a metadata-loss notice was raised (never an error). */
    bool found = false;
    for (int i = 0; i < g_notices.count; i++) {
        if (strstr(g_notices.items[i].msg, "pivot")) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "dropping a non-default pivot must raise a notice");
}
// #endregion

static bool setup_all(const char *dir) {
    g_dir = dir;
    fill(g_wide, 120 * 24, 200, 60, 60);
    fill(g_tall, 24 * 100, 60, 200, 60);
    fill(g_piv, 30 * 20, 60, 60, 200);
    tp_export_notices_init(&g_notices);

    /* register test-only descriptors over the json writer. */
    g_nopivot = (tp_exporter){.id = "test-nopivot",
                              .display_name = "test nopivot",
                              .extension = "json",
                              .caps = {.rotate90 = true,
                                       .flips = true,
                                       .polygons = true,
                                       .pivot = false,
                                       .slice9 = true,
                                       .multipage = true,
                                       .aliases = true},
                              .write = tp_export_json_neotolis_write};
    g_norot = (tp_exporter){.id = "test-norot",
                            .display_name = "test norot",
                            .extension = "json",
                            .caps = {.rotate90 = false,
                                     .flips = false,
                                     .polygons = true,
                                     .pivot = true,
                                     .slice9 = true,
                                     .multipage = true,
                                     .aliases = true},
                            .write = tp_export_json_neotolis_write};
    if (tp_exporter_register(&g_nopivot) != TP_STATUS_OK || tp_exporter_register(&g_norot) != TP_STATUS_OK) {
        return false;
    }

    g_proj = tp_project_create();
    if (!g_proj) {
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(g_proj, 0);
    a->shape = 0; /* RECT */
    a->allow_transform = true;
    a->power_of_two = false;
    a->padding = 0;
    a->margin = 0;
    a->alpha_threshold = 1;
    a->max_size = 1024;
    a->pixels_per_unit = 1.0F;

    (void)snprintf(g_A, sizeof g_A, "%s/run_A", dir);
    (void)snprintf(g_B, sizeof g_B, "%s/run_B", dir);
    (void)snprintf(g_C, sizeof g_C, "%s/run_C", dir);
    if (tp_project_atlas_add_target(a, "json-neotolis", g_A, NULL) != TP_STATUS_OK ||
        tp_project_atlas_add_target(a, "test-nopivot", g_B, NULL) != TP_STATUS_OK ||
        tp_project_atlas_add_target(a, "test-norot", g_C, NULL) != TP_STATUS_OK) {
        return false;
    }

    tp_pack_sprite_desc sprites[3];
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "wide", .rgba = g_wide, .w = 120, .h = 24, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[1] = (tp_pack_sprite_desc){.name = "tall", .rgba = g_tall, .w = 24, .h = 100, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[2] = (tp_pack_sprite_desc){.name = "piv", .rgba = g_piv, .w = 30, .h = 20, .origin_x = 1.5F, .origin_y = -0.25F};

    g_arena = tp_arena_create(0);
    if (!g_arena) {
        return false;
    }
    tp_error e = {{0}};
    tp_status st = tp_export_run(g_proj, 0, sprites, 3, dir, g_arena, &g_notices, &g_pack_runs, &e);
    if (st != TP_STATUS_OK) {
        (void)fprintf(stderr, "tp_export_run failed: %s (%s)\n", tp_status_str(st), e.msg);
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    if (!setup_all(dir)) {
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_shared_run_count);
    RUN_TEST(test_full_target_has_diagonal);
    RUN_TEST(test_norot_target_is_identity);
    RUN_TEST(test_nopivot_drops_pivot_with_notice);
    int rc = UNITY_END();
    tp_export_notices_free(&g_notices);
    tp_project_destroy(g_proj);
    tp_arena_destroy(g_arena);
    return rc;
}
