#include <stdint.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "tp_core/tp_format.h"
#include "tp_core/tp_id.h"
#include "tp_format_descriptor_internal.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_fs_internal.h"

#ifndef TP_FORMAT_FIXTURE_ROOT
#error "TP_FORMAT_FIXTURE_ROOT is required"
#endif

#ifndef TP_FORMAT_CATALOG_TEST_DIR
#error "TP_FORMAT_CATALOG_TEST_DIR is required"
#endif

#define FORMAT_TEST_PATH_CAP (TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U)

static bool path_join(char *out, size_t capacity, const char *left,
                      const char *right) {
    const int written = snprintf(out, capacity, "%s/%s", left, right);
    return written >= 0 && (size_t)written < capacity;
}

static bool read_file_owned(const char *path, unsigned char **out,
                            size_t *out_size) {
    *out = NULL;
    *out_size = 0U;
    tp_fs_info info;
    if (!tp_fs_stat(path, &info) || info.kind != TP_FS_KIND_REGULAR ||
        info.size > (uint64_t)SIZE_MAX) {
        return false;
    }
    const size_t size = (size_t)info.size;
    unsigned char *bytes = (unsigned char *)malloc(size > 0U ? size : 1U);
    if (!bytes) {
        return false;
    }
    FILE *file = tp_fs_fopen(path, "rb");
    bool ok = false;
    if (file) {
        const bool read_ok = tp_fs_read_all(file, bytes, size);
        const bool close_ok = tp_fs_close(file);
        ok = read_ok && close_ok;
    }
    if (!ok) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *out_size = size;
    return true;
}

static bool copy_fixture_file(const char *package_name,
                              const char *file_name,
                              const char *destination_directory) {
    char package_path[FORMAT_TEST_PATH_CAP];
    char source_path[FORMAT_TEST_PATH_CAP];
    char destination_path[FORMAT_TEST_PATH_CAP];
    if (!path_join(package_path, sizeof package_path, TP_FORMAT_FIXTURE_ROOT,
                   package_name) ||
        !path_join(source_path, sizeof source_path, package_path, file_name) ||
        !path_join(destination_path, sizeof destination_path,
                   destination_directory, file_name)) {
        return false;
    }
    unsigned char *bytes = NULL;
    size_t byte_count = 0U;
    if (!read_file_owned(source_path, &bytes, &byte_count)) {
        return false;
    }
    const bool ok = tp_fs_write_file(destination_path, bytes, byte_count);
    free(bytes);
    return ok;
}

static bool copy_fixture_package(const char *root, const char *package_name,
                                 bool copy_extra_entry) {
    char destination[FORMAT_TEST_PATH_CAP];
    if (!path_join(destination, sizeof destination, root, package_name) ||
        !tp_fs_create_dir(destination) ||
        !copy_fixture_file(package_name, "format.json", destination) ||
        !copy_fixture_file(package_name, "export.lua", destination)) {
        return false;
    }
    return !copy_extra_entry ||
           copy_fixture_file(package_name, "unexpected.txt", destination);
}

static bool prepare_broken_root(char out[FORMAT_TEST_PATH_CAP]) {
    static const struct {
        const char *name;
        bool extra_entry;
    } packages[] = {
        {"duplicate-id-a", false},
        {"duplicate-id-b", false},
        {"invalid-duplicate-key", false},
        {"invalid-extra-entry", true},
        {"invalid-reserved-id", false},
        {"invalid-unknown-member", false},
    };
    if (!path_join(out, FORMAT_TEST_PATH_CAP, TP_FORMAT_CATALOG_TEST_DIR,
                   "broken-root") ||
        !tp_fs_create_dir(out)) {
        return false;
    }
    for (size_t i = 0U; i < sizeof packages / sizeof packages[0]; ++i) {
        if (!copy_fixture_package(out, packages[i].name,
                                  packages[i].extra_entry)) {
            return false;
        }
    }
    return true;
}

static bool read_fixture_descriptor(const char *package_name,
                                    unsigned char **out,
                                    size_t *out_size) {
    char package_path[FORMAT_TEST_PATH_CAP];
    char descriptor_path[FORMAT_TEST_PATH_CAP];
    return path_join(package_path, sizeof package_path, TP_FORMAT_FIXTURE_ROOT,
                     package_name) &&
           path_join(descriptor_path, sizeof descriptor_path, package_path,
                     "format.json") &&
           read_file_owned(descriptor_path, out, out_size);
}

static void write_u32le(unsigned char out[4], uint32_t value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8U);
    out[2] = (unsigned char)(value >> 16U);
    out[3] = (unsigned char)(value >> 24U);
}

static void write_u64le(unsigned char out[8], uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        out[shift / 8U] = (unsigned char)(value >> shift);
    }
}

