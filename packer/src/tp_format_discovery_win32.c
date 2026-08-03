#include "tp_format_discovery_internal.h"

#ifdef _WIN32

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "tp_core/tp_id.h"
#include "tp_fs_internal.h"
#include "tp_format_descriptor_internal.h"
#include "tp_hex.h"

#define TP_FORMAT_DIRECTORY_BUFFER_BYTES 65536U
#define TP_FORMAT_WINDOWS_PATH_CODE_UNITS 32767U

typedef struct tp_format_directory_entry {
    const wchar_t *name;
    size_t name_code_units;
    DWORD attributes;
} tp_format_directory_entry;

typedef enum tp_format_directory_next_result {
    TP_FORMAT_DIRECTORY_NEXT_ENTRY = 1,
    TP_FORMAT_DIRECTORY_NEXT_END,
    TP_FORMAT_DIRECTORY_NEXT_ERROR,
} tp_format_directory_next_result;

typedef struct tp_format_directory_reader {
    HANDLE handle;
    _Alignas(FILE_ID_BOTH_DIR_INFO)
        unsigned char buffer[TP_FORMAT_DIRECTORY_BUFFER_BYTES];
    size_t next_offset;
    bool has_buffer;
    DWORD error_code;
} tp_format_directory_reader;

typedef struct tp_format_handle_attributes {
    DWORD attributes;
    DWORD reparse_tag;
} tp_format_handle_attributes;

static bool format_win32_is_oom(DWORD native_code) {
    return native_code == ERROR_NOT_ENOUGH_MEMORY ||
           native_code == ERROR_OUTOFMEMORY ||
           native_code == ERROR_NO_SYSTEM_RESOURCES ||
           native_code == ERROR_COMMITMENT_LIMIT;
}

static tp_status format_status_for_native(DWORD native_code,
                                          tp_status fallback) {
    return format_win32_is_oom(native_code) ? TP_STATUS_OOM : fallback;
}

static char *format_string_duplicate(const char *text) {
    const size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1U);
    if (copy) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

static void format_failure_set(tp_format_discovery_failure *failure,
                               tp_format_diagnostic_code code,
                               const char *message) {
    failure->code = code;
    (void)snprintf(failure->message, sizeof failure->message, "%s",
                   message ? message : "format discovery failed");
    tp_error_trim_partial_utf8(failure->message);
}

static tp_status format_error_set(tp_error *error, tp_status status,
                                  const char *path, DWORD native_code,
                                  const char *message) {
    const tp_status result =
        native_code != ERROR_SUCCESS
            ? tp_error_set(error, status, "%s (Windows error %lu)", message,
                           (unsigned long)native_code)
            : tp_error_set(error, status, "%s", message);
    if (error) {
        (void)snprintf(error->file_io.path, sizeof error->file_io.path, "%s",
                       path ? path : "");
        tp_error_trim_partial_utf8(error->file_io.path);
        error->file_io.native_code = (int)(uint32_t)native_code;
    }
    return result;
}

static tp_status format_root_failure(
    tp_format_discovery_result *out, tp_format_discovery_failure *failure,
    tp_format_diagnostic_code code, tp_status status, const char *root,
    DWORD native_code, const char *message, tp_error *error) {
    tp_format_discovery_result_destroy(out);
    format_failure_set(failure, code, message);
    return format_error_set(error, status, root, native_code, message);
}

static void format_candidate_destroy(tp_format_discovered_candidate *candidate) {
    if (!candidate) {
        return;
    }
    free(candidate->key);
    free(candidate->package_path);
    free(candidate->descriptor_bytes);
    free(candidate->source_bytes);
    memset(candidate, 0, sizeof *candidate);
}

static void format_candidate_set_fault(
    tp_format_discovered_candidate *candidate,
    tp_format_diagnostic_code code, const char *message) {
    candidate->fault_code = code;
    (void)snprintf(candidate->fault_message,
                   sizeof candidate->fault_message, "%s", message);
    tp_error_trim_partial_utf8(candidate->fault_message);
}

static void format_candidate_set_win32_fault(
    tp_format_discovered_candidate *candidate,
    tp_format_diagnostic_code code, const char *operation,
    DWORD native_code) {
    candidate->fault_code = code;
    (void)snprintf(candidate->fault_message,
                   sizeof candidate->fault_message,
                   "%s (Windows error %lu)", operation,
                   (unsigned long)native_code);
    tp_error_trim_partial_utf8(candidate->fault_message);
}

static bool wide_is_separator(wchar_t value) {
    return value == L'\\' || value == L'/';
}

