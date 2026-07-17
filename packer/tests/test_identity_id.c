/* F1-00 task 2: 128-bit runtime ID generation via an injectable RNG seam
 * (fixed / short-read / failure seams -> structured tp_status, never abort).
 * Deterministic. */

#include "tp_core/tp_id.h"
#include "tp_hex.h" /* shared lowercase-hex encoder -- same code production uses (drift guard) */
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* --- injected RNG seams --- */
static int fixed_fill(void *ctx, uint8_t *out, size_t len) {
    memcpy(out, (const uint8_t *)ctx, len);
    return (int)len;
}
static int short_fill(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    size_t n = len > 10U ? 10U : len;
    memset(out, 0xAB, n);
    return (int)n; /* deliberately < 16 */
}
static int fail_fill(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    (void)out;
    (void)len;
    return -1;
}

void test_generate_fixed_bytes(void) {
    uint8_t seed[16];
    for (int i = 0; i < 16; i++) {
        seed[i] = (uint8_t)(0x10 + i);
    }
    tp_rng rng = {fixed_fill, seed};
    tp_id128 id;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id128_generate(&rng, &id, &err));
    char hex[33];
    tp_hex_encode_lower(id.bytes, 16U, hex);
    TEST_ASSERT_EQUAL_STRING("101112131415161718191a1b1c1d1e1f", hex);
    TEST_ASSERT_FALSE(tp_id128_is_nil(id));
}

void test_generate_short_read_is_structured(void) {
    tp_rng rng = {short_fill, NULL};
    tp_id128 id;
    tp_error err;
    tp_status st = tp_id128_generate(&rng, &id, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RNG_FAILED, st);
    TEST_ASSERT_EQUAL_STRING("rng_failed", tp_status_id(st));
    TEST_ASSERT_TRUE(tp_id128_is_nil(id)); /* left nil on failure */
}

void test_generate_failure_is_structured(void) {
    tp_rng rng = {fail_fill, NULL};
    tp_id128 id;
    tp_error err;
    tp_status st = tp_id128_generate(&rng, &id, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RNG_FAILED, st);
    TEST_ASSERT_EQUAL_STRING("rng_failed", tp_status_id(st));
    TEST_ASSERT_TRUE(tp_id128_is_nil(id));
}

void test_generate_null_args(void) {
    tp_id128 id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_id128_generate(NULL, &id, NULL));
    tp_rng rng = {NULL, NULL};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_id128_generate(&rng, &id, NULL));
    tp_rng ok = {fail_fill, NULL};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_id128_generate(&ok, NULL, NULL));
}

void test_nil_and_eq(void) {
    tp_id128 z = tp_id128_nil();
    TEST_ASSERT_TRUE(tp_id128_is_nil(z));
    TEST_ASSERT_TRUE(tp_id128_eq(z, tp_id128_nil()));
    uint8_t seed[16] = {1};
    tp_rng rng = {fixed_fill, seed};
    tp_id128 a;
    (void)tp_id128_generate(&rng, &a, NULL);
    TEST_ASSERT_FALSE(tp_id128_eq(a, z));
    TEST_ASSERT_TRUE(tp_id128_eq(a, a));
}

/* Default OS RNG seam works without engine-private API. */
void test_os_rng_produces_distinct_ids(void) {
    tp_rng rng = tp_rng_os();
    tp_id128 a, b;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id128_generate(&rng, &a, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id128_generate(&rng, &b, NULL));
    TEST_ASSERT_FALSE(tp_id128_is_nil(a));
    TEST_ASSERT_FALSE(tp_id128_eq(a, b)); /* 1/2^128 false-fail: effectively never */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_generate_fixed_bytes);
    RUN_TEST(test_generate_short_read_is_structured);
    RUN_TEST(test_generate_failure_is_structured);
    RUN_TEST(test_generate_null_args);
    RUN_TEST(test_nil_and_eq);
    RUN_TEST(test_os_rng_produces_distinct_ids);
    return UNITY_END();
}