static void fingerprint_for_bytes(
    uint32_t api_version, const unsigned char *descriptor_bytes,
    size_t descriptor_byte_count, const unsigned char *source_bytes,
    size_t source_byte_count, char out[33]) {
    static const unsigned char tag[] = "ntpacker-format-package-v1";
    static const char hex[] = "0123456789abcdef";
    unsigned char api[4];
    unsigned char descriptor_size[8];
    unsigned char source_size[8];
    write_u32le(api, api_version);
    write_u64le(descriptor_size, (uint64_t)descriptor_byte_count);
    write_u64le(source_size, (uint64_t)source_byte_count);
    tp_hasher hasher = tp_hasher_init();
    tp_hasher_update(&hasher, tag, sizeof tag);
    tp_hasher_update(&hasher, api, sizeof api);
    tp_hasher_update(&hasher, descriptor_size, sizeof descriptor_size);
    tp_hasher_update(&hasher, descriptor_bytes, descriptor_byte_count);
    tp_hasher_update(&hasher, source_size, sizeof source_size);
    tp_hasher_update(&hasher, source_bytes, source_byte_count);
    const tp_id128 fingerprint = tp_hasher_final(hasher);
    for (size_t i = 0U; i < sizeof fingerprint.bytes; ++i) {
        out[i * 2U] = hex[fingerprint.bytes[i] >> 4U];
        out[i * 2U + 1U] = hex[fingerprint.bytes[i] & 0x0fU];
    }
    out[32] = '\0';
}

static bool expected_fixture_fingerprint(const char *package_name,
                                         char out[33]) {
    char package_path[FORMAT_TEST_PATH_CAP];
    char descriptor_path[FORMAT_TEST_PATH_CAP];
    char source_path[FORMAT_TEST_PATH_CAP];
    unsigned char *descriptor = NULL;
    unsigned char *source = NULL;
    size_t descriptor_size = 0U;
    size_t source_size = 0U;
    if (!path_join(package_path, sizeof package_path, TP_FORMAT_FIXTURE_ROOT,
                   package_name) ||
        !path_join(descriptor_path, sizeof descriptor_path, package_path,
                   "format.json") ||
        !path_join(source_path, sizeof source_path, package_path,
                   "export.lua") ||
        !read_file_owned(descriptor_path, &descriptor, &descriptor_size) ||
        !read_file_owned(source_path, &source, &source_size)) {
        free(descriptor);
        free(source);
        return false;
    }
    fingerprint_for_bytes(TP_FORMAT_API_VERSION, descriptor, descriptor_size,
                          source, source_size, out);
    free(descriptor);
    free(source);
    return true;
}

static void assert_descriptor_rejection(
    const char *package_name, tp_format_diagnostic_code expected_code) {
    unsigned char *bytes = NULL;
    size_t byte_count = 0U;
    TEST_ASSERT_TRUE(read_fixture_descriptor(package_name, &bytes,
                                             &byte_count));
    tp_format_descriptor_parse_result parsed = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_descriptor_v1_parse(bytes, byte_count, &parsed, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DESCRIPTOR_REJECTED, parsed.outcome);
    TEST_ASSERT_EQUAL_INT(expected_code, parsed.rejection_code);
    TEST_ASSERT_NULL(parsed.owned_descriptor);
    free(bytes);
}

static void assert_descriptor_text_rejection(
    const char *text, tp_format_diagnostic_code expected_code) {
    tp_format_descriptor_parse_result parsed = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_descriptor_v1_parse(
            (const unsigned char *)text, strlen(text), &parsed, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DESCRIPTOR_REJECTED, parsed.outcome);
    TEST_ASSERT_EQUAL_INT(expected_code, parsed.rejection_code);
    TEST_ASSERT_NULL(parsed.owned_descriptor);
}

static const tp_format_diagnostic *diagnostic_report_find_code(
    const tp_format_diagnostic_report *report,
    tp_format_diagnostic_code code) {
    const size_t count = tp_format_diagnostic_report_count(report);
    for (size_t i = 0U; i < count; ++i) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(report, i);
        if (diagnostic && diagnostic->code == code) {
            return diagnostic;
        }
    }
    return NULL;
}

static bool diagnostic_report_has_code(
    const tp_format_diagnostic_report *report,
    tp_format_diagnostic_code code) {
    return diagnostic_report_find_code(report, code) != NULL;
}

static void assert_nullable_equal(const char *expected, const char *actual) {
    if (!expected || !actual) {
        TEST_ASSERT_TRUE(expected == actual);
        return;
    }
    TEST_ASSERT_EQUAL_STRING(expected, actual);
}

