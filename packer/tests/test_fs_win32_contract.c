#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "unity.h"

#include "tp_fs_internal.h"

enum { WIN32_MAX_EXTENDED_PATH = 32767 };

void setUp(void) {}
void tearDown(void) {}

static char *path_with_fill(const char *prefix, size_t fill) {
    const size_t prefix_length = strlen(prefix);
    char *path = (char *)malloc(prefix_length + fill + 1U);
    TEST_ASSERT_NOT_NULL(path);
    memcpy(path, prefix, prefix_length);
    size_t component_length = 0U;
    for (size_t i = 0U; i < fill; ++i) {
        const size_t remaining = fill - i;
        if (component_length == 32U && remaining > 1U) {
            path[prefix_length + i] = '\\';
            component_length = 0U;
        } else {
            path[prefix_length + i] = 'a';
            component_length++;
        }
    }
    path[prefix_length + fill] = '\0';
    return path;
}

static void assert_last_valid_and_first_invalid(const char *prefix,
                                                size_t added_prefix_length,
                                                size_t skipped_input_length,
                                                const wchar_t *wide_prefix) {
    const size_t input_prefix_length = strlen(prefix);
    const size_t last_input_length =
        (size_t)(WIN32_MAX_EXTENDED_PATH - 1) - added_prefix_length +
        skipped_input_length;
    TEST_ASSERT_TRUE(last_input_length >= input_prefix_length);
    const size_t last_fill = last_input_length - input_prefix_length;

    char *last = path_with_fill(prefix, last_fill);
    errno = 0;
    wchar_t *wide = tp_fs_win32_path_alloc(last);
    TEST_ASSERT_NOT_NULL(wide);
    TEST_ASSERT_EQUAL_size_t((size_t)(WIN32_MAX_EXTENDED_PATH - 1),
                             wcslen(wide));
    TEST_ASSERT_EQUAL_INT(0, wcsncmp(wide_prefix, wide, wcslen(wide_prefix)));
    TEST_ASSERT_EQUAL_INT(L'a', wide[wcslen(wide) - 1U]);

    wchar_t *copy = (wchar_t *)malloc(
        (size_t)WIN32_MAX_EXTENDED_PATH * sizeof *copy);
    TEST_ASSERT_NOT_NULL(copy);
    errno = 0;
    TEST_ASSERT_FALSE(tp_fs_win32_path_copy(
        last, copy, (size_t)(WIN32_MAX_EXTENDED_PATH - 1)));
    TEST_ASSERT_EQUAL_INT(ERANGE, errno);
    errno = 0;
    TEST_ASSERT_TRUE(tp_fs_win32_path_copy(
        last, copy, (size_t)WIN32_MAX_EXTENDED_PATH));
    TEST_ASSERT_EQUAL_size_t(wcslen(wide), wcslen(copy));
    TEST_ASSERT_EQUAL_INT(0, wcscmp(wide, copy));
    free(copy);
    free(wide);
    free(last);

    char *first_invalid = path_with_fill(prefix, last_fill + 1U);
    errno = 0;
    TEST_ASSERT_NULL(tp_fs_win32_path_alloc(first_invalid));
    TEST_ASSERT_EQUAL_INT(ENAMETOOLONG, errno);
    free(first_invalid);
}

void test_drive_extended_output_has_exact_length_boundary(void) {
    assert_last_valid_and_first_invalid("C:\\", 4U, 0U, L"\\\\?\\C:\\");
}

void test_unc_extended_output_has_exact_length_boundary(void) {
    assert_last_valid_and_first_invalid("\\\\server\\share\\", 8U, 2U,
                                        L"\\\\?\\UNC\\server\\share\\");
}

void test_raw_namespaces_and_invalid_utf8_fail_closed(void) {
    static const char *const raw_paths[] = {
        "\\\\.\\PhysicalDrive0",
        "\\\\.\\C:\\x",
        "\\\\?\\C:\\x",
        "\\\\?\\UNC\\server\\share\\x",
        "//./PhysicalDrive0",
        "//?/C:/x",
    };
    for (size_t i = 0U; i < sizeof raw_paths / sizeof raw_paths[0]; ++i) {
        wchar_t output[64] = {L'X', L'\0'};
        errno = 0;
        TEST_ASSERT_FALSE_MESSAGE(
            tp_fs_win32_path_copy(raw_paths[i], output,
                                  sizeof output / sizeof output[0]),
            raw_paths[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(EINVAL, errno, raw_paths[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(L'\0', output[0], raw_paths[i]);
    }

    const char invalid[] = {'C', ':', '\\', (char)0xC3, '(', '\0'};
    errno = 0;
    TEST_ASSERT_NULL(tp_fs_win32_path_alloc(invalid));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
}

void test_unicode_drive_path_converts_without_policy_loss(void) {
    static const char utf8[] =
        "C:\\projects\\\xD0\xBD\xD1\x82\xD0\xBF\xD0\xB0\xD0\xBA\xD0\xB5\xD1\x80\\"
        "\xE7\xB4\xA0\xE6\x9D\x90.ntpacker_project";
    static const wchar_t expected[] =
        L"C:\\projects\\\x043D\x0442\x043F\x0430\x043A\x0435\x0440\\"
        L"\x7D20\x6750.ntpacker_project";
    errno = 0;
    wchar_t *actual = tp_fs_win32_path_alloc(utf8);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_INT(0, wcscmp(expected, actual));
    free(actual);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_drive_extended_output_has_exact_length_boundary);
    RUN_TEST(test_unc_extended_output_has_exact_length_boundary);
    RUN_TEST(test_raw_namespaces_and_invalid_utf8_fail_closed);
    RUN_TEST(test_unicode_drive_path_converts_without_policy_loss);
    return UNITY_END();
}
