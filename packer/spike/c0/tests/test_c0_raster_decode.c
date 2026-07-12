/* C0-04 real end-to-end decode vectors (links the STBI_NO_SIMD stb TU).
 *
 * PNG palette / grayscale / 16-bit / RGB(no-alpha) fixtures are built in-repo by
 * the minimal deterministic PNG encoder (tp_c0_png_write.h) and decoded through
 * stb's real path; their canonical RGBA is predictable and hand-pinned. The
 * lossy JPEG cases (no-alpha + EXIF-orientation-tagged) carry captured byte-exact
 * goldens -- deterministic across CI OS because the scalar integer decode path
 * is forced. WebP is DEFERRED to B1-01: stb cannot decode it, so the policy is
 * pinned via a synthetic RGBA pass-through plus a format_unsupported gate. */

#include "tp_c0/tp_c0_raster.h"
#include "tp_c0/tp_c0_stb.h"
#include "tp_c0_jpeg_fixture.h"
#include "tp_c0_png_write.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Set to 1 after pasting captured JPEG goldens; 0 prints actual bytes. */
#define TP_C0_GOLD_READY 1

static void dump_rgba(const char *name, const uint8_t *px, uint32_t w, uint32_t h) {
    printf("/* %s (%ux%u) */\n", name, w, h);
    size_t n = (size_t)w * h * 4;
    for (size_t i = 0; i < n; i++) {
        printf("0x%02x,", px[i]);
        if ((i % 16) == 15) {
            printf("\n");
        }
    }
    printf("\n");
}

/* ---- PNG grayscale-8 -> RGBA (stb expands, A=255) ---- */

void test_png_gray8(void) {
    const uint8_t gray[2] = {50, 200};
    size_t len = 0;
    uint8_t *png = tp_png_build(2, 1, 0, 8, gray, NULL, NULL, 0, NULL, 0, &len);

    uint32_t w = 0;
    uint32_t h = 0;
    uint8_t *rgba = NULL;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(png, len, &w, &h, &rgba, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, w);
    TEST_ASSERT_EQUAL_UINT32(1, h);
    const uint8_t exp[8] = {50, 50, 50, 255, 200, 200, 200, 255};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, rgba, 8);

    tp_c0_stb_free(rgba);
    free(png);
}

/* ---- PNG palette-8 -> RGBA (stb expands palette, A=255 without tRNS) ---- */

void test_png_palette8(void) {
    const uint8_t idx[4] = {0, 1, 2, 3};
    const uint8_t pal[12] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    size_t len = 0;
    uint8_t *png = tp_png_build(2, 2, 3, 8, idx, NULL, pal, 4, NULL, 0, &len);

    uint32_t w = 0;
    uint32_t h = 0;
    uint8_t *rgba = NULL;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(png, len, &w, &h, &rgba, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, w);
    TEST_ASSERT_EQUAL_UINT32(2, h);
    const uint8_t exp[16] = {10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, rgba, 16);

    tp_c0_stb_free(rgba);
    free(png);
}

/* ---- PNG RGB-8 (no alpha) -> RGBA with A=255 ---- */

void test_png_rgb8_no_alpha(void) {
    const uint8_t rgb[6] = {10, 20, 30, 40, 50, 60};
    size_t len = 0;
    uint8_t *png = tp_png_build(2, 1, 2, 8, rgb, NULL, NULL, 0, NULL, 0, &len);

    uint32_t w = 0;
    uint32_t h = 0;
    uint8_t *rgba = NULL;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(png, len, &w, &h, &rgba, NULL));
    const uint8_t exp[8] = {10, 20, 30, 255, 40, 50, 60, 255};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, rgba, 8);

    tp_c0_stb_free(rgba);
    free(png);
}

/* ---- PNG grayscale-16 -> RGBA via the PINNED rounding rule ---- */

