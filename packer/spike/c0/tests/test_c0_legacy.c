/* C0-01 task 5: deterministic legacy synthetic IDs + collision fallback.
 * Golden default-hash vectors (independent FNV-1a/128 reference), stable across
 * repeated loads, kind-sensitivity, and a forced-collision hash seam proving the
 * salt-sweep disambiguation is itself deterministic and can report exhaustion. */

#include "tp_c0/tp_c0_legacy.h"
#include "tp_c0_test_util.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_default_hash_golden(void) {
    char hex[33];
    id_hex(tp_c0_legacy_hash_default(NULL, TP_C0_ID_KIND_ATLAS, "0", 0), hex);
    TEST_ASSERT_EQUAL_STRING("8d46a888569812f658ec17242724828c", hex);
    id_hex(tp_c0_legacy_hash_default(NULL, TP_C0_ID_KIND_ATLAS, "1", 0), hex);
    TEST_ASSERT_EQUAL_STRING("79d4ed0c989812f658ef8fa75431a215", hex);
    id_hex(tp_c0_legacy_hash_default(NULL, TP_C0_ID_KIND_SOURCE, "0|assets", 0), hex);
    TEST_ASSERT_EQUAL_STRING("b2e5fe717349046b7deaae0f63b0a6c6", hex);
}

void test_assign_is_deterministic_across_loads(void) {
    tp_c0_legacy_entry a[3] = {
        {TP_C0_ID_KIND_ATLAS, "0", {{0}}},
        {TP_C0_ID_KIND_SOURCE, "0|assets", {{0}}},
        {TP_C0_ID_KIND_ANIM, "0|walk", {{0}}},
    };
    tp_c0_legacy_entry b[3] = {
        {TP_C0_ID_KIND_ATLAS, "0", {{0}}},
        {TP_C0_ID_KIND_SOURCE, "0|assets", {{0}}},
        {TP_C0_ID_KIND_ANIM, "0|walk", {{0}}},
    };
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_legacy_assign(a, 3, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_legacy_assign(b, 3, NULL, NULL, NULL));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(tp_c0_id128_eq(a[i].id, b[i].id));
        TEST_ASSERT_FALSE(tp_c0_id128_is_nil(a[i].id));
    }
    /* first entry matches the standalone golden (salt 0, no collision) */
    char hex[33];
    id_hex(a[0].id, hex);
    TEST_ASSERT_EQUAL_STRING("8d46a888569812f658ec17242724828c", hex);
}

void test_kind_changes_id(void) {
    tp_c0_id128 as_atlas = tp_c0_legacy_hash_default(NULL, TP_C0_ID_KIND_ATLAS, "0", 0);
    tp_c0_id128 as_source = tp_c0_legacy_hash_default(NULL, TP_C0_ID_KIND_SOURCE, "0", 0);
    TEST_ASSERT_FALSE(tp_c0_id128_eq(as_atlas, as_source));
}

/* Toy seam: salt 0 ignores the tuple, so two entries collide; a salt bump moves
 * byte[15]. Proves the fallback disambiguates deterministically. */
static tp_c0_id128 toy_hash(void *ctx, tp_c0_id_kind kind, const char *tuple, uint32_t salt) {
    (void)ctx;
    (void)tuple;
    tp_c0_id128 id = tp_c0_id128_nil();
    id.bytes[0] = (uint8_t)kind;
    id.bytes[15] = (uint8_t)salt;
    return id;
}

void test_forced_collision_disambiguates_deterministically(void) {
    tp_c0_legacy_entry e[2] = {
        {TP_C0_ID_KIND_ATLAS, "alpha", {{0}}},
        {TP_C0_ID_KIND_ATLAS, "beta", {{0}}}, /* collides at salt 0 */
    };
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_legacy_assign(e, 2, toy_hash, NULL, NULL));
    TEST_ASSERT_FALSE(tp_c0_id128_eq(e[0].id, e[1].id));
    TEST_ASSERT_EQUAL_UINT8(0, e[0].id.bytes[15]); /* salt 0 */
    TEST_ASSERT_EQUAL_UINT8(1, e[1].id.bytes[15]); /* bumped to salt 1 */

    /* re-run -> identical assignment (reproducible collision fallback) */
    tp_c0_legacy_entry e2[2] = {
        {TP_C0_ID_KIND_ATLAS, "alpha", {{0}}},
        {TP_C0_ID_KIND_ATLAS, "beta", {{0}}},
    };
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_legacy_assign(e2, 2, toy_hash, NULL, NULL));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(e[0].id, e2[0].id));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(e[1].id, e2[1].id));
}

/* Degenerate seam: constant regardless of salt -> the sweep must report
 * exhaustion instead of looping forever. */
static tp_c0_id128 constant_hash(void *ctx, tp_c0_id_kind kind, const char *tuple, uint32_t salt) {
    (void)ctx;
    (void)kind;
    (void)tuple;
    (void)salt;
    tp_c0_id128 id = tp_c0_id128_nil();
    id.bytes[0] = 0x42;
    return id;
}

/* F6: the hash-set assigner must stay unique and deterministic at scale. */
void test_assign_1000_unique_and_deterministic(void) {
    enum { N = 1000 };
    static tp_c0_legacy_entry a[N];
    static tp_c0_legacy_entry b[N];
    static char tuples[N][16];
    for (int i = 0; i < N; i++) {
        snprintf(tuples[i], sizeof tuples[i], "e%d", i);
        a[i].kind = TP_C0_ID_KIND_SOURCE;
        a[i].tuple = tuples[i];
        a[i].id = tp_c0_id128_nil();
        b[i].kind = TP_C0_ID_KIND_SOURCE;
        b[i].tuple = tuples[i];
        b[i].id = tp_c0_id128_nil();
    }
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_legacy_assign(a, N, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_legacy_assign(b, N, NULL, NULL, NULL));
    for (int i = 0; i < N; i++) {
        TEST_ASSERT_FALSE(tp_c0_id128_is_nil(a[i].id));
        TEST_ASSERT_TRUE(tp_c0_id128_eq(a[i].id, b[i].id)); /* deterministic across loads */
    }
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            TEST_ASSERT_FALSE(tp_c0_id128_eq(a[i].id, a[j].id)); /* all unique */
        }
    }
}

void test_collision_exhaustion_is_structured(void) {
    tp_c0_legacy_entry e[2] = {
        {TP_C0_ID_KIND_ATLAS, "a", {{0}}},
        {TP_C0_ID_KIND_ATLAS, "b", {{0}}},
    };
    tp_c0_detail d = tp_c0_legacy_assign(e, 2, constant_hash, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_COLLISION_EXHAUSTED, d);
    TEST_ASSERT_EQUAL_STRING("collision_exhausted", tp_c0_detail_id(d));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_hash_golden);
    RUN_TEST(test_assign_is_deterministic_across_loads);
    RUN_TEST(test_kind_changes_id);
    RUN_TEST(test_forced_collision_disambiguates_deterministically);
    RUN_TEST(test_assign_1000_unique_and_deterministic);
    RUN_TEST(test_collision_exhaustion_is_structured);
    return UNITY_END();
}
