/* C0-04 reference normalization pipeline (decoder-independent). Pure synthetic
 * vectors: channel expansion + A=255, the pinned 16->8 rounding rule, all 8
 * EXIF orientations on an asymmetric pattern, ICC notice policy, unknown-EXIF
 * policy, container probe, and EXIF-orientation parsing. */

#include "tp_c0/tp_c0_raster.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- 16-bit -> 8-bit pinned reduction ---- */

void test_reduce16_endpoints(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tp_c0_raster_reduce16(0));
    TEST_ASSERT_EQUAL_UINT8(255, tp_c0_raster_reduce16(0xFFFF));
    TEST_ASSERT_EQUAL_UINT8(1, tp_c0_raster_reduce16(0x0101)); /* 257/257 = 1 */
    TEST_ASSERT_EQUAL_UINT8(128, tp_c0_raster_reduce16(0x8080));
}

void test_reduce16_rounds_not_truncates(void) {
    /* These are the values where round-to-nearest and stb's `>> 8` diverge --
     * proof the policy is NOT stb's truncation. */
    TEST_ASSERT_EQUAL_UINT8(1, tp_c0_raster_reduce16(0x00FF)); /* 255/257 -> 1, truncation -> 0 */
    TEST_ASSERT_EQUAL_UINT8(2, tp_c0_raster_reduce16(0x01FF)); /* 511/257 -> 2, truncation -> 1 */
    TEST_ASSERT_NOT_EQUAL((uint8_t)(0x00FF >> 8), tp_c0_raster_reduce16(0x00FF));
    TEST_ASSERT_NOT_EQUAL((uint8_t)(0x01FF >> 8), tp_c0_raster_reduce16(0x01FF));
}

/* ---- channel expansion + straight-alpha A=255 ---- */

void test_expand8_gray(void) {
    const uint8_t in[2] = {50, 200};
    uint8_t out[8];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_expand8(in, 2, 1, TP_C0_RASTER_GRAY8, out, NULL));
    const uint8_t exp[8] = {50, 50, 50, 255, 200, 200, 200, 255};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, out, 8);
}

void test_expand8_gray_alpha(void) {
    const uint8_t in[4] = {50, 128, 200, 64};
    uint8_t out[8];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_expand8(in, 2, 1, TP_C0_RASTER_GRAYA8, out, NULL));
    const uint8_t exp[8] = {50, 50, 50, 128, 200, 200, 200, 64};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, out, 8);
}

void test_expand8_rgb_sets_opaque_alpha(void) {
    const uint8_t in[3] = {10, 20, 30};
    uint8_t out[4];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_expand8(in, 1, 1, TP_C0_RASTER_RGB8, out, NULL));
    const uint8_t exp[4] = {10, 20, 30, 255};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, out, 4);
}

void test_expand8_rgba_passthrough(void) {
    const uint8_t in[4] = {10, 20, 30, 40};
    uint8_t out[4];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_expand8(in, 1, 1, TP_C0_RASTER_RGBA8, out, NULL));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(in, out, 4);
}

void test_expand16_gray_and_rgba(void) {
    const uint16_t g[2] = {0x0101, 0xFFFF};
    uint8_t og[8];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_expand16(g, 2, 1, TP_C0_RASTER_GRAY8, og, NULL));
    const uint8_t eg[8] = {1, 1, 1, 255, 255, 255, 255, 255};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(eg, og, 8);

    const uint16_t p[4] = {0x0101, 0x0200, 0x00FF, 0xFFFF};
    uint8_t op[4];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_expand16(p, 1, 1, TP_C0_RASTER_RGBA8, op, NULL));
    const uint8_t ep[4] = {1, 2, 1, 255};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ep, op, 4);
}

/* ---- EXIF orientation: all 8 values on an asymmetric 3x2 pattern ---- */

/* Codes are unique per source pixel so any wrong flip/rotate is caught; the
 * pattern is non-square so dimension-swapping orientations (5..8) are visible. */
static void codes_to_rgba(const uint8_t *codes, size_t n, uint8_t *out) {
    for (size_t i = 0; i < n; i++) {
        out[i * 4 + 0] = codes[i];
        out[i * 4 + 1] = codes[i];
        out[i * 4 + 2] = codes[i];
        out[i * 4 + 3] = 255;
    }
}

