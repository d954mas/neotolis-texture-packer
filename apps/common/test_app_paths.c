#include "app_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_format.h"
#include "tp_core/tp_identity.h"
#include "unity.h"

#ifdef _WIN32
#define STATE_ENV "LOCALAPPDATA"
#define TEST_BASE "C:/agent-policy-test"
#define TEST_DATA "C:/agent-policy-test\\ntpacker"
#else
#define STATE_ENV "XDG_STATE_HOME"
#define TEST_BASE "/agent-policy-test"
#define TEST_DATA "/agent-policy-test/ntpacker"
#endif

static char saved_state[TP_IDENTITY_PATH_MAX];
static bool had_state;
#ifndef _WIN32
static char saved_home[TP_IDENTITY_PATH_MAX];
static bool had_home;
#endif

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    (void)_putenv_s(name, value ? value : "");
#else
    if (value) {
        (void)setenv(name, value, 1);
    } else {
        (void)unsetenv(name);
    }
#endif
}

void setUp(void) {
    const char *value = getenv(STATE_ENV);
    had_state = value != NULL;
    (void)snprintf(saved_state, sizeof saved_state, "%s", value ? value : "");
#ifndef _WIN32
    value = getenv("HOME");
    had_home = value != NULL;
    (void)snprintf(saved_home, sizeof saved_home, "%s", value ? value : "");
#endif
}

void tearDown(void) {
    set_env(STATE_ENV, had_state ? saved_state : NULL);
#ifndef _WIN32
    set_env("HOME", had_home ? saved_home : NULL);
#endif
}

static void test_gui_and_agent_use_the_same_platform_data_root(void) {
    set_env(STATE_ENV, TEST_BASE);
    char gui_root[TP_IDENTITY_PATH_MAX];
    char agent_root[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(app_paths_data_root(gui_root, sizeof gui_root, true));
    TEST_ASSERT_TRUE(app_paths_data_root(agent_root, sizeof agent_root, false));
    TEST_ASSERT_EQUAL_STRING(TEST_DATA, gui_root);
    TEST_ASSERT_EQUAL_STRING(gui_root, agent_root);
}

static void test_missing_environment_allows_only_gui_executable_fallback(void) {
    set_env(STATE_ENV, NULL);
#ifndef _WIN32
    set_env("HOME", NULL);
#endif
    char agent_root[TP_IDENTITY_PATH_MAX] = "stale";
    TEST_ASSERT_FALSE(app_paths_data_root(agent_root, sizeof agent_root, false));
    TEST_ASSERT_EQUAL_STRING("", agent_root);
    char exe[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
        tp_executable_directory(exe, sizeof exe, NULL));
    char expected[TP_IDENTITY_PATH_MAX];
    (void)snprintf(expected, sizeof expected, "%s/ntpacker-data", exe);
    char gui_root[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(app_paths_data_root(gui_root, sizeof gui_root, true));
    TEST_ASSERT_EQUAL_STRING(expected, gui_root);
}

static void test_a_truncated_root_is_unusable(void) {
    set_env(STATE_ENV, TEST_BASE);
    char output[4] = "old";
    TEST_ASSERT_FALSE(app_paths_data_root(output, sizeof output, false));
    TEST_ASSERT_EQUAL_STRING("", output);
}

#ifndef _WIN32
static void test_home_supplies_state_root_without_xdg(void) {
    set_env(STATE_ENV, NULL);
    set_env("HOME", TEST_BASE);
    char output[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(app_paths_data_root(output, sizeof output, false));
    TEST_ASSERT_EQUAL_STRING(TEST_BASE "/.local/state/ntpacker", output);
}
#endif

static void test_recovery_key_preserves_existing_gui_journals(void) {
    const tp_id128 key = app_recovery_key();
    TEST_ASSERT_EQUAL_MEMORY("ntpk_recovery_01", key.bytes, 16U);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_gui_and_agent_use_the_same_platform_data_root);
    RUN_TEST(test_missing_environment_allows_only_gui_executable_fallback);
    RUN_TEST(test_a_truncated_root_is_unusable);
#ifndef _WIN32
    RUN_TEST(test_home_supplies_state_root_without_xdg);
#endif
    RUN_TEST(test_recovery_key_preserves_existing_gui_journals);
    return UNITY_END();
}
