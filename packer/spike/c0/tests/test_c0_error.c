/* C0-01: pins every tp_c0_detail machine token. These are the --json/MCP error
 * ids a client matches on, so a rename is a visible contract change, not a
 * silent edit (mirrors tp_core test_status_id). */

#include "tp_c0/tp_c0_error.h"
#include "unity.h"

#include <stdbool.h>

void setUp(void) {}
void tearDown(void) {}

void test_all_tokens_pinned(void) {
    TEST_ASSERT_EQUAL_STRING("ok", tp_c0_detail_id(TP_C0_OK));
    TEST_ASSERT_EQUAL_STRING("null_arg", tp_c0_detail_id(TP_C0_ERR_NULL_ARG));
    TEST_ASSERT_EQUAL_STRING("empty", tp_c0_detail_id(TP_C0_ERR_EMPTY));
    TEST_ASSERT_EQUAL_STRING("buffer_too_small", tp_c0_detail_id(TP_C0_ERR_BUFFER_TOO_SMALL));
    TEST_ASSERT_EQUAL_STRING("oom", tp_c0_detail_id(TP_C0_ERR_OOM));
    TEST_ASSERT_EQUAL_STRING("invalid_utf8", tp_c0_detail_id(TP_C0_ERR_INVALID_UTF8));
    TEST_ASSERT_EQUAL_STRING("id_bad_prefix", tp_c0_detail_id(TP_C0_ERR_ID_BAD_PREFIX));
    TEST_ASSERT_EQUAL_STRING("id_bad_hex", tp_c0_detail_id(TP_C0_ERR_ID_BAD_HEX));
    TEST_ASSERT_EQUAL_STRING("id_bad_length", tp_c0_detail_id(TP_C0_ERR_ID_BAD_LENGTH));
    TEST_ASSERT_EQUAL_STRING("id_trailing", tp_c0_detail_id(TP_C0_ERR_ID_TRAILING));
    TEST_ASSERT_EQUAL_STRING("id_nil", tp_c0_detail_id(TP_C0_ERR_ID_NIL));
    TEST_ASSERT_EQUAL_STRING("rng_short", tp_c0_detail_id(TP_C0_ERR_RNG_SHORT));
    TEST_ASSERT_EQUAL_STRING("rng_fail", tp_c0_detail_id(TP_C0_ERR_RNG_FAIL));
    TEST_ASSERT_EQUAL_STRING("path_not_absolute", tp_c0_detail_id(TP_C0_ERR_PATH_NOT_ABSOLUTE));
    TEST_ASSERT_EQUAL_STRING("path_drive_relative", tp_c0_detail_id(TP_C0_ERR_PATH_DRIVE_REL));
    TEST_ASSERT_EQUAL_STRING("path_bad_unc", tp_c0_detail_id(TP_C0_ERR_PATH_BAD_UNC));
    TEST_ASSERT_EQUAL_STRING("path_device", tp_c0_detail_id(TP_C0_ERR_PATH_DEVICE));
    TEST_ASSERT_EQUAL_STRING("key_absolute", tp_c0_detail_id(TP_C0_ERR_KEY_ABSOLUTE));
    TEST_ASSERT_EQUAL_STRING("key_traversal", tp_c0_detail_id(TP_C0_ERR_KEY_TRAVERSAL));
    TEST_ASSERT_EQUAL_STRING("collision_exhausted", tp_c0_detail_id(TP_C0_ERR_COLLISION_EXHAUSTED));
    /* C0-02 operation / transaction tokens (append-only). */
    TEST_ASSERT_EQUAL_STRING("op_unknown", tp_c0_detail_id(TP_C0_ERR_OP_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("bad_json", tp_c0_detail_id(TP_C0_ERR_BAD_JSON));
    TEST_ASSERT_EQUAL_STRING("txn_bad_version", tp_c0_detail_id(TP_C0_ERR_TXN_BAD_VERSION));
    TEST_ASSERT_EQUAL_STRING("txn_bad_id", tp_c0_detail_id(TP_C0_ERR_TXN_BAD_ID));
    TEST_ASSERT_EQUAL_STRING("txn_duplicate_id", tp_c0_detail_id(TP_C0_ERR_TXN_DUPLICATE_ID));
    TEST_ASSERT_EQUAL_STRING("txn_missing_field", tp_c0_detail_id(TP_C0_ERR_TXN_MISSING_FIELD));
    TEST_ASSERT_EQUAL_STRING("txn_bad_type", tp_c0_detail_id(TP_C0_ERR_TXN_BAD_TYPE));
    TEST_ASSERT_EQUAL_STRING("unknown_field", tp_c0_detail_id(TP_C0_ERR_UNKNOWN_FIELD));
    TEST_ASSERT_EQUAL_STRING("selector_ambiguous", tp_c0_detail_id(TP_C0_ERR_SELECTOR_AMBIGUOUS));
    TEST_ASSERT_EQUAL_STRING("selector_unresolved", tp_c0_detail_id(TP_C0_ERR_SELECTOR_UNRESOLVED));
    TEST_ASSERT_EQUAL_STRING("revision_conflict", tp_c0_detail_id(TP_C0_ERR_REVISION_CONFLICT));
    TEST_ASSERT_EQUAL_STRING("invalid_revision", tp_c0_detail_id(TP_C0_ERR_INVALID_REVISION));
    /* C0-03 recovery-journal tokens (append-only). */
    TEST_ASSERT_EQUAL_STRING("journal_short", tp_c0_detail_id(TP_C0_ERR_JOURNAL_SHORT));
    TEST_ASSERT_EQUAL_STRING("journal_bad_magic", tp_c0_detail_id(TP_C0_ERR_JOURNAL_BAD_MAGIC));
    TEST_ASSERT_EQUAL_STRING("journal_bad_version", tp_c0_detail_id(TP_C0_ERR_JOURNAL_BAD_VERSION));
    TEST_ASSERT_EQUAL_STRING("journal_bad_kind", tp_c0_detail_id(TP_C0_ERR_JOURNAL_BAD_KIND));
    TEST_ASSERT_EQUAL_STRING("journal_bad_checksum", tp_c0_detail_id(TP_C0_ERR_JOURNAL_BAD_CHECKSUM));
    TEST_ASSERT_EQUAL_STRING("journal_too_large", tp_c0_detail_id(TP_C0_ERR_JOURNAL_TOO_LARGE));
    TEST_ASSERT_EQUAL_STRING("journal_retention_full", tp_c0_detail_id(TP_C0_ERR_JOURNAL_RETENTION_FULL));
}

void test_tokens_are_machine_ids(void) {
    for (int s = TP_C0_OK; s <= TP_C0_ERR_JOURNAL_RETENTION_FULL; s++) {
        const char *id = tp_c0_detail_id((tp_c0_detail)s);
        for (const char *c = id; *c; c++) {
            bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
            TEST_ASSERT_TRUE_MESSAGE(ok, id);
        }
    }
}

void test_unknown_detail(void) {
    TEST_ASSERT_EQUAL_STRING("unknown", tp_c0_detail_id((tp_c0_detail)9999));
}

void test_detail_count_sentinel(void) {
    /* tp_c0_txn_result.c's code_from_str iterates [0, TP_C0_DETAIL_COUNT), so an
     * append-only token still round-trips on version skew rather than being
     * dropped as "unknown error code". Pin that COUNT sits past the last real
     * code and is not itself a decodable token (it maps to ""). */
    TEST_ASSERT_TRUE(TP_C0_DETAIL_COUNT > TP_C0_ERR_JOURNAL_RETENTION_FULL);
    TEST_ASSERT_EQUAL_STRING("journal_retention_full", tp_c0_detail_id((tp_c0_detail)(TP_C0_DETAIL_COUNT - 1)));
    TEST_ASSERT_EQUAL_STRING("", tp_c0_detail_id(TP_C0_DETAIL_COUNT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_all_tokens_pinned);
    RUN_TEST(test_tokens_are_machine_ids);
    RUN_TEST(test_unknown_detail);
    RUN_TEST(test_detail_count_sentinel);
    return UNITY_END();
}
