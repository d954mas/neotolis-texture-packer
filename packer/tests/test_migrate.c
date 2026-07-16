/* F1-01 Part C (low-level): deterministic legacy synthetic-ID assigner promoted
 * from the accepted C0-01 spike. Golden default-hash vectors (identical to the
 * spike: same "lid1" tag + kind bytes), stability across repeated loads, kind
 * sensitivity, and a forced-collision seam proving the bounded salt sweep is
 * itself deterministic and reports exhaustion as a structured tp_status. */

#include "tp_core/tp_project_migrate.h"
#include "tp_hex.h"
#include "unity.h"
#include "../src/tp_project_internal.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void id_hex(tp_id128 id, char out[33]) { tp_hex_encode_lower(id.bytes, 16, out); }

void test_default_hash_golden(void) {
    char hex[33];
    id_hex(tp_legacy_hash_default(NULL, TP_ID_KIND_ATLAS, "0", 0), hex);
    TEST_ASSERT_EQUAL_STRING("8d46a888569812f658ec17242724828c", hex);
    id_hex(tp_legacy_hash_default(NULL, TP_ID_KIND_ATLAS, "1", 0), hex);
    TEST_ASSERT_EQUAL_STRING("79d4ed0c989812f658ef8fa75431a215", hex);
    id_hex(tp_legacy_hash_default(NULL, TP_ID_KIND_SOURCE, "0|assets", 0), hex);
    TEST_ASSERT_EQUAL_STRING("b2e5fe717349046b7deaae0f63b0a6c6", hex);
}

void test_assign_is_deterministic_across_loads(void) {
    tp_legacy_entry a[3] = {
        {TP_ID_KIND_ATLAS, "0", {{0}}},
        {TP_ID_KIND_SOURCE, "0|assets", {{0}}},
        {TP_ID_KIND_ANIM, "0|walk", {{0}}},
    };
    tp_legacy_entry b[3] = {
        {TP_ID_KIND_ATLAS, "0", {{0}}},
        {TP_ID_KIND_SOURCE, "0|assets", {{0}}},
        {TP_ID_KIND_ANIM, "0|walk", {{0}}},
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_legacy_assign(a, 3, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_legacy_assign(b, 3, NULL, NULL, NULL));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(tp_id128_eq(a[i].id, b[i].id));
        TEST_ASSERT_FALSE(tp_id128_is_nil(a[i].id));
    }
    char hex[33]; /* first entry == the standalone golden (salt 0, no collision) */
    id_hex(a[0].id, hex);
    TEST_ASSERT_EQUAL_STRING("8d46a888569812f658ec17242724828c", hex);
}

void test_kind_changes_id(void) {
    tp_id128 as_atlas = tp_legacy_hash_default(NULL, TP_ID_KIND_ATLAS, "0", 0);
    tp_id128 as_source = tp_legacy_hash_default(NULL, TP_ID_KIND_SOURCE, "0", 0);
    TEST_ASSERT_FALSE(tp_id128_eq(as_atlas, as_source));
}

/* Toy seam: salt 0 ignores the tuple, so two entries collide; a salt bump moves
 * byte[15]. Proves the bounded sweep disambiguates deterministically. */
static tp_id128 toy_hash(void *ctx, tp_id_kind kind, const char *tuple, uint32_t salt) {
    (void)ctx;
    (void)tuple;
    tp_id128 id = tp_id128_nil();
    id.bytes[0] = (uint8_t)kind;
    id.bytes[15] = (uint8_t)salt;
    return id;
}

void test_forced_collision_disambiguates_deterministically(void) {
    tp_legacy_entry e[2] = {
        {TP_ID_KIND_ATLAS, "alpha", {{0}}},
        {TP_ID_KIND_ATLAS, "beta", {{0}}}, /* collides at salt 0 */
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_legacy_assign(e, 2, toy_hash, NULL, NULL));
    TEST_ASSERT_FALSE(tp_id128_eq(e[0].id, e[1].id));
    TEST_ASSERT_EQUAL_UINT8(0, e[0].id.bytes[15]); /* salt 0 */
    TEST_ASSERT_EQUAL_UINT8(1, e[1].id.bytes[15]); /* bumped to salt 1 */

    tp_legacy_entry e2[2] = {
        {TP_ID_KIND_ATLAS, "alpha", {{0}}},
        {TP_ID_KIND_ATLAS, "beta", {{0}}},
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_legacy_assign(e2, 2, toy_hash, NULL, NULL));
    TEST_ASSERT_TRUE(tp_id128_eq(e[0].id, e2[0].id));
    TEST_ASSERT_TRUE(tp_id128_eq(e[1].id, e2[1].id));
}

/* Degenerate seam: constant regardless of salt -> the sweep must report
 * exhaustion instead of looping forever. */