typedef struct {
    uint32_t orientation;
    uint32_t w;
    uint32_t h;
    uint8_t codes[6];
} orient_golden;

void test_orientation_all_eight(void) {
    const uint8_t src_codes[6] = {10, 20, 30, 40, 50, 60}; /* 3 wide, 2 tall */
    uint8_t src[24];
    codes_to_rgba(src_codes, 6, src);

    const orient_golden golden[8] = {
        {1, 3, 2, {10, 20, 30, 40, 50, 60}}, /* identity */
        {2, 3, 2, {30, 20, 10, 60, 50, 40}}, /* mirror horizontal */
        {3, 3, 2, {60, 50, 40, 30, 20, 10}}, /* rotate 180 */
        {4, 3, 2, {40, 50, 60, 10, 20, 30}}, /* mirror vertical */
        {5, 2, 3, {10, 40, 20, 50, 30, 60}}, /* transpose */
        {6, 2, 3, {40, 10, 50, 20, 60, 30}}, /* rotate 90 CW */
        {7, 2, 3, {60, 30, 50, 20, 40, 10}}, /* transverse */
        {8, 2, 3, {30, 60, 20, 50, 10, 40}}, /* rotate 90 CCW */
    };

    for (size_t i = 0; i < 8; i++) {
        tp_c0_image img = {0, 0, NULL};
        tp_c0_raster_notices notices;
        tp_c0_raster_notices_reset(&notices);
        tp_c0_detail rc = tp_c0_raster_apply_orientation(src, 3, 2, golden[i].orientation, &img, &notices, NULL);
        TEST_ASSERT_EQUAL_INT(TP_C0_OK, rc);
        TEST_ASSERT_EQUAL_UINT32(golden[i].w, img.width);
        TEST_ASSERT_EQUAL_UINT32(golden[i].h, img.height);
        uint8_t exp[24];
        codes_to_rgba(golden[i].codes, 6, exp);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, img.rgba, 24);
        TEST_ASSERT_FALSE(tp_c0_raster_notices_has(&notices, TP_C0_NOTE_EXIF_ORIENTATION_UNKNOWN));
        tp_c0_image_free(&img);
    }
}

void test_orientation_unknown_is_identity_plus_notice(void) {
    const uint8_t src_codes[6] = {10, 20, 30, 40, 50, 60};
    uint8_t src[24];
    codes_to_rgba(src_codes, 6, src);

    const uint32_t bad[2] = {0, 9};
    for (size_t i = 0; i < 2; i++) {
        tp_c0_image img = {0, 0, NULL};
        tp_c0_raster_notices notices;
        tp_c0_raster_notices_reset(&notices);
        TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_raster_apply_orientation(src, 3, 2, bad[i], &img, &notices, NULL));
        TEST_ASSERT_EQUAL_UINT32(3, img.width);
        TEST_ASSERT_EQUAL_UINT32(2, img.height);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(src, img.rgba, 24); /* pixels = identity */
        TEST_ASSERT_TRUE(tp_c0_raster_notices_has(&notices, TP_C0_NOTE_EXIF_ORIENTATION_UNKNOWN));
        tp_c0_image_free(&img);
    }
}

/* ---- ICC notice policy (pixels never touched) ---- */

void test_icc_notice_policy(void) {
    tp_c0_raster_notices n;

    tp_c0_raster_notices_reset(&n);
    tp_c0_raster_note_icc(true, true, &n);
    TEST_ASSERT_TRUE(tp_c0_raster_notices_has(&n, TP_C0_NOTE_ICC_IGNORED));
    TEST_ASSERT_FALSE(tp_c0_raster_notices_has(&n, TP_C0_NOTE_ICC_PROFILE_BAD));

    tp_c0_raster_notices_reset(&n);
    tp_c0_raster_note_icc(true, false, &n);
    TEST_ASSERT_TRUE(tp_c0_raster_notices_has(&n, TP_C0_NOTE_ICC_PROFILE_BAD));
    TEST_ASSERT_FALSE(tp_c0_raster_notices_has(&n, TP_C0_NOTE_ICC_IGNORED));

    tp_c0_raster_notices_reset(&n);
    tp_c0_raster_note_icc(false, false, &n);
    TEST_ASSERT_EQUAL_UINT(0, n.count);
}

