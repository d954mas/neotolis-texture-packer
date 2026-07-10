/* PNG page export (ROADMAP Phase 2 acceptance): the exported page decodes to the
 * expected dims; a known probe pixel proves content + orientation (top-left maps
 * correctly); straight-alpha is the default and the premultiply toggle changes
 * the bytes. Packs one crafted sprite via tp_pack, then writes + decodes pages. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"
#include "unity.h"

#define SW 32
#define SH 24
static uint8_t g_pix[SW * SH * 4];
static const uint8_t MARKER[4] = {255, 0, 0, 255};   /* opaque TL corner marker */
static const uint8_t FILL[4] = {200, 100, 50, 128};  /* semi-transparent fill    */

static const char *g_dir;
static tp_arena *g_arena;
static tp_result *g_res;
static const tp_sprite *g_sp;

void setUp(void) {}
void tearDown(void) {}

static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *b = (sz > 0) ? (uint8_t *)malloc((size_t)sz) : NULL;
    size_t rd = b ? fread(b, 1, (size_t)sz, f) : 0;
    (void)fclose(f);
    if (!b || rd != (size_t)sz) {
        free(b);
        return NULL;
    }
    *out_size = (size_t)sz;
    return b;
}

// #region tests
void test_page_decodes_to_expected_dims(void) {
    const tp_page *pg = &g_res->pages[g_sp->page];
    char base[1024];
    (void)snprintf(base, sizeof base, "%s/png_straight", g_dir);
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_export_write_pages(g_res, base, false, &e), e.msg);

    char path[1088];
    (void)snprintf(path, sizeof path, "%s-%d.png", base, g_sp->page);
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char *img = stbi_load(path, &w, &h, &comp, 4);
    TEST_ASSERT_NOT_NULL_MESSAGE(img, "exported page must decode");
    TEST_ASSERT_EQUAL_INT(pg->w, w);
    TEST_ASSERT_EQUAL_INT(pg->h, h);
    stbi_image_free(img);
}

void test_probe_pixel_content_and_orientation(void) {
    const tp_page *pg = &g_res->pages[g_sp->page];
    char base[1024];
    (void)snprintf(base, sizeof base, "%s/png_probe", g_dir);
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_write_pages(g_res, base, false, &e));

    char path[1088];
    (void)snprintf(path, sizeof path, "%s-%d.png", base, g_sp->page);
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char *img = stbi_load(path, &w, &h, &comp, 4);
    TEST_ASSERT_NOT_NULL(img);
    TEST_ASSERT_EQUAL_INT(0, g_sp->transform); /* allow_transform off -> identity */

    /* TL marker: source (0,0) -> page (frame.x, frame.y). Proves orientation. */
    const unsigned char *tl = &img[((size_t)g_sp->frame.y * (size_t)w + (size_t)g_sp->frame.x) * 4];
    TEST_ASSERT_EQUAL_UINT8(MARKER[0], tl[0]);
    TEST_ASSERT_EQUAL_UINT8(MARKER[1], tl[1]);
    TEST_ASSERT_EQUAL_UINT8(MARKER[2], tl[2]);
    TEST_ASSERT_EQUAL_UINT8(MARKER[3], tl[3]);

    /* interior fill: straight alpha keeps rgb untouched. */
    int fx = g_sp->frame.x + 5;
    int fy = g_sp->frame.y + 5;
    const unsigned char *f = &img[((size_t)fy * (size_t)w + (size_t)fx) * 4];
    TEST_ASSERT_EQUAL_UINT8(FILL[0], f[0]);
    TEST_ASSERT_EQUAL_UINT8(FILL[1], f[1]);
    TEST_ASSERT_EQUAL_UINT8(FILL[2], f[2]);
    TEST_ASSERT_EQUAL_UINT8(FILL[3], f[3]);
    (void)pg;
    stbi_image_free(img);
}