static bool wide_root_is_absolute(const wchar_t *root) {
    const size_t length = wcslen(root);
    if (length >= 4U && wide_is_separator(root[0]) &&
        wide_is_separator(root[1]) &&
        (root[2] == L'?' || root[2] == L'.') &&
        wide_is_separator(root[3])) {
        return false;
    }
    const bool drive =
        length >= 3U &&
        ((root[0] >= L'A' && root[0] <= L'Z') ||
         (root[0] >= L'a' && root[0] <= L'z')) &&
        root[1] == L':' && wide_is_separator(root[2]);
    const bool unc = length >= 5U && wide_is_separator(root[0]) &&
                     wide_is_separator(root[1]) &&
                     !wide_is_separator(root[2]);
    return drive || unc;
}

static tp_status root_utf8_to_wide(const char *root, wchar_t **out,
                                   tp_error *error) {
    *out = NULL;
    if (!root || root[0] == '\0') {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format root must be non-empty");
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, NULL, 0);
    if (required <= 0) {
        const DWORD native_code = GetLastError();
        return format_error_set(error,
                                format_status_for_native(
                                    native_code, TP_STATUS_INVALID_UTF8),
                                root, native_code,
                                "format root is not strict UTF-8");
    }
    if ((size_t)required > TP_FORMAT_WINDOWS_PATH_CODE_UNITS) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "format root exceeds the Windows path limit");
    }
    wchar_t *wide =
        (wchar_t *)malloc((size_t)required * sizeof *wide);
    if (!wide) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "format root UTF-16 allocation failed");
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1,
                            wide, required) == 0) {
        const DWORD native_code = GetLastError();
        free(wide);
        return format_error_set(error,
                                format_status_for_native(
                                    native_code, TP_STATUS_INVALID_UTF8),
                                root, native_code,
                                "format root UTF-16 conversion failed");
    }
    if (!wide_root_is_absolute(wide)) {
        free(wide);
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format root must be an absolute Windows path");
    }
    free(wide);
    errno = 0;
    wide = tp_fs_win32_path_alloc(root);
    if (!wide) {
        return tp_error_set(
            error, errno == ENOMEM ? TP_STATUS_OOM : TP_STATUS_INVALID_ARGUMENT,
            errno == ENOMEM ? "format root UTF-16 allocation failed"
                            : "format root is not an admitted Windows path");
    }
    *out = wide;
    return TP_STATUS_OK;
}

static tp_status wide_path_join(const wchar_t *parent, const wchar_t *child,
                                size_t child_code_units, wchar_t **out) {
    *out = NULL;
    const size_t parent_length = wcslen(parent);
    const bool needs_separator =
        parent_length > 0U && !wide_is_separator(parent[parent_length - 1U]);
    const size_t separator_count = needs_separator ? 1U : 0U;
    if (parent_length > SIZE_MAX - separator_count ||
        parent_length + separator_count > SIZE_MAX - child_code_units - 1U) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    const size_t total =
        parent_length + separator_count + child_code_units;
    if (total >= TP_FORMAT_WINDOWS_PATH_CODE_UNITS) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    wchar_t *joined = (wchar_t *)malloc((total + 1U) * sizeof *joined);
    if (!joined) {
        return TP_STATUS_OOM;
    }
    memcpy(joined, parent, parent_length * sizeof *joined);
    size_t offset = parent_length;
    if (needs_separator) {
        joined[offset++] = L'\\';
    }
    memcpy(joined + offset, child, child_code_units * sizeof *joined);
    joined[total] = L'\0';
    *out = joined;
    return TP_STATUS_OK;
}

static tp_status handle_attributes(HANDLE handle,
                                   tp_format_handle_attributes *out,
                                   DWORD *native_code) {
    FILE_ATTRIBUTE_TAG_INFO info;
    memset(&info, 0, sizeof info);
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                      &info, sizeof info)) {
        *native_code = GetLastError();
        return TP_STATUS_FILE_IO_FAILED;
    }
    out->attributes = info.FileAttributes;
    out->reparse_tag = info.ReparseTag;
    *native_code = ERROR_SUCCESS;
    return TP_STATUS_OK;
}

static tp_status handle_final_path(HANDLE handle, wchar_t **out,
                                   DWORD *native_code) {
    *out = NULL;
    wchar_t stack_path[512];
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_NT;
    DWORD copied = GetFinalPathNameByHandleW(
        handle, stack_path, (DWORD)(sizeof stack_path / sizeof stack_path[0]),
        flags);
    if (copied == 0U) {
        *native_code = GetLastError();
        return TP_STATUS_FILE_IO_FAILED;
    }
    if (copied < (DWORD)(sizeof stack_path / sizeof stack_path[0])) {
        wchar_t *copy =
            (wchar_t *)malloc(((size_t)copied + 1U) * sizeof *copy);
        if (!copy) {
            return TP_STATUS_OOM;
        }
        memcpy(copy, stack_path, ((size_t)copied + 1U) * sizeof *copy);
        *out = copy;
        *native_code = ERROR_SUCCESS;
        return TP_STATUS_OK;
    }
    if ((size_t)copied >= TP_FORMAT_WINDOWS_PATH_CODE_UNITS) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    const DWORD capacity = copied + 1U;
    wchar_t *path = (wchar_t *)malloc((size_t)capacity * sizeof *path);
    if (!path) {
        return TP_STATUS_OOM;
    }
    copied = GetFinalPathNameByHandleW(handle, path, capacity, flags);
    if (copied == 0U || copied >= capacity) {
        *native_code = copied == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW;
        free(path);
        return TP_STATUS_FILE_IO_FAILED;
    }
    path[copied] = L'\0';
    *out = path;
    *native_code = ERROR_SUCCESS;
    return TP_STATUS_OK;
}

