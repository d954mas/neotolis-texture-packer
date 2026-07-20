#include "nt_utf8_argv.h"
#include "nt_utf8_fs.h"

#if defined(_WIN32)

#include <windows.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_wide_paths_become_exact_utf8(void) {
    wchar_t executable[] = L"ntpacker.exe";
    wchar_t command[] = L"inspect";
    wchar_t path[] = L"C:\\projects\\\x043D\x0442\x043F\x0430\x043A\x0435\x0440\\\x043F\x0440\x043E\x0435\x043A\x0442.ntpacker_project";
    wchar_t *wide[] = {executable, command, path};
    nt_utf8_argv args = {0};
    char error[128] = {0};
    TEST_ASSERT_TRUE_MESSAGE(
        nt_utf8_argv_convert(3, wide, &args, error, sizeof error), error);
    TEST_ASSERT_EQUAL_INT(3, args.argc);
    TEST_ASSERT_EQUAL_STRING(
        "C:\\projects\\\xD0\xBD\xD1\x82\xD0\xBF\xD0\xB0\xD0\xBA\xD0\xB5\xD1\x80\\"
        "\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82.ntpacker_project",
        args.argv[2]);
    TEST_ASSERT_NULL(args.argv[3]);
    nt_utf8_argv_dispose(&args);
}

void test_unpaired_surrogate_fails_closed(void) {
    wchar_t executable[] = L"ntpacker.exe";
    wchar_t invalid[] = {(wchar_t)0xD800, L'\0'};
    wchar_t *wide[] = {executable, invalid};
    nt_utf8_argv args = {0};
    char error[128] = {0};
    TEST_ASSERT_FALSE(
        nt_utf8_argv_convert(2, wide, &args, error, sizeof error));
    TEST_ASSERT_NULL(args.argv);
    TEST_ASSERT_TRUE(strstr(error, "invalid UTF-16") != NULL);
}

void test_windows_path_sources_are_strict_utf8(void) {
    char value[512] = {0};
    char error[160] = {0};
    bool found = false;
    TEST_ASSERT_TRUE_MESSAGE(
        nt_win_utf16_to_utf8(L"C:\\тест\\素材", value, sizeof value,
                             error, sizeof error),
        error);
    TEST_ASSERT_EQUAL_STRING("C:\\тест\\素材", value);

    TEST_ASSERT_TRUE(SetEnvironmentVariableW(
        L"NTPACKER_UTF8_PATH_TEST", L"C:\\путь\\素材"));
    TEST_ASSERT_TRUE_MESSAGE(
        nt_win_environment_utf8(L"NTPACKER_UTF8_PATH_TEST", value,
                                sizeof value, &found, error, sizeof error),
        error);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("C:\\путь\\素材", value);

    TEST_ASSERT_TRUE(
        SetEnvironmentVariableW(L"NTPACKER_UTF8_PATH_TEST", L""));
    found = false;
    TEST_ASSERT_TRUE_MESSAGE(
        nt_win_environment_utf8(L"NTPACKER_UTF8_PATH_TEST", value,
                                sizeof value, &found, error, sizeof error),
        error);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("", value);
    TEST_ASSERT_TRUE(SetEnvironmentVariableW(L"NTPACKER_UTF8_PATH_TEST", NULL));

    TEST_ASSERT_TRUE_MESSAGE(nt_win_current_directory_utf8(
                                 value, sizeof value, error, sizeof error),
                             error);
    TEST_ASSERT_NOT_EQUAL(0, value[0]);
    TEST_ASSERT_TRUE_MESSAGE(
        nt_win_temp_path_utf8(value, sizeof value, error, sizeof error), error);
    TEST_ASSERT_NOT_EQUAL(0, value[0]);
    TEST_ASSERT_TRUE_MESSAGE(
        nt_win_module_path_utf8(value, sizeof value, error, sizeof error),
        error);
    TEST_ASSERT_NOT_EQUAL(0, value[0]);
}

void test_utf8_filesystem_boundary_is_strict_and_unicode_safe(void) {
    static const char source[] =
        "ntpacker-\xD1\x82\xD0\xB5\xD1\x81\xD1\x82-\xE7\xB4\xA0\xE6\x9D\x90.tmp";
    static const char destination[] =
        "ntpacker-\xD0\xBF\xD1\x83\xD1\x82\xD1\x8C-\xE7\xB4\xA0\xE6\x9D\x90.tmp";
    wchar_t wide[256] = {0};

    (void)nt_utf8_remove(source);
    (void)nt_utf8_remove(destination);
    TEST_ASSERT_TRUE(nt_utf8_path_to_utf16(source, wide,
                                           sizeof wide / sizeof wide[0]));
    TEST_ASSERT_EQUAL_INT(
        0, wcscmp(L"ntpacker-\x0442\x0435\x0441\x0442-\x7D20\x6750.tmp",
                  wide));

    FILE *file = nt_utf8_fopen(source, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(4U, fwrite("data", 1U, 4U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(0, nt_utf8_rename(source, destination));

    file = nt_utf8_fopen(destination, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char bytes[5] = {0};
    TEST_ASSERT_EQUAL_size_t(4U, fread(bytes, 1U, 4U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_STRING("data", bytes);
    TEST_ASSERT_EQUAL_INT(0, nt_utf8_remove(destination));

    const char invalid[] = {(char)0xC3, (char)0x28, '\0'};
    errno = 0;
    TEST_ASSERT_FALSE(nt_utf8_path_to_utf16(
        invalid, wide, sizeof wide / sizeof wide[0]));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
}

void test_utf8_filesystem_boundary_controls_windows_namespaces_and_long_paths(void) {
    wchar_t wide[1024] = {0};
    errno = 0;
    TEST_ASSERT_FALSE(nt_utf8_path_to_utf16("\\\\?\\C:\\blocked", wide,
                                             sizeof wide / sizeof wide[0]));
    TEST_ASSERT_EQUAL_INT(EINVAL, errno);

    wchar_t too_small[2] = {0};
    errno = 0;
    TEST_ASSERT_FALSE(nt_utf8_path_to_utf16("three", too_small,
                                             sizeof too_small /
                                                 sizeof too_small[0]));
    TEST_ASSERT_EQUAL_INT(ERANGE, errno);

    char long_relative[300];
    memset(long_relative, 'a', sizeof long_relative - 1U);
    long_relative[sizeof long_relative - 1U] = '\0';
    TEST_ASSERT_TRUE(nt_utf8_path_to_utf16(
        long_relative, wide, sizeof wide / sizeof wide[0]));
    TEST_ASSERT_EQUAL_INT(0, wcsncmp(L"\\\\?\\", wide, 4U));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wide_paths_become_exact_utf8);
    RUN_TEST(test_unpaired_surrogate_fails_closed);
    RUN_TEST(test_windows_path_sources_are_strict_utf8);
    RUN_TEST(test_utf8_filesystem_boundary_is_strict_and_unicode_safe);
    RUN_TEST(
        test_utf8_filesystem_boundary_controls_windows_namespaces_and_long_paths);
    return UNITY_END();
}

#else

int main(void) { return 0; }

#endif
