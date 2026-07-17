#include "gui_paths.h"

#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_normalized_copy_has_no_600_byte_cliff(void) {
    char input[601];
    memset(input, 'a', 599U);
    input[599] = '\0';
    char output[600];
    TEST_ASSERT_TRUE(gui_paths_copy_normalized(input, output, sizeof output));
    TEST_ASSERT_EQUAL_size_t(599U, strlen(output));

    input[599] = 'b';
    input[600] = '\0';
    TEST_ASSERT_FALSE(gui_paths_copy_normalized(input, output, sizeof output));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
}

void test_project_extension_is_all_or_nothing_at_capacity(void) {
    static const char extension[] = ".ntpacker_project";
    char input[GUI_PATHS_MAX];
    const size_t fit = GUI_PATHS_MAX - sizeof extension;
    memset(input, 'p', fit);
    input[fit] = '\0';
    char output[GUI_PATHS_MAX];
    TEST_ASSERT_TRUE(gui_paths_project_file(input, output, sizeof output));
    TEST_ASSERT_EQUAL_size_t(GUI_PATHS_MAX - 1U, strlen(output));
    TEST_ASSERT_NOT_NULL(strstr(output, extension));

    input[fit] = 'q';
    input[fit + 1U] = '\0';
    TEST_ASSERT_FALSE(gui_paths_project_file(input, output, sizeof output));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
}

void test_target_path_relativization_is_checked_and_normalized(void) {
    char output[GUI_PATHS_MAX];
    TEST_ASSERT_TRUE(gui_paths_relativize_to_project(
        "C:\\project\\out\\atlas", "C:/project/demo.ntpacker_project",
        output, sizeof output));
    TEST_ASSERT_EQUAL_STRING("out/atlas", output);

    char invalid[] = {'C', ':', '/', (char)0xc3, 'x', '\0'};
    TEST_ASSERT_FALSE(gui_paths_relativize_to_project(
        invalid, "C:/project/demo.ntpacker_project", output,
        sizeof output));
    TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_normalized_copy_has_no_600_byte_cliff);
    RUN_TEST(test_project_extension_is_all_or_nothing_at_capacity);
    RUN_TEST(test_target_path_relativization_is_checked_and_normalized);
    return UNITY_END();
}