static void assert_diagnostics_equal(
    const tp_format_diagnostic_report *left,
    const tp_format_diagnostic_report *right) {
    const size_t count = tp_format_diagnostic_report_count(left);
    TEST_ASSERT_EQUAL_size_t(count,
                             tp_format_diagnostic_report_count(right));
    TEST_ASSERT_EQUAL(tp_format_diagnostic_report_truncated(left),
                      tp_format_diagnostic_report_truncated(right));
    for (size_t i = 0U; i < count; ++i) {
        const tp_format_diagnostic *a =
            tp_format_diagnostic_report_at(left, i);
        const tp_format_diagnostic *b =
            tp_format_diagnostic_report_at(right, i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_NOT_NULL(b);
        TEST_ASSERT_EQUAL_INT(a->severity, b->severity);
        TEST_ASSERT_EQUAL_INT(a->code, b->code);
        TEST_ASSERT_EQUAL_INT(a->phase, b->phase);
        assert_nullable_equal(a->format_id, b->format_id);
        assert_nullable_equal(a->package_path, b->package_path);
        TEST_ASSERT_EQUAL_UINT32(a->line, b->line);
        TEST_ASSERT_EQUAL_UINT32(a->column, b->column);
        assert_nullable_equal(a->message, b->message);
        TEST_ASSERT_EQUAL_size_t(a->frame_count, b->frame_count);
    }
}

void setUp(void) {
    tp_fs_remove_tree(TP_FORMAT_CATALOG_TEST_DIR);
    TEST_ASSERT_TRUE(tp_fs_create_dir(TP_FORMAT_CATALOG_TEST_DIR));
}

void tearDown(void) {
    tp_fs_remove_tree(TP_FORMAT_CATALOG_TEST_DIR);
}

void test_strict_descriptor_parser_accepts_full_surface_and_rejects_fixtures(
    void) {
    unsigned char *bytes = NULL;
    size_t byte_count = 0U;
    TEST_ASSERT_TRUE(read_fixture_descriptor("valid-full", &bytes,
                                             &byte_count));
    tp_format_descriptor_parse_result parsed = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_descriptor_v1_parse(bytes, byte_count, &parsed, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DESCRIPTOR_ADMITTED, parsed.outcome);
    const tp_format_descriptor *descriptor =
        tp_format_owned_descriptor_view(parsed.owned_descriptor);
    TEST_ASSERT_NOT_NULL(descriptor);
    TEST_ASSERT_EQUAL_UINT32(TP_FORMAT_API_VERSION, descriptor->api_version);
    TEST_ASSERT_EQUAL_STRING("fixture-full", descriptor->id);
    TEST_ASSERT_EQUAL_STRING("Fixture Full Surface", descriptor->display_name);
    TEST_ASSERT_EQUAL_HEX8(TP_PACK_TRANSFORMS_ALL,
                           descriptor->caps.transform_mask);
    TEST_ASSERT_TRUE(descriptor->caps.polygons);
    TEST_ASSERT_TRUE(descriptor->caps.pivot);
    TEST_ASSERT_TRUE(descriptor->caps.slice9);
    TEST_ASSERT_TRUE(descriptor->caps.multipage);
    TEST_ASSERT_TRUE(descriptor->caps.aliases);
    TEST_ASSERT_TRUE(descriptor->caps.animations);
    TEST_ASSERT_EQUAL_INT(2, descriptor->artifact_count);
    TEST_ASSERT_EQUAL_STRING("metadata", descriptor->artifacts[0].id);
    TEST_ASSERT_EQUAL_STRING(".txt", descriptor->artifacts[0].suffix);
    TEST_ASSERT_EQUAL_STRING("index", descriptor->artifacts[1].id);
    TEST_ASSERT_EQUAL_STRING(".index.json", descriptor->artifacts[1].suffix);
    TEST_ASSERT_EQUAL_INT(1, descriptor->host_fact_count);
    TEST_ASSERT_EQUAL_STRING("metadata_resource", descriptor->host_facts[0].id);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_HOST_FACT_PROJECT_RESOURCE,
                          descriptor->host_facts[0].kind);
    TEST_ASSERT_EQUAL_STRING("metadata",
                             descriptor->host_facts[0].output_id);
    TEST_ASSERT_EQUAL_STRING("game.project",
                             descriptor->host_facts[0].root_marker);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_HOST_FACT_MISSING_BASENAME_NOTICE,
                          descriptor->host_facts[0].missing);
    tp_format_owned_descriptor_destroy(parsed.owned_descriptor);
    free(bytes);

    assert_descriptor_rejection(
        "invalid-duplicate-key",
        TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON);
    assert_descriptor_rejection("invalid-unknown-member",
                                TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA);
    assert_descriptor_rejection("invalid-reserved-id",
                                TP_FORMAT_DIAGNOSTIC_FORMAT_ID_RESERVED);

    static const char leading_zero_api[] =
        "{\"api_version\":01,\"id\":\"fixture-number\","
        "\"display_name\":\"Number\",\"capabilities\":{"
        "\"transforms\":[\"identity\"],\"polygons\":false,"
        "\"pivot\":false,\"slice9\":false,\"multipage\":false,"
        "\"aliases\":false,\"animations\":false},\"outputs\":[{"
        "\"id\":\"metadata\",\"suffix\":\".txt\"}]}";
    assert_descriptor_text_rejection(
        leading_zero_api,
        TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON);

    static const char overflowing_api[] =
        "{\"api_version\":4294967296,\"id\":\"fixture-number\","
        "\"display_name\":\"Number\",\"capabilities\":{"
        "\"transforms\":[\"identity\"],\"polygons\":false,"
        "\"pivot\":false,\"slice9\":false,\"multipage\":false,"
        "\"aliases\":false,\"animations\":false},\"outputs\":[{"
        "\"id\":\"metadata\",\"suffix\":\".txt\"}]}";
    assert_descriptor_text_rejection(
        overflowing_api, TP_FORMAT_DIAGNOSTIC_API_UNSUPPORTED);

    static const char negative_api[] =
        "{\"api_version\":-2,\"id\":\"fixture-number\","
        "\"display_name\":\"Number\",\"capabilities\":{"
        "\"transforms\":[\"identity\"],\"polygons\":false,"
        "\"pivot\":false,\"slice9\":false,\"multipage\":false,"
        "\"aliases\":false,\"animations\":false},\"outputs\":[{"
        "\"id\":\"metadata\",\"suffix\":\".txt\"}]}";
    assert_descriptor_text_rejection(
        negative_api, TP_FORMAT_DIAGNOSTIC_API_UNSUPPORTED);

    static const char negative_zero_api[] =
        "{\"api_version\":-0,\"id\":\"fixture-number\","
        "\"display_name\":\"Number\",\"capabilities\":{"
        "\"transforms\":[\"identity\"],\"polygons\":false,"
        "\"pivot\":false,\"slice9\":false,\"multipage\":false,"
        "\"aliases\":false,\"animations\":false},\"outputs\":[{"
        "\"id\":\"metadata\",\"suffix\":\".txt\"}]}";
    assert_descriptor_text_rejection(
        negative_zero_api, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA);

    static const char unpaired_surrogate[] =
        "{\"api_version\":1,\"id\":\"fixture-unicode\","
        "\"display_name\":\"\\uD800\",\"capabilities\":{"
        "\"transforms\":[\"identity\"],\"polygons\":false,"
        "\"pivot\":false,\"slice9\":false,\"multipage\":false,"
        "\"aliases\":false,\"animations\":false},\"outputs\":[{"
        "\"id\":\"metadata\",\"suffix\":\".txt\"}]}";
    assert_descriptor_text_rejection(
        unpaired_surrogate,
        TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON);

    const char del_package_name[] = {'a', (char)0x7f, '\0'};
    TEST_ASSERT_FALSE(tp_format_package_name_is_portable(del_package_name));
}

