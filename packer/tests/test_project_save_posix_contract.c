#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"

#include "tp_core/tp_identity.h"
#include "tp_core/tp_project.h"

#ifndef TP_PROJECT_SAVE_TEST_DIR
#error "TP_PROJECT_SAVE_TEST_DIR is required"
#endif

void setUp(void) {}
void tearDown(void) {}

static void write_sentinel(const char *path) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(8U, fwrite("sentinel", 1U, 8U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

void test_save_replaces_final_symlink_without_following_target(void) {
    const char *target = TP_PROJECT_SAVE_TEST_DIR "/target.txt";
    const char *link = TP_PROJECT_SAVE_TEST_DIR "/project.ntpacker_project";
    (void)unlink(link);
    (void)unlink(target);
    write_sentinel(target);
    TEST_ASSERT_EQUAL_INT(0, symlink(target, link));

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &error));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, link, &error));

    struct stat link_info;
    TEST_ASSERT_EQUAL_INT(0, lstat(link, &link_info));
    TEST_ASSERT_TRUE(S_ISREG(link_info.st_mode));
    TEST_ASSERT_FALSE(S_ISLNK(link_info.st_mode));

    char sentinel[9] = {0};
    FILE *file = fopen(target, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(8U, fread(sentinel, 1U, 8U, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_STRING("sentinel", sentinel);

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_load(link, &loaded, &error));
    TEST_ASSERT_NOT_NULL(loaded);
    tp_project_destroy(loaded);
    tp_project_destroy(project);
    TEST_ASSERT_EQUAL_INT(0, unlink(link));
    TEST_ASSERT_EQUAL_INT(0, unlink(target));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_save_replaces_final_symlink_without_following_target);
    return UNITY_END();
}
