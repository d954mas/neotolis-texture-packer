/* No-follow discovery faults, unavailable rows, and fail-closed limits. */

#define _CRT_SECURE_NO_WARNINGS

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
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

static void test_rejected_package_bytes_do_not_consume_admitted_limit(void) {
    static const char descriptor[] = "{}";
    const size_t package_count =
        TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX / TP_FORMAT_SOURCE_MAX_BYTES + 1U;
    TEST_ASSERT_LESS_OR_EQUAL_size_t(TP_FORMAT_PACKAGE_MAX, package_count);
    TEST_ASSERT_GREATER_THAN_size_t(
        TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX,
        package_count * TP_FORMAT_SOURCE_MAX_BYTES);

    unsigned char *source =
        (unsigned char *)malloc(TP_FORMAT_SOURCE_MAX_BYTES);
    TEST_ASSERT_NOT_NULL(source);
    memset(source, 'x', TP_FORMAT_SOURCE_MAX_BYTES);

    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char name[32];
    reset_fixture_directory(root, sizeof root,
                            "format-rejected-byte-budget");
    for (size_t i = 0U; i < package_count; ++i) {
        const int written = snprintf(name, sizeof name, "pkg%03zu", i);
        TEST_ASSERT_GREATER_THAN_INT(0, written);
        TEST_ASSERT_LESS_THAN_size_t(sizeof name, (size_t)written);
        make_package_directory(package, sizeof package, root, name);
        write_child(package, "format.json", descriptor,
                    sizeof descriptor - 1U);
        write_child(package, "export.lua", source,
                    TP_FORMAT_SOURCE_MAX_BYTES);
    }
    free(source);

    tp_format_catalog *catalog = scan_broken_only_root(root);
    TEST_ASSERT_FALSE(tp_format_catalog_limit_fail_closed(catalog));
    TEST_ASSERT_EQUAL_size_t(
        tp_format_catalog_row_count(tp_format_catalog_native()) +
            package_count,
        tp_format_catalog_row_count(catalog));
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "pkg000", TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA));

    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
}

#ifdef _WIN32
typedef struct tp_test_mount_point_reparse {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_name_offset;
    USHORT substitute_name_length;
    USHORT print_name_offset;
    USHORT print_name_length;
    WCHAR path_buffer[(MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 16U) /
                        sizeof(WCHAR)];
} tp_test_mount_point_reparse;

static bool test_wide_is_separator(wchar_t value) {
    return value == L'\\' || value == L'/';
}

