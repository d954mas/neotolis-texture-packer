/* json-neotolis golden fidelity + determinism (ROADMAP Phase 2 acceptance).
 *
 * For every fixture case (rotated/flipped/trimmed/polygon/slice9/alias/multipage
 * + edges): build a .ntpack via nt_builder -> parse back -> normalize -> export
 * json-neotolis (full caps). Then parse the emitted JSON with cJSON and assert
 * every fixture-known field survives at full fidelity, and re-export is
 * byte-identical (determinism). Golden decision: programmatic field assertions +
 * determinism memcmp (no committed golden file -- it would churn with the packer;
 * ROADMAP acceptance is schema-match + fidelity, which these assertions pin). */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_core/tp_pack_read.h"
#include "unity.h"

#include "tp_fixtures.h"

typedef struct {
    const tp_fixture_case *cs;
    tp_result *res;
    char base[1024]; /* out path base (no extension) */
    char json[1088]; /* base + ".json" */
} jc;

static jc g[16];
static int gn;
static tp_arena *g_arena;
static tp_name_map *g_names;
static const char *g_dir;

void setUp(void) {}
void tearDown(void) {}

// #region helpers
static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        (void)fclose(f);
        return NULL;
    }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    size_t rd = b ? fread(b, 1, (size_t)sz, f) : 0;
    (void)fclose(f);
    if (!b || rd != (size_t)sz) {
        free(b);
        return NULL;
    }
    *out_size = (size_t)sz;
    return b;
}

static cJSON *load_json(const char *path) {
    size_t n = 0;
    uint8_t *b = read_whole_file(path, &n);
    if (!b) {
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)b, n);
    free(b);
    return root;
}

static jc *find_case(const char *name) {
    for (int i = 0; i < gn; i++) {
        if (strcmp(g[i].cs->name, name) == 0) {
            return &g[i];
        }
    }
    return NULL;
}

static cJSON *sprite_by_name(cJSON *root, const char *name) {
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    cJSON *s = NULL;
    cJSON_ArrayForEach(s, sprites) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(s, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) {
            return s;
        }
    }
    return NULL;
}

/* Exports one case to its own base; leaves the parsed cJSON to the caller. */
static bool export_case(jc *j) {
    tp_export_prepared prep;
    tp_error e = {{0}};
    if (tp_normalize(j->res, NULL, g_arena, &prep, &e) != TP_STATUS_OK) {
        (void)fprintf(stderr, "normalize failed for %s: %s\n", j->cs->name, e.msg);
        return false;
    }
    tp_export_caps caps = tp_export_caps_full();
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_status st = tp_export_json_neotolis_write(&prep, &caps, j->base, &notices, &e);
    /* full caps => zero metadata-loss notices. */
    bool no_notices = (notices.count == 0);
    tp_export_notices_free(&notices);
    if (st != TP_STATUS_OK) {
        (void)fprintf(stderr, "export failed for %s: %s\n", j->cs->name, e.msg);
        return false;
    }
    return no_notices;
}
// #endregion

// #region tests
void test_structure_all_cases(void) {
    for (int i = 0; i < gn; i++) {
        cJSON *root = load_json(g[i].json);
        char m[128];
        (void)snprintf(m, sizeof m, "case '%s' json parse", g[i].cs->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(root, m);

        cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNumber(ver) && ver->valueint == TP_JSON_NEOTOLIS_SCHEMA_VERSION, m);

        cJSON *atlas = cJSON_GetObjectItemCaseSensitive(root, "atlas");
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(atlas) && strcmp(atlas->valuestring, g[i].cs->name) == 0, m);

        cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsArray(pages), m);
        TEST_ASSERT_EQUAL_INT_MESSAGE(g[i].res->page_count, cJSON_GetArraySize(pages), m);
        cJSON *pg = NULL;
        cJSON_ArrayForEach(pg, pages) {
            cJSON *w = cJSON_GetObjectItemCaseSensitive(pg, "w");
            cJSON *h = cJSON_GetObjectItemCaseSensitive(pg, "h");
            cJSON *file = cJSON_GetObjectItemCaseSensitive(pg, "file");
            cJSON *pm = cJSON_GetObjectItemCaseSensitive(pg, "premultiplied");
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNumber(w) && w->valueint > 0, m);
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNumber(h) && h->valueint > 0, m);
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(file) && strstr(file->valuestring, ".png"), m);
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsFalse(pm), m); /* straight-alpha default */
        }

        cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsArray(sprites), m);
        TEST_ASSERT_EQUAL_INT_MESSAGE(g[i].cs->sprite_count, cJSON_GetArraySize(sprites), m);
        /* every sprite has a frame + sourceSize + spriteSourceSize. */
        cJSON *sp = NULL;
        cJSON_ArrayForEach(sp, sprites) {
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(sp, "frame")), m);
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(sp, "sourceSize")), m);
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(sp, "spriteSourceSize")), m);
        }
        cJSON_Delete(root);
    }
}

