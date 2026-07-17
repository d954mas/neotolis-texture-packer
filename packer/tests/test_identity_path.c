/* F1-00 task 5: canonical-path fixtures for production tp_core.
 * These are LEXICAL-DETERMINISTIC vectors: the host rule set is a parameter on
 * the internal core, so BOTH the POSIX and Windows contracts run on every CI OS
 * -- no platform-specific golden output. The
 * public native-host wrappers are covered too. The realpath (filesystem) half is
 * exercised separately in test_identity_session.c. */

#include "tp_core/tp_identity.h"
#include "tp_identity_internal.h" /* host-parameterized lexical core (src-only) */
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void expect_ok(const char *in, tp_host host, const char *expected) {
    char out[TP_IDENTITY_PATH_MAX];
    tp_error err;
    tp_status st = tp_path_canonical_lexical(in, host, out, sizeof out, &err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, in);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, out, in);
}

static void expect_err(const char *in, tp_host host, tp_status want) {
    char out[TP_IDENTITY_PATH_MAX];
    tp_status st = tp_path_canonical_lexical(in, host, out, sizeof out, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, st, in);
}

void test_posix_canonical(void) {
    expect_ok("/a/b/c.ntpacker_project", TP_HOST_POSIX, "/a/b/c.ntpacker_project");
    expect_ok("/a//b/./c", TP_HOST_POSIX, "/a/b/c");
    expect_ok("/a/b/../c", TP_HOST_POSIX, "/a/c");
    expect_ok("/../a", TP_HOST_POSIX, "/a"); /* '..' clamps at root */
    expect_ok("/..", TP_HOST_POSIX, "/");
    expect_ok("/a/b/", TP_HOST_POSIX, "/a/b"); /* trailing slash stripped */
    expect_ok("/", TP_HOST_POSIX, "/");
    expect_ok("/a/b\\c", TP_HOST_POSIX, "/a/b\\c"); /* '\\' is a literal byte on POSIX */
}

void test_posix_nonexistent_save_as_destination(void) {
    /* Pure/lexical: the final component need not exist -- Save As works. */
    expect_ok("/home/u/new/does-not-exist-yet.ntpacker_project", TP_HOST_POSIX,
              "/home/u/new/does-not-exist-yet.ntpacker_project");
}

void test_posix_symlink_is_not_resolved_lexically(void) {
    /* Lexical only: '..' resolved textually, symlink-looking component preserved. */
    expect_ok("/real/../linkdir/p.ntpacker_project", TP_HOST_POSIX, "/linkdir/p.ntpacker_project");
}

void test_posix_rejects(void) {
    expect_err("a/b", TP_HOST_POSIX, TP_STATUS_PATH_NOT_ABSOLUTE);
    expect_err("", TP_HOST_POSIX, TP_STATUS_INVALID_ARGUMENT);
}

void test_identity_paths_reject_invalid_utf8_before_path_rules(void) {
    const char posix[] = {'/', (char)0xc3, 'x', '\0'};
    const char windows[] = {'C', ':', '/', (char)0xc3, 'x', '\0'};
    expect_err(posix, TP_HOST_POSIX, TP_STATUS_INVALID_UTF8);
    expect_err(windows, TP_HOST_WINDOWS, TP_STATUS_INVALID_UTF8);

    char out[TP_IDENTITY_PATH_MAX];
#if defined(_WIN32)
    const char *native = windows;
#else
    const char *native = posix;
#endif
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_identity_path_lexical(native, out, sizeof out,
                                                   NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_identity_path_canonical(native, out, sizeof out,
                                                     NULL));
}