static bool wide_span_equal(const wchar_t *left, size_t left_length,
                            const wchar_t *right, size_t right_length) {
    return left_length == right_length &&
           memcmp(left, right, left_length * sizeof *left) == 0;
}

static bool final_path_is_immediate_child(const wchar_t *parent,
                                          const wchar_t *child,
                                          const wchar_t *expected_name,
                                          size_t expected_length) {
    size_t parent_length = wcslen(parent);
    while (parent_length > 0U &&
           wide_is_separator(parent[parent_length - 1U])) {
        parent_length--;
    }
    const size_t child_length = wcslen(child);
    if (parent_length == 0U ||
        parent_length > SIZE_MAX - expected_length - 1U ||
        child_length != parent_length + 1U + expected_length ||
        !wide_is_separator(child[parent_length]) ||
        CompareStringOrdinal(parent, (int)parent_length, child,
                             (int)parent_length, FALSE) != CSTR_EQUAL) {
        return false;
    }
    return wide_span_equal(child + parent_length + 1U, expected_length,
                           expected_name, expected_length);
}

static void directory_reader_init(tp_format_directory_reader *reader,
                                  HANDLE handle) {
    memset(reader, 0, sizeof *reader);
    reader->handle = handle;
}

static tp_format_directory_next_result directory_reader_next(
    tp_format_directory_reader *reader, tp_format_directory_entry *out) {
    for (;;) {
        if (!reader->has_buffer) {
            memset(reader->buffer, 0, sizeof reader->buffer);
            if (!GetFileInformationByHandleEx(
                    reader->handle, FileIdBothDirectoryInfo,
                    reader->buffer, (DWORD)sizeof reader->buffer)) {
                const DWORD native_code = GetLastError();
                if (native_code == ERROR_NO_MORE_FILES) {
                    return TP_FORMAT_DIRECTORY_NEXT_END;
                }
                reader->error_code = native_code;
                return TP_FORMAT_DIRECTORY_NEXT_ERROR;
            }
            reader->next_offset = 0U;
            reader->has_buffer = true;
        }

        const size_t fixed_bytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
        if (reader->next_offset > sizeof reader->buffer - fixed_bytes) {
            reader->error_code = ERROR_INVALID_DATA;
            return TP_FORMAT_DIRECTORY_NEXT_ERROR;
        }
        const FILE_ID_BOTH_DIR_INFO *entry =
            (const FILE_ID_BOTH_DIR_INFO *)(const void *)(
                reader->buffer + reader->next_offset);
        const size_t name_bytes = (size_t)entry->FileNameLength;
        if ((name_bytes % sizeof(wchar_t)) != 0U ||
            name_bytes > sizeof reader->buffer - reader->next_offset -
                             fixed_bytes) {
            reader->error_code = ERROR_INVALID_DATA;
            return TP_FORMAT_DIRECTORY_NEXT_ERROR;
        }

        const ULONG next = entry->NextEntryOffset;
        if (next == 0U) {
            reader->has_buffer = false;
        } else {
            const size_t next_size = (size_t)next;
            if (next_size < fixed_bytes + name_bytes ||
                next_size > sizeof reader->buffer - reader->next_offset) {
                reader->error_code = ERROR_INVALID_DATA;
                return TP_FORMAT_DIRECTORY_NEXT_ERROR;
            }
            reader->next_offset += next_size;
        }

        out->name = entry->FileName;
        out->name_code_units = name_bytes / sizeof(wchar_t);
        out->attributes = entry->FileAttributes;
        return TP_FORMAT_DIRECTORY_NEXT_ENTRY;
    }
}

static bool directory_entry_is_dot(const tp_format_directory_entry *entry) {
    return (entry->name_code_units == 1U && entry->name[0] == L'.') ||
           (entry->name_code_units == 2U && entry->name[0] == L'.' &&
            entry->name[1] == L'.');
}

static bool package_name_to_portable_utf8(const wchar_t *name, size_t length,
                                          char out[TP_FORMAT_PACKAGE_NAME_MAX_BYTES + 1U]) {
    if (length == 0U ||
        (length == 1U && name[0] == L'.') ||
        (length == 2U && name[0] == L'.' && name[1] == L'.') ||
        length > (size_t)INT_MAX) {
        return false;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, name, (int)length, NULL, 0, NULL, NULL);
    if (required <= 0 ||
        (size_t)required > TP_FORMAT_PACKAGE_NAME_MAX_BYTES) {
        return false;
    }
    const int copied = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, name, (int)length, out,
        required, NULL, NULL);
    if (copied != required) {
        return false;
    }
    out[copied] = '\0';
    return tp_format_package_name_is_portable(out);
}