static bool create_directory_link(const char *link_path,
                                  const char *target_path) {
    wchar_t *link_wide = tp_fs_win32_path_alloc(link_path);
    wchar_t *target_wide = tp_fs_win32_path_alloc(target_path);
    if (!link_wide || !target_wide || wcslen(target_wide) < 3U ||
        target_wide[1] != L':' || !test_wide_is_separator(target_wide[2])) {
        free(link_wide);
        free(target_wide);
        return false;
    }

    tp_mkdirs(link_path);
    if (!tp_fs_is_dir(link_path)) {
        free(link_wide);
        free(target_wide);
        return false;
    }
    HANDLE link = CreateFileW(
        link_wide, GENERIC_WRITE, 0U, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (link == INVALID_HANDLE_VALUE) {
        free(link_wide);
        free(target_wide);
        tp_fs_remove_tree(link_path);
        return false;
    }

    static const wchar_t nt_prefix[] = L"\\??\\";
    const size_t prefix_length = sizeof nt_prefix / sizeof nt_prefix[0] - 1U;
    const size_t target_length = wcslen(target_wide);
    const size_t substitute_length = prefix_length + target_length;
    const size_t path_code_units = substitute_length + 1U + target_length + 1U;
    const size_t path_offset = offsetof(tp_test_mount_point_reparse,
                                        path_buffer);
    const size_t total_bytes =
        path_offset + path_code_units * sizeof(wchar_t);
    tp_test_mount_point_reparse reparse;
    memset(&reparse, 0, sizeof reparse);
    bool created = false;
    if (total_bytes <= sizeof reparse) {
        reparse.tag = IO_REPARSE_TAG_MOUNT_POINT;
        reparse.data_length = (USHORT)(total_bytes - 8U);
        reparse.substitute_name_offset = 0U;
        reparse.substitute_name_length =
            (USHORT)(substitute_length * sizeof(wchar_t));
        reparse.print_name_offset =
            (USHORT)((substitute_length + 1U) * sizeof(wchar_t));
        reparse.print_name_length =
            (USHORT)(target_length * sizeof(wchar_t));
        memcpy(reparse.path_buffer, nt_prefix,
               prefix_length * sizeof(wchar_t));
        memcpy(reparse.path_buffer + prefix_length, target_wide,
               (target_length + 1U) * sizeof(wchar_t));
        memcpy(reparse.path_buffer + substitute_length + 1U, target_wide,
               (target_length + 1U) * sizeof(wchar_t));
        for (size_t i = 0U; i < path_code_units; ++i) {
            if (reparse.path_buffer[i] == L'/') {
                reparse.path_buffer[i] = L'\\';
            }
        }

        DWORD ignored = 0U;
        created = DeviceIoControl(
                      link, FSCTL_SET_REPARSE_POINT, &reparse,
                      (DWORD)total_bytes, NULL, 0U, &ignored, NULL) != 0;
    }
    (void)CloseHandle(link);
    free(link_wide);
    free(target_wide);
    if (!created) {
        tp_fs_remove_tree(link_path);
    }
    return created;
}

static bool create_file_link(const char *link_path,
                             const char *target_path) {
    wchar_t *link_wide = tp_fs_win32_path_alloc(link_path);
    wchar_t *target_wide = tp_fs_win32_path_alloc(target_path);
    if (!link_wide || !target_wide) {
        free(link_wide);
        free(target_wide);
        return false;
    }
    DWORD flags = 0U;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
    const bool created =
        CreateSymbolicLinkW(link_wide, target_wide, flags) != 0;
    free(link_wide);
    free(target_wide);
    return created;
}
#else
static bool create_directory_link(const char *link_path,
                                  const char *target_path) {
    return symlink(target_path, link_path) == 0;
}

static bool create_file_link(const char *link_path,
                             const char *target_path) {
    return symlink(target_path, link_path) == 0;
}
#endif

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

#ifdef _WIN32
static HANDLE open_delete_capable_directory(const char *path) {
    wchar_t *wide = tp_fs_win32_path_alloc(path);
    if (!wide) {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE handle = CreateFileW(
        wide, DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    return handle;
}

static void test_win32_root_scan_requires_stable_directory_identity(void) {
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root,
                            "format-root-delete-capable-handle");
    HANDLE held = open_delete_capable_directory(root);
    TEST_ASSERT_NOT_EQUAL(INVALID_HANDLE_VALUE, held);

    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure = NULL;
    tp_error error = {{0}};
    const tp_status status =
        tp_format_catalog_scan_root(root, &scan, &failure, &error);
    const bool rejected =
        status != TP_STATUS_OK && scan == NULL && failure != NULL &&
        report_has_code(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO);

    tp_format_catalog_scan_destroy(scan);
    tp_format_diagnostic_report_destroy(failure);
    (void)CloseHandle(held);
    tp_fs_remove_tree(root);
    TEST_ASSERT_TRUE_MESSAGE(
        rejected,
        "scanner must reject a root whose identity can be replaced");
}

static void test_win32_package_scan_requires_stable_directory_identity(void) {
    static const char descriptor[] = "{}";
    static const char source[] = "return {}\n";
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root,
                            "format-package-delete-capable-handle");
    make_package_directory(package, sizeof package, root, "replaceable");
    write_child(package, "format.json", descriptor, sizeof descriptor - 1U);
    write_child(package, "export.lua", source, sizeof source - 1U);
    HANDLE held = open_delete_capable_directory(package);
    TEST_ASSERT_NOT_EQUAL(INVALID_HANDLE_VALUE, held);

    tp_format_catalog *catalog = scan_broken_only_root(root);
    const bool rejected = catalog_row_has_code(
        catalog, "replaceable", TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED);

    (void)CloseHandle(held);
    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
    TEST_ASSERT_TRUE_MESSAGE(
        rejected,
        "scanner must reject a package whose identity can be replaced");
}
#endif

static void test_root_link_is_rejected_when_host_allows_fixture(void) {
    char target[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char root_link[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(target, sizeof target, "format-root-link-target");
    TEST_ASSERT_TRUE(join_path(root_link, sizeof root_link, g_scratch,
                               "format-root-link"));
    remove_link(root_link, true);
    tp_fs_remove_tree(root_link);
    if (!create_directory_link(root_link, target)) {
        tp_fs_remove_tree(target);
#ifdef _WIN32
        TEST_FAIL_MESSAGE(
            "unprivileged directory-junction fixture creation failed");
#else
        TEST_IGNORE_MESSAGE(
            "host policy does not allow creating a directory symlink fixture");
#endif
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

static void test_package_link_becomes_an_unavailable_row(void) {
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char target_package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char package_link[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root, "format-package-links");
    reset_fixture_directory(target_package, sizeof target_package,
                            "format-package-link-target");
    TEST_ASSERT_TRUE(join_path(package_link, sizeof package_link, root,
                               "linked-package"));

    if (!create_directory_link(package_link, target_package)) {
        tp_fs_remove_tree(root);
        tp_fs_remove_tree(target_package);
#ifdef _WIN32
        TEST_FAIL_MESSAGE(
            "unprivileged directory-junction fixture creation failed");
#else
        TEST_IGNORE_MESSAGE(
            "host policy does not allow creating a package-link fixture");
#endif
    }

    tp_format_catalog *catalog = scan_broken_only_root(root);
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "linked-package", TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE));
    assert_row_diagnostic_path(
        catalog, "linked-package", TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE,
        "formats/linked-package");

    tp_format_catalog_release(catalog);
    remove_link(package_link, true);
    tp_fs_remove_tree(root);
    tp_fs_remove_tree(target_package);
}

static void test_file_link_becomes_an_unavailable_row_when_supported(void) {
    static const char source[] = "return {}\n";
    char root[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char file_package[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char descriptor_target[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    char descriptor_link[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    reset_fixture_directory(root, sizeof root, "format-file-link");
    TEST_ASSERT_TRUE(join_path(descriptor_target, sizeof descriptor_target,
                               g_scratch, "linked-format.json"));
    (void)tp_fs_remove_file(descriptor_target);
    TEST_ASSERT_TRUE(tp_fs_write_file(descriptor_target, "{}", 2U));

    make_package_directory(file_package, sizeof file_package, root,
                           "linked-file");
    TEST_ASSERT_TRUE(join_path(descriptor_link, sizeof descriptor_link,
                               file_package, "format.json"));
    write_child(file_package, "export.lua", source, sizeof source - 1U);

    if (!create_file_link(descriptor_link, descriptor_target)) {
        remove_link(descriptor_link, false);
        tp_fs_remove_tree(root);
        (void)tp_fs_remove_file(descriptor_target);
        TEST_IGNORE_MESSAGE(
            "host policy does not allow creating a file-link fixture");
    }

    tp_format_catalog *catalog = scan_broken_only_root(root);
    TEST_ASSERT_TRUE(catalog_row_has_code(
        catalog, "linked-file",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE));
    assert_row_diagnostic_path(
        catalog, "linked-file",
        TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE,
        "formats/linked-file/format.json");

    tp_format_catalog_release(catalog);
    tp_fs_remove_tree(root);
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
    RUN_TEST(test_rejected_package_bytes_do_not_consume_admitted_limit);
#ifdef _WIN32
    RUN_TEST(test_win32_root_scan_requires_stable_directory_identity);
    RUN_TEST(test_win32_package_scan_requires_stable_directory_identity);
#endif
    RUN_TEST(test_root_link_is_rejected_when_host_allows_fixture);
    RUN_TEST(test_package_link_becomes_an_unavailable_row);
    RUN_TEST(test_file_link_becomes_an_unavailable_row_when_supported);
    RUN_TEST(test_hardlinked_fixed_file_is_rejected);
#ifndef _WIN32
    RUN_TEST(test_fifo_fixed_file_is_rejected_without_blocking);
    RUN_TEST(test_socket_fixed_file_is_rejected_as_nonregular);
#endif
    return UNITY_END();
}