void test_descriptor_rejects_escaped_nul_in_object_key(void) {
    static const char descriptor[] =
        "{\"api_version\\u0000ignored\":1,\"id\":\"fixture-nul-key\","
        "\"display_name\":\"NUL key\",\"capabilities\":{"
        "\"transforms\":[\"identity\"],\"polygons\":false,"
        "\"pivot\":false,\"slice9\":false,\"multipage\":false,"
        "\"aliases\":false,\"animations\":false},\"outputs\":[{"
        "\"id\":\"metadata\",\"suffix\":\".txt\"}]}";
    assert_descriptor_text_rejection(
        descriptor, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_UTF8);
}

void test_descriptor_rejects_escaped_nul_in_string_value(void) {
    static const char descriptor[] =
        "{\"api_version\":1,\"id\":\"fixture-nul\\u0000ignored\","
        "\"display_name\":\"NUL value\",\"capabilities\":{"
        "\"transforms\":[\"identity\"],\"polygons\":false,"
        "\"pivot\":false,\"slice9\":false,\"multipage\":false,"
        "\"aliases\":false,\"animations\":false},\"outputs\":[{"
        "\"id\":\"metadata\",\"suffix\":\".txt\"}]}";
    assert_descriptor_text_rejection(
        descriptor, TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_UTF8);
}

