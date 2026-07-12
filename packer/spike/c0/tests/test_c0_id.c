/* C0-01 task 2: 128-bit ID generation via injectable RNG (fixed / short-read /
 * failure seams -> structured error, never abort), plus the versioned stable
 * hash and sprite_id golden vectors cross-checked against an independent
 * FNV-1a/128 reference. */

#include "tp_c0/tp_c0_id.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void id_hex(tp_c0_id128 id, char out[33]) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = h[id.bytes[i] >> 4];
        out[2 * i + 1] = h[id.bytes[i] & 0x0F];
    }
    out[32] = '\0';
}

/* --- RNG seams --- */
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
    tp_c0_rng rng = {fixed_fill, seed};
    tp_c0_id128 id;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id128_generate(&rng, &id, &err));
    char hex[33];
    id_hex(id, hex);
    TEST_ASSERT_EQUAL_STRING("101112131415161718191a1b1c1d1e1f", hex);
    TEST_ASSERT_FALSE(tp_c0_id128_is_nil(id));
}

void test_generate_short_read_is_structured(void) {
    tp_c0_rng rng = {short_fill, NULL};
    tp_c0_id128 id;
    tp_error err;
    tp_c0_detail d = tp_c0_id128_generate(&rng, &id, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_RNG_SHORT, d);
    TEST_ASSERT_EQUAL_STRING("rng_short", tp_c0_detail_id(d));
    TEST_ASSERT_TRUE(tp_c0_id128_is_nil(id)); /* left nil on failure */
}

void test_generate_failure_is_structured(void) {
    tp_c0_rng rng = {fail_fill, NULL};
    tp_c0_id128 id;
    tp_error err;
    tp_c0_detail d = tp_c0_id128_generate(&rng, &id, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_RNG_FAIL, d);
    TEST_ASSERT_EQUAL_STRING("rng_fail", tp_c0_detail_id(d));
}

void test_generate_null_args(void) {
    tp_c0_id128 id;
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_NULL_ARG, tp_c0_id128_generate(NULL, &id, NULL));
    tp_c0_rng rng = {NULL, NULL};
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_NULL_ARG, tp_c0_id128_generate(&rng, &id, NULL));
    tp_c0_rng ok = {fail_fill, NULL};
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_NULL_ARG, tp_c0_id128_generate(&ok, NULL, NULL));
}

void test_nil_and_eq(void) {
    tp_c0_id128 z = tp_c0_id128_nil();
    TEST_ASSERT_TRUE(tp_c0_id128_is_nil(z));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(z, tp_c0_id128_nil()));
    uint8_t seed[16] = {1};
    tp_c0_rng rng = {fixed_fill, seed};
    tp_c0_id128 a;
    (void)tp_c0_id128_generate(&rng, &a, NULL);
    TEST_ASSERT_FALSE(tp_c0_id128_eq(a, z));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(a, a));
}

/* --- stable hash golden vectors (independent FNV-1a/128 reference) --- */
void test_hash128_golden(void) {
    char hex[33];
    id_hex(tp_c0_hash128("", 0), hex);
    TEST_ASSERT_EQUAL_STRING("6c62272e07bb014262b821756295c58d", hex); /* == FNV offset basis */
    id_hex(tp_c0_hash128("abc", 3), hex);
    TEST_ASSERT_EQUAL_STRING("a68d622cec8b5822836dbc7977af7f3b", hex);
}

void test_hash128_streaming_matches_oneshot(void) {
    tp_c0_hasher h = tp_c0_hasher_init();
    tp_c0_hasher_update(&h, "ab", 2);
    tp_c0_hasher_update(&h, "c", 1);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(tp_c0_hasher_final(h), tp_c0_hash128("abc", 3)));
}

void test_sprite_id_golden_and_rename_invariance(void) {
    tp_c0_id128 src;
    for (int i = 0; i < 16; i++) {
        src.bytes[i] = (uint8_t)i;
    }
    char hex[33];
    id_hex(tp_c0_sprite_id(src, "sub/button.png"), hex);
    TEST_ASSERT_EQUAL_STRING("d7b835171f3e59ce989654d46c6aaca0", hex);

    /* sprite_id is a pure function of (source_id, source-local key): a logical/
     * export rename never feeds it, so the ID is rename-invariant by construction.
     * A source-local key change (external file rename) DOES change it. */
    tp_c0_id128 same = tp_c0_sprite_id(src, "sub/button.png");
    TEST_ASSERT_TRUE(tp_c0_id128_eq(same, tp_c0_sprite_id(src, "sub/button.png")));
    tp_c0_id128 renamed_file = tp_c0_sprite_id(src, "sub/button2.png");
    TEST_ASSERT_FALSE(tp_c0_id128_eq(same, renamed_file));

    /* different source, same key -> different sprite */
    tp_c0_id128 src2 = src;
    src2.bytes[0] ^= 0xFFU;
    TEST_ASSERT_FALSE(tp_c0_id128_eq(same, tp_c0_sprite_id(src2, "sub/button.png")));
}

/* --- default OS RNG seam works without engine-private API --- */
void test_os_rng_produces_distinct_ids(void) {
    tp_c0_rng rng = tp_c0_rng_os();
    tp_c0_id128 a, b;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id128_generate(&rng, &a, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id128_generate(&rng, &b, NULL));
    TEST_ASSERT_FALSE(tp_c0_id128_is_nil(a));
    TEST_ASSERT_FALSE(tp_c0_id128_eq(a, b)); /* 1/2^128 false-fail: effectively never */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_generate_fixed_bytes);
    RUN_TEST(test_generate_short_read_is_structured);
    RUN_TEST(test_generate_failure_is_structured);
    RUN_TEST(test_generate_null_args);
    RUN_TEST(test_nil_and_eq);
    RUN_TEST(test_hash128_golden);
    RUN_TEST(test_hash128_streaming_matches_oneshot);
    RUN_TEST(test_sprite_id_golden_and_rename_invariance);
    RUN_TEST(test_os_rng_produces_distinct_ids);
    return UNITY_END();
}