void test_transform_fidelity(void) {
    jc *j = find_case("rotated");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = load_json(j->json);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    int nonzero = 0;
    cJSON *sp = NULL;
    cJSON_ArrayForEach(sp, sprites) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(sp, "transform");
        if (cJSON_IsNumber(t) && t->valueint != 0) {
            nonzero++;
            cJSON *ts = cJSON_GetObjectItemCaseSensitive(sp, "transformStr");
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(ts) && strlen(ts->valuestring) > 0, "transformStr present");
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(nonzero >= 1, "rotated case must emit >=1 non-identity transform");
    cJSON_Delete(root);
}

void test_polygon_fidelity(void) {
    jc *j = find_case("polygon");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = load_json(j->json);
    TEST_ASSERT_NOT_NULL(root);
    int with_poly = 0;
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    cJSON *sp = NULL;
    cJSON_ArrayForEach(sp, sprites) {
        cJSON *poly = cJSON_GetObjectItemCaseSensitive(sp, "polygon");
        if (poly) {
            TEST_ASSERT_TRUE(cJSON_IsObject(poly));
            cJSON *verts = cJSON_GetObjectItemCaseSensitive(poly, "verts");
            cJSON *idx = cJSON_GetObjectItemCaseSensitive(poly, "indices");
            TEST_ASSERT_TRUE(cJSON_IsArray(verts) && cJSON_GetArraySize(verts) >= 3);
            TEST_ASSERT_TRUE(cJSON_IsArray(idx) && cJSON_GetArraySize(idx) >= 3);

            /* Parity with the decode/Defold invariant: the hull's trim-local
             * vertex bbox has its min corner at (0,0) and its max corner at the
             * trim footprint (spriteSourceSize.w/h == frame.w/h). This is what
             * makes the GUI canvas hull overlay hug the sprite -- before the
             * tp_pack_read normalization it did not. */
            cJSON *v0 = cJSON_GetArrayItem(verts, 0);
            double minx = cJSON_GetArrayItem(v0, 0)->valuedouble, maxx = minx;
            double miny = cJSON_GetArrayItem(v0, 1)->valuedouble, maxy = miny;
            cJSON *vv = NULL;
            cJSON_ArrayForEach(vv, verts) {
                double x = cJSON_GetArrayItem(vv, 0)->valuedouble;
                double y = cJSON_GetArrayItem(vv, 1)->valuedouble;
                if (x < minx) { minx = x; }
                if (x > maxx) { maxx = x; }
                if (y < miny) { miny = y; }
                if (y > maxy) { maxy = y; }
            }
            cJSON *ss = cJSON_GetObjectItemCaseSensitive(sp, "spriteSourceSize");
            double ssw = cJSON_GetObjectItemCaseSensitive(ss, "w")->valuedouble;
            double ssh = cJSON_GetObjectItemCaseSensitive(ss, "h")->valuedouble;
            TEST_ASSERT_TRUE_MESSAGE(minx == 0.0 && miny == 0.0, "hull verts bbox min corner must be (0,0)");
            TEST_ASSERT_TRUE_MESSAGE(maxx == ssw && maxy == ssh,
                                     "hull verts bbox max corner must equal spriteSourceSize (footprint)");
            with_poly++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(with_poly >= 1, "polygon case must emit polygon verts for the hull sprites");
    cJSON_Delete(root);
}

void test_slice9_fidelity(void) {
    jc *j = find_case("slice9");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = load_json(j->json);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *sp = sprite_by_name(root, "slice9_panel");
    TEST_ASSERT_NOT_NULL(sp);
    cJSON *sl = cJSON_GetObjectItemCaseSensitive(sp, "slice9");
    TEST_ASSERT_TRUE(cJSON_IsArray(sl) && cJSON_GetArraySize(sl) == 4);
    int want[4] = {6, 7, 8, 9};
    for (int k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_INT(want[k], cJSON_GetArrayItem(sl, k)->valueint);
    }
    cJSON_Delete(root);
}

void test_pivot_fidelity(void) {
    jc *j = find_case("off_pivot");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = load_json(j->json);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *sp = sprite_by_name(root, "off_pivot_rect");
    TEST_ASSERT_NOT_NULL(sp);
    cJSON *piv = cJSON_GetObjectItemCaseSensitive(sp, "pivot");
    TEST_ASSERT_TRUE(cJSON_IsArray(piv) && cJSON_GetArraySize(piv) == 2);
    /* pivot round-trips to the user's y-down input exactly (1.5, -0.25) -- the
     * builder's y-flip and the reader's y-flip cancel (fixture invariant). */
    TEST_ASSERT_TRUE(fabs(cJSON_GetArrayItem(piv, 0)->valuedouble - 1.5) < 1e-6);
    TEST_ASSERT_TRUE(fabs(cJSON_GetArrayItem(piv, 1)->valuedouble - (-0.25)) < 1e-6);
    cJSON_Delete(root);
}

void test_alias_fidelity(void) {
    jc *j = find_case("alias");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = load_json(j->json);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *a = sprite_by_name(root, "alias_a");
    cJSON *b = sprite_by_name(root, "alias_b");
    TEST_ASSERT_NOT_NULL_MESSAGE(a, "both aliased names must be emitted");
    TEST_ASSERT_NOT_NULL_MESSAGE(b, "both aliased names must be emitted");
    /* exactly one links to the other's final name (the original). */
    cJSON *ab = cJSON_GetObjectItemCaseSensitive(b, "alias_of");
    cJSON *aa = cJSON_GetObjectItemCaseSensitive(a, "alias_of");
    TEST_ASSERT_TRUE(cJSON_IsString(ab) && strcmp(ab->valuestring, "alias_a") == 0);
    TEST_ASSERT_TRUE(cJSON_IsNull(aa));
    cJSON_Delete(root);
}

void test_multipage_fidelity(void) {
    jc *j = find_case("multipage");
    TEST_ASSERT_NOT_NULL(j);
    cJSON *root = load_json(j->json);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    TEST_ASSERT_TRUE(cJSON_GetArraySize(pages) >= 2);
    /* sprites reference >= 2 distinct page indices. */
    bool seen[64] = {false};
    int distinct = 0;
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    cJSON *sp = NULL;
    cJSON_ArrayForEach(sp, sprites) {
        int p = cJSON_GetObjectItemCaseSensitive(sp, "page")->valueint;
        if (p >= 0 && p < 64 && !seen[p]) {
            seen[p] = true;
            distinct++;
        }
    }
    TEST_ASSERT_TRUE(distinct >= 2);
    cJSON_Delete(root);
}

void test_determinism_reexport(void) {
    /* Snapshot each case's json + pages, then re-export to the SAME base
     * (overwrite) and assert byte-identical output. Same base => the embedded
     * page filenames are identical, so this is a true determinism check. */
    for (int i = 0; i < gn; i++) {
        char jpath[1088];
        (void)snprintf(jpath, sizeof jpath, "%s.json", g[i].base);
        size_t n1 = 0;
        uint8_t *b1 = read_whole_file(jpath, &n1);
        char m[128];
        (void)snprintf(m, sizeof m, "case '%s' json determinism", g[i].cs->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(b1, m);

        int npages = g[i].res->page_count;
        uint8_t *pg1[16] = {0};
        size_t pn1[16] = {0};
        for (int p = 0; p < npages; p++) {
            char pp[1104];
            (void)snprintf(pp, sizeof pp, "%s-%d.png", g[i].base, p);
            pg1[p] = read_whole_file(pp, &pn1[p]);
            TEST_ASSERT_NOT_NULL_MESSAGE(pg1[p], "page png snapshot");
        }

        tp_export_prepared prep;
        tp_error e = {{0}};
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(g[i].res, NULL, g_arena, &prep, &e));
        tp_export_caps caps = tp_export_caps_full();
        tp_export_notices notices;
        tp_export_notices_init(&notices);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_json_neotolis_write(&prep, &caps, g[i].base, &notices, &e));
        tp_export_notices_free(&notices);

        size_t n2 = 0;
        uint8_t *b2 = read_whole_file(jpath, &n2);
        TEST_ASSERT_NOT_NULL_MESSAGE(b2, m);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n1, (uint32_t)n2, m);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(b1, b2, n1), m);
        free(b1);
        free(b2);

        for (int p = 0; p < npages; p++) {
            char pp[1104];
            (void)snprintf(pp, sizeof pp, "%s-%d.png", g[i].base, p);
            size_t pn2 = 0;
            uint8_t *pg2 = read_whole_file(pp, &pn2);
            TEST_ASSERT_NOT_NULL_MESSAGE(pg2, "page png determinism");
            TEST_ASSERT_EQUAL_UINT32(pn1[p], pn2);
            TEST_ASSERT_EQUAL_INT(0, memcmp(pg1[p], pg2, pn1[p]));
            free(pg1[p]);
            free(pg2);
        }
    }
}
// #endregion