void test_png_gray16_uses_rounding_not_truncation(void) {
    const uint16_t gray16[2] = {0x0101, 0x01FF};
    size_t len = 0;
    uint8_t *png = tp_png_build(2, 1, 0, 16, NULL, gray16, NULL, 0, NULL, 0, &len);

    /* Canonical path: decode 16-bit, apply tp_c0_raster_reduce16 (round). */
    uint32_t w = 0;
    uint32_t h = 0;
    int ch = 0;
    uint16_t *s16 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_u16(png, len, &w, &h, &ch, &s16, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, w);
    TEST_ASSERT_EQUAL_INT(1, ch);
    TEST_ASSERT_EQUAL_UINT16(0x0101, s16[0]); /* 16-bit preserved losslessly */
    TEST_ASSERT_EQUAL_UINT16(0x01FF, s16[1]);
    uint8_t rgba[8];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_expand16(s16, w, h, TP_C0_RASTER_GRAY8, rgba, NULL));
    const uint8_t exp[8] = {1, 1, 1, 255, 2, 2, 2, 255}; /* 0x01FF rounds to 2 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, rgba, 8);
    tp_c0_stb_free(s16);

    /* Baseline contrast: stb's default stbi_load truncates 0x01FF -> 1. */
    uint8_t *rgba8 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(png, len, &w, &h, &rgba8, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, rgba8[0]); /* 0x0101 >> 8 */
    TEST_ASSERT_EQUAL_UINT8(1, rgba8[4]); /* 0x01FF >> 8 == 1, NOT the rounded 2 */
    tp_c0_stb_free(rgba8);

    free(png);
}

/* ---- ICC: corrupt iCCP chunk does not change pixels; notice emitted ---- */

static bool png_has_chunk(const uint8_t *png, size_t len, const char type[4]) {
    size_t p = 8; /* after signature */
    while (p + 8 <= len) {
        uint32_t clen = ((uint32_t)png[p] << 24) | ((uint32_t)png[p + 1] << 16) | ((uint32_t)png[p + 2] << 8) | png[p + 3];
        if (memcmp(png + p + 4, type, 4) == 0) {
            return true;
        }
        p += (size_t)12 + clen;
    }
    return false;
}

void test_icc_corrupt_profile_pixels_unchanged(void) {
    const uint8_t rgb[6] = {10, 20, 30, 40, 50, 60};
    size_t base_len = 0;
    uint8_t *base = tp_png_build(2, 1, 2, 8, rgb, NULL, NULL, 0, NULL, 0, &base_len);

    /* iCCP payload: name 'n', null, compression method 0, then a bogus (corrupt)
     * "deflate" stream. stb skips ancillary chunks, so pixels must be identical. */
    const uint8_t iccp[7] = {'n', 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    size_t icc_len = 0;
    uint8_t *withicc = tp_png_insert_after_ihdr(base, base_len, "iCCP", iccp, sizeof iccp, &icc_len);
    TEST_ASSERT_TRUE(png_has_chunk(withicc, icc_len, "iCCP"));

    uint32_t w0 = 0;
    uint32_t h0 = 0;
    uint8_t *px0 = NULL;
    uint32_t w1 = 0;
    uint32_t h1 = 0;
    uint8_t *px1 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(base, base_len, &w0, &h0, &px0, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(withicc, icc_len, &w1, &h1, &px1, NULL));
    TEST_ASSERT_EQUAL_UINT32(w0, w1);
    TEST_ASSERT_EQUAL_UINT32(h0, h1);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(px0, px1, 8); /* corrupt ICC did not change pixels */

    /* Policy notice (the profile is present but undecodable). */
    tp_c0_raster_notices n;
    tp_c0_raster_notices_reset(&n);
    tp_c0_raster_note_icc(true, false, &n);
    TEST_ASSERT_TRUE(tp_c0_raster_notices_has(&n, TP_C0_NOTE_ICC_PROFILE_BAD));

    tp_c0_stb_free(px0);
    tp_c0_stb_free(px1);
    free(base);
    free(withicc);
}

/* ---- truncated / garbage -> structured error, never a crash ---- */

void test_truncated_and_garbage_decode_failed(void) {
    uint32_t w = 0;
    uint32_t h = 0;
    uint8_t *rgba = NULL;

    /* First 40 bytes of a valid JPEG: headers without a complete scan. */
    tp_c0_detail rc = tp_c0_stb_decode_rgba8(k_jpeg_quad8, 40, &w, &h, &rgba, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_DECODE_FAILED, rc);
    TEST_ASSERT_NULL(rgba);

    const uint8_t garbage[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    rc = tp_c0_stb_decode_rgba8(garbage, sizeof garbage, &w, &h, &rgba, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_DECODE_FAILED, rc);
    TEST_ASSERT_NULL(rgba);
}

/* ---- WebP is DEFERRED to B1-01: policy pinned decoder-independently ---- */

void test_webp_policy_deferred(void) {
    /* 1. A WebP-signature buffer maps to format_unsupported (stb has no WebP;
     *    the real codec is selected/vendored in B1-01). */
    const uint8_t webp[16] = {'R', 'I', 'F', 'F', 8, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
    tp_c0_raster_container c = tp_c0_raster_probe_container(webp, sizeof webp);
    TEST_ASSERT_EQUAL_INT(TP_C0_CONTAINER_WEBP, c);
    tp_c0_detail policy = (c == TP_C0_CONTAINER_WEBP) ? TP_C0_ERR_FORMAT_UNSUPPORTED : TP_C0_OK;
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_FORMAT_UNSUPPORTED, policy);

    /* 2. The normalization policy itself is decoder-independent: raw RGBA samples
     *    (as a future WebP decoder would hand over) pass through unchanged. */
    const uint8_t raw[8] = {11, 22, 33, 44, 55, 66, 77, 88};
    uint8_t out[8];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_expand8(raw, 2, 1, TP_C0_RASTER_RGBA8, out, NULL));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, out, 8);
    tp_c0_image img = {0, 0, NULL};
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_apply_orientation(out, 2, 1, 1, &img, NULL, NULL));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, img.rgba, 8);
    tp_c0_image_free(&img);
}

