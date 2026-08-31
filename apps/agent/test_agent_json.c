#include <string.h>
#include "unity.h"
#include "app_json.h"

void setUp(void) {}
void tearDown(void) {}

static void valid_unicode_and_numbers(void) {
    const char input[] = "{\"name\":\"\\u0422\\ud83d\\ude00\",\"n\":-2.5e+3}";
    tp_error err = {0};
    cJSON *root = app_json_parse(input, sizeof input - 1U, &err);
    TEST_ASSERT_NOT_NULL(root);
    const char *keys[] = {"name", "n"};
    TEST_ASSERT_TRUE(app_json_object_fields(root, keys, 2U));
    TEST_ASSERT_TRUE(cJSON_GetObjectItemCaseSensitive(root, "n")->valuedouble == -2500.0);
    cJSON_Delete(root);
}

static void rejects_ambiguous_bytes_and_non_json_numbers(void) {
    static const char *const invalid[] = {
        "{\"a\":01}", "{\"a\":1.}", "{\"a\":-.2}", "{\"a\":1e}",
        "{\"a\":1e999}", "{\"a\":NaN}", "{\"a\":\"\\u0000\"}",
        "{\"a\":\"\x01\"}", "{\"a\":\"\xc0\xaf\"}",
        "{} trailing", "{\"a\":\"\\ud800\"}", "\xef\xbb\xbf{}"
    };
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; ++i) {
        tp_error err = {0};
        cJSON *root = app_json_parse(invalid[i], strlen(invalid[i]), &err);
        TEST_ASSERT_NULL_MESSAGE(root, invalid[i]);
        TEST_ASSERT_NOT_EMPTY(err.msg);
    }
    const char nul[] = {'{', '}', '\0', ' '};
    tp_error err = {0};
    TEST_ASSERT_NULL(app_json_parse(nul, sizeof nul, &err));
}

static void closed_object_rejects_duplicate_and_unknown(void) {
    const char *keys[] = {"mode"};
    cJSON *root = cJSON_Parse("{\"mode\":1,\"mode\":2}");
    TEST_ASSERT_FALSE(app_json_object_fields(root, keys, 1U));
    cJSON_Delete(root);
    root = cJSON_Parse("{\"mode\":1,\"other\":2}");
    TEST_ASSERT_FALSE(app_json_object_fields(root, keys, 1U));
    cJSON_Delete(root);
}

static void original_nested_bytes_remain_exact(void) {
    const char input[] = " {\"other\": [1,{\"x\":\"}\\\"\"}],\"par\\u0061ms\" : {\"transaction\": {  \"schema\" : 1 }}} ";
    tp_error err = {0};
    cJSON *root = app_json_parse(input, sizeof input - 1U, &err);
    TEST_ASSERT_NOT_NULL(root);
    size_t length = 0U;
    const char *params = app_json_member_span(input, sizeof input - 1U, root, "params", &length);
    TEST_ASSERT_NOT_NULL(params);
    const cJSON *parsed_params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const char *transaction = app_json_member_span(params, length, parsed_params, "transaction", &length);
    const char expected[] = "{  \"schema\" : 1 }";
    TEST_ASSERT_EQUAL(sizeof expected - 1U, length);
    TEST_ASSERT_EQUAL_MEMORY(expected, transaction, length);
    cJSON_Delete(root);
}

static void exact_integer_spelling(void) {
    const char *valid[] = {"0", "-0", "1.0", "100e-2", "1e2", "0.001e3",
        "9007199254740991", "900719925474099100e-2", "0e-999999999999999999"};
    const char *invalid[] = {"-1", "1.0000000000000001", "0.99999999999999999",
        "1e-9999999999999999", "1e99999999999999999", "9007199254740992",
        "9007199254740991.1", "9007199254740990.999999", "-1e-999"};
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i)
        TEST_ASSERT_TRUE_MESSAGE(app_json_safe_integer(valid[i], strlen(valid[i])), valid[i]);
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        TEST_ASSERT_FALSE_MESSAGE(app_json_safe_integer(invalid[i], strlen(invalid[i])), invalid[i]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(valid_unicode_and_numbers);
    RUN_TEST(rejects_ambiguous_bytes_and_non_json_numbers);
    RUN_TEST(closed_object_rejects_duplicate_and_unknown);
    RUN_TEST(original_nested_bytes_remain_exact);
    RUN_TEST(exact_integer_spelling);
    return UNITY_END();
}