void test_windows_canonical(void) {
    expect_ok("C:\\Users\\me\\proj.ntpacker_project", TP_HOST_WINDOWS, "C:/Users/me/proj.ntpacker_project");
    expect_ok("c:/x", TP_HOST_WINDOWS, "C:/x"); /* drive letter upper-cased */
    expect_ok("C:\\a\\..\\b", TP_HOST_WINDOWS, "C:/b");
    expect_ok("C:/a//b/./c", TP_HOST_WINDOWS, "C:/a/b/c");
    expect_ok("C:", TP_HOST_WINDOWS, "C:/"); /* bare drive -> drive root */
    expect_ok("C:\\", TP_HOST_WINDOWS, "C:/");
}

void test_windows_unc(void) {
    expect_ok("\\\\server\\share\\a\\b", TP_HOST_WINDOWS, "//server/share/a/b");
    expect_ok("//server/share", TP_HOST_WINDOWS, "//server/share");
    expect_ok("//server/share/a/../b", TP_HOST_WINDOWS, "//server/share/b");
}

void test_windows_unc_separator_collapse(void) {
    expect_ok("//server//share/x", TP_HOST_WINDOWS, "//server/share/x");
    expect_ok("\\\\nas\\\\\\\\assets\\proj", TP_HOST_WINDOWS, "//nas/assets/proj");
    expect_ok("///server/share", TP_HOST_WINDOWS, "//server/share");
    expect_err("//server//", TP_HOST_WINDOWS, TP_STATUS_PATH_BAD_UNC);
}