static bool setup_all(const char *dir) {
    g_dir = dir;
    int n = 0;
    const tp_fixture_case *cases = tp_fixtures_all(&n);
    if (n <= 0 || n > (int)(sizeof g / sizeof g[0])) {
        return false;
    }
    g_arena = tp_arena_create(0);
    g_names = tp_name_map_create();
    if (!g_arena || !g_names || !tp_fixtures_register_names(g_names)) {
        return false;
    }
    gn = n;
    for (int i = 0; i < n; i++) {
        g[i].cs = &cases[i];
        char path[1024];
        if (!tp_fixture_build(&cases[i], dir, path, sizeof path)) {
            (void)fprintf(stderr, "build failed for %s\n", cases[i].name);
            return false;
        }
        tp_result **results = NULL;
        int count = 0;
        tp_error err = {{0}};
        if (tp_pack_read_file(path, g_names, g_arena, &results, &count, &err) != TP_STATUS_OK || count != 1) {
            (void)fprintf(stderr, "parse failed for %s: %s\n", cases[i].name, err.msg);
            return false;
        }
        g[i].res = results[0];
        (void)snprintf(g[i].base, sizeof g[i].base, "%s/js_%s", dir, cases[i].name);
        (void)snprintf(g[i].json, sizeof g[i].json, "%s.json", g[i].base);
        if (!export_case(&g[i])) {
            (void)fprintf(stderr, "export/notice check failed for %s\n", cases[i].name);
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    if (!setup_all(dir)) {
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_structure_all_cases);
    RUN_TEST(test_transform_fidelity);
    RUN_TEST(test_polygon_fidelity);
    RUN_TEST(test_slice9_fidelity);
    RUN_TEST(test_pivot_fidelity);
    RUN_TEST(test_alias_fidelity);
    RUN_TEST(test_multipage_fidelity);
    RUN_TEST(test_determinism_reexport);
    int rc = UNITY_END();
    tp_arena_destroy(g_arena);
    tp_name_map_destroy(g_names);
    return rc;
}
