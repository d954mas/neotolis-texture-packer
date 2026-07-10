/* Defold exporter tests (ROADMAP Phase 5). Proves:
 *   (a) golden  -- exact `.tpinfo` + `.tpatlas` bytes for a hand-built result
 *                  (untrimmed rect, trimmed rect, pivot px, is_solid true/false);
 *   (b) rotated -- the one representable 90-degree mask emits rotated:true with a
 *                  swapped frame_rect and an unrotated source_rect (direct writer
 *                  call; the v1 pipeline clamps Defold to identity);
 *   (c) determinism -- real fixtures packed via nt_builder, exported twice,
 *                  byte-identical .tpinfo/.tpatlas/pages;
 *   (d) caps/repack -- tp_export_run over an allow_transform atlas repacks the
 *                  Defold target IDENTITY-only (no rotated:true) and drops 9-slice
 *                  with a notice, while json-neotolis keeps the rotation;
 *   (e) animations -- playback enum table (all 7 ids + out-of-range), fps/flip
 *                  mapping, and every sprite name present so bob can auto-promote
 *                  it to a 1-frame animation;
 *   (f) demo     -- the exporter runs clean over the three real defold-demo
 *                  atlases (basic/BoxFlip, rotate, anim_trim/10-frame) with the
 *                  same fields as the upstream .tpinfo. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_pack_read.h"
#include "tp_core/tp_project.h"
#include "unity.h"

#include "tp_fixtures.h"

static const char *g_dir;      /* scratch output dir (argv[1]) */
static const char *g_demo_dir; /* defold-demo asset dir (argv[2] or compile def) */

void setUp(void) {}
void tearDown(void) {}

// #region helpers
static char *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        (void)fclose(f);
        return NULL;
    }
    char *b = (char *)malloc((size_t)sz + 1U);
    size_t rd = b ? fread(b, 1, (size_t)sz, f) : 0;
    (void)fclose(f);
    if (!b || rd != (size_t)sz) {
        free(b);
        return NULL;
    }
    b[sz] = '\0';
    if (out_size) {
        *out_size = (size_t)sz;
    }
    return b;
}

static int count_occurrences(const char *hay, const char *needle) {
    int n = 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl) {
        n++;
    }
    return n;
}

static const tp_export_caps *defold_caps(void) {
    const tp_exporter *e = tp_exporter_find("defold");
    return e ? &e->caps : NULL;
}
// #endregion

// #region (a) golden ---------------------------------------------------------
/* Hand-built one-page atlas: hero (16x16 untrimmed, opaque -> is_solid) and gem
 * (16x16 source trimmed to 8x6 at offset 2,3, one transparent texel -> not
 * solid). Page 24x16. One explicit flipbook "spin". Every emitted value is
 * hand-predictable so the golden never churns with the packer. */

static uint8_t g_golden_page[24 * 16 * 4];

static void golden_fill_page(void) {
    memset(g_golden_page, 0, sizeof g_golden_page);
    /* hero region (0..15, 0..15): fully opaque */
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint8_t *p = &g_golden_page[((size_t)y * 24 + (size_t)x) * 4];
            p[0] = 100;
            p[1] = 100;
            p[2] = 100;
            p[3] = 255;
        }
    }
    /* gem region (16..23, 0..5): opaque except one transparent texel at (16,0) */
    for (int y = 0; y < 6; y++) {
        for (int x = 16; x < 24; x++) {
            uint8_t *p = &g_golden_page[((size_t)y * 24 + (size_t)x) * 4];
            p[0] = 50;
            p[1] = 60;
            p[2] = 70;
            p[3] = (x == 16 && y == 0) ? 0 : 255;
        }
    }
}

