#include "app_automation_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nt_utf8_fs.h"
#include "tp_core/tp_scan.h"
#include "unity.h"

#define POLICY_PATH APP_AUTOMATION_TEST_ROOT "/automation/permissions.json"

void setUp(void) {
    tp_mkdirs(APP_AUTOMATION_TEST_ROOT "/automation");
    (void)nt_utf8_rmdir(POLICY_PATH);
    (void)nt_utf8_remove(POLICY_PATH);
}

void tearDown(void) {
    (void)nt_utf8_remove(POLICY_PATH);
    (void)nt_utf8_rmdir(POLICY_PATH);
    (void)nt_utf8_rmdir(APP_AUTOMATION_TEST_ROOT "/automation");
    (void)nt_utf8_rmdir(APP_AUTOMATION_TEST_ROOT);
}

static void write_bytes(const char *bytes, size_t length) {
    FILE *file = nt_utf8_fopen(POLICY_PATH, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(length, fwrite(bytes, 1U, length, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_document(const char *text) {
    write_bytes(text, strlen(text));
}

static void expect_mode(app_automation_mode expected) {
    app_automation_mode mode = APP_AUTOMATION_DISABLED;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
        app_automation_policy_read(APP_AUTOMATION_TEST_ROOT, &mode, &err), err.msg);
    TEST_ASSERT_EQUAL_INT(expected, mode);
}

static void expect_refusal(void) {
    app_automation_mode mode = APP_AUTOMATION_ALLOW_ALL;
    tp_error err = {{0}};
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK,
        app_automation_policy_read(APP_AUTOMATION_TEST_ROOT, &mode, &err));
    TEST_ASSERT_EQUAL_INT(APP_AUTOMATION_DISABLED, mode);
    TEST_ASSERT_NOT_EQUAL('\0', err.msg[0]);
}

static void test_missing_policy_defaults_to_ask_without_creation(void) {
    expect_mode(APP_AUTOMATION_ASK);
    TEST_ASSERT_NULL(nt_utf8_fopen(POLICY_PATH, "rb"));
    app_automation_mode mode = APP_AUTOMATION_DISABLED;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
        app_automation_policy_read(APP_AUTOMATION_TEST_ROOT "/uncreated",
                                     &mode, NULL));
    TEST_ASSERT_EQUAL_INT(APP_AUTOMATION_ASK, mode);
    TEST_ASSERT_FALSE(tp_scan_is_dir(APP_AUTOMATION_TEST_ROOT "/uncreated"));
}

static void test_modes_are_read_again_after_each_replacement(void) {
    write_document("{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[]}");
    expect_mode(APP_AUTOMATION_ALLOW_ALL);
    write_document("{\"schema\":1,\"mode\":\"disabled\",\"projects\":[]}");
    expect_mode(APP_AUTOMATION_DISABLED);
    write_document("{\"schema\":1,\"mode\":\"ask\",\"projects\":[]}");
    expect_mode(APP_AUTOMATION_ASK);
    write_document("invalid");
    expect_refusal();
}

static void test_project_decisions_accept_native_absolute_paths(void) {
#ifdef _WIN32
    const char *path = "C:/project/demo.ntpacker_project";
#else
    const char *path = "/project/demo.ntpacker_project";
#endif
    char text[512];
    (void)snprintf(text, sizeof text,
        "{\"schema\":1,\"mode\":\"ask\",\"projects\":[{\"path\":\"%s\",\"decision\":\"allow\"}]}", path);
    write_document(text);
    expect_mode(APP_AUTOMATION_ASK);
    (void)snprintf(text, sizeof text,
        "{\"schema\":1,\"mode\":\"ask\",\"projects\":[{\"path\":\"%s\",\"decision\":\"deny\"}]}", path);
    write_document(text);
    expect_mode(APP_AUTOMATION_ASK);
}

static void test_invalid_documents_never_fall_back_to_allow(void) {
    const char *const invalid[] = {
        "", "{}", "[]", "null",
        "{\"schema\":2,\"mode\":\"ask\",\"projects\":[]}",
        "{\"schema\":01,\"mode\":\"ask\",\"projects\":[]}",
        "{\"schema\":1.0000000000000001,\"mode\":\"allow_all\",\"projects\":[]}",
        "{\"schema\":1,\"mode\":\"allow_all\"}",
        "{\"schema\":1,\"mode\":\"ALLOW_ALL\",\"projects\":[]}",
        "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":{},\"extra\":1}",
        "{\"schema\":1,\"mode\":\"disabled\",\"mode\":\"allow_all\",\"projects\":[]}",
        "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[],\"extra\":1}",
        "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[{\"path\":\"relative\",\"decision\":\"allow\"}]}",
        "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[{\"path\":\"/a\",\"path\":\"/b\",\"decision\":\"allow\"}]}",
        "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[{\"path\":\"/a\",\"decision\":\"allow\",\"extra\":1}]}",
        "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[{\"path\":\"/a\",\"decision\":\"maybe\"}]}",
        "{\"schema\":1,\"mode\":\"allow_all\\u0000disabled\",\"projects\":[]}",
        "{\"schema\":1,\"mode\":\"allow_all\n\",\"projects\":[]}",
        "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[]} trailing"
    };
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; ++i) {
        write_document(invalid[i]);
        expect_refusal();
    }
}

static void test_invalid_utf8_and_embedded_nul_are_refused(void) {
    static const char bad_utf8[] = "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[{\"path\":\"/\xc3x\",\"decision\":\"allow\"}]}";
    write_bytes(bad_utf8, sizeof bad_utf8 - 1U);
    expect_refusal();
    static const char embedded_nul[] = "{\"schema\":1,\"mode\":\"allow_all\",\"projects\":[]}\0ignored";
    write_bytes(embedded_nul, sizeof embedded_nul - 1U);
    expect_refusal();
}

static void test_oversized_policy_is_refused(void) {
    const size_t length = 1024U * 1024U + 1U;
    char *bytes = malloc(length);
    TEST_ASSERT_NOT_NULL(bytes);
    memset(bytes, ' ', length);
    static const char document[] = "{\"schema\":1,\"mode\":\"ask\",\"projects\":[]}";
    memcpy(bytes, document, sizeof document - 1U);
    write_bytes(bytes, length - 1U);
    expect_mode(APP_AUTOMATION_ASK);
    write_bytes(bytes, length);
    free(bytes);
    expect_refusal();
}

static void test_unreadable_policy_is_refused(void) {
    tp_mkdirs(POLICY_PATH);
    expect_refusal();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_missing_policy_defaults_to_ask_without_creation);
    RUN_TEST(test_modes_are_read_again_after_each_replacement);
    RUN_TEST(test_project_decisions_accept_native_absolute_paths);
    RUN_TEST(test_invalid_documents_never_fall_back_to_allow);
    RUN_TEST(test_invalid_utf8_and_embedded_nul_are_refused);
    RUN_TEST(test_oversized_policy_is_refused);
    RUN_TEST(test_unreadable_policy_is_refused);
    return UNITY_END();
}
