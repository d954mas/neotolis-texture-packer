/* C0-01 task 4: canonical shape-ID (prefix + 128-bit hex) format/parse rules --
 * prefixes atlas_/source_/anim_/target_, lowercase canonical output, case-
 * insensitive hex on input, nil handling, and every malformed-input reason. */

#include "tp_c0/tp_c0_id.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static int nib(char c) {
    return (c <= '9') ? (c - '0') : ((c | 0x20) - 'a' + 10);
}

static tp_c0_id128 id_from_hex(const char *hex32) {
    tp_c0_id128 id;
    for (int i = 0; i < 16; i++) {
        id.bytes[i] = (uint8_t)((nib(hex32[2 * i]) << 4) | nib(hex32[2 * i + 1]));
    }
    return id;
}

void test_format_each_kind(void) {
    tp_c0_id128 id = id_from_hex("0123456789abcdeffedcba9876543210");
    char out[TP_C0_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id_format(TP_C0_ID_KIND_ATLAS, id, out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_STRING("atlas_0123456789abcdeffedcba9876543210", out);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id_format(TP_C0_ID_KIND_SOURCE, id, out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_STRING("source_0123456789abcdeffedcba9876543210", out);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id_format(TP_C0_ID_KIND_ANIM, id, out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_STRING("anim_0123456789abcdeffedcba9876543210", out);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id_format(TP_C0_ID_KIND_TARGET, id, out, sizeof out, NULL));
    TEST_ASSERT_EQUAL_STRING("target_0123456789abcdeffedcba9876543210", out);
}

void test_format_rejects_invalid_kind_and_small_buffer(void) {
    tp_c0_id128 id = tp_c0_id128_nil();
    char out[TP_C0_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_BAD_PREFIX, tp_c0_id_format(TP_C0_ID_KIND_INVALID, id, out, sizeof out, NULL));
    char tiny[8];
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_BUFFER_TOO_SMALL, tp_c0_id_format(TP_C0_ID_KIND_ATLAS, id, tiny, sizeof tiny, NULL));
}

void test_roundtrip_and_binary_string_equivalence(void) {
    tp_c0_id128 id = id_from_hex("00112233445566778899aabbccddeeff");
    char text[TP_C0_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id_format(TP_C0_ID_KIND_SOURCE, id, text, sizeof text, NULL));
    tp_c0_id_kind kind;
    tp_c0_id128 back;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id_parse(text, &kind, &back, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_ID_KIND_SOURCE, kind);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(id, back)); /* binary shape == parsed string shape */
}

void test_parse_accepts_uppercase_hex_emits_lowercase(void) {
    tp_c0_id_kind kind;
    tp_c0_id128 id;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id_parse("atlas_0123456789ABCDEFfedcba9876543210", &kind, &id, NULL));
    char out[TP_C0_ID_TEXT_CAP];
    (void)tp_c0_id_format(kind, id, out, sizeof out, NULL);
    TEST_ASSERT_EQUAL_STRING("atlas_0123456789abcdeffedcba9876543210", out); /* canonical is lowercase */
}

void test_parse_nil_is_ok_but_flagged(void) {
    tp_c0_id128 id;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_id_parse("target_00000000000000000000000000000000", NULL, &id, NULL));
    TEST_ASSERT_TRUE(tp_c0_id128_is_nil(id)); /* parses structurally; caller rejects nil */
}

void test_parse_errors(void) {
    tp_c0_id128 id;
    tp_c0_id_kind k;
    /* unknown / missing / wrong-case prefix */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_BAD_PREFIX, tp_c0_id_parse("sprite_00112233445566778899aabbccddeeff", &k, &id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_BAD_PREFIX, tp_c0_id_parse("00112233445566778899aabbccddeeff", &k, &id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_BAD_PREFIX, tp_c0_id_parse("ATLAS_00112233445566778899aabbccddeeff", &k, &id, NULL));
    /* empty / null */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_EMPTY, tp_c0_id_parse("", &k, &id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_NULL_ARG, tp_c0_id_parse(NULL, &k, &id, NULL));
    /* too short / too long / non-hex / trailing */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_BAD_LENGTH, tp_c0_id_parse("atlas_00112233", &k, &id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_TRAILING, tp_c0_id_parse("atlas_00112233445566778899aabbccddeeff00", &k, &id, NULL));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_BAD_HEX, tp_c0_id_parse("atlas_0011223344556677889gaabbccddeeff", &k, &id, NULL));
}

void test_detail_tokens_stable(void) {
    TEST_ASSERT_EQUAL_STRING("id_bad_prefix", tp_c0_detail_id(TP_C0_ERR_ID_BAD_PREFIX));
    TEST_ASSERT_EQUAL_STRING("id_bad_hex", tp_c0_detail_id(TP_C0_ERR_ID_BAD_HEX));
    TEST_ASSERT_EQUAL_STRING("id_bad_length", tp_c0_detail_id(TP_C0_ERR_ID_BAD_LENGTH));
    TEST_ASSERT_EQUAL_STRING("id_trailing", tp_c0_detail_id(TP_C0_ERR_ID_TRAILING));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_format_each_kind);
    RUN_TEST(test_format_rejects_invalid_kind_and_small_buffer);
    RUN_TEST(test_roundtrip_and_binary_string_equivalence);
    RUN_TEST(test_parse_accepts_uppercase_hex_emits_lowercase);
    RUN_TEST(test_parse_nil_is_ok_but_flagged);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_detail_tokens_stable);
    return UNITY_END();
}