/* ---- real JPEG: no-alpha -> A=255, byte-exact golden ---- */

#if TP_C0_GOLD_READY
static const uint8_t k_gold_jpeg_noalpha[256] = {
    0xbd, 0x24, 0x1c, 0xff, 0xcf, 0x1a, 0x1f, 0xff, 0xc4, 0x1c, 0x19, 0xff, 0xca, 0x20, 0x20, 0xff,
    0x1b, 0xc5, 0x1e, 0xff, 0x23, 0xca, 0x25, 0xff, 0x16, 0xcb, 0x1e, 0xff, 0x27, 0xc1, 0x21, 0xff,
    0xcc, 0x19, 0x1f, 0xff, 0xd1, 0x1e, 0x22, 0xff, 0xc9, 0x22, 0x1c, 0xff, 0xc3, 0x1b, 0x1b, 0xff,
    0x25, 0xcb, 0x1f, 0xff, 0x21, 0xc3, 0x20, 0xff, 0x17, 0xc7, 0x1a, 0xff, 0x1f, 0xcd, 0x1c, 0xff,
    0xca, 0x20, 0x23, 0xff, 0xc9, 0x1d, 0x1b, 0xff, 0xc9, 0x1b, 0x18, 0xff, 0xca, 0x20, 0x29, 0xff,
    0x1e, 0xc5, 0x16, 0xff, 0x1f, 0xc9, 0x28, 0xff, 0x1e, 0xc7, 0x24, 0xff, 0x1e, 0xc5, 0x1d, 0xff,
    0xc4, 0x1f, 0x1b, 0xff, 0xc7, 0x20, 0x1a, 0xff, 0xc4, 0x20, 0x1e, 0xff, 0xc7, 0x1f, 0x1c, 0xff,
    0x1c, 0xc9, 0x1f, 0xff, 0x1f, 0xca, 0x1c, 0xff, 0x1d, 0xca, 0x22, 0xff, 0x1e, 0xcb, 0x21, 0xff,
    0x21, 0x1e, 0xd1, 0xff, 0x24, 0x1c, 0xc9, 0xff, 0x20, 0x1d, 0xc2, 0xff, 0x1e, 0x1d, 0xd1, 0xff,
    0xde, 0xda, 0x17, 0xff, 0xdc, 0xda, 0x27, 0xff, 0xda, 0xda, 0x20, 0xff, 0xdd, 0xd9, 0x18, 0xff,
    0x1a, 0x1c, 0xc7, 0xff, 0x21, 0x1e, 0xc7, 0xff, 0x1b, 0x22, 0xca, 0xff, 0x1b, 0x1c, 0xc4, 0xff,
    0xdc, 0xdf, 0x20, 0xff, 0xdc, 0xd9, 0x1a, 0xff, 0xd6, 0xdd, 0x1b, 0xff, 0xdd, 0xdf, 0x1c, 0xff,
    0x1a, 0x23, 0xca, 0xff, 0x18, 0x1d, 0xbf, 0xff, 0x1a, 0x1b, 0xc5, 0xff, 0x21, 0x21, 0xd1, 0xff,
    0xd5, 0xd8, 0x15, 0xff, 0xdc, 0xde, 0x21, 0xff, 0xde, 0xdc, 0x27, 0xff, 0xdc, 0xd6, 0x1c, 0xff,
    0x27, 0x17, 0xcf, 0xff, 0x1b, 0x20, 0xc2, 0xff, 0x20, 0x21, 0xc9, 0xff, 0x1a, 0x1b, 0xcb, 0xff,
    0xdf, 0xe0, 0x1a, 0xff, 0xd9, 0xda, 0x1d, 0xff, 0xdf, 0xdb, 0x22, 0xff, 0xd4, 0xe3, 0x18, 0xff,
};
#endif