static const char *const EXPECTED_TPINFO =
    "# Exported by neotolis-texture-packer\n"
    "# Format: Defold extension-texturepacker .tpinfo (protobuf text)\n"
    "\n"
    "version: \"2.0\"\n"
    "description: \"Exported using neotolis-texture-packer\"\n"
    "pages {\n"
    "  name: \"defold_golden-0.png\"\n"
    "  size {\n"
    "    width: 24\n"
    "    height: 16\n"
    "  }\n"
    "  sprites {\n"
    "    name: \"gem\"\n"
    "    trimmed: true\n"
    "    rotated: false\n"
    "    is_solid: false\n"
    "    corner_offset {\n"
    "      x: 2\n"
    "      y: 3\n"
    "    }\n"
    "    source_rect {\n"
    "      x: 2\n"
    "      y: 3\n"
    "      width: 8\n"
    "      height: 6\n"
    "    }\n"
    "    pivot {\n"
    "      x: 8\n"
    "      y: 8\n"
    "    }\n"
    "    frame_rect {\n"
    "      x: 16\n"
    "      y: 0\n"
    "      width: 8\n"
    "      height: 6\n"
    "    }\n"
    "    untrimmed_size {\n"
    "      width: 16\n"
    "      height: 16\n"
    "    }\n"
    "    indices: [1, 2, 3, 0, 1, 3]\n"
    "    vertices {\n"
    "      x: 10\n"
    "      y: 3\n"
    "    }\n"
    "    vertices {\n"
    "      x: 2\n"
    "      y: 3\n"
    "    }\n"
    "    vertices {\n"
    "      x: 2\n"
    "      y: 9\n"
    "    }\n"
    "    vertices {\n"
    "      x: 10\n"
    "      y: 9\n"
    "    }\n"
    "  }\n"
    "  sprites {\n"
    "    name: \"hero\"\n"
    "    trimmed: false\n"
    "    rotated: false\n"
    "    is_solid: true\n"
    "    corner_offset {\n"
    "      x: 0\n"
    "      y: 0\n"
    "    }\n"
    "    source_rect {\n"
    "      x: 0\n"
    "      y: 0\n"
    "      width: 16\n"
    "      height: 16\n"
    "    }\n"
    "    pivot {\n"
    "      x: 8\n"
    "      y: 8\n"
    "    }\n"
    "    frame_rect {\n"
    "      x: 0\n"
    "      y: 0\n"
    "      width: 16\n"
    "      height: 16\n"
    "    }\n"
    "    untrimmed_size {\n"
    "      width: 16\n"
    "      height: 16\n"
    "    }\n"
    "    indices: [1, 2, 3, 0, 1, 3]\n"
    "    vertices {\n"
    "      x: 16\n"
    "      y: 0\n"
    "    }\n"
    "    vertices {\n"
    "      x: 0\n"
    "      y: 0\n"
    "    }\n"
    "    vertices {\n"
    "      x: 0\n"
    "      y: 16\n"
    "    }\n"
    "    vertices {\n"
    "      x: 16\n"
    "      y: 16\n"
    "    }\n"
    "  }\n"
    "}\n";

static const char *const EXPECTED_TPATLAS =
    "file: \"defold_golden.tpinfo\"\n"
    "rename_patterns: \"\"\n"
    "animations {\n"
    "  id: \"spin\"\n"
    "  images: \"hero\"\n"
    "  images: \"gem\"\n"
    "  playback: PLAYBACK_LOOP_FORWARD\n"
    "  fps: 12\n"
    "  flip_horizontal: 1\n"
    "  flip_vertical: 0\n"
    "}\n"
    "is_paged_atlas: false\n";

static tp_result golden_result(tp_sprite sprites[2], tp_page *page) {
    memset(sprites, 0, sizeof(tp_sprite) * 2);
    /* hero: untrimmed 16x16 at (0,0) */
    sprites[0].name = "hero";
    sprites[0].page = 0;
    sprites[0].frame.x = 0;
    sprites[0].frame.y = 0;
    sprites[0].frame.w = 16;
    sprites[0].frame.h = 16;
    sprites[0].trimmed = false;
    sprites[0].spriteSourceSize.x = 0;
    sprites[0].spriteSourceSize.y = 0;
    sprites[0].spriteSourceSize.w = 16;
    sprites[0].spriteSourceSize.h = 16;
    sprites[0].sourceSize.w = 16;
    sprites[0].sourceSize.h = 16;
    sprites[0].pivot.x = 0.5F;
    sprites[0].pivot.y = 0.5F;
    sprites[0].alias_of = -1;
    /* gem: 16x16 source, trimmed to 8x6 at (2,3), placed at (16,0) */
    sprites[1].name = "gem";
    sprites[1].page = 0;
    sprites[1].frame.x = 16;
    sprites[1].frame.y = 0;
    sprites[1].frame.w = 8;
    sprites[1].frame.h = 6;
    sprites[1].trimmed = true;
    sprites[1].spriteSourceSize.x = 2;
    sprites[1].spriteSourceSize.y = 3;
    sprites[1].spriteSourceSize.w = 8;
    sprites[1].spriteSourceSize.h = 6;
    sprites[1].sourceSize.w = 16;
    sprites[1].sourceSize.h = 16;
    sprites[1].pivot.x = 0.5F;
    sprites[1].pivot.y = 0.5F;
    sprites[1].alias_of = -1;

    golden_fill_page();
    memset(page, 0, sizeof *page);
    page->image_name = "defold_golden";
    page->w = 24;
    page->h = 16;
    page->rgba = g_golden_page;
    page->premultiplied = false;

    tp_result r;
    memset(&r, 0, sizeof r);
    r.atlas_name = "defold_golden";
    r.pixels_per_unit = 1.0F;
    r.pages = page;
    r.page_count = 1;
    r.sprites = sprites;
    r.sprite_count = 2;
    return r;
}