static void invalid_package_name_key(const wchar_t *name, size_t length,
                                     char out[54]) {
    tp_hasher hasher = tp_hasher_init();
    for (size_t i = 0U; i < length; ++i) {
        const uint16_t code_unit = (uint16_t)name[i];
        const unsigned char little_endian[2] = {
            (unsigned char)(code_unit & 0xFFU),
            (unsigned char)(code_unit >> 8U),
        };
        tp_hasher_update(&hasher, little_endian, sizeof little_endian);
    }
    const tp_id128 hash = tp_hasher_final(hasher);
    static const char prefix[] = "invalid-name-win16-";
    memcpy(out, prefix, sizeof prefix - 1U);
    tp_hex_encode_lower(hash.bytes, sizeof hash.bytes,
                        out + sizeof prefix - 1U);
}

static tp_status candidate_initialize(
    const tp_format_directory_entry *entry,
    tp_format_discovered_candidate *candidate, wchar_t **out_wide_name,
    tp_error *error) {
    memset(candidate, 0, sizeof *candidate);
    *out_wide_name = NULL;

    char portable[TP_FORMAT_PACKAGE_NAME_MAX_BYTES + 1U];
    if (!package_name_to_portable_utf8(entry->name,
                                       entry->name_code_units, portable)) {
        char key[54];
        invalid_package_name_key(entry->name, entry->name_code_units, key);
        candidate->key = format_string_duplicate(key);
        if (!candidate->key) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "invalid package-name row allocation failed");
        }
        format_candidate_set_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_NAME_INVALID,
            "package directory name is not portable API-v1 UTF-8");
        return TP_STATUS_OK;
    }

    candidate->key = format_string_duplicate(portable);
    char logical[sizeof "formats/" + TP_FORMAT_PACKAGE_NAME_MAX_BYTES];
    const int logical_length =
        snprintf(logical, sizeof logical, "formats/%s", portable);
    if (!candidate->key || logical_length < 0 ||
        (size_t)logical_length >= sizeof logical) {
        format_candidate_destroy(candidate);
        return tp_error_set(error, TP_STATUS_OOM,
                            "package row allocation failed");
    }
    candidate->package_path = format_string_duplicate(logical);
    if (!candidate->package_path) {
        format_candidate_destroy(candidate);
        return tp_error_set(error, TP_STATUS_OOM,
                            "package logical-path allocation failed");
    }

    wchar_t *wide_name =
        (wchar_t *)malloc((entry->name_code_units + 1U) * sizeof *wide_name);
    if (!wide_name) {
        format_candidate_destroy(candidate);
        return tp_error_set(error, TP_STATUS_OOM,
                            "package UTF-16 name allocation failed");
    }
    memcpy(wide_name, entry->name,
           entry->name_code_units * sizeof *wide_name);
    wide_name[entry->name_code_units] = L'\0';
    *out_wide_name = wide_name;
    return TP_STATUS_OK;
}

static bool package_has_exact_entries(
    HANDLE package_handle, tp_format_discovered_candidate *candidate,
    DWORD *native_code) {
    bool descriptor_found = false;
    bool source_found = false;
    tp_format_directory_reader reader;
    directory_reader_init(&reader, package_handle);
    for (;;) {
        tp_format_directory_entry entry;
        const tp_format_directory_next_result next =
            directory_reader_next(&reader, &entry);
        if (next == TP_FORMAT_DIRECTORY_NEXT_END) {
            break;
        }
        if (next == TP_FORMAT_DIRECTORY_NEXT_ERROR) {
            *native_code = reader.error_code;
            format_candidate_set_win32_fault(
                candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
                "package directory enumeration failed", reader.error_code);
            return false;
        }
        if (directory_entry_is_dot(&entry)) {
            continue;
        }
        if (wide_span_equal(entry.name, entry.name_code_units, L"format.json",
                            sizeof L"format.json" / sizeof(wchar_t) - 1U)) {
            if (descriptor_found) {
                format_candidate_set_fault(
                    candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY,
                    "package contains duplicate format.json entries");
                return false;
            }
            descriptor_found = true;
        } else if (wide_span_equal(
                       entry.name, entry.name_code_units, L"export.lua",
                       sizeof L"export.lua" / sizeof(wchar_t) - 1U)) {
            if (source_found) {
                format_candidate_set_fault(
                    candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY,
                    "package contains duplicate export.lua entries");
                return false;
            }
            source_found = true;
        } else {
            format_candidate_set_fault(
                candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY,
                "package contains an entry other than format.json and export.lua");
            return false;
        }
    }
    if (!descriptor_found) {
        candidate->fault_file = TP_FORMAT_DISCOVERY_FAULT_DESCRIPTOR;
        format_candidate_set_fault(candidate,
                                   TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING,
                                   "package is missing format.json");
        return false;
    }
    if (!source_found) {
        candidate->fault_file = TP_FORMAT_DISCOVERY_FAULT_SOURCE;
        format_candidate_set_fault(candidate,
                                   TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING,
                                   "package is missing export.lua");
        return false;
    }
    *native_code = ERROR_SUCCESS;
    return true;
}