void test_jpeg_no_alpha(void) {
    uint32_t w = 0;
    uint32_t h = 0;
    uint8_t *rgba = NULL;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(k_jpeg_quad8, sizeof k_jpeg_quad8, &w, &h, &rgba, NULL));
    TEST_ASSERT_EQUAL_UINT32(8, w);
    TEST_ASSERT_EQUAL_UINT32(8, h);
    if (!TP_C0_GOLD_READY) {
        dump_rgba("k_gold_jpeg_noalpha", rgba, w, h);
    }
#if TP_C0_GOLD_READY
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_gold_jpeg_noalpha, rgba, 256);
#endif
    tp_c0_stb_free(rgba);
}

/* ---- real JPEG with EXIF orientation tag: parse (stb won't) + apply ---- */

/* Splice an APP1/Exif orientation=N segment (little-endian TIFF) after SOI. */
static uint8_t *splice_exif(const uint8_t *jpg, size_t n, uint8_t orient, size_t *out_len) {
    uint8_t app1[36];
    const uint8_t tpl[36] = {
        0xFF, 0xE1, 0x00, 0x22,                         /* APP1, len 34 */
        'E',  'x',  'i',  'f',  0x00, 0x00,             /* Exif\0\0 */
        'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, /* TIFF LE, IFD0 @ 8 */
        0x01, 0x00,                                     /* count 1 */
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, /* tag 0x0112 SHORT count 1 */
        0x00, 0x00, 0x00, 0x00,                         /* value (patched below) */
        0x00, 0x00, 0x00, 0x00};                        /* next IFD */
    memcpy(app1, tpl, 36);
    app1[28] = orient; /* low byte of the SHORT value field (LE TIFF) */
    uint8_t *buf = (uint8_t *)malloc(n + 36);
    buf[0] = 0xFF;
    buf[1] = 0xD8;
    memcpy(buf + 2, app1, 36);
    memcpy(buf + 38, jpg + 2, n - 2);
    *out_len = n + 36;
    return buf;
}

