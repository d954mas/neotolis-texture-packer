/* F1-01 Part A: the promoted structural-ID primitive surface (tp_id).
 *
 * Pins the production shape-ID parse/format (atlas_/source_/anim_/target_ + 32
 * hex), the versioned stable hash (FNV-1a/128) and sprite_id via byte-goldens
 * cross-checked against the accepted C0-01 spike vectors, plus the production
 * tp_status error mapping. A mix change to the hash is a visible golden diff. */

#include "tp_core/tp_id.h"
#include "tp_hex.h" /* shared lowercase-hex encoder (drift guard) */
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void id_hex(tp_id128 id, char out[33]) { tp_hex_encode_lower(id.bytes, 16, out); }

/* --- generate via injected RNG (fixed bytes) then format/parse round-trip --- */
static int fixed_fill(void *ctx, uint8_t *out, size_t len) {
    memcpy(out, (const uint8_t *)ctx, len);
    return (int)len;
}

void test_generate_format_parse_roundtrip(void) {
    uint8_t seed[16];
    for (int i = 0; i < 16; i++) {
        seed[i] = (uint8_t)(0x10 + i);
    }
    tp_rng rng = {fixed_fill, seed};
    tp_id128 id;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id128_generate(&rng, &id, NULL));

    char text[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, id, text, sizeof text, NULL));
    TEST_ASSERT_EQUAL_STRING("atlas_101112131415161718191a1b1c1d1e1f", text);

    tp_id_kind k = TP_ID_KIND_INVALID;
    tp_id128 back;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_parse(text, &k, &back, NULL));
    TEST_ASSERT_EQUAL_INT(TP_ID_KIND_ATLAS, k);
    TEST_ASSERT_TRUE(tp_id128_eq(id, back));
}

/* --- format: every kind prefix, nil, and the error paths --- */
void test_format_kinds_and_errors(void) {
    tp_id128 nil = tp_id128_nil();
    char text[TP_ID_TEXT_CAP];

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, nil, text, sizeof text, NULL));
    TEST_ASSERT_EQUAL_STRING("atlas_00000000000000000000000000000000", text);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_SOURCE, nil, text, sizeof text, NULL));
    TEST_ASSERT_EQUAL_STRING("source_00000000000000000000000000000000", text);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ANIM, nil, text, sizeof text, NULL));
    TEST_ASSERT_EQUAL_STRING("anim_00000000000000000000000000000000", text);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_TARGET, nil, text, sizeof text, NULL));
    TEST_ASSERT_EQUAL_STRING("target_00000000000000000000000000000000", text);

    /* "source_"/"target_" (7) + 32 + NUL == 40 == TP_ID_TEXT_CAP is the exact cap. */
    TEST_ASSERT_EQUAL_INT(40, TP_ID_TEXT_CAP);

    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_id_format(TP_ID_KIND_INVALID, nil, text, sizeof text, &err));
    TEST_ASSERT_EQUAL_STRING("id_malformed", tp_status_id(TP_STATUS_ID_MALFORMED));
    /* buffer one short of "target_" + 32 hex + NUL -> OUT_OF_BOUNDS, never a write past the end. */
    char tiny[39];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_id_format(TP_ID_KIND_TARGET, nil, tiny, sizeof tiny, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_id_format(TP_ID_KIND_ATLAS, nil, NULL, 0, &err));
}