void test_golden_bytes(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_sprite sprites[2];
    tp_page page;
    tp_result r = golden_result(sprites, &page);

    const char *spin_frames[2] = {"hero", "gem"};
    tp_export_anim_in spin = {.id = "spin",
                              .frames = spin_frames,
                              .frame_count = 2,
                              .fps = 12.0F,
                              .playback = 1,
                              .flip_h = true,
                              .flip_v = false};
    tp_normalize_opts opts;
    tp_normalize_opts_defaults(&opts);
    opts.animations = &spin;
    opts.animation_count = 1;

    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_normalize(&r, &opts, ar, &prep, &e), e.msg);

    tp_export_notices notices;
    tp_export_notices_init(&notices);
    char base[1024];
    (void)snprintf(base, sizeof base, "%s/defold_golden", g_dir);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_export_defold_write(&prep, defold_caps(), base, &notices, &e), e.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, notices.count, "full-fidelity golden must raise zero notices");
    tp_export_notices_free(&notices);

    char path[1088];
    (void)snprintf(path, sizeof path, "%s.tpinfo", base);
    size_t n = 0;
    char *got = read_whole_file(path, &n);
    TEST_ASSERT_NOT_NULL_MESSAGE(got, "golden .tpinfo readable");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(EXPECTED_TPINFO, got, ".tpinfo bytes must match golden");
    free(got);

    (void)snprintf(path, sizeof path, "%s.tpatlas", base);
    got = read_whole_file(path, &n);
    TEST_ASSERT_NOT_NULL_MESSAGE(got, "golden .tpatlas readable");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(EXPECTED_TPATLAS, got, ".tpatlas bytes must match golden");
    free(got);

    tp_arena_destroy(ar);
}
// #endregion

// #region (b) rotated geometry ----------------------------------------------
/* Direct writer call with the one representable 90-degree mask (DIAGONAL|FLIP_H).
 * The v1 pipeline never produces this for Defold (identity clamp), so this pins
 * the writer's rotation math against the .tpinfo convention for the future
 * rotation-only engine policy. */
void test_rotated_geometry(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);

    static uint8_t px[8 * 8 * 4];
    for (size_t i = 0; i < sizeof px / 4; i++) {
        px[i * 4 + 3] = 255;
    }
    tp_page page;
    memset(&page, 0, sizeof page);
    page.image_name = "rot";
    page.w = 8;
    page.h = 8;
    page.rgba = px;

    tp_sprite s;
    memset(&s, 0, sizeof s);
    s.name = "r";
    s.page = 0;
    /* frame w/h are UNROTATED trim dims (8x4); footprint on page is 4x8 */
    s.frame.x = 0;
    s.frame.y = 0;
    s.frame.w = 8;
    s.frame.h = 4;
    s.transform = (uint8_t)(TP_TRANSFORM_DIAGONAL | TP_TRANSFORM_FLIP_H);
    s.trimmed = false;
    s.spriteSourceSize.w = 8;
    s.spriteSourceSize.h = 4;
    s.sourceSize.w = 8;
    s.sourceSize.h = 4;
    s.pivot.x = 0.5F;
    s.pivot.y = 0.5F;
    s.alias_of = -1;

    tp_result r;
    memset(&r, 0, sizeof r);
    r.atlas_name = "rot";
    r.pixels_per_unit = 1.0F;
    r.pages = &page;
    r.page_count = 1;
    r.sprites = &s;
    r.sprite_count = 1;

    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, NULL, ar, &prep, &e));
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    char base[1024];
    (void)snprintf(base, sizeof base, "%s/defold_rot", g_dir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_defold_write(&prep, defold_caps(), base, &notices, &e));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, notices.count, "representable rotation raises no notice");
    tp_export_notices_free(&notices);

    char path[1088];
    (void)snprintf(path, sizeof path, "%s.tpinfo", base);
    char *got = read_whole_file(path, NULL);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_TRUE_MESSAGE(strstr(got, "rotated: true") != NULL, "diagonal|flipH must map to rotated:true");
    /* frame_rect footprint swapped to 4x8; source_rect stays unrotated 8x4 */
    TEST_ASSERT_TRUE_MESSAGE(strstr(got, "frame_rect {\n      x: 0\n      y: 0\n      width: 4\n      height: 8\n    }") != NULL,
                             "rotated frame_rect swaps width/height");
    TEST_ASSERT_TRUE_MESSAGE(strstr(got, "source_rect {\n      x: 0\n      y: 0\n      width: 8\n      height: 4\n    }") != NULL,
                             "source_rect stays unrotated");
    free(got);
    tp_arena_destroy(ar);
}
// #endregion