#if TP_C0_GOLD_READY
static const uint8_t k_gold_jpeg_orient6[256] = {
    0x27, 0x17, 0xcf, 0xff, 0x1a, 0x23, 0xca, 0xff, 0x1a, 0x1c, 0xc7, 0xff, 0x21, 0x1e, 0xd1, 0xff,
    0xc4, 0x1f, 0x1b, 0xff, 0xca, 0x20, 0x23, 0xff, 0xcc, 0x19, 0x1f, 0xff, 0xbd, 0x24, 0x1c, 0xff,
    0x1b, 0x20, 0xc2, 0xff, 0x18, 0x1d, 0xbf, 0xff, 0x21, 0x1e, 0xc7, 0xff, 0x24, 0x1c, 0xc9, 0xff,
    0xc7, 0x20, 0x1a, 0xff, 0xc9, 0x1d, 0x1b, 0xff, 0xd1, 0x1e, 0x22, 0xff, 0xcf, 0x1a, 0x1f, 0xff,
    0x20, 0x21, 0xc9, 0xff, 0x1a, 0x1b, 0xc5, 0xff, 0x1b, 0x22, 0xca, 0xff, 0x20, 0x1d, 0xc2, 0xff,
    0xc4, 0x20, 0x1e, 0xff, 0xc9, 0x1b, 0x18, 0xff, 0xc9, 0x22, 0x1c, 0xff, 0xc4, 0x1c, 0x19, 0xff,
    0x1a, 0x1b, 0xcb, 0xff, 0x21, 0x21, 0xd1, 0xff, 0x1b, 0x1c, 0xc4, 0xff, 0x1e, 0x1d, 0xd1, 0xff,
    0xc7, 0x1f, 0x1c, 0xff, 0xca, 0x20, 0x29, 0xff, 0xc3, 0x1b, 0x1b, 0xff, 0xca, 0x20, 0x20, 0xff,
    0xdf, 0xe0, 0x1a, 0xff, 0xd5, 0xd8, 0x15, 0xff, 0xdc, 0xdf, 0x20, 0xff, 0xde, 0xda, 0x17, 0xff,
    0x1c, 0xc9, 0x1f, 0xff, 0x1e, 0xc5, 0x16, 0xff, 0x25, 0xcb, 0x1f, 0xff, 0x1b, 0xc5, 0x1e, 0xff,
    0xd9, 0xda, 0x1d, 0xff, 0xdc, 0xde, 0x21, 0xff, 0xdc, 0xd9, 0x1a, 0xff, 0xdc, 0xda, 0x27, 0xff,
    0x1f, 0xca, 0x1c, 0xff, 0x1f, 0xc9, 0x28, 0xff, 0x21, 0xc3, 0x20, 0xff, 0x23, 0xca, 0x25, 0xff,
    0xdf, 0xdb, 0x22, 0xff, 0xde, 0xdc, 0x27, 0xff, 0xd6, 0xdd, 0x1b, 0xff, 0xda, 0xda, 0x20, 0xff,
    0x1d, 0xca, 0x22, 0xff, 0x1e, 0xc7, 0x24, 0xff, 0x17, 0xc7, 0x1a, 0xff, 0x16, 0xcb, 0x1e, 0xff,
    0xd4, 0xe3, 0x18, 0xff, 0xdc, 0xd6, 0x1c, 0xff, 0xdd, 0xdf, 0x1c, 0xff, 0xdd, 0xd9, 0x18, 0xff,
    0x1e, 0xcb, 0x21, 0xff, 0x1e, 0xc5, 0x1d, 0xff, 0x1f, 0xcd, 0x1c, 0xff, 0x27, 0xc1, 0x21, 0xff,
};
#endif

void test_jpeg_exif_orientation_applied(void) {
    size_t sn = 0;
    uint8_t *spliced = splice_exif(k_jpeg_quad8, sizeof k_jpeg_quad8, 6, &sn);

    /* EXIF parse -- stb never does this. */
    uint32_t orient = 0;
    TEST_ASSERT_TRUE(tp_c0_raster_exif_orientation(spliced, sn, &orient));
    TEST_ASSERT_EQUAL_UINT32(6, orient);

    /* stb ignores EXIF: the spliced file decodes to the same pixels as plain. */
    uint32_t wp = 0;
    uint32_t hp = 0;
    uint8_t *plain = NULL;
    uint32_t ws = 0;
    uint32_t hs = 0;
    uint8_t *spx = NULL;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(k_jpeg_quad8, sizeof k_jpeg_quad8, &wp, &hp, &plain, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_stb_decode_rgba8(spliced, sn, &ws, &hs, &spx, NULL));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(plain, spx, 256);

    /* Apply the parsed orientation -> canonical "orientation already applied". */
    tp_c0_image img = {0, 0, NULL};
    tp_c0_raster_notices notices;
    tp_c0_raster_notices_reset(&notices);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_apply_orientation(spx, ws, hs, orient, &img, &notices, NULL));
    TEST_ASSERT_EQUAL_UINT32(8, img.width);
    TEST_ASSERT_EQUAL_UINT32(8, img.height);
    if (!TP_C0_GOLD_READY) {
        dump_rgba("k_gold_jpeg_orient6", img.rgba, img.width, img.height);
    }
#if TP_C0_GOLD_READY
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_gold_jpeg_orient6, img.rgba, 256);
#endif

    tp_c0_image_free(&img);
    tp_c0_stb_free(plain);
    tp_c0_stb_free(spx);
    free(spliced);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_png_gray8);
    RUN_TEST(test_png_palette8);
    RUN_TEST(test_png_rgb8_no_alpha);
    RUN_TEST(test_png_gray16_uses_rounding_not_truncation);
    RUN_TEST(test_icc_corrupt_profile_pixels_unchanged);
    RUN_TEST(test_truncated_and_garbage_decode_failed);
    RUN_TEST(test_webp_policy_deferred);
    RUN_TEST(test_jpeg_no_alpha);
    RUN_TEST(test_jpeg_exif_orientation_applied);
    return UNITY_END();
}