void test_descriptor_materialization_is_numeric_locale_independent(void) {
    const char *current = setlocale(LC_NUMERIC, NULL);
    char saved_locale[256];
    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_LESS_THAN_size_t(sizeof saved_locale, strlen(current));
    (void)snprintf(saved_locale, sizeof saved_locale, "%s", current);

    static const char *const candidates[] = {
        "", "de_DE.UTF-8", "de_DE.utf8", "de_DE",
        "fr_FR.UTF-8", "fr_FR.utf8", "fr_FR",
        "German_Germany.1252", "French_France.1252",
        "Russian_Russia.1251",
    };
    bool activated = false;
    for (size_t i = 0U; i < sizeof candidates / sizeof candidates[0]; ++i) {
        if (setlocale(LC_NUMERIC, candidates[i]) && localeconv() &&
            localeconv()->decimal_point &&
            strcmp(localeconv()->decimal_point, ".") != 0) {
            activated = true;
            break;
        }
    }
    if (!activated) {
        (void)setlocale(LC_NUMERIC, saved_locale);
        TEST_IGNORE_MESSAGE(
            "host has no installed numeric locale with a non-dot decimal separator");
    }

    static const char descriptor[] =
        "{\"api_version\":1,\"id\":\"fixture-locale\","
        "\"display_name\":\"Locale\",\"capabilities\":{"
        "\"transforms\":[\"identity\"],\"polygons\":false,"
        "\"pivot\":false,\"slice9\":false,\"multipage\":false,"
        "\"aliases\":false,\"animations\":false},\"outputs\":[{"
        "\"id\":\"metadata\",\"suffix\":\".txt\"}],"
        "\"future_number\":1.5}";
    tp_format_descriptor_parse_result parsed = {0};
    tp_error error = {{0}};
    const tp_status status = tp_format_descriptor_v1_parse(
        (const unsigned char *)descriptor, strlen(descriptor), &parsed,
        &error);
    const bool restored = setlocale(LC_NUMERIC, saved_locale) != NULL;

    TEST_ASSERT_TRUE(restored);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, status, error.msg);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DESCRIPTOR_REJECTED, parsed.outcome);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
                          parsed.rejection_code);
    TEST_ASSERT_NULL(parsed.owned_descriptor);
}

void test_diagnostic_reports_are_deep_owned_and_bounded(void) {
    char format_id[] = "fixture-full";
    char package_path[] = "formats/valid-full/format.json";
    char message[] = "descriptor detail";
    char frame_text[] = "@formats/fixture-full/export.lua:12";
    tp_format_diagnostic_frame frame = {
        .text = frame_text,
        .line = 12U,
    };
    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
        .phase = TP_FORMAT_PHASE_DESCRIPTOR,
        .format_id = format_id,
        .package_path = package_path,
        .line = 3U,
        .column = 7U,
        .message = message,
        .frames = &frame,
        .frame_count = 1U,
    };
    tp_error error = {{0}};
    tp_format_diagnostic_report *report = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_diagnostic_report_create_internal(&report, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_diagnostic_report_append_internal(report, &diagnostic,
                                                     &error));
    tp_format_diagnostic_report *clone = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_diagnostic_report_clone_internal(report, &clone, &error));
    format_id[0] = 'X';
    package_path[0] = 'X';
    message[0] = 'X';
    frame_text[0] = 'X';
    tp_format_diagnostic_report_destroy(report);

    const tp_format_diagnostic *owned =
        tp_format_diagnostic_report_at(clone, 0U);
    TEST_ASSERT_NOT_NULL(owned);
    TEST_ASSERT_EQUAL_STRING("fixture-full", owned->format_id);
    TEST_ASSERT_EQUAL_STRING("formats/valid-full/format.json",
                             owned->package_path);
    TEST_ASSERT_EQUAL_STRING("descriptor detail", owned->message);
    TEST_ASSERT_EQUAL_size_t(1U, owned->frame_count);
    TEST_ASSERT_EQUAL_STRING("@formats/fixture-full/export.lua:12",
                             owned->frames[0].text);
    TEST_ASSERT_GREATER_THAN_size_t(
        0U, tp_format_diagnostic_report_dynamic_bytes_internal(clone));
    tp_format_diagnostic_report_destroy(clone);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_diagnostic_report_create_internal(&report, &error));
    const tp_format_diagnostic repeated = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_ROOT_IO,
        .phase = TP_FORMAT_PHASE_DISCOVERY,
        .message = "bounded",
    };
    for (size_t i = 0U; i < TP_FORMAT_DIAGNOSTIC_MAX + 8U; ++i) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            tp_format_diagnostic_report_append_internal(report, &repeated,
                                                         &error));
    }
    TEST_ASSERT_TRUE(tp_format_diagnostic_report_truncated(report));
    TEST_ASSERT_EQUAL_size_t(
        TP_FORMAT_DIAGNOSTIC_MAX,
        tp_format_diagnostic_report_count(report));
    const tp_format_diagnostic *marker = tp_format_diagnostic_report_at(
        report, TP_FORMAT_DIAGNOSTIC_MAX - 1U);
    TEST_ASSERT_NOT_NULL(marker);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED,
                          marker->code);
    TEST_ASSERT_EQUAL_STRING(
        "diagnostics_truncated",
        tp_format_diagnostic_code_id(marker->code));
    TEST_ASSERT_EQUAL_STRING(
        "warning", tp_format_diagnostic_severity_id(marker->severity));
    TEST_ASSERT_EQUAL_STRING("discovery",
                             tp_format_diagnostic_phase_id(marker->phase));
    tp_format_diagnostic_report_destroy(report);
}