void test_premultiply_toggle_changes_bytes(void) {
    char sbase[1024];
    char pbase[1024];
    (void)snprintf(sbase, sizeof sbase, "%s/png_s", g_dir);
    (void)snprintf(pbase, sizeof pbase, "%s/png_p", g_dir);
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_write_pages(g_res, sbase, false, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_export_write_pages(g_res, pbase, true, &e));

    char sp[1088];
    char pp[1088];
    (void)snprintf(sp, sizeof sp, "%s-%d.png", sbase, g_sp->page);
    (void)snprintf(pp, sizeof pp, "%s-%d.png", pbase, g_sp->page);

    /* the encoded files must differ (premultiply mutates the fill pixels). */
    size_t ns = 0;
    size_t np = 0;
    uint8_t *bs = read_whole_file(sp, &ns);
    uint8_t *bp = read_whole_file(pp, &np);
    TEST_ASSERT_NOT_NULL(bs);
    TEST_ASSERT_NOT_NULL(bp);
    bool differ = (ns != np) || memcmp(bs, bp, ns) != 0;
    TEST_ASSERT_TRUE_MESSAGE(differ, "premultiply toggle must change page bytes");
    free(bs);
    free(bp);

    /* decoded premultiplied fill = fill.rgb * a/255 (rounded). */
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char *img = stbi_load(pp, &w, &h, &comp, 4);
    TEST_ASSERT_NOT_NULL(img);
    int fx = g_sp->frame.x + 5;
    int fy = g_sp->frame.y + 5;
    const unsigned char *f = &img[((size_t)fy * (size_t)w + (size_t)fx) * 4];
    uint8_t er = (uint8_t)((FILL[0] * FILL[3] + 127) / 255);
    uint8_t eg = (uint8_t)((FILL[1] * FILL[3] + 127) / 255);
    uint8_t eb = (uint8_t)((FILL[2] * FILL[3] + 127) / 255);
    TEST_ASSERT_EQUAL_UINT8(er, f[0]);
    TEST_ASSERT_EQUAL_UINT8(eg, f[1]);
    TEST_ASSERT_EQUAL_UINT8(eb, f[2]);
    TEST_ASSERT_EQUAL_UINT8(FILL[3], f[3]);
    stbi_image_free(img);
}
// #endregion

static bool setup_all(const char *dir) {
    g_dir = dir;
    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < SW; x++) {
            uint8_t *p = &g_pix[(y * SW + x) * 4];
            memcpy(p, FILL, 4);
        }
    }
    memcpy(&g_pix[0], MARKER, 4); /* local (0,0) marker */

    g_arena = tp_arena_create(0);
    if (!g_arena) {
        return false;
    }
    tp_pack_sprite_desc sp;
    memset(&sp, 0, sizeof sp);
    sp.name = "probe";
    sp.rgba = g_pix;
    sp.w = SW;
    sp.h = SH;
    sp.origin_x = 0.5F;
    sp.origin_y = 0.5F;

    tp_pack_settings s;
    tp_pack_settings_defaults(&s);
    s.atlas_name = "png_test";
    s.work_dir = dir;
    s.sprites = &sp;
    s.sprite_count = 1;
    s.shape = 0; /* RECT */
    s.allow_transform = false;
    s.power_of_two = false;
    s.padding = 0;
    s.margin = 0;
    s.alpha_threshold = 1;
    s.pixels_per_unit = 1.0F;

    tp_error e = {{0}};
    if (tp_pack(&s, g_arena, &g_res, &e) != TP_STATUS_OK || !g_res || g_res->sprite_count != 1) {
        (void)fprintf(stderr, "png test pack failed: %s\n", e.msg);
        return false;
    }
    g_sp = &g_res->sprites[0];
    return true;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    if (!setup_all(dir)) {
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_page_decodes_to_expected_dims);
    RUN_TEST(test_probe_pixel_content_and_orientation);
    RUN_TEST(test_premultiply_toggle_changes_bytes);
    int rc = UNITY_END();
    tp_arena_destroy(g_arena);
    return rc;
}
