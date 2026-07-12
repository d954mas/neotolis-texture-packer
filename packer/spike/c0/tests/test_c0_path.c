/* C0-01 task 1: canonical saved-project-path policy. Host rule set is an
 * explicit parameter, so BOTH POSIX and Windows contracts run deterministically
 * on every CI OS -- no platform-specific golden output. Covers separators,
 * drive-letter case, UNC, '.'/'..'/repeated-sep normalization, trailing slash,
 * a nonexistent Save As destination, drive-relative/not-absolute/bad-UNC
 * rejects, the symlink (no-resolution) policy, and the case-equality policy. */

#include "tp_c0/tp_c0_path.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void expect_ok(const char *in, tp_c0_host host, const char *expected) {
    char out[TP_C0_PATH_MAX];
    tp_error err;
    tp_c0_detail d = tp_c0_project_path_canonical(in, host, out, sizeof out, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_C0_OK, d, in);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, out, in);
}

static void expect_err(const char *in, tp_c0_host host, tp_c0_detail want) {
    char out[TP_C0_PATH_MAX];
    tp_c0_detail d = tp_c0_project_path_canonical(in, host, out, sizeof out, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, d, in);
}

void test_posix_canonical(void) {
    expect_ok("/a/b/c.ntpacker_project", TP_C0_HOST_POSIX, "/a/b/c.ntpacker_project");
    expect_ok("/a//b/./c", TP_C0_HOST_POSIX, "/a/b/c");
    expect_ok("/a/b/../c", TP_C0_HOST_POSIX, "/a/c");
    expect_ok("/../a", TP_C0_HOST_POSIX, "/a"); /* '..' clamps at root */
    expect_ok("/..", TP_C0_HOST_POSIX, "/");
    expect_ok("/a/b/", TP_C0_HOST_POSIX, "/a/b"); /* trailing slash stripped */
    expect_ok("/", TP_C0_HOST_POSIX, "/");
    expect_ok("/a/b\\c", TP_C0_HOST_POSIX, "/a/b\\c"); /* '\\' is a literal byte on POSIX */
}

void test_posix_nonexistent_save_as_destination(void) {
    /* Pure/lexical: the final component need not exist -- Save As works. */
    expect_ok("/home/u/new/does-not-exist-yet.ntpacker_project", TP_C0_HOST_POSIX,
              "/home/u/new/does-not-exist-yet.ntpacker_project");
}

void test_posix_symlink_is_not_resolved(void) {
    /* Lexical only: '..' is resolved textually, a symlink-looking component is
     * preserved (no realpath). Symlink equivalence is deferred to the FS step. */
    expect_ok("/real/../linkdir/p.ntpacker_project", TP_C0_HOST_POSIX, "/linkdir/p.ntpacker_project");
}

void test_posix_rejects(void) {
    expect_err("a/b", TP_C0_HOST_POSIX, TP_C0_ERR_PATH_NOT_ABSOLUTE);
    expect_err("", TP_C0_HOST_POSIX, TP_C0_ERR_EMPTY);
}

void test_windows_canonical(void) {
    expect_ok("C:\\Users\\me\\proj.ntpacker_project", TP_C0_HOST_WINDOWS, "C:/Users/me/proj.ntpacker_project");
    expect_ok("c:/x", TP_C0_HOST_WINDOWS, "C:/x");    /* drive letter upper-cased */
    expect_ok("C:\\a\\..\\b", TP_C0_HOST_WINDOWS, "C:/b");
    expect_ok("C:/a//b/./c", TP_C0_HOST_WINDOWS, "C:/a/b/c");
    expect_ok("C:", TP_C0_HOST_WINDOWS, "C:/");        /* bare drive -> drive root */
    expect_ok("C:\\", TP_C0_HOST_WINDOWS, "C:/");
}

void test_windows_unc(void) {
    expect_ok("\\\\server\\share\\a\\b", TP_C0_HOST_WINDOWS, "//server/share/a/b");
    expect_ok("//server/share", TP_C0_HOST_WINDOWS, "//server/share");
    expect_ok("//server/share/a/../b", TP_C0_HOST_WINDOWS, "//server/share/b");
}

void test_windows_rejects(void) {
    expect_err("C:foo", TP_C0_HOST_WINDOWS, TP_C0_ERR_PATH_DRIVE_REL);
    expect_err("/foo", TP_C0_HOST_WINDOWS, TP_C0_ERR_PATH_NOT_ABSOLUTE); /* drive missing */
    expect_err("foo", TP_C0_HOST_WINDOWS, TP_C0_ERR_PATH_NOT_ABSOLUTE);
    expect_err("//server", TP_C0_HOST_WINDOWS, TP_C0_ERR_PATH_BAD_UNC);
    expect_err("//server/", TP_C0_HOST_WINDOWS, TP_C0_ERR_PATH_BAD_UNC);
}

void test_case_equality_policy(void) {
    /* POSIX identity is byte-exact; Windows folds ASCII case. */
    TEST_ASSERT_FALSE(tp_c0_project_path_equal("/A/b", "/a/b", TP_C0_HOST_POSIX));
    TEST_ASSERT_TRUE(tp_c0_project_path_equal("/a/b", "/a/b", TP_C0_HOST_POSIX));
    TEST_ASSERT_TRUE(tp_c0_project_path_equal("C:/Foo/Bar", "c:/foo/bar", TP_C0_HOST_WINDOWS));
    TEST_ASSERT_FALSE(tp_c0_project_path_equal("C:/Foo/Bar", "c:/foo/bar", TP_C0_HOST_POSIX));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_posix_canonical);
    RUN_TEST(test_posix_nonexistent_save_as_destination);
    RUN_TEST(test_posix_symlink_is_not_resolved);
    RUN_TEST(test_posix_rejects);
    RUN_TEST(test_windows_canonical);
    RUN_TEST(test_windows_unc);
    RUN_TEST(test_windows_rejects);
    RUN_TEST(test_case_equality_policy);
    return UNITY_END();
}