void test_windows_device_paths(void) {
    /* "\\?\" is a transparent alias for the drive and UNC forms; every other
     * "\\?\..." and all "\\.\..." device paths are rejected with path_device. */
    expect_ok("\\\\?\\C:\\work\\p", TP_HOST_WINDOWS, "C:/work/p");
    expect_ok("\\\\?\\c:", TP_HOST_WINDOWS, "C:/"); /* verbatim bare drive */
    expect_ok("\\\\?\\UNC\\server\\share\\a\\b", TP_HOST_WINDOWS, "//server/share/a/b");
    expect_ok("\\\\?\\unc\\server\\share", TP_HOST_WINDOWS, "//server/share"); /* UNC case-insensitive */
    expect_err("\\\\?\\GLOBALROOT\\Device\\X", TP_HOST_WINDOWS, TP_STATUS_PATH_DEVICE);
    expect_err("\\\\?\\Volume{abc}\\x", TP_HOST_WINDOWS, TP_STATUS_PATH_DEVICE);
    expect_err("\\\\.\\PhysicalDrive0", TP_HOST_WINDOWS, TP_STATUS_PATH_DEVICE);
    expect_err("\\\\.\\C:\\x", TP_HOST_WINDOWS, TP_STATUS_PATH_DEVICE); /* device ns, not aliased */

    /* the verbatim spelling is the SAME identity as the unprefixed spelling */
    char a[TP_IDENTITY_PATH_MAX];
    char b[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_path_canonical_lexical("\\\\?\\C:\\work\\p", TP_HOST_WINDOWS, a, sizeof a, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_path_canonical_lexical("C:\\work\\p", TP_HOST_WINDOWS, b, sizeof b, NULL));
    TEST_ASSERT_TRUE(tp_path_equal_host(a, b, TP_HOST_WINDOWS));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_path_canonical_lexical("\\\\?\\UNC\\srv\\shr\\p", TP_HOST_WINDOWS, a, sizeof a, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_path_canonical_lexical("\\\\srv\\shr\\p", TP_HOST_WINDOWS, b, sizeof b, NULL));
    TEST_ASSERT_TRUE(tp_path_equal_host(a, b, TP_HOST_WINDOWS));
}

void test_windows_rejects(void) {
    expect_err("C:foo", TP_HOST_WINDOWS, TP_STATUS_PATH_DRIVE_RELATIVE);
    expect_err("/foo", TP_HOST_WINDOWS, TP_STATUS_PATH_NOT_ABSOLUTE); /* drive missing */
    expect_err("foo", TP_HOST_WINDOWS, TP_STATUS_PATH_NOT_ABSOLUTE);
    expect_err("//server", TP_HOST_WINDOWS, TP_STATUS_PATH_BAD_UNC);
    expect_err("//server/", TP_HOST_WINDOWS, TP_STATUS_PATH_BAD_UNC);
}

void test_dotdot_pop_in_place(void) {
    expect_ok("/a/b/c/../../d", TP_HOST_POSIX, "/a/d");       /* double pop */
    expect_ok("/a/../../b", TP_HOST_POSIX, "/b");             /* pop then clamp */
    expect_ok("C:/a/b/../../c", TP_HOST_WINDOWS, "C:/c");     /* pop to drive root */
    expect_ok("C:/a/../..", TP_HOST_WINDOWS, "C:/");          /* pop then clamp at drive */
    expect_ok("//s/sh/a/b/../..", TP_HOST_WINDOWS, "//s/sh"); /* pop back to UNC root */
    expect_ok("//s/sh/a/../b", TP_HOST_WINDOWS, "//s/sh/b");
}

void test_case_equality_policy(void) {
    /* POSIX identity is byte-exact; Windows folds ASCII case. */
    TEST_ASSERT_FALSE(tp_path_equal_host("/A/b", "/a/b", TP_HOST_POSIX));
    TEST_ASSERT_TRUE(tp_path_equal_host("/a/b", "/a/b", TP_HOST_POSIX));
    TEST_ASSERT_TRUE(tp_path_equal_host("C:/Foo/Bar", "c:/foo/bar", TP_HOST_WINDOWS));
    TEST_ASSERT_FALSE(tp_path_equal_host("C:/Foo/Bar", "c:/foo/bar", TP_HOST_POSIX));
}

/* --- move/copy fixture: a copy at another lexical path is another identity --- */
void test_move_copy_distinct_identity(void) {
    /* §5.1: "A copy at another path is another project." Two different canonical
     * paths are never equal; the same path (any spelling) is one identity. */
    TEST_ASSERT_FALSE(tp_path_equal_host("/proj/a.ntpacker_project", "/proj/b.ntpacker_project", TP_HOST_POSIX));
    char a[TP_IDENTITY_PATH_MAX];
    char b[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_path_canonical_lexical("/proj/./sub/../a.ntpacker_project", TP_HOST_POSIX, a, sizeof a, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_path_canonical_lexical("/proj/a.ntpacker_project", TP_HOST_POSIX, b, sizeof b, NULL));
    TEST_ASSERT_TRUE(tp_path_equal_host(a, b, TP_HOST_POSIX)); /* same file, two spellings */
}

/* --- the public native-host wrappers resolve to the current OS rule set --- */
void test_native_wrappers_match_native_host(void) {
    char pub[TP_IDENTITY_PATH_MAX];
    char core[TP_IDENTITY_PATH_MAX];
    const char *in =
#if defined(_WIN32)
        "C:\\a\\.\\b\\..\\c";
#else
        "/a/./b/../c";
#endif
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_identity_path_lexical(in, pub, sizeof pub, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_path_canonical_lexical(in, tp_host_native(), core, sizeof core, NULL));
    TEST_ASSERT_EQUAL_STRING(core, pub);
    TEST_ASSERT_TRUE(tp_identity_path_equal(pub, core));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_posix_canonical);
    RUN_TEST(test_posix_nonexistent_save_as_destination);
    RUN_TEST(test_posix_symlink_is_not_resolved_lexically);
    RUN_TEST(test_posix_rejects);
    RUN_TEST(test_identity_paths_reject_invalid_utf8_before_path_rules);
    RUN_TEST(test_windows_canonical);
    RUN_TEST(test_windows_unc);
    RUN_TEST(test_windows_unc_separator_collapse);
    RUN_TEST(test_windows_device_paths);
    RUN_TEST(test_windows_rejects);
    RUN_TEST(test_dotdot_pop_in_place);
    RUN_TEST(test_case_equality_policy);
    RUN_TEST(test_move_copy_distinct_identity);
    RUN_TEST(test_native_wrappers_match_native_host);
    return UNITY_END();
}
