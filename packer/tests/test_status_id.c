/* Pins tp_status_id()'s stable machine tokens: this string is a --json error
 * contract an agent matches on, so every enum value's token is frozen here.
 * Renaming a token requires a schema bump, not a silent edit. */

#include "tp_core/tp_error.h"
#include "unity.h"

#include <stdbool.h>

void setUp(void) {}
void tearDown(void) {}

void test_status_id_tokens(void) {
    TEST_ASSERT_EQUAL_STRING("ok", tp_status_id(TP_STATUS_OK));
    TEST_ASSERT_EQUAL_STRING("unimplemented", tp_status_id(TP_STATUS_UNIMPLEMENTED));
    TEST_ASSERT_EQUAL_STRING("invalid_argument", tp_status_id(TP_STATUS_INVALID_ARGUMENT));
    TEST_ASSERT_EQUAL_STRING("bad_magic", tp_status_id(TP_STATUS_BAD_MAGIC));
    TEST_ASSERT_EQUAL_STRING("bad_version", tp_status_id(TP_STATUS_BAD_VERSION));
    TEST_ASSERT_EQUAL_STRING("out_of_bounds", tp_status_id(TP_STATUS_OUT_OF_BOUNDS));
    TEST_ASSERT_EQUAL_STRING("hash_collision", tp_status_id(TP_STATUS_HASH_COLLISION));
    TEST_ASSERT_EQUAL_STRING("unknown_region", tp_status_id(TP_STATUS_UNKNOWN_REGION));
    TEST_ASSERT_EQUAL_STRING("page_not_found", tp_status_id(TP_STATUS_PAGE_NOT_FOUND));
    TEST_ASSERT_EQUAL_STRING("unsupported_texture", tp_status_id(TP_STATUS_UNSUPPORTED_TEXTURE));
    TEST_ASSERT_EQUAL_STRING("oom", tp_status_id(TP_STATUS_OOM));
    TEST_ASSERT_EQUAL_STRING("builder_failed", tp_status_id(TP_STATUS_BUILDER_FAILED));
    TEST_ASSERT_EQUAL_STRING("bad_project", tp_status_id(TP_STATUS_BAD_PROJECT));
}

/* Tokens are machine ids: lowercase, no spaces (unlike tp_status_str prose). */
void test_status_id_is_machine_token(void) {
    for (int s = TP_STATUS_OK; s <= TP_STATUS_BAD_PROJECT; s++) {
        const char *id = tp_status_id((tp_status)s);
        for (const char *c = id; *c; c++) {
            TEST_ASSERT_TRUE_MESSAGE(*c != ' ', id);
            bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
            TEST_ASSERT_TRUE_MESSAGE(ok, id);
        }
    }
}

/* Out-of-range value hits the post-switch fallthrough (a defensive default). */
void test_status_id_unknown(void) {
    TEST_ASSERT_EQUAL_STRING("unknown_status", tp_status_id((tp_status)9999));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_status_id_tokens);
    RUN_TEST(test_status_id_is_machine_token);
    RUN_TEST(test_status_id_unknown);
    return UNITY_END();
}