void test_fixture_scan_is_deterministic_and_cannot_install_before_compile(
    void) {
    tp_format_catalog_scan *first = NULL;
    tp_format_catalog_scan *second = NULL;
    tp_format_diagnostic_report *failure = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog_scan_root(TP_FORMAT_FIXTURE_ROOT, &first, &failure,
                                    &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(failure);
    TEST_ASSERT_EQUAL_size_t(2U,
                             tp_format_catalog_scan_compile_count(first));

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog_scan_root(TP_FORMAT_FIXTURE_ROOT, &second, &failure,
                                    &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_NULL(failure);
    TEST_ASSERT_EQUAL_size_t(2U,
                             tp_format_catalog_scan_compile_count(second));

    static const char *expected_ids[] = {"fixture-full", "fixture-minimal"};
    static const char *expected_paths[] = {"formats/valid-full",
                                           "formats/valid-minimal"};
    for (size_t i = 0U; i < 2U; ++i) {
        tp_format_compile_candidate a = {0};
        tp_format_compile_candidate b = {0};
        TEST_ASSERT_TRUE(tp_format_catalog_scan_compile_at(first, i, &a));
        TEST_ASSERT_TRUE(tp_format_catalog_scan_compile_at(second, i, &b));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, a.candidate_index);
        TEST_ASSERT_EQUAL_STRING(expected_ids[i], a.descriptor->id);
        TEST_ASSERT_EQUAL_STRING(expected_paths[i], a.package_path);
        TEST_ASSERT_EQUAL_STRING(a.descriptor->id, b.descriptor->id);
        TEST_ASSERT_EQUAL_STRING(a.package_path, b.package_path);
        TEST_ASSERT_EQUAL_STRING(a.fingerprint, b.fingerprint);
        TEST_ASSERT_EQUAL_size_t(a.descriptor_byte_count,
                                 b.descriptor_byte_count);
        TEST_ASSERT_EQUAL_size_t(a.source_byte_count, b.source_byte_count);
        TEST_ASSERT_EQUAL_MEMORY(a.descriptor_bytes, b.descriptor_bytes,
                                 a.descriptor_byte_count);
        TEST_ASSERT_EQUAL_MEMORY(a.source_bytes, b.source_bytes,
                                 a.source_byte_count);
        char expected_fingerprint[33];
        fingerprint_for_bytes(
            a.descriptor->api_version, a.descriptor_bytes,
            a.descriptor_byte_count, a.source_bytes, a.source_byte_count,
            expected_fingerprint);
        TEST_ASSERT_EQUAL_STRING(expected_fingerprint, a.fingerprint);
    }
    tp_format_compile_candidate out_of_range = {0};
    TEST_ASSERT_FALSE(
        tp_format_catalog_scan_compile_at(first, 2U, &out_of_range));

    tp_format_catalog *catalog = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_UNIMPLEMENTED,
        tp_format_catalog_scan_finish_without_compile(&first, &catalog,
                                                      &error));
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(catalog);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "awaiting isolated Lua compilation"));

    tp_format_catalog_scan_destroy(first);
    tp_format_catalog_scan_destroy(second);
}