static tp_id128 constant_hash(void *ctx, tp_id_kind kind, const char *tuple, uint32_t salt) {
    (void)ctx;
    (void)kind;
    (void)tuple;
    (void)salt;
    tp_id128 id = tp_id128_nil();
    id.bytes[0] = 0x42;
    return id;
}

void test_collision_exhaustion_is_structured(void) {
    tp_legacy_entry e[2] = {
        {TP_ID_KIND_ATLAS, "a", {{0}}},
        {TP_ID_KIND_ATLAS, "b", {{0}}},
    };
    tp_status st = tp_legacy_assign(e, 2, constant_hash, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_COLLISION_EXHAUSTED, st);
    TEST_ASSERT_EQUAL_STRING("id_collision_exhausted", tp_status_id(st));
}

typedef struct cluster_hash_context {
    tp_id128 ids[65];
} cluster_hash_context;

static tp_id128 cluster_hash(void *ctx, tp_id_kind kind, const char *tuple,
                             uint32_t salt) {
    (void)kind;
    (void)salt;
    cluster_hash_context *cluster = (cluster_hash_context *)ctx;
    size_t index = 0U;
    for (; *tuple; tuple++) {
        index = index * 10U + (size_t)(*tuple - '0');
    }
    return cluster->ids[index];
}

void test_adversarial_legacy_bucket_cluster_is_bounded(void) {
    enum { RECORDS = 65, TABLE_MASK = 255 };
    cluster_hash_context cluster = {0};
    int found = 0;
    for (uint32_t candidate = 1U; found < RECORDS; candidate++) {
        tp_id128 id = tp_id128_nil();
        id.bytes[12] = (uint8_t)(candidate >> 24U);
        id.bytes[13] = (uint8_t)(candidate >> 16U);
        id.bytes[14] = (uint8_t)(candidate >> 8U);
        id.bytes[15] = (uint8_t)candidate;
        if ((tp_id128_bucket(id) & (uint64_t)TABLE_MASK) == 0U) {
            cluster.ids[found++] = id;
        }
    }

    tp_legacy_entry entries[RECORDS] = {{0}};
    char tuples[RECORDS][8];
    for (int i = 0; i < RECORDS; i++) {
        (void)snprintf(tuples[i], sizeof tuples[i], "%d", i);
        entries[i].kind = TP_ID_KIND_SOURCE;
        entries[i].tuple = tuples[i];
    }
    tp_project__test_legacy_id_work_reset();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_legacy_assign(entries, RECORDS, cluster_hash, &cluster, NULL));
    const size_t probes = tp_project__test_legacy_id_work_take();
    TEST_ASSERT_LESS_OR_EQUAL_size_t((size_t)RECORDS * 64U, probes);
}

void test_legacy_index_oom_has_no_quadratic_fallback(void) {
    tp_legacy_entry entry = {
        TP_ID_KIND_ATLAS, "0", {{0xffU}},
    };
    tp_project__test_fail_next_legacy_id_index_alloc();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM,
                          tp_legacy_assign(&entry, 1U, NULL, NULL, NULL));
    TEST_ASSERT_TRUE(tp_id128_is_nil(entry.id));
}

/* The hash-set assigner must stay unique and deterministic at scale. */
void test_assign_1000_unique_and_deterministic(void) {
    enum { N = 1000 };
    static tp_legacy_entry a[N];
    static tp_legacy_entry b[N];
    static char tuples[N][16];
    for (int i = 0; i < N; i++) {
        (void)snprintf(tuples[i], sizeof tuples[i], "e%d", i);
        a[i].kind = TP_ID_KIND_SOURCE;
        a[i].tuple = tuples[i];
        a[i].id = tp_id128_nil();
        b[i].kind = TP_ID_KIND_SOURCE;
        b[i].tuple = tuples[i];
        b[i].id = tp_id128_nil();
    }
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_legacy_assign(a, N, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_legacy_assign(b, N, NULL, NULL, NULL));
    for (int i = 0; i < N; i++) {
        TEST_ASSERT_FALSE(tp_id128_is_nil(a[i].id));
        TEST_ASSERT_TRUE(tp_id128_eq(a[i].id, b[i].id));
    }
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            TEST_ASSERT_FALSE(tp_id128_eq(a[i].id, a[j].id));
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_hash_golden);
    RUN_TEST(test_assign_is_deterministic_across_loads);
    RUN_TEST(test_kind_changes_id);
    RUN_TEST(test_forced_collision_disambiguates_deterministically);
    RUN_TEST(test_collision_exhaustion_is_structured);
    RUN_TEST(test_adversarial_legacy_bucket_cluster_is_bounded);
    RUN_TEST(test_legacy_index_oom_has_no_quadratic_fallback);
    RUN_TEST(test_assign_1000_unique_and_deterministic);
    return UNITY_END();
}
