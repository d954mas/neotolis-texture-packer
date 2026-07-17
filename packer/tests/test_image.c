#define _CRT_SECURE_NO_WARNINGS

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_image.h"
#include "tp_fs_internal.h"
#include "unity.h"

static const char *g_dir;

void setUp(void) {}
void tearDown(void) {}

static bool join_path(char *out, size_t cap, const char *leaf) {
    int n = snprintf(out, cap, "%s/%s", g_dir, leaf);
    return n > 0 && (size_t)n < cap;
}

static void make_tga(uint8_t out[34], uint16_t width, uint16_t height) {
    memset(out, 0, 34U);
    out[2] = 2U; /* uncompressed true-color */
    out[12] = (uint8_t)(width & 0xffU);
    out[13] = (uint8_t)(width >> 8U);
    out[14] = (uint8_t)(height & 0xffU);
    out[15] = (uint8_t)(height >> 8U);
    out[16] = 32U;
    out[17] = 0x28U; /* 8 alpha bits, top-left origin */

    /* TGA stores BGRA.  The 2x2 valid fixture starts red, green, blue, white. */
    static const uint8_t pixels[16] = {
        0U, 0U, 255U, 255U,
        0U, 255U, 0U, 255U,
        255U, 0U, 0U, 255U,
        255U, 255U, 255U, 255U,
    };
    memcpy(out + 18U, pixels, sizeof pixels);
}

static void test_loads_unicode_path_as_canonical_rgba8(void) {
    static const char leaf[] =
        "\xD0\xB8\xD0\xB7\xD0\xBE\xD0\xB1\xD1\x80\xD0\xB0\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5.tga";
    char path[1024];
    TEST_ASSERT_TRUE(join_path(path, sizeof path, leaf));
    uint8_t tga[34];
    make_tga(tga, 2U, 2U);
    TEST_ASSERT_TRUE(tp_fs_write_file(path, tga, sizeof tga));

    tp_image_rgba8 image = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_image_load_file(path, &image, &error),
                                  error.msg);
    TEST_ASSERT_EQUAL_INT(2, image.width);
    TEST_ASSERT_EQUAL_INT(2, image.height);
    TEST_ASSERT_NOT_NULL(image.pixels);
    static const uint8_t expected_first_pixel[4] = {255U, 0U, 0U, 255U};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_first_pixel, image.pixels, 4U);

    tp_image_free(&image);
    TEST_ASSERT_NULL(image.pixels);
    TEST_ASSERT_EQUAL_INT(0, image.width);
    TEST_ASSERT_EQUAL_INT(0, image.height);
}

static void test_rejects_invalid_utf8_path(void) {
    static const char invalid_path[] = {'b', 'a', 'd', '-', (char)0xc3, '(', '\0'};
    tp_image_rgba8 image = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_image_load_file(invalid_path, &image, &error));
    TEST_ASSERT_NULL(image.pixels);
}

static void test_empty_file_is_structured_decode_error(void) {
    char path[1024];
    TEST_ASSERT_TRUE(join_path(path, sizeof path, "empty.img"));
    TEST_ASSERT_TRUE(tp_fs_write_file(path, "", 0U));
    tp_image_rgba8 image = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNSUPPORTED_TEXTURE,
                          tp_image_load_file(path, &image, &error));
    TEST_ASSERT_NULL(image.pixels);
    TEST_ASSERT_TRUE(error.msg[0] != '\0');
}

static void test_corrupt_file_is_structured_decode_error(void) {
    char path[1024];
    TEST_ASSERT_TRUE(join_path(path, sizeof path, "corrupt.img"));
    static const uint8_t bytes[] = {0xdeU, 0xadU, 0xbeU, 0xefU};
    TEST_ASSERT_TRUE(tp_fs_write_file(path, bytes, sizeof bytes));
    tp_image_rgba8 image = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNSUPPORTED_TEXTURE,
                          tp_image_load_file(path, &image, &error));
    TEST_ASSERT_NULL(image.pixels);
    TEST_ASSERT_TRUE(error.msg[0] != '\0');
}

static void test_oversized_dimensions_are_rejected_before_decode(void) {
    char path[1024];
    TEST_ASSERT_TRUE(join_path(path, sizeof path, "oversized.tga"));
    uint8_t tga[34];
    make_tga(tga, (uint16_t)(TP_IMAGE_MAX_DIMENSION + 1U), 1U);
    TEST_ASSERT_TRUE(tp_fs_write_file(path, tga, sizeof tga));
    tp_image_rgba8 image = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_image_load_file(path, &image, &error));
    TEST_ASSERT_NULL(image.pixels);
    TEST_ASSERT_TRUE(strstr(error.msg, "dimension") != NULL);
}

static void test_oversized_decoded_byte_count_is_rejected_before_decode(void) {
    char path[1024];
    TEST_ASSERT_TRUE(join_path(path, sizeof path, "oversized-decoded.tga"));
    uint8_t tga[34];
    make_tga(tga, (uint16_t)TP_IMAGE_MAX_DIMENSION,
             (uint16_t)TP_IMAGE_MAX_DIMENSION);
    TEST_ASSERT_TRUE(tp_fs_write_file(path, tga, sizeof tga));
    tp_image_rgba8 image = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_image_load_file(path, &image, &error));
    TEST_ASSERT_NULL(image.pixels);
    TEST_ASSERT_TRUE(strstr(error.msg, "decoded size") != NULL);
}

static void test_oversized_encoded_file_is_rejected_before_allocation(void) {
    TEST_ASSERT_TRUE(TP_IMAGE_MAX_FILE_BYTES < (size_t)LONG_MAX);
    char path[1024];
    TEST_ASSERT_TRUE(join_path(path, sizeof path, "oversized-file.img"));
    FILE *file = tp_fs_fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, (long)TP_IMAGE_MAX_FILE_BYTES, SEEK_SET));
    TEST_ASSERT_NOT_EQUAL(EOF, fputc(0, file));
    TEST_ASSERT_TRUE(tp_fs_close(file));

    tp_image_rgba8 image = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_image_load_file(path, &image, &error));
    TEST_ASSERT_NULL(image.pixels);
    TEST_ASSERT_TRUE(strstr(error.msg, "file size") != NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return 2;
    }
    g_dir = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_loads_unicode_path_as_canonical_rgba8);
    RUN_TEST(test_rejects_invalid_utf8_path);
    RUN_TEST(test_empty_file_is_structured_decode_error);
    RUN_TEST(test_corrupt_file_is_structured_decode_error);
    RUN_TEST(test_oversized_dimensions_are_rejected_before_decode);
    RUN_TEST(test_oversized_decoded_byte_count_is_rejected_before_decode);
    RUN_TEST(test_oversized_encoded_file_is_rejected_before_allocation);
    return UNITY_END();
}