void test_duplicate_id_includes_source_rejected_descriptor(void) {
    char root[FORMAT_TEST_PATH_CAP];
    char package[FORMAT_TEST_PATH_CAP];
    char source_path[FORMAT_TEST_PATH_CAP];
    TEST_ASSERT_TRUE(path_join(root, sizeof root, TP_FORMAT_CATALOG_TEST_DIR,
                               "source-rejected-duplicate-root"));
    TEST_ASSERT_TRUE(tp_fs_create_dir(root));
    TEST_ASSERT_TRUE(copy_fixture_package(root, "duplicate-id-a", false));
    TEST_ASSERT_TRUE(copy_fixture_package(root, "duplicate-id-b", false));
    TEST_ASSERT_TRUE(path_join(package, sizeof package, root,
                               "duplicate-id-b"));
    TEST_ASSERT_TRUE(path_join(source_path, sizeof source_path, package,
                               "export.lua"));
    const unsigned char binary_source[] = {0x1bU, 'L', 'u', 'a'};
    TEST_ASSERT_TRUE(tp_fs_write_file(source_path, binary_source,
                                      sizeof binary_source));

    tp_format_catalog_scan *scan = NULL;
    tp_format_diagnostic_report *failure = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog_scan_root(root, &scan, &failure, &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(scan);
    TEST_ASSERT_NULL(failure);
    TEST_ASSERT_EQUAL_size_t(0U,
                             tp_format_catalog_scan_compile_count(scan));

    tp_format_catalog *catalog = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_catalog_scan_finish_without_compile(&scan, &catalog,
                                                      &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(catalog);
    bool saw_a = false;
    bool saw_b = false;
    const size_t row_count = tp_format_catalog_row_count(catalog);
    for (size_t i = 0U; i < row_count; ++i) {
        tp_format_catalog_row row = {0};
        TEST_ASSERT_TRUE(tp_format_catalog_row_at(catalog, i, &row));
        if (!row.key || (strcmp(row.key, "duplicate-id-a") != 0 &&
                         strcmp(row.key, "duplicate-id-b") != 0)) {
            continue;
        }
        TEST_ASSERT_FALSE(row.available);
        TEST_ASSERT_NOT_NULL(row.descriptor);
        TEST_ASSERT_TRUE(diagnostic_report_has_code(
            row.diagnostics,
            TP_FORMAT_DIAGNOSTIC_DUPLICATE_FORMAT_ID));
        const tp_format_diagnostic *duplicate = diagnostic_report_find_code(
            row.diagnostics,
            TP_FORMAT_DIAGNOSTIC_DUPLICATE_FORMAT_ID);
        TEST_ASSERT_NOT_NULL(duplicate);
        char expected_descriptor_path[FORMAT_TEST_PATH_CAP];
        TEST_ASSERT_TRUE(path_join(
            expected_descriptor_path, sizeof expected_descriptor_path,
            row.package_path, "format.json"));
        TEST_ASSERT_EQUAL_STRING(expected_descriptor_path,
                                 duplicate->package_path);
        if (strcmp(row.key, "duplicate-id-a") == 0) {
            saw_a = true;
        } else {
            saw_b = true;
            TEST_ASSERT_TRUE(diagnostic_report_has_code(
                row.diagnostics, TP_FORMAT_DIAGNOSTIC_SOURCE_BINARY));
            const tp_format_diagnostic *source = diagnostic_report_find_code(
                row.diagnostics, TP_FORMAT_DIAGNOSTIC_SOURCE_BINARY);
            TEST_ASSERT_NOT_NULL(source);
            TEST_ASSERT_EQUAL_STRING(
                "formats/duplicate-id-b/export.lua", source->package_path);
        }
    }
    TEST_ASSERT_TRUE(saw_a);
    TEST_ASSERT_TRUE(saw_b);
    tp_format_catalog_release(catalog);
}

void test_broken_only_catalog_rows_and_diagnostics_are_deterministic(void) {
    char root[FORMAT_TEST_PATH_CAP];
    TEST_ASSERT_TRUE(prepare_broken_root(root));

    tp_format_catalog *catalogs[2] = {NULL, NULL};
    tp_error error = {{0}};
    for (size_t i = 0U; i < 2U; ++i) {
        tp_format_catalog_scan *scan = NULL;
        tp_format_diagnostic_report *failure = NULL;
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            TP_STATUS_OK,
            tp_format_catalog_scan_root(root, &scan, &failure, &error),
            error.msg);
        TEST_ASSERT_NOT_NULL(scan);
        TEST_ASSERT_NULL(failure);
        TEST_ASSERT_EQUAL_size_t(
            0U, tp_format_catalog_scan_compile_count(scan));
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            TP_STATUS_OK,
            tp_format_catalog_scan_finish_without_compile(
                &scan, &catalogs[i], &error),
            error.msg);
        TEST_ASSERT_NULL(scan);
        TEST_ASSERT_NOT_NULL(catalogs[i]);
    }

    static const char *expected_keys[] = {
        "duplicate-id-a",
        "duplicate-id-b",
        "invalid-duplicate-key",
        "invalid-extra-entry",
        "invalid-reserved-id",
        "invalid-unknown-member",
    };
    static const tp_format_diagnostic_code expected_codes[] = {
        TP_FORMAT_DIAGNOSTIC_DUPLICATE_FORMAT_ID,
        TP_FORMAT_DIAGNOSTIC_DUPLICATE_FORMAT_ID,
        TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON,
        TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY,
        TP_FORMAT_DIAGNOSTIC_FORMAT_ID_RESERVED,
        TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
    };
    static const char *const expected_diagnostic_paths[] = {
        "formats/duplicate-id-a/format.json",
        "formats/duplicate-id-b/format.json",
        "formats/invalid-duplicate-key/format.json",
        "formats/invalid-extra-entry",
        "formats/invalid-reserved-id/format.json",
        "formats/invalid-unknown-member/format.json",
    };
    const size_t native_count =
        tp_format_catalog_row_count(tp_format_catalog_native());
    TEST_ASSERT_EQUAL_size_t(native_count + 6U,
                             tp_format_catalog_row_count(catalogs[0]));
    TEST_ASSERT_EQUAL_size_t(tp_format_catalog_row_count(catalogs[0]),
                             tp_format_catalog_row_count(catalogs[1]));
    TEST_ASSERT_EQUAL_STRING(root, tp_format_catalog_root(catalogs[0]));
    TEST_ASSERT_FALSE(tp_format_catalog_root_missing(catalogs[0]));
    TEST_ASSERT_FALSE(tp_format_catalog_limit_fail_closed(catalogs[0]));

    for (size_t i = 0U; i < 6U; ++i) {
        tp_format_catalog_row a;
        tp_format_catalog_row b;
        TEST_ASSERT_TRUE(tp_format_catalog_row_at(
            catalogs[0], native_count + i, &a));
        TEST_ASSERT_TRUE(tp_format_catalog_row_at(
            catalogs[1], native_count + i, &b));
        TEST_ASSERT_EQUAL_INT(TP_FORMAT_IMPLEMENTATION_LUA,
                              a.implementation);
        TEST_ASSERT_FALSE(a.available);
        TEST_ASSERT_EQUAL_STRING(expected_keys[i], a.key);
        TEST_ASSERT_EQUAL_STRING(a.key, b.key);
        assert_nullable_equal(a.package_path, b.package_path);
        assert_nullable_equal(a.fingerprint, b.fingerprint);
        TEST_ASSERT_NOT_NULL(a.diagnostics);
        TEST_ASSERT_EQUAL_size_t(
            1U, tp_format_diagnostic_report_count(a.diagnostics));
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(a.diagnostics, 0U);
        TEST_ASSERT_NOT_NULL(diagnostic);
        TEST_ASSERT_EQUAL_INT(expected_codes[i], diagnostic->code);
        TEST_ASSERT_EQUAL_INT(
            expected_codes[i] == TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY
                ? TP_FORMAT_PHASE_DISCOVERY
                : TP_FORMAT_PHASE_DESCRIPTOR,
            diagnostic->phase);
        TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_ERROR,
                              diagnostic->severity);
        TEST_ASSERT_EQUAL_STRING(expected_diagnostic_paths[i],
                                 diagnostic->package_path);
        assert_diagnostics_equal(a.diagnostics, b.diagnostics);

        if (i < 2U) {
            char expected_fingerprint[33];
            TEST_ASSERT_NOT_NULL(a.descriptor);
            TEST_ASSERT_EQUAL_STRING("fixture-duplicate-id",
                                     a.descriptor->id);
            TEST_ASSERT_NOT_NULL(a.fingerprint);
            TEST_ASSERT_TRUE(expected_fixture_fingerprint(
                expected_keys[i], expected_fingerprint));
            TEST_ASSERT_EQUAL_STRING(expected_fingerprint, a.fingerprint);
            TEST_ASSERT_EQUAL_STRING("fixture-duplicate-id",
                                     diagnostic->format_id);
        } else {
            TEST_ASSERT_NULL(a.descriptor);
            TEST_ASSERT_NULL(a.fingerprint);
        }
    }

    tp_format_resolution resolution = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_catalog_resolve(catalogs[0], "fixture-duplicate-id",
                                  &resolution, &error));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_RESOLUTION_UNAVAILABLE,
                          resolution.state);
    TEST_ASSERT_NULL(resolution.descriptor);
    TEST_ASSERT_NOT_NULL(resolution.diagnostics);
    TEST_ASSERT_NULL(tp_format_catalog_find_available(
        catalogs[0], "fixture-duplicate-id"));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_format_catalog_resolve(catalogs[0], "not-installed", &resolution,
                                  &error));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_RESOLUTION_ABSENT, resolution.state);

    tp_format_catalog_release(catalogs[0]);
    tp_format_catalog_release(catalogs[1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(
        test_strict_descriptor_parser_accepts_full_surface_and_rejects_fixtures);
    RUN_TEST(test_descriptor_rejects_escaped_nul_in_object_key);
    RUN_TEST(test_descriptor_rejects_escaped_nul_in_string_value);
    RUN_TEST(test_descriptor_materialization_is_numeric_locale_independent);
    RUN_TEST(test_diagnostic_reports_are_deep_owned_and_bounded);
    RUN_TEST(
        test_fixture_scan_is_deterministic_and_cannot_install_before_compile);
    RUN_TEST(test_duplicate_id_includes_source_rejected_descriptor);
    RUN_TEST(
        test_broken_only_catalog_rows_and_diagnostics_are_deterministic);
    return UNITY_END();
}