static tp_status read_package_file(
    const wchar_t *package_open_path, const wchar_t *package_final_path,
    const wchar_t *fixed_name, size_t fixed_name_length, size_t size_limit,
    tp_format_diagnostic_code missing_code,
    tp_format_discovered_candidate *candidate, unsigned char **out_bytes,
    size_t *out_byte_count, tp_error *error) {
    *out_bytes = NULL;
    *out_byte_count = 0U;
    candidate->fault_file =
        missing_code == TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING
            ? TP_FORMAT_DISCOVERY_FAULT_DESCRIPTOR
            : TP_FORMAT_DISCOVERY_FAULT_SOURCE;
    wchar_t *file_path = NULL;
    tp_status status = wide_path_join(package_open_path, fixed_name,
                                      fixed_name_length, &file_path);
    if (status == TP_STATUS_OOM) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "package file path allocation failed");
    }
    if (status != TP_STATUS_OK) {
        format_candidate_set_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package file path exceeds the Windows path limit");
        return TP_STATUS_OK;
    }

    HANDLE file = CreateFileW(
        file_path, GENERIC_READ | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        NULL);
    const DWORD open_code =
        file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(file_path);
    if (file == INVALID_HANDLE_VALUE) {
        if (format_win32_is_oom(open_code)) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "package file open exhausted host memory");
        }
        if (open_code == ERROR_FILE_NOT_FOUND ||
            open_code == ERROR_PATH_NOT_FOUND) {
            format_candidate_set_fault(
                candidate, missing_code,
                missing_code == TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING
                    ? "package is missing format.json"
                    : "package is missing export.lua");
        } else {
            format_candidate_set_win32_fault(
                candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
                "package file open failed", open_code);
        }
        return TP_STATUS_OK;
    }

    status = TP_STATUS_OK;
    DWORD native_code = ERROR_SUCCESS;
    tp_format_handle_attributes attributes;
    if (handle_attributes(file, &attributes, &native_code) != TP_STATUS_OK) {
        if (format_win32_is_oom(native_code)) {
            (void)CloseHandle(file);
            return tp_error_set(
                error, TP_STATUS_OOM,
                "package file attribute query exhausted host memory");
        }
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package file attribute query failed", native_code);
        goto cleanup;
    }
    if ((attributes.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        attributes.reparse_tag != 0U) {
        format_candidate_set_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE,
            "package file is a reparse point");
        goto cleanup;
    }

    FILE_STANDARD_INFO standard;
    memset(&standard, 0, sizeof standard);
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                      sizeof standard)) {
        native_code = GetLastError();
        if (format_win32_is_oom(native_code)) {
            (void)CloseHandle(file);
            return tp_error_set(
                error, TP_STATUS_OOM,
                "package file identity query exhausted host memory");
        }
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package file size query failed", native_code);
        goto cleanup;
    }
    SetLastError(ERROR_SUCCESS);
    const DWORD file_type = GetFileType(file);
    const DWORD type_code = GetLastError();
    if (file_type == FILE_TYPE_UNKNOWN && type_code != ERROR_SUCCESS) {
        if (format_win32_is_oom(type_code)) {
            (void)CloseHandle(file);
            return tp_error_set(
                error, TP_STATUS_OOM,
                "package file type query exhausted host memory");
        }
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package file type query failed", type_code);
        goto cleanup;
    }
    if (standard.Directory ||
        (attributes.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        file_type != FILE_TYPE_DISK ||
        standard.EndOfFile.QuadPart < 0) {
        format_candidate_set_fault(candidate,
                                   TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE,
                                   "package entry is not a regular file");
        goto cleanup;
    }
    if (standard.NumberOfLinks != 1U) {
        format_candidate_set_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE,
            "package file is hard-linked outside its fixed identity");
        goto cleanup;
    }
    const uint64_t file_size = (uint64_t)standard.EndOfFile.QuadPart;
    if (file_size > (uint64_t)size_limit) {
        format_candidate_set_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TOO_LARGE,
            "package file exceeds its API-v1 byte limit");
        goto cleanup;
    }

    wchar_t *final_path = NULL;
    status = handle_final_path(file, &final_path, &native_code);
    if (status == TP_STATUS_OOM || format_win32_is_oom(native_code)) {
        (void)CloseHandle(file);
        return tp_error_set(error, TP_STATUS_OOM,
                            "package final-path allocation failed");
    }
    if (status != TP_STATUS_OK) {
        free(final_path);
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package file final-path query failed", native_code);
        goto cleanup;
    }
    if (!final_path_is_immediate_child(package_final_path, final_path,
                                       fixed_name, fixed_name_length)) {
        free(final_path);
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE,
            "package file escapes its opened package directory",
            ERROR_SUCCESS);
        goto cleanup;
    }
    free(final_path);

    const size_t byte_count = (size_t)file_size;
    unsigned char *bytes = (unsigned char *)malloc(byte_count + 1U);
    if (!bytes) {
        (void)CloseHandle(file);
        return tp_error_set(error, TP_STATUS_OOM,
                            "package byte snapshot allocation failed");
    }
    size_t offset = 0U;
    while (offset < byte_count) {
        DWORD copied = 0U;
        const DWORD request = (DWORD)(byte_count - offset);
        if (!ReadFile(file, bytes + offset, request, &copied, NULL)) {
            native_code = GetLastError();
            free(bytes);
            if (format_win32_is_oom(native_code)) {
                (void)CloseHandle(file);
                return tp_error_set(
                    error, TP_STATUS_OOM,
                    "package file read exhausted host memory");
            }
            format_candidate_set_win32_fault(
                candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
                "package file read failed", native_code);
            goto cleanup;
        }
        if (copied == 0U) {
            free(bytes);
            format_candidate_set_fault(
                candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
                "package file ended before its opened size");
            goto cleanup;
        }
        offset += (size_t)copied;
    }
    unsigned char probe = 0U;
    DWORD probe_count = 0U;
    if (!ReadFile(file, &probe, 1U, &probe_count, NULL)) {
        native_code = GetLastError();
        free(bytes);
        if (format_win32_is_oom(native_code)) {
            (void)CloseHandle(file);
            return tp_error_set(
                error, TP_STATUS_OOM,
                "package file EOF probe exhausted host memory");
        }
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package file EOF probe failed", native_code);
        goto cleanup;
    }
    if (probe_count != 0U) {
        free(bytes);
        format_candidate_set_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package file changed size while it was read");
        goto cleanup;
    }
    bytes[byte_count] = 0U;
    *out_bytes = bytes;
    *out_byte_count = byte_count;

