/* No-follow discovery faults, unavailable rows, and fail-closed limits. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "tp_core/tp_format.h"
#include "tp_core/tp_scan.h"
#include "tp_fs_internal.h"
#include "unity.h"

static const char *g_scratch;

void setUp(void) {}
void tearDown(void) {}

static bool join_path(char *out, size_t out_cap, const char *parent,
                      const char *child) {
    const int written = snprintf(out, out_cap, "%s/%s", parent, child);
    return written >= 0 && (size_t)written < out_cap;
}

static void reset_fixture_directory(char *out, size_t out_cap,
                                    const char *name) {
    TEST_ASSERT_TRUE(join_path(out, out_cap, g_scratch, name));
    tp_fs_remove_tree(out);
    (void)tp_fs_remove_file(out);
    tp_mkdirs(out);
    TEST_ASSERT_TRUE(tp_fs_is_dir(out));
}

static void make_package_directory(char *out, size_t out_cap,
                                   const char *root, const char *name) {
    TEST_ASSERT_TRUE(join_path(out, out_cap, root, name));
    tp_mkdirs(out);
    TEST_ASSERT_TRUE(tp_fs_is_dir(out));
}

static void write_child(const char *parent, const char *name,
                        const void *bytes, size_t byte_count) {
    char path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    TEST_ASSERT_TRUE(join_path(path, sizeof path, parent, name));
    TEST_ASSERT_TRUE(tp_fs_write_file(path, bytes, byte_count));
}

static bool report_has_code(const tp_format_diagnostic_report *report,
                            tp_format_diagnostic_code code) {
    const size_t count = tp_format_diagnostic_report_count(report);
    for (size_t i = 0U; i < count; ++i) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(report, i);
        if (diagnostic && diagnostic->code == code) {
            return true;
        }
    }
    return false;
}

static bool catalog_row_has_code(const tp_format_catalog *catalog,
                                 const char *key,
                                 tp_format_diagnostic_code code) {
    const size_t count = tp_format_catalog_row_count(catalog);
    for (size_t i = 0U; i < count; ++i) {
        tp_format_catalog_row row;
        if (!tp_format_catalog_row_at(catalog, i, &row) || !row.key ||
            strcmp(row.key, key) != 0) {
            continue;
        }
        return row.implementation == TP_FORMAT_IMPLEMENTATION_LUA &&
               !row.available && report_has_code(row.diagnostics, code);
    }
    return false;
}

static const tp_format_diagnostic *catalog_row_diagnostic(
    const tp_format_catalog *catalog, const char *key,
    tp_format_diagnostic_code code) {
    const size_t count = tp_format_catalog_row_count(catalog);
    for (size_t i = 0U; i < count; ++i) {
        tp_format_catalog_row row;
        if (!tp_format_catalog_row_at(catalog, i, &row) || !row.key ||
            strcmp(row.key, key) != 0) {
            continue;
        }
        const size_t diagnostic_count =
            tp_format_diagnostic_report_count(row.diagnostics);
        for (size_t j = 0U; j < diagnostic_count; ++j) {
            const tp_format_diagnostic *diagnostic =
                tp_format_diagnostic_report_at(row.diagnostics, j);
            if (diagnostic && diagnostic->code == code) {
                return diagnostic;
            }
        }
    }
    return NULL;
}

static void assert_row_diagnostic_path(
    const tp_format_catalog *catalog, const char *key,
    tp_format_diagnostic_code code, const char *expected_path) {
    const tp_format_diagnostic *diagnostic =
        catalog_row_diagnostic(catalog, key, code);
    TEST_ASSERT_NOT_NULL(diagnostic);
    TEST_ASSERT_EQUAL_STRING(expected_path, diagnostic->package_path);
}

static tp_format_catalog *scan_broken_only_root(const char *root) {
    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_catalog_scan_root(root, &scan, &failure, &error));
    TEST_ASSERT_NOT_NULL(scan);
    TEST_ASSERT_NULL(failure);
    TEST_ASSERT_EQUAL_size_t(0U, tp_format_catalog_scan_compile_count(scan));

    tp_format_catalog *catalog = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_catalog_scan_finish_without_compile(&scan, &catalog,
                                                      &error));
    TEST_ASSERT_NULL(scan);
    TEST_ASSERT_NOT_NULL(catalog);
    return catalog;
}

static void test_regular_file_root_is_rejected_with_root_diagnostic(void) {
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    TEST_ASSERT_TRUE(join_path(root, sizeof root, g_scratch,
                               "format-root-regular-file"));
    tp_fs_remove_tree(root);
    (void)tp_fs_remove_file(root);
    TEST_ASSERT_TRUE(tp_fs_write_file(root, "not a directory", 15U));

    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure = NULL;
    tp_error error = {{0}};
    const tp_status status =
        tp_format_catalog_scan_root(root, &scan, &failure, &error);
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, status);
    TEST_ASSERT_NULL(scan);
    TEST_ASSERT_NOT_NULL(failure);
    TEST_ASSERT_TRUE(report_has_code(
        failure, TP_FORMAT_DIAGNOSTIC_ROOT_NOT_DIRECTORY));
    const tp_format_diagnostic *diagnostic =
        tp_format_diagnostic_report_at(failure, 0U);
    TEST_ASSERT_NOT_NULL(diagnostic);
    TEST_ASSERT_EQUAL_STRING("formats", diagnostic->package_path);
    TEST_ASSERT_NOT_EQUAL('\0', error.msg[0]);

    tp_format_diagnostic_report_destroy(failure);
    TEST_ASSERT_TRUE(tp_fs_remove_file(root));
}

static void test_broken_package_shapes_become_unavailable_rows(void) {
    static const char descriptor[] = "{}";
    static const char source[] = "return {}\n";
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root, "format-broken-packages");

    make_package_directory(package, sizeof package, root,
                           "missing-descriptor");
    write_child(package, "export.lua", source, sizeof source - 1U);

    make_package_directory(package, sizeof package, root, "missing-source");
    write_child(package, "format.json", descriptor,
                sizeof descriptor - 1U);

    make_package_directory(package, sizeof package, root, "extra-entry");
    write_child(package, "unexpected.txt", "x", 1U);

    make_package_directory(package, sizeof package, root,
                           "descriptor-directory");
    TEST_ASSERT_TRUE(join_path(path, sizeof path, package, "format.json"));
    tp_mkdirs(path);
    TEST_ASSERT_TRUE(tp_fs_is_dir(path));
    write_child(package, "export.lua", source, sizeof source - 1U);

    tp_format_catalog *catalog = scan_broken_only_root(root);
    TEST_ASSERT_EQUAL_size_t(
        tp_format_catalog_row_count(tp_format_catalog_native()) + 4U,
        tp_format_catalog_row_count(catalog));
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "missing-descriptor",
        TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING));
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "missing-source", TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING));
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "extra-entry", TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY));
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "descriptor-directory",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE));
    assert_row_diagnostic_path(
        catalog, "missing-descriptor",
        TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING,
        "formats/missing-descriptor/format.json");
    assert_row_diagnostic_path(
        catalog, "missing-source", TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING,
        "formats/missing-source/export.lua");
    assert_row_diagnostic_path(
        catalog, "extra-entry", TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY,
        "formats/extra-entry");
    assert_row_diagnostic_path(
        catalog, "descriptor-directory",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE,
        "formats/descriptor-directory/format.json");
    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
}

static void test_oversized_descriptor_becomes_unavailable(void) {
    static const char source[] = "return {}\n";
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root, "format-oversized-descriptor");
    make_package_directory(package, sizeof package, root, "oversized");

    const size_t oversized_count = TP_FORMAT_DESCRIPTOR_MAX_BYTES + 1U;
    unsigned char *oversized = (unsigned char *)malloc(oversized_count);
    TEST_ASSERT_NOT_NULL(oversized);
    memset(oversized, 'x', oversized_count);
    write_child(package, "format.json", oversized, oversized_count);
    free(oversized);
    write_child(package, "export.lua", source, sizeof source - 1U);

    tp_format_catalog *catalog = scan_broken_only_root(root);
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "oversized",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TOO_LARGE));
    assert_row_diagnostic_path(
        catalog, "oversized",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TOO_LARGE,
        "formats/oversized/format.json");
    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
}

static void test_package_limit_fails_closed_to_native_only(void) {
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char name[32];
    reset_fixture_directory(root, sizeof root, "format-package-limit");

    for (size_t i = 0U; i <= TP_FORMAT_PACKAGE_MAX; ++i) {
        const int written = snprintf(name, sizeof name, "pkg%03zu", i);
        TEST_ASSERT_GREATER_THAN_INT(0, written);
        TEST_ASSERT_LESS_THAN_size_t(sizeof name, (size_t)written);
        make_package_directory(package, sizeof package, root, name);
    }

    tp_format_catalog *catalog = scan_broken_only_root(root);
    TEST_ASSERT_FALSE(tp_format_catalog_root_missing(catalog));
    TEST_ASSERT_TRUE(tp_format_catalog_limit_fail_closed(catalog));
    TEST_ASSERT_EQUAL_size_t(
        tp_format_catalog_row_count(tp_format_catalog_native()),
        tp_format_catalog_row_count(catalog));
    const tp_format_diagnostic_report *root_diagnostics =
        tp_format_catalog_root_diagnostics(catalog);
    TEST_ASSERT_NOT_NULL(root_diagnostics);
    TEST_ASSERT_TRUE(report_has_code(
        root_diagnostics, TP_FORMAT_DIAGNOSTIC_CATALOG_LIMIT));
    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
}

static bool create_link(const char *link_path, const char *target_path,
                        bool directory) {
#ifdef _WIN32
    wchar_t *link_wide = tp_fs_win32_path_alloc(link_path);
    wchar_t *target_wide = tp_fs_win32_path_alloc(target_path);
    if (!link_wide || !target_wide) {
        free(link_wide);
        free(target_wide);
        return false;
    }
    DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0U;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
    const bool created =
        CreateSymbolicLinkW(link_wide, target_wide, flags) != 0;
    free(link_wide);
    free(target_wide);
    return created;
#else
    (void)directory;
    return symlink(target_path, link_path) == 0;
#endif
}

static void remove_link(const char *link_path, bool directory) {
#ifdef _WIN32
    wchar_t *link_wide = tp_fs_win32_path_alloc(link_path);
    if (link_wide) {
        if (directory) {
            (void)RemoveDirectoryW(link_wide);
        } else {
            (void)DeleteFileW(link_wide);
        }
        free(link_wide);
    }
#else
    (void)directory;
    (void)unlink(link_path);
#endif
}

static bool create_hard_link(const char *link_path,
                             const char *target_path) {
#ifdef _WIN32
    wchar_t *link_wide = tp_fs_win32_path_alloc(link_path);
    wchar_t *target_wide = tp_fs_win32_path_alloc(target_path);
    if (!link_wide || !target_wide) {
        free(link_wide);
        free(target_wide);
        return false;
    }
    const bool created = CreateHardLinkW(link_wide, target_wide, NULL) != 0;
    free(link_wide);
    free(target_wide);
    return created;
#else
    return link(target_path, link_path) == 0;
#endif
}

static void test_root_link_is_rejected_when_host_allows_fixture(void) {
    char target[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char root_link[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(target, sizeof target, "format-root-link-target");
    TEST_ASSERT_TRUE(join_path(root_link, sizeof root_link, g_scratch,
                               "format-root-link"));
    remove_link(root_link, true);
    tp_fs_remove_tree(root_link);
    if (!create_link(root_link, target, true)) {
        tp_fs_remove_tree(target);
        TEST_IGNORE_MESSAGE(
            "host policy does not allow creating a directory symlink fixture");
    }

    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure = NULL;
    tp_error error = {{0}};
    const tp_status status =
        tp_format_catalog_scan_root(root_link, &scan, &failure, &error);
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, status);
    TEST_ASSERT_NULL(scan);
    TEST_ASSERT_NOT_NULL(failure);
    TEST_ASSERT_TRUE(
        report_has_code(failure, TP_FORMAT_DIAGNOSTIC_ROOT_REPARSE));

    tp_format_diagnostic_report_destroy(failure);
    remove_link(root_link, true);
    tp_fs_remove_tree(target);
}

static void test_package_and_file_links_become_unavailable_rows(void) {
    static const char source[] = "return {}\n";
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char target_package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package_link[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char file_package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char descriptor_target[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char descriptor_link[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root, "format-package-links");
    reset_fixture_directory(target_package, sizeof target_package,
                            "format-package-link-target");
    TEST_ASSERT_TRUE(join_path(package_link, sizeof package_link, root,
                               "linked-package"));
    TEST_ASSERT_TRUE(join_path(descriptor_target, sizeof descriptor_target,
                               g_scratch, "linked-format.json"));
    (void)tp_fs_remove_file(descriptor_target);
    TEST_ASSERT_TRUE(tp_fs_write_file(descriptor_target, "{}", 2U));

    make_package_directory(file_package, sizeof file_package, root,
                           "linked-file");
    TEST_ASSERT_TRUE(join_path(descriptor_link, sizeof descriptor_link,
                               file_package, "format.json"));
    write_child(file_package, "export.lua", source, sizeof source - 1U);

    if (!create_link(package_link, target_package, true) ||
        !create_link(descriptor_link, descriptor_target, false)) {
        remove_link(package_link, true);
        remove_link(descriptor_link, false);
        tp_fs_remove_tree(root);
        tp_fs_remove_tree(target_package);
        (void)tp_fs_remove_file(descriptor_target);
        TEST_IGNORE_MESSAGE(
            "host policy does not allow creating package-link fixtures");
    }

    tp_format_catalog *catalog = scan_broken_only_root(root);
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "linked-package", TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE));
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "linked-file",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE));
    assert_row_diagnostic_path(
        catalog, "linked-package", TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE,
        "formats/linked-package");
    assert_row_diagnostic_path(
        catalog, "linked-file",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE,
        "formats/linked-file/format.json");

    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
    tp_fs_remove_tree(target_package);
    (void)tp_fs_remove_file(descriptor_target);
}

static void test_hardlinked_fixed_file_is_rejected(void) {
    static const char source[] = "return {}\n";
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char fixed_path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char descriptor_target[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root,
                            "format-hardlinked-fixed-file");

    make_package_directory(package, sizeof package, root,
                           "hardlinked-descriptor");
    TEST_ASSERT_TRUE(join_path(descriptor_target, sizeof descriptor_target,
                               g_scratch, "hardlinked-format.json"));
    (void)tp_fs_remove_file(descriptor_target);
    TEST_ASSERT_TRUE(tp_fs_write_file(descriptor_target, "{}", 2U));
    TEST_ASSERT_TRUE(join_path(fixed_path, sizeof fixed_path, package,
                               "format.json"));
    TEST_ASSERT_TRUE(create_hard_link(fixed_path, descriptor_target));
    write_child(package, "export.lua", source, sizeof source - 1U);

    tp_format_catalog *catalog = scan_broken_only_root(root);
    assert_row_diagnostic_path(
        catalog, "hardlinked-descriptor",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE,
        "formats/hardlinked-descriptor/format.json");

    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
    (void)tp_fs_remove_file(descriptor_target);
}

#ifndef _WIN32
static void test_fifo_fixed_file_is_rejected_without_blocking(void) {
    static const char source[] = "return {}\n";
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char fixed_path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root, "format-fifo-fixed-file");

    make_package_directory(package, sizeof package, root,
                           "fifo-descriptor");
    TEST_ASSERT_TRUE(join_path(fixed_path, sizeof fixed_path, package,
                               "format.json"));
    if (mkfifo(fixed_path, 0600) != 0) {
        tp_fs_remove_tree(root);
        TEST_IGNORE_MESSAGE(
            "host filesystem cannot create a FIFO fixture");
    }
    write_child(package, "export.lua", source, sizeof source - 1U);

    tp_format_catalog *catalog = scan_broken_only_root(root);
    assert_row_diagnostic_path(
        catalog, "fifo-descriptor",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE,
        "formats/fifo-descriptor/format.json");
    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
}

static void test_socket_fixed_file_is_rejected_as_nonregular(void) {
    static const char source[] = "return {}\n";
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char fixed_path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root, "format-socket-fixed-file");
    make_package_directory(package, sizeof package, root,
                           "socket-descriptor");
    TEST_ASSERT_TRUE(join_path(fixed_path, sizeof fixed_path, package,
                               "format.json"));

    const int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        tp_fs_remove_tree(root);
        TEST_IGNORE_MESSAGE("host cannot create a Unix-domain socket fixture");
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    static const char socket_name[] = "format.json";
    char previous_directory[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    if (!getcwd(previous_directory, sizeof previous_directory) ||
        chdir(package) != 0) {
        (void)close(socket_fd);
        tp_fs_remove_tree(root);
        TEST_IGNORE_MESSAGE(
            "host cannot enter the Unix-domain socket fixture directory");
    }
    memcpy(address.sun_path, socket_name, sizeof socket_name);
    const int bind_result = bind(
        socket_fd, (const struct sockaddr *)(const void *)&address,
        (socklen_t)sizeof address);
    const int restore_result = chdir(previous_directory);
    TEST_ASSERT_EQUAL_INT(0, restore_result);
    if (bind_result != 0) {
        (void)close(socket_fd);
        tp_fs_remove_tree(root);
        TEST_IGNORE_MESSAGE(
            "host filesystem cannot create a Unix-domain socket fixture");
    }
    write_child(package, "export.lua", source, sizeof source - 1U);

    tp_format_catalog *catalog = scan_broken_only_root(root);
    assert_row_diagnostic_path(
        catalog, "socket-descriptor",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE,
        "formats/socket-descriptor/format.json");
    tp_format_catalog_release(catalog);
    (void)close(socket_fd);
    tp_fs_remove_tree(root);
}
#endif

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_regular_file_root_is_rejected_with_root_diagnostic);
    RUN_TEST(test_broken_package_shapes_become_unavailable_rows);
    RUN_TEST(test_oversized_descriptor_becomes_unavailable);
    RUN_TEST(test_package_limit_fails_closed_to_native_only);
    RUN_TEST(test_root_link_is_rejected_when_host_allows_fixture);
    RUN_TEST(test_package_and_file_links_become_unavailable_rows);
    RUN_TEST(test_hardlinked_fixed_file_is_rejected);
#ifndef _WIN32
    RUN_TEST(test_fifo_fixed_file_is_rejected_without_blocking);
    RUN_TEST(test_socket_fixed_file_is_rejected_as_nonregular);
#endif
    return UNITY_END();
}