// #region (c) determinism ----------------------------------------------------
static bool export_fixture(const char *case_name, const char *base, tp_arena *ar, tp_name_map *names, tp_error *e) {
    char pack[1024];
    int i = 0, n = 0;
    const tp_fixture_case *cases = tp_fixtures_all(&n);
    const tp_fixture_case *cs = NULL;
    for (i = 0; i < n; i++) {
        if (strcmp(cases[i].name, case_name) == 0) {
            cs = &cases[i];
            break;
        }
    }
    if (!cs || !tp_fixture_build(cs, g_dir, pack, sizeof pack)) {
        return false;
    }
    tp_result **results = NULL;
    int count = 0;
    if (tp_pack_read_file(pack, names, ar, &results, &count, e) != TP_STATUS_OK || count != 1) {
        return false;
    }
    tp_export_prepared prep;
    if (tp_normalize(results[0], NULL, ar, &prep, e) != TP_STATUS_OK) {
        return false;
    }
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_status st = tp_export_defold_write(&prep, defold_caps(), base, &notices, e);
    tp_export_notices_free(&notices);
    return st == TP_STATUS_OK;
}

static void assert_same_file(const char *a, const char *b, const char *what) {
    size_t na = 0, nb = 0;
    char *ba = read_whole_file(a, &na);
    char *bb = read_whole_file(b, &nb);
    TEST_ASSERT_NOT_NULL_MESSAGE(ba, what);
    TEST_ASSERT_NOT_NULL_MESSAGE(bb, what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)na, (uint32_t)nb, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(ba, bb, na), what);
    free(ba);
    free(bb);
}

void test_determinism_byte_identical(void) {
    /* A cross-section of packer output: trimmed, multipage (>1 page), a real
     * concave polygon hull, and an alias. Export each to one base, snapshot the
     * artifacts, re-export to the SAME base, and byte-compare .tpinfo/.tpatlas and
     * page 0 (same base => identical embedded page names, a true determinism check). */
    tp_arena *ar = tp_arena_create(0);
    tp_name_map *names = tp_name_map_create();
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_NOT_NULL(names);
    TEST_ASSERT_TRUE(tp_fixtures_register_names(names));

    static const char *const kCases[] = {"trimmed", "multipage", "polygon", "alias"};
    for (int c = 0; c < (int)(sizeof kCases / sizeof kCases[0]); c++) {
        char base[1024];
        (void)snprintf(base, sizeof base, "%s/byte_%s", g_dir, kCases[c]);
        tp_error e = {{0}};
        TEST_ASSERT_TRUE(export_fixture(kCases[c], base, ar, names, &e));

        /* snapshot .tpinfo + .tpatlas + page 0 */
        char tpinfo[1088], tpatlas[1088], png[1104], snap_i[1120], snap_a[1120], snap_p[1136];
        (void)snprintf(tpinfo, sizeof tpinfo, "%s.tpinfo", base);
        (void)snprintf(tpatlas, sizeof tpatlas, "%s.tpatlas", base);
        (void)snprintf(png, sizeof png, "%s-0.png", base);
        (void)snprintf(snap_i, sizeof snap_i, "%s.snap_i", base);
        (void)snprintf(snap_a, sizeof snap_a, "%s.snap_a", base);
        (void)snprintf(snap_p, sizeof snap_p, "%s.snap_p", base);
        /* copy originals to snapshot paths */
        for (int k = 0; k < 3; k++) {
            const char *src = (k == 0) ? tpinfo : (k == 1) ? tpatlas : png;
            const char *dst = (k == 0) ? snap_i : (k == 1) ? snap_a : snap_p;
            size_t sz = 0;
            char *b = read_whole_file(src, &sz);
            TEST_ASSERT_NOT_NULL(b);
            FILE *f = fopen(dst, "wb");
            TEST_ASSERT_NOT_NULL(f);
            (void)fwrite(b, 1, sz, f);
            (void)fclose(f);
            free(b);
        }
        /* re-export to the same base */
        TEST_ASSERT_TRUE(export_fixture(kCases[c], base, ar, names, &e));
        assert_same_file(tpinfo, snap_i, ".tpinfo determinism");
        assert_same_file(tpatlas, snap_a, ".tpatlas determinism");
        assert_same_file(png, snap_p, "page png determinism");
    }
    tp_arena_destroy(ar);
    tp_name_map_destroy(names);
}
// #endregion

// #region (d) caps / repack --------------------------------------------------
static uint8_t g_wide[120 * 24 * 4];
static uint8_t g_tall[24 * 100 * 4];
static uint8_t g_panel[40 * 40 * 4];

static void fill_rgba(uint8_t *p, int n, uint8_t r, uint8_t gg, uint8_t b) {
    for (int i = 0; i < n; i++) {
        p[i * 4 + 0] = r;
        p[i * 4 + 1] = gg;
        p[i * 4 + 2] = b;
        p[i * 4 + 3] = 255;
    }
}