cleanup:
    if (!CloseHandle(file) && candidate->fault_code == 0 &&
        *out_bytes == NULL) {
        native_code = GetLastError();
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package file close failed", native_code);
    }
    return TP_STATUS_OK;
}

static tp_status scan_package(
    const wchar_t *root_open_path, const wchar_t *root_final_path,
    const wchar_t *package_name, size_t package_name_length,
    DWORD enumerated_attributes,
    tp_format_discovered_candidate *candidate, tp_error *error) {
    wchar_t *package_path = NULL;
    tp_status status = wide_path_join(root_open_path, package_name,
                                      package_name_length, &package_path);
    if (status == TP_STATUS_OOM) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "package path allocation failed");
    }
    if (status != TP_STATUS_OK) {
        format_candidate_set_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package path exceeds the Windows path limit");
        return TP_STATUS_OK;
    }

    HANDLE package = CreateFileW(
        package_path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    const DWORD open_code =
        package == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (package == INVALID_HANDLE_VALUE) {
        if (format_win32_is_oom(open_code)) {
            free(package_path);
            return tp_error_set(
                error, TP_STATUS_OOM,
                "package directory open exhausted host memory");
        }
        if ((enumerated_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            format_candidate_set_fault(candidate,
                                       TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE,
                                       "package directory is a reparse point");
        } else {
            format_candidate_set_win32_fault(
                candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
                "package directory open failed", open_code);
        }
        free(package_path);
        return TP_STATUS_OK;
    }

    DWORD native_code = ERROR_SUCCESS;
    tp_format_handle_attributes attributes;
    if (handle_attributes(package, &attributes, &native_code) != TP_STATUS_OK) {
        if (format_win32_is_oom(native_code)) {
            (void)CloseHandle(package);
            free(package_path);
            return tp_error_set(
                error, TP_STATUS_OOM,
                "package attribute query exhausted host memory");
        }
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package directory attribute query failed", native_code);
        goto cleanup;
    }
    if ((attributes.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        attributes.reparse_tag != 0U) {
        format_candidate_set_fault(candidate,
                                   TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE,
                                   "package directory is a reparse point");
        goto cleanup;
    }
    if ((attributes.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        format_candidate_set_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE,
            "package candidate is no longer a directory");
        goto cleanup;
    }

    wchar_t *package_final_path = NULL;
    status = handle_final_path(package, &package_final_path, &native_code);
    if (status == TP_STATUS_OOM || format_win32_is_oom(native_code)) {
        (void)CloseHandle(package);
        free(package_path);
        return tp_error_set(error, TP_STATUS_OOM,
                            "package final-path allocation failed");
    }
    if (status != TP_STATUS_OK) {
        free(package_final_path);
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            "package directory final-path query failed", native_code);
        goto cleanup;
    }
    if (!final_path_is_immediate_child(root_final_path, package_final_path,
                                       package_name,
                                       package_name_length)) {
        free(package_final_path);
        format_candidate_set_win32_fault(
            candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE,
            "package directory escapes the opened format root",
            ERROR_SUCCESS);
        goto cleanup;
    }

    if (!package_has_exact_entries(package, candidate, &native_code)) {
        if (format_win32_is_oom(native_code)) {
            free(package_final_path);
            (void)CloseHandle(package);
            free(package_path);
            return tp_error_set(
                error, TP_STATUS_OOM,
                "package enumeration exhausted host memory");
        }
        free(package_final_path);
        goto cleanup;
    }

    unsigned char *descriptor_bytes = NULL;
    size_t descriptor_byte_count = 0U;
    status = read_package_file(
        package_path, package_final_path, L"format.json",
        sizeof L"format.json" / sizeof(wchar_t) - 1U,
        TP_FORMAT_DESCRIPTOR_MAX_BYTES,
        TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING, candidate,
        &descriptor_bytes, &descriptor_byte_count, error);
    if (status != TP_STATUS_OK) {
        free(package_final_path);
        (void)CloseHandle(package);
        free(package_path);
        return status;
    }
    if (candidate->fault_code != 0) {
        free(package_final_path);
        free(descriptor_bytes);
        goto cleanup;
    }

    unsigned char *source_bytes = NULL;
    size_t source_byte_count = 0U;
    status = read_package_file(
        package_path, package_final_path, L"export.lua",
        sizeof L"export.lua" / sizeof(wchar_t) - 1U,
        TP_FORMAT_SOURCE_MAX_BYTES, TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING,
        candidate, &source_bytes, &source_byte_count, error);
    free(package_final_path);
    if (status != TP_STATUS_OK) {
        free(descriptor_bytes);
        (void)CloseHandle(package);
        free(package_path);
        return status;
    }
    if (candidate->fault_code != 0) {
        free(descriptor_bytes);
        free(source_bytes);
        goto cleanup;
    }

    candidate->descriptor_bytes = descriptor_bytes;
    candidate->descriptor_byte_count = descriptor_byte_count;
    candidate->source_bytes = source_bytes;
    candidate->source_byte_count = source_byte_count;

cleanup:
    (void)CloseHandle(package);
    free(package_path);
    return TP_STATUS_OK;
}

static tp_status append_candidate(tp_format_discovery_result *result,
                                  tp_format_discovered_candidate *candidate,
                                  tp_error *error) {
    const size_t new_count = result->candidate_count + 1U;
    tp_format_discovered_candidate *resized =
        (tp_format_discovered_candidate *)realloc(
            result->candidates, new_count * sizeof *resized);
    if (!resized) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "format candidate array allocation failed");
    }
    result->candidates = resized;
    result->candidates[result->candidate_count] = *candidate;
    result->candidate_count = new_count;
    memset(candidate, 0, sizeof *candidate);
    return TP_STATUS_OK;
}

