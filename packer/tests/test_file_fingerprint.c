#include "tp_core/tp_identity.h"
#include "unity.h"

#include <stdio.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

void setUp(void) {}
void tearDown(void) {}

static const char *s_path = "tp_file_fingerprint_test.tmp";

static void write_bytes(const char *bytes) {
    FILE *f = fopen(s_path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(3U, fwrite(bytes, 1U, 3U, f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

void test_fingerprint_tracks_exact_file_bytes(void) {
    (void)remove(s_path);
    write_bytes("abc");
    tp_id128 first = {{0}};
    tp_id128 same = {{0}};
    tp_id128 changed = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_file_fingerprint(s_path, &first, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_file_fingerprint(s_path, &same, NULL));
    TEST_ASSERT_TRUE(tp_id128_eq(first, same));

    write_bytes("abd");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_file_fingerprint(s_path, &changed, NULL));
    TEST_ASSERT_FALSE(tp_id128_eq(first, changed));
    TEST_ASSERT_EQUAL_INT(0, remove(s_path));
}

void test_fingerprint_failure_is_structured_and_clears_output(void) {
    (void)remove(s_path);
    tp_id128 out = {{0xFF}};
    tp_error err = {0};
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_identity_file_fingerprint(s_path, &out, &err));
    TEST_ASSERT_TRUE(tp_id128_is_nil(out));
    TEST_ASSERT_NOT_EQUAL(0, err.msg[0]);
}

void test_buffer_fingerprint_hashes_exact_bytes(void) {
    tp_id128 from_buffer = {{0}};
    tp_id128 from_file = {{0}};
    write_bytes("abc");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_bytes_fingerprint("abc", 3U, &from_buffer, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_file_fingerprint(s_path, &from_file, NULL));
    TEST_ASSERT_TRUE(tp_id128_eq(from_buffer, from_file));

    tp_id128 empty = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_bytes_fingerprint(NULL, 0U, &empty, NULL));
    TEST_ASSERT_FALSE(tp_id128_is_nil(empty));

    tp_id128 invalid = {{0xFF}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_identity_bytes_fingerprint(NULL, 1U, &invalid, NULL));
    TEST_ASSERT_TRUE(tp_id128_is_nil(invalid));
    TEST_ASSERT_EQUAL_INT(0, remove(s_path));
}

void test_fingerprint_rejects_oversized_file(void) {
    (void)remove(s_path);
    FILE *f = fopen(s_path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, (long)TP_IDENTITY_FILE_MAX_BYTES, SEEK_SET));
    TEST_ASSERT_NOT_EQUAL(EOF, fputc('x', f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));

    tp_id128 out = {{0xFF}};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_identity_file_fingerprint(s_path, &out, &err));
    TEST_ASSERT_TRUE(tp_id128_is_nil(out));
    TEST_ASSERT_NOT_EQUAL(0, err.msg[0]);
    TEST_ASSERT_EQUAL_INT(0, remove(s_path));
}

#ifndef _WIN32
void test_fingerprint_rejects_symlink(void) {
    const char *target = "tp_file_fingerprint_target.tmp";
    (void)remove(s_path);
    (void)remove(target);
    FILE *f = fopen(target, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(3U, fwrite("abc", 1U, 3U, f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    TEST_ASSERT_EQUAL_INT(0, symlink(target, s_path));

    tp_id128 out = {{0xFF}};
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_identity_file_fingerprint(s_path, &out, NULL));
    TEST_ASSERT_TRUE(tp_id128_is_nil(out));
    TEST_ASSERT_EQUAL_INT(0, remove(s_path));
    TEST_ASSERT_EQUAL_INT(0, remove(target));
}

void test_fingerprint_rejects_fifo_without_blocking(void) {
    (void)remove(s_path);
    TEST_ASSERT_EQUAL_INT(0, mkfifo(s_path, 0600));
    tp_id128 out = {{0xFF}};
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_identity_file_fingerprint(s_path, &out, NULL));
    TEST_ASSERT_TRUE(tp_id128_is_nil(out));
    TEST_ASSERT_EQUAL_INT(0, remove(s_path));
}
#endif

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fingerprint_tracks_exact_file_bytes);
    RUN_TEST(test_fingerprint_failure_is_structured_and_clears_output);
    RUN_TEST(test_buffer_fingerprint_hashes_exact_bytes);
    RUN_TEST(test_fingerprint_rejects_oversized_file);
#ifndef _WIN32
    RUN_TEST(test_fingerprint_rejects_symlink);
    RUN_TEST(test_fingerprint_rejects_fifo_without_blocking);
#endif
    return UNITY_END();
}