void test_caps_repack_identity_and_slice9_notice(void) {
    fill_rgba(g_wide, 120 * 24, 200, 60, 60);
    fill_rgba(g_tall, 24 * 100, 60, 200, 60);
    fill_rgba(g_panel, 40 * 40, 60, 60, 200);

    tp_project *proj = tp_project_create();
    TEST_ASSERT_NOT_NULL(proj);
    tp_project_atlas *a = tp_project_get_atlas(proj, 0);
    a->shape = 0; /* RECT */
    a->allow_transform = true;
    a->power_of_two = false;
    a->padding = 0;
    a->margin = 0;
    a->alpha_threshold = 1;
    a->max_size = 1024;
    a->pixels_per_unit = 1.0F;

    char json_base[1024], defold_base[1024];
    (void)snprintf(json_base, sizeof json_base, "%s/caps_json", g_dir);
    (void)snprintf(defold_base, sizeof defold_base, "%s/caps_defold", g_dir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", json_base, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "defold", defold_base, NULL));

    tp_pack_sprite_desc sprites[3];
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "wide", .rgba = g_wide, .w = 120, .h = 24, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[1] = (tp_pack_sprite_desc){.name = "tall", .rgba = g_tall, .w = 24, .h = 100, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[2] = (tp_pack_sprite_desc){.name = "panel", .rgba = g_panel, .w = 40, .h = 40, .origin_x = 0.5F, .origin_y = 0.5F,
                                       .slice9_lrtb = {6, 7, 8, 9}};

    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    int pack_runs = 0;
    tp_error e = {{0}};
    tp_status st = tp_export_run(proj, 0, sprites, 3, g_dir, ar, &notices, &pack_runs, &e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, e.msg);

    /* json (full D4) and defold (identity) have different effective settings -> 2 runs. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, pack_runs, "json rotation vs defold identity => two pack runs");

    /* defold repacked identity-only: no rotated:true anywhere. */
    char path[1088];
    (void)snprintf(path, sizeof path, "%s.tpinfo", defold_base);
    char *tpinfo = read_whole_file(path, NULL);
    TEST_ASSERT_NOT_NULL(tpinfo);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_occurrences(tpinfo, "rotated: true"),
                                  "defold target must repack identity-only (no rotation)");
    /* every sprite present; 9-slice is NOT a field in .tpinfo. */
    TEST_ASSERT_TRUE(strstr(tpinfo, "name: \"panel\"") != NULL);
    TEST_ASSERT_NULL_MESSAGE(strstr(tpinfo, "slice9"), ".tpinfo has no 9-slice field");
    free(tpinfo);

    /* json (full caps) DID rotate a sprite -- proves a real repack, not a filter. */
    (void)snprintf(path, sizeof path, "%s.json", json_base);
    char *json = read_whole_file(path, NULL);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE_MESSAGE(strstr(json, "\"transform\"") != NULL, "json-neotolis keeps the packer's rotation");
    free(json);

    /* dropping the panel's 9-slice raised a metadata-loss notice (never an error). */
    bool slice9_notice = false;
    for (int i = 0; i < notices.count; i++) {
        if (strstr(notices.items[i].msg, "slice9")) {
            slice9_notice = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(slice9_notice, "defold must drop 9-slice with a notice");

    tp_export_notices_free(&notices);
    tp_project_destroy(proj);
    tp_arena_destroy(ar);
}
// #endregion

// #region (e) animations -----------------------------------------------------
void test_playback_enum_and_flags(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);

    /* one sprite so the atlas is valid; 7 explicit anims (playback 0..6) + one
     * out-of-range (99). */
    tp_sprite s;
    memset(&s, 0, sizeof s);
    s.name = "a";
    s.frame.w = 8;
    s.frame.h = 8;
    s.spriteSourceSize.w = 8;
    s.spriteSourceSize.h = 8;
    s.sourceSize.w = 8;
    s.sourceSize.h = 8;
    s.pivot.x = 0.5F;
    s.pivot.y = 0.5F;
    s.alias_of = -1;
    static uint8_t px[8 * 8 * 4];
    for (size_t i = 0; i < sizeof px / 4; i++) {
        px[i * 4 + 3] = 255;
    }
    tp_page page;
    memset(&page, 0, sizeof page);
    page.image_name = "a";
    page.w = 8;
    page.h = 8;
    page.rgba = px;
    tp_result r;
    memset(&r, 0, sizeof r);
    r.atlas_name = "anims";
    r.pixels_per_unit = 1.0F;
    r.pages = &page;
    r.page_count = 1;
    r.sprites = &s;
    r.sprite_count = 1;

    const char *frame_a[1] = {"a"};
    tp_export_anim_in in[8];
    for (int i = 0; i < 8; i++) {
        memset(&in[i], 0, sizeof in[i]);
        in[i].frames = frame_a;
        in[i].frame_count = 1;
        in[i].fps = 24.0F;
    }
    in[0].id = "p0"; in[0].playback = 0;
    in[1].id = "p1"; in[1].playback = 1;
    in[2].id = "p2"; in[2].playback = 2;
    in[3].id = "p3"; in[3].playback = 3;
    in[4].id = "p4"; in[4].playback = 4;
    in[5].id = "p5"; in[5].playback = 5;
    in[6].id = "p6"; in[6].playback = 6;
    in[7].id = "pX"; in[7].playback = 99; in[7].flip_h = true; in[7].flip_v = true;

    tp_normalize_opts opts;
    tp_normalize_opts_defaults(&opts);
    opts.animations = in;
    opts.animation_count = 8;

    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, &opts, ar, &prep, &e));
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    char base[1024];
    (void)snprintf(base, sizeof base, "%s/defold_anims", g_dir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_defold_write(&prep, defold_caps(), base, &notices, &e));

    char path[1088];
    (void)snprintf(path, sizeof path, "%s.tpatlas", base);
    char *tpa = read_whole_file(path, NULL);
    TEST_ASSERT_NOT_NULL(tpa);
    TEST_ASSERT_TRUE(strstr(tpa, "playback: PLAYBACK_ONCE_FORWARD") != NULL);
    TEST_ASSERT_TRUE(strstr(tpa, "playback: PLAYBACK_LOOP_FORWARD") != NULL);
    TEST_ASSERT_TRUE(strstr(tpa, "playback: PLAYBACK_ONCE_BACKWARD") != NULL);
    TEST_ASSERT_TRUE(strstr(tpa, "playback: PLAYBACK_LOOP_BACKWARD") != NULL);
    TEST_ASSERT_TRUE(strstr(tpa, "playback: PLAYBACK_ONCE_PINGPONG") != NULL);
    TEST_ASSERT_TRUE(strstr(tpa, "playback: PLAYBACK_LOOP_PINGPONG") != NULL);
    TEST_ASSERT_TRUE(strstr(tpa, "playback: PLAYBACK_NONE") != NULL);
    /* out-of-range playback -> ONCE_FORWARD + a notice; flips map to 1. */
    TEST_ASSERT_TRUE(strstr(tpa, "flip_horizontal: 1") != NULL);
    TEST_ASSERT_TRUE(strstr(tpa, "flip_vertical: 1") != NULL);
    /* PLAYBACK_ONCE_FORWARD appears twice (p0 + pX fallback). */
    TEST_ASSERT_EQUAL_INT(2, count_occurrences(tpa, "playback: PLAYBACK_ONCE_FORWARD"));
    free(tpa);

    bool unknown_notice = false;
    for (int i = 0; i < notices.count; i++) {
        if (strstr(notices.items[i].msg, "unknown playback")) {
            unknown_notice = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(unknown_notice, "out-of-range playback raises a notice");
    tp_export_notices_free(&notices);

    /* implicit 1-frame anims: the sprite name must appear in the .tpinfo so bob
     * can auto-promote it (we do NOT emit implicit anims into the .tpatlas). */
    (void)snprintf(path, sizeof path, "%s.tpinfo", base);
    char *tpi = read_whole_file(path, NULL);
    TEST_ASSERT_NOT_NULL(tpi);
    TEST_ASSERT_TRUE_MESSAGE(strstr(tpi, "name: \"a\"") != NULL, "sprite name present for implicit 1-frame animation");
    free(tpi);

    tp_arena_destroy(ar);
}
// #endregion

// #region (f) real demo assets ----------------------------------------------
typedef struct {
    const char *sprite_name;
    const char *rel_png; /* relative to the atlas source dir */
} demo_file;

typedef struct {
    const char *atlas;
    const char *src_subdir; /* under g_demo_dir */
    const demo_file *files;
    int file_count;
    const char *anim_id; /* NULL = no explicit animation */
    const char *const *anim_frames;
    int anim_frame_count;
    int anim_playback;
    float anim_fps;
} demo_atlas;

static bool pack_demo_atlas(const demo_atlas *da, tp_arena *ar) {
    /* decode PNGs */
    tp_pack_sprite_desc *descs = (tp_pack_sprite_desc *)calloc((size_t)da->file_count, sizeof(tp_pack_sprite_desc));
    uint8_t **pixels = (uint8_t **)calloc((size_t)da->file_count, sizeof(uint8_t *));
    if (!descs || !pixels) {
        free(descs);
        free(pixels);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < da->file_count && ok; i++) {
        char png[1024];
        (void)snprintf(png, sizeof png, "%s/%s/%s", g_demo_dir, da->src_subdir, da->files[i].rel_png);
        int w = 0, h = 0, ch = 0;
        pixels[i] = stbi_load(png, &w, &h, &ch, 4);
        if (!pixels[i]) {
            (void)fprintf(stderr, "demo: cannot decode %s\n", png);
            ok = false;
            break;
        }
        descs[i].name = da->files[i].sprite_name;
        descs[i].rgba = pixels[i];
        descs[i].w = w;
        descs[i].h = h;
        descs[i].origin_x = 0.5F;
        descs[i].origin_y = 0.5F;
    }

    if (ok) {
        tp_pack_settings base;
        (void)tp_pack_settings_defaults(&base);
        base.atlas_name = da->atlas;
        base.work_dir = g_dir;
        base.sprites = descs;
        base.sprite_count = da->file_count;
        base.pixels_per_unit = 1.0F;

        /* effective settings for the defold target (identity-only clamp). */
        tp_pack_settings eff;
        tp_error e = {{0}};
        ok = tp_export_effective_settings(&base, defold_caps(), &eff) == TP_STATUS_OK;

        tp_result *res = NULL;
        if (ok) {
            ok = tp_pack(&eff, ar, &res, &e) == TP_STATUS_OK;
            if (!ok) {
                (void)fprintf(stderr, "demo: pack '%s' failed: %s\n", da->atlas, e.msg);
            }
        }

        tp_normalize_opts opts;
        tp_normalize_opts_defaults(&opts);
        tp_export_anim_in anim;
        if (da->anim_id) {
            memset(&anim, 0, sizeof anim);
            anim.id = da->anim_id;
            anim.frames = da->anim_frames;
            anim.frame_count = da->anim_frame_count;
            anim.playback = da->anim_playback;
            anim.fps = da->anim_fps;
            opts.animations = &anim;
            opts.animation_count = 1;
        }
        /* explicit animations only for the demo (matches ux.md export ruling). */
        opts.auto_group_animations = false;

        tp_export_prepared prep;
        if (ok) {
            ok = tp_normalize(res, &opts, ar, &prep, &e) == TP_STATUS_OK;
        }
        if (ok) {
            char b[1024];
            (void)snprintf(b, sizeof b, "%s/demo_%s", g_dir, da->atlas);
            tp_export_notices notices;
            tp_export_notices_init(&notices);
            ok = tp_export_defold_write(&prep, defold_caps(), b, &notices, &e) == TP_STATUS_OK;
            tp_export_notices_free(&notices);
            if (!ok) {
                (void)fprintf(stderr, "demo: export '%s' failed: %s\n", da->atlas, e.msg);
            }
        }
    }

    for (int i = 0; i < da->file_count; i++) {
        if (pixels[i]) {
            stbi_image_free(pixels[i]);
        }
    }
    free(descs);
    free(pixels);
    return ok;
}

static void assert_demo_structure(const char *atlas, const demo_file *files, int file_count, const char *anim_id,
                                  const char *playback_token) {
    char path[1088];
    (void)snprintf(path, sizeof path, "%s/demo_%s.tpinfo", g_dir, atlas);
    char *tpi = read_whole_file(path, NULL);
    char msg[160];
    (void)snprintf(msg, sizeof msg, "demo '%s' .tpinfo readable", atlas);
    TEST_ASSERT_NOT_NULL_MESSAGE(tpi, msg);

    /* same fields as the upstream .tpinfo */
    TEST_ASSERT_TRUE_MESSAGE(strstr(tpi, "version: \"2.0\"") != NULL, "version present");
    TEST_ASSERT_TRUE_MESSAGE(strstr(tpi, "pages {") != NULL, "pages present");
    const char *const need[] = {"corner_offset {", "source_rect {", "pivot {", "frame_rect {",
                                "untrimmed_size {", "is_solid:", "rotated:", "trimmed:"};
    for (int i = 0; i < (int)(sizeof need / sizeof need[0]); i++) {
        (void)snprintf(msg, sizeof msg, "demo '%s' field '%s'", atlas, need[i]);
        TEST_ASSERT_TRUE_MESSAGE(strstr(tpi, need[i]) != NULL, msg);
    }
    /* identity-only (Defold clamp) -- no baked rotation slipped through. */
    (void)snprintf(msg, sizeof msg, "demo '%s' identity-only", atlas);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_occurrences(tpi, "rotated: true"), msg);
    /* every input sprite present (so every one is a usable 1-frame animation). */
    for (int i = 0; i < file_count; i++) {
        char want[128];
        (void)snprintf(want, sizeof want, "name: \"%s\"", files[i].sprite_name);
        (void)snprintf(msg, sizeof msg, "demo '%s' sprite '%s'", atlas, files[i].sprite_name);
        TEST_ASSERT_TRUE_MESSAGE(strstr(tpi, want) != NULL, msg);
    }
    free(tpi);

    (void)snprintf(path, sizeof path, "%s/demo_%s.tpatlas", g_dir, atlas);
    char *tpa = read_whole_file(path, NULL);
    (void)snprintf(msg, sizeof msg, "demo '%s' .tpatlas readable", atlas);
    TEST_ASSERT_NOT_NULL_MESSAGE(tpa, msg);
    TEST_ASSERT_TRUE_MESSAGE(strstr(tpa, "is_paged_atlas:") != NULL, "tpatlas has is_paged_atlas");
    if (anim_id) {
        char want[128];
        (void)snprintf(want, sizeof want, "id: \"%s\"", anim_id);
        (void)snprintf(msg, sizeof msg, "demo '%s' animation '%s'", atlas, anim_id);
        TEST_ASSERT_TRUE_MESSAGE(strstr(tpa, want) != NULL, msg);
        (void)snprintf(msg, sizeof msg, "demo '%s' playback '%s'", atlas, playback_token);
        TEST_ASSERT_TRUE_MESSAGE(strstr(tpa, playback_token) != NULL, msg);
    }
    free(tpa);
}

void test_demo_atlases(void) {
    if (!g_demo_dir || g_demo_dir[0] == '\0') {
        TEST_IGNORE_MESSAGE("no defold-demo dir provided");
        return;
    }
    static const demo_file basic_files[] = {
        {"box_fill_128", "box_fill_128.png"},   {"box_fill_64", "box_fill_64.png"},
        {"box_small_128", "box_small_128.png"}, {"circle_fill_128", "circle_fill_128.png"},
        {"circle_fill_64", "circle_fill_64.png"}, {"shape_L_128", "shape_L_128.png"},
        {"triangle_fill_128", "triangle_fill_128.png"}, {"triangle_fill_64", "triangle_fill_64.png"},
        {"test-0", "anim/test-0.png"}, {"test-1", "anim/test-1.png"}, {"test-2", "anim/test-2.png"}};
    static const demo_file rotate_files[] = {{"a", "a.png"}, {"b", "b.png"}, {"c", "c.png"}};
    static const demo_file trim_files[] = {
        {"sq1", "sq1.png"}, {"sq2", "sq2.png"}, {"sq3", "sq3.png"}, {"sq4", "sq4.png"}, {"sq5", "sq5.png"},
        {"sq6", "sq6.png"}, {"sq7", "sq7.png"}, {"sq8", "sq8.png"}, {"sq9", "sq9.png"}, {"sq10", "sq10.png"}};
    static const char *const box_frames[] = {"box_fill_128", "box_fill_64"};
    static const char *const trim_frames[] = {"sq1", "sq2", "sq3", "sq4", "sq5",
                                              "sq6", "sq7", "sq8", "sq9", "sq10"};

    demo_atlas basic = {"basic", "examples/basic/original", basic_files,
                        (int)(sizeof basic_files / sizeof basic_files[0]),
                        "BoxFlip", box_frames, 2, 1, 4.0F};
    demo_atlas rotate = {"rotate", "examples/rotate", rotate_files,
                         (int)(sizeof rotate_files / sizeof rotate_files[0]), NULL, NULL, 0, 0, 0.0F};
    demo_atlas anim_trim = {"anim_trim", "examples/anim_trim/anims", trim_files,
                            (int)(sizeof trim_files / sizeof trim_files[0]),
                            "anim_trim", trim_frames, 10, 1, 6.0F};

    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);

    TEST_ASSERT_TRUE_MESSAGE(pack_demo_atlas(&basic, ar), "export demo 'basic'");
    TEST_ASSERT_TRUE_MESSAGE(pack_demo_atlas(&rotate, ar), "export demo 'rotate'");
    TEST_ASSERT_TRUE_MESSAGE(pack_demo_atlas(&anim_trim, ar), "export demo 'anim_trim'");

    assert_demo_structure("basic", basic_files, basic.file_count, "BoxFlip", "playback: PLAYBACK_LOOP_FORWARD");
    assert_demo_structure("rotate", rotate_files, rotate.file_count, NULL, NULL);
    assert_demo_structure("anim_trim", trim_files, anim_trim.file_count, "anim_trim", "playback: PLAYBACK_LOOP_FORWARD");

    tp_arena_destroy(ar);
}
// #endregion

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    g_demo_dir = (argc > 2) ? argv[2] : "";
#ifdef TP_DEFOLD_DEMO_DIR
    if (g_demo_dir[0] == '\0') {
        g_demo_dir = TP_DEFOLD_DEMO_DIR;
    }
#endif
    UNITY_BEGIN();
    RUN_TEST(test_golden_bytes);
    RUN_TEST(test_rotated_geometry);
    RUN_TEST(test_determinism_byte_identical);
    RUN_TEST(test_caps_repack_identity_and_slice9_notice);
    RUN_TEST(test_playback_enum_and_flags);
    RUN_TEST(test_demo_atlases);
    return UNITY_END();
}