static void make_limit_fail_closed(tp_format_discovery_result *result) {
    for (size_t i = 0U; i < result->candidate_count; ++i) {
        format_candidate_destroy(&result->candidates[i]);
    }
    free(result->candidates);
    result->candidates = NULL;
    result->candidate_count = 0U;
    result->limit_fail_closed = true;
}

tp_status tp_format_discovery_read_root(
    const char *root, tp_format_discovery_result *out,
    tp_format_discovery_failure *failure, tp_error *error) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (failure) {
        memset(failure, 0, sizeof *failure);
    }
    if (!root || !out || !failure) {
        if (failure) {
            format_failure_set(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                               "format discovery requires root and outputs");
        }
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format discovery requires root and outputs");
    }

    wchar_t *root_wide = NULL;
    tp_status status = root_utf8_to_wide(root, &root_wide, error);
    if (status != TP_STATUS_OK) {
        format_failure_set(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                           error && error->msg[0] != '\0'
                               ? error->msg
                               : "format root is invalid");
        return status;
    }
    out->root = format_string_duplicate(root);
    if (!out->root) {
        free(root_wide);
        return format_root_failure(
            out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO, TP_STATUS_OOM,
            root, ERROR_SUCCESS, "format root copy allocation failed", error);
    }

    HANDLE root_handle = CreateFileW(
        root_wide, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    const DWORD open_code =
        root_handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (root_handle == INVALID_HANDLE_VALUE) {
        free(root_wide);
        if (open_code == ERROR_FILE_NOT_FOUND ||
            open_code == ERROR_PATH_NOT_FOUND) {
            out->root_missing = true;
            return TP_STATUS_OK;
        }
        return format_root_failure(
            out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
            format_status_for_native(open_code,
                                     TP_STATUS_PATH_RESOLVE_FAILED),
            root, open_code,
            "format root open failed", error);
    }

    DWORD native_code = ERROR_SUCCESS;
    tp_format_handle_attributes attributes;
    if (handle_attributes(root_handle, &attributes, &native_code) !=
        TP_STATUS_OK) {
        (void)CloseHandle(root_handle);
        free(root_wide);
        return format_root_failure(
            out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
            format_status_for_native(native_code,
                                     TP_STATUS_PATH_RESOLVE_FAILED),
            root, native_code,
            "format root attribute query failed", error);
    }
    if ((attributes.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        attributes.reparse_tag != 0U) {
        (void)CloseHandle(root_handle);
        free(root_wide);
        return format_root_failure(
            out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_REPARSE,
            TP_STATUS_PATH_RESOLVE_FAILED, root, ERROR_SUCCESS,
            "format root is a reparse point", error);
    }
    if ((attributes.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        (void)CloseHandle(root_handle);
        free(root_wide);
        return format_root_failure(
            out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_NOT_DIRECTORY,
            TP_STATUS_PATH_RESOLVE_FAILED, root, ERROR_SUCCESS,
            "format root is not a directory", error);
    }

    wchar_t *root_final_path = NULL;
    status = handle_final_path(root_handle, &root_final_path, &native_code);
    if (status != TP_STATUS_OK) {
        (void)CloseHandle(root_handle);
        free(root_wide);
        return format_root_failure(
            out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
            status == TP_STATUS_OOM || format_win32_is_oom(native_code)
                ? TP_STATUS_OOM
                : TP_STATUS_PATH_RESOLVE_FAILED,
            root, native_code,
            status == TP_STATUS_OOM
                ? "format root final-path allocation failed"
                : "format root final-path query failed",
            error);
    }

    tp_format_directory_reader reader;
    directory_reader_init(&reader, root_handle);
    size_t root_entry_count = 0U;
    size_t package_byte_count = 0U;
    for (;;) {
        tp_format_directory_entry entry;
        const tp_format_directory_next_result next =
            directory_reader_next(&reader, &entry);
        if (next == TP_FORMAT_DIRECTORY_NEXT_END) {
            break;
        }
        if (next == TP_FORMAT_DIRECTORY_NEXT_ERROR) {
            const DWORD enumeration_code = reader.error_code;
            free(root_final_path);
            (void)CloseHandle(root_handle);
            free(root_wide);
            return format_root_failure(
                out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                format_status_for_native(enumeration_code,
                                         TP_STATUS_PATH_RESOLVE_FAILED),
                root, enumeration_code,
                "format root enumeration failed", error);
        }
        if (directory_entry_is_dot(&entry)) {
            continue;
        }
        root_entry_count++;
        if (root_entry_count > TP_FORMAT_ROOT_ENTRY_MAX) {
            make_limit_fail_closed(out);
            break;
        }
        if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
            continue;
        }
        if (out->candidate_count >= TP_FORMAT_PACKAGE_MAX) {
            make_limit_fail_closed(out);
            break;
        }

        tp_format_discovered_candidate candidate;
        wchar_t *package_name = NULL;
        status = candidate_initialize(&entry, &candidate, &package_name,
                                      error);
        if (status != TP_STATUS_OK) {
            free(root_final_path);
            (void)CloseHandle(root_handle);
            free(root_wide);
            return format_root_failure(
                out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO, status, root,
                ERROR_SUCCESS, "format candidate materialization failed",
                error);
        }
        if (package_name) {
            status = scan_package(root_wide, root_final_path, package_name,
                                  entry.name_code_units, entry.attributes,
                                  &candidate, error);
            free(package_name);
            if (status != TP_STATUS_OK) {
                format_candidate_destroy(&candidate);
                free(root_final_path);
                (void)CloseHandle(root_handle);
                free(root_wide);
                return format_root_failure(
                    out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO, status, root,
                    ERROR_SUCCESS, "format package materialization failed",
                    error);
            }
        }

        if (candidate.fault_code == 0) {
            const size_t candidate_bytes = candidate.descriptor_byte_count +
                                           candidate.source_byte_count;
            if (package_byte_count >
                TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX - candidate_bytes) {
                format_candidate_destroy(&candidate);
                make_limit_fail_closed(out);
                break;
            }
            package_byte_count += candidate_bytes;
        }
        status = append_candidate(out, &candidate, error);
        if (status != TP_STATUS_OK) {
            format_candidate_destroy(&candidate);
            free(root_final_path);
            (void)CloseHandle(root_handle);
            free(root_wide);
            return format_root_failure(
                out, failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO, status, root,
                ERROR_SUCCESS, "format candidate array allocation failed",
                error);
        }
    }

    free(root_final_path);
    (void)CloseHandle(root_handle);
    free(root_wide);
    return TP_STATUS_OK;
}

#endif /* _WIN32 */