/* --- parse: mixed-case hex re-emits lowercase; nil OK; every malformed reason --- */
void test_parse_and_errors(void) {
    tp_id_kind k = TP_ID_KIND_INVALID;
    tp_id128 id;

    /* uppercase hex accepted, re-emitted lowercase on the round trip */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_id_parse("anim_0123456789ABCDEFfedcba9876543210", &k, &id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_ID_KIND_ANIM, k);
    char text[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(k, id, text, sizeof text, NULL));
    TEST_ASSERT_EQUAL_STRING("anim_0123456789abcdeffedcba9876543210", text);

    /* nil parses OK (is_nil is the caller's required-id gate, not the parser's). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_id_parse("atlas_00000000000000000000000000000000", &k, &id, NULL));
    TEST_ASSERT_TRUE(tp_id128_is_nil(id));

    /* out_kind/out_id may be NULL. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_id_parse("target_ffffffffffffffffffffffffffffffff", NULL, NULL, NULL));

    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_id_parse(NULL, &k, &id, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_id_parse("", &k, &id, &err));                   /* empty */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_id_parse("bogus_00", &k, &id, &err));           /* bad prefix */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED,
                          tp_id_parse("atlas_zz000000000000000000000000000000", &k, &id, &err));     /* bad hex */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_id_parse("atlas_00", &k, &id, &err));           /* short */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED,
                          tp_id_parse("atlas_000000000000000000000000000000001", &k, &id, &err));    /* trailing */
    /* case-SENSITIVE prefix: "ATLAS_" is not a valid kind prefix. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED,
                          tp_id_parse("ATLAS_00000000000000000000000000000000", &k, &id, &err));
}

/* --- stable hash byte-goldens (independent FNV-1a/128 reference vectors) --- */
void test_hash128_golden(void) {
    char hex[33];
    id_hex(tp_hash128("", 0), hex);
    TEST_ASSERT_EQUAL_STRING("6c62272e07bb014262b821756295c58d", hex); /* == FNV offset basis */
    id_hex(tp_hash128("abc", 3), hex);
    TEST_ASSERT_EQUAL_STRING("a68d622cec8b5822836dbc7977af7f3b", hex);
}

void test_hash128_streaming_matches_oneshot(void) {
    tp_hasher h = tp_hasher_init();
    tp_hasher_update(&h, "ab", 2);
    tp_hasher_update(&h, "c", 1);
    TEST_ASSERT_TRUE(tp_id128_eq(tp_hasher_final(h), tp_hash128("abc", 3)));
}

void test_sprite_id_golden_and_rename_invariance(void) {
    tp_id128 src;
    for (int i = 0; i < 16; i++) {
        src.bytes[i] = (uint8_t)i;
    }
    char hex[33];
    id_hex(tp_sprite_id(src, "sub/button.png"), hex);
    TEST_ASSERT_EQUAL_STRING("d7b835171f3e59ce989654d46c6aaca0", hex);

    /* pure function of (source_id, source-local key): rename-invariant by
     * construction; a source-local key change DOES change it. */
    tp_id128 same = tp_sprite_id(src, "sub/button.png");
    TEST_ASSERT_TRUE(tp_id128_eq(same, tp_sprite_id(src, "sub/button.png")));
    TEST_ASSERT_FALSE(tp_id128_eq(same, tp_sprite_id(src, "sub/button2.png")));
    tp_id128 src2 = src;
    src2.bytes[0] ^= 0xFFU;
    TEST_ASSERT_FALSE(tp_id128_eq(same, tp_sprite_id(src2, "sub/button.png")));
}

/* --- bucket: distinct ids bucket differently (a map key, not the id) --- */
void test_bucket_distinct(void) {
    tp_id128 a = tp_id128_nil();
    tp_id128 b = tp_id128_nil();
    b.bytes[7] = 0x01U;
    TEST_ASSERT_NOT_EQUAL(tp_id128_bucket(a), tp_id128_bucket(b));
    TEST_ASSERT_EQUAL_UINT64(tp_id128_bucket(a), tp_id128_bucket(tp_id128_nil()));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_generate_format_parse_roundtrip);
    RUN_TEST(test_format_kinds_and_errors);
    RUN_TEST(test_parse_and_errors);
    RUN_TEST(test_hash128_golden);
    RUN_TEST(test_hash128_streaming_matches_oneshot);
    RUN_TEST(test_sprite_id_golden_and_rename_invariance);
    RUN_TEST(test_bucket_distinct);
    return UNITY_END();
}