/* ---- container probe ---- */

void test_container_probe(void) {
    const uint8_t png[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t jpg[4] = {0xFF, 0xD8, 0xFF, 0xE0};
    const uint8_t webp[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const uint8_t junk[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_INT(TP_C0_CONTAINER_PNG, tp_c0_raster_probe_container(png, 8));
    TEST_ASSERT_EQUAL_INT(TP_C0_CONTAINER_JPEG, tp_c0_raster_probe_container(jpg, 4));
    TEST_ASSERT_EQUAL_INT(TP_C0_CONTAINER_WEBP, tp_c0_raster_probe_container(webp, 12));
    TEST_ASSERT_EQUAL_INT(TP_C0_CONTAINER_UNKNOWN, tp_c0_raster_probe_container(junk, 4));
    TEST_ASSERT_EQUAL_INT(TP_C0_CONTAINER_UNKNOWN, tp_c0_raster_probe_container(png, 3)); /* too short */
}

/* ---- EXIF orientation parsing (both TIFF byte orders) ---- */

void test_exif_orientation_little_endian(void) {
    /* SOI + APP1(Exif, II TIFF, orientation=6) + EOI */
    const uint8_t jpeg[] = {
        0xFF, 0xD8,                               /* SOI */
        0xFF, 0xE1, 0x00, 0x22,                   /* APP1, len = 0x22 = 34 */
        'E', 'x', 'i', 'f', 0x00, 0x00,           /* Exif\0\0 */
        'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, /* TIFF LE, IFD0 @ 8 */
        0x01, 0x00,                               /* entry count = 1 */
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, /* tag 0x0112 SHORT =6 */
        0x00, 0x00, 0x00, 0x00,                   /* next IFD = 0 */
        0xFF, 0xD9};                              /* EOI */
    uint32_t o = 0;
    TEST_ASSERT_TRUE(tp_c0_raster_exif_orientation(jpeg, sizeof jpeg, &o));
    TEST_ASSERT_EQUAL_UINT32(6, o);
}

void test_exif_orientation_big_endian(void) {
    const uint8_t jpeg[] = {
        0xFF, 0xD8,
        0xFF, 0xE1, 0x00, 0x22,
        'E', 'x', 'i', 'f', 0x00, 0x00,
        'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08, /* TIFF BE, IFD0 @ 8 */
        0x00, 0x01,                                   /* entry count = 1 */
        0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, /* tag 0x0112 SHORT =3 */
        0x00, 0x00, 0x00, 0x00,
        0xFF, 0xD9};
    uint32_t o = 0;
    TEST_ASSERT_TRUE(tp_c0_raster_exif_orientation(jpeg, sizeof jpeg, &o));
    TEST_ASSERT_EQUAL_UINT32(3, o);
}

void test_exif_orientation_absent(void) {
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xD9};
    uint32_t o = 123;
    TEST_ASSERT_FALSE(tp_c0_raster_exif_orientation(jpeg, sizeof jpeg, &o));
}

void test_exif_orientation_truncated_no_crash(void) {
    /* APP1 declares a length that runs off the end -> false, no over-read. */
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x40, 'E', 'x', 'i', 'f'};
    uint32_t o = 0;
    TEST_ASSERT_FALSE(tp_c0_raster_exif_orientation(jpeg, sizeof jpeg, &o));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reduce16_endpoints);
    RUN_TEST(test_reduce16_rounds_not_truncates);
    RUN_TEST(test_expand8_gray);
    RUN_TEST(test_expand8_gray_alpha);
    RUN_TEST(test_expand8_rgb_sets_opaque_alpha);
    RUN_TEST(test_expand8_rgba_passthrough);
    RUN_TEST(test_expand16_gray_and_rgba);
    RUN_TEST(test_orientation_all_eight);
    RUN_TEST(test_orientation_unknown_is_identity_plus_notice);
    RUN_TEST(test_icc_notice_policy);
    RUN_TEST(test_container_probe);
    RUN_TEST(test_exif_orientation_little_endian);
    RUN_TEST(test_exif_orientation_big_endian);
    RUN_TEST(test_exif_orientation_absent);
    RUN_TEST(test_exif_orientation_truncated_no_crash);
    return UNITY_END();
}
