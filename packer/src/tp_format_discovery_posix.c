#include "tp_format_discovery_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_utf8.h"
#include "tp_format_descriptor_internal.h"
#include "tp_hex.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static char *discovery_strdup(const char *text) {
    const size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1U);
    if (copy) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

static void set_failure(tp_format_discovery_failure *failure,
                        tp_format_diagnostic_code code,
                        const char *message) {
    if (!failure) {
        return;
    }
    failure->code = code;
    (void)snprintf(failure->message, sizeof failure->message, "%s",
                   message ? message : "format discovery failed");
    tp_error_trim_partial_utf8(failure->message);
}

static void set_candidate_fault(tp_format_discovered_candidate *candidate,
                                tp_format_diagnostic_code code,
                                const char *message) {
    candidate->fault_code = code;
    (void)snprintf(candidate->fault_message,
                   sizeof candidate->fault_message, "%s",
                   message ? message : "format package is unavailable");
    tp_error_trim_partial_utf8(candidate->fault_message);
}

static void invalid_name_key(const char *native_name, char out[52]) {
    const tp_id128 id = tp_hash128(native_name, strlen(native_name));
    memcpy(out, "invalid-name-posix-", 19U);
    tp_hex_encode_lower(id.bytes, sizeof id.bytes, out + 19U);
}

static char *logical_package_path(const char *name) {
    static const char prefix[] = "formats/";
    const size_t name_length = strlen(name);
    const size_t prefix_length = sizeof prefix - 1U;
    char *path = (char *)malloc(prefix_length + name_length + 1U);
    if (!path) {
        return NULL;
    }
    memcpy(path, prefix, prefix_length);
    memcpy(path + prefix_length, name, name_length + 1U);
    return path;
}

static tp_status candidate_initialize(
    const char *name, bool portable_name,
    tp_format_discovered_candidate *candidate, tp_error *error) {
    memset(candidate, 0, sizeof *candidate);
    if (portable_name) {
        candidate->key = discovery_strdup(name);
        candidate->package_path = logical_package_path(name);
    } else {
        char key[52];
        invalid_name_key(name, key);
        candidate->key = discovery_strdup(key);
    }
    if (!candidate->key || (portable_name && !candidate->package_path)) {
        tp_format_discovered_candidate_destroy(candidate);
        return tp_error_set(error, TP_STATUS_OOM,
                            "format candidate name allocation failed");
    }
    return TP_STATUS_OK;
}

static int duplicate_cloexec(int descriptor) {
    int copy;
    do {
        copy = dup(descriptor);
    } while (copy < 0 && errno == EINTR);
    if (copy >= 0) {
        (void)fcntl(copy, F_SETFD, FD_CLOEXEC);
    }
    return copy;
}

static tp_format_diagnostic_code read_package_file(
    int package_fd, const char *name, size_t byte_limit,
    unsigned char **bytes_out, size_t *size_out, char *message,
    size_t message_capacity, bool *out_oom) {
    *bytes_out = NULL;
    *size_out = 0U;
    *out_oom = false;
    int descriptor;
    do {
        descriptor = openat(package_fd, name,
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        const int native_error = errno;
        if (native_error == ENOMEM) {
            *out_oom = true;
        }
        tp_format_diagnostic_code code =
            native_error == ENOENT
                ? (strcmp(name, "format.json") == 0
                       ? TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING
                       : TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING)
                : native_error == ELOOP
                      ? TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE
                      : TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED;
        if (code == TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED &&
            !*out_oom) {
            struct stat rejected_info;
            if (fstatat(package_fd, name, &rejected_info,
                        AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISLNK(rejected_info.st_mode)) {
                    code = TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE;
                } else if (!S_ISREG(rejected_info.st_mode)) {
                    code = TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE;
                }
            } else if (errno == ENOMEM) {
                *out_oom = true;
            }
        }
        (void)snprintf(message, message_capacity,
                       "cannot open %s through the package handle: %s", name,
                       strerror(native_error));
        return code;
    }

    struct stat info;
    if (fstat(descriptor, &info) != 0) {
        const int native_error = errno;
        if (native_error == ENOMEM) {
            *out_oom = true;
        }
        (void)snprintf(message, message_capacity,
                       "cannot inspect opened %s: %s", name,
                       strerror(native_error));
        (void)close(descriptor);
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED;
    }
    if (!S_ISREG(info.st_mode)) {
        (void)snprintf(message, message_capacity,
                       "%s is not a real regular file", name);
        (void)close(descriptor);
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE;
    }
    if (info.st_nlink != 1) {
        (void)snprintf(message, message_capacity,
                       "%s is hard-linked outside the package identity",
                       name);
        (void)close(descriptor);
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE;
    }
    if (info.st_size < 0 || (uintmax_t)info.st_size > byte_limit) {
        (void)snprintf(message, message_capacity,
                       "%s exceeds its %zu-byte limit", name, byte_limit);
        (void)close(descriptor);
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TOO_LARGE;
    }
    const size_t expected = (size_t)info.st_size;
    unsigned char *bytes = (unsigned char *)malloc(expected + 1U);
    if (!bytes) {
        *out_oom = true;
        (void)snprintf(message, message_capacity,
                       "allocation failed while reading %s", name);
        (void)close(descriptor);
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED;
    }
    size_t offset = 0U;
    while (offset < expected) {
        ssize_t got;
        do {
            got = read(descriptor, bytes + offset, expected - offset);
        } while (got < 0 && errno == EINTR);
        if (got <= 0) {
            const int native_error = got < 0 ? errno : 0;
            if (native_error == ENOMEM) {
                *out_oom = true;
            }
            if (got < 0) {
                (void)snprintf(message, message_capacity,
                               "cannot read opened %s: %s", name,
                               strerror(native_error));
            } else {
                (void)snprintf(
                    message, message_capacity,
                    "opened %s changed during its bounded read", name);
            }
            free(bytes);
            (void)close(descriptor);
            return TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED;
        }
        offset += (size_t)got;
    }
    unsigned char extra = 0U;
    ssize_t extra_count;
    do {
        extra_count = read(descriptor, &extra, 1U);
    } while (extra_count < 0 && errno == EINTR);
    if (extra_count != 0) {
        const int native_error = extra_count < 0 ? errno : 0;
        if (native_error == ENOMEM) {
            *out_oom = true;
        }
        if (extra_count < 0) {
            (void)snprintf(message, message_capacity,
                           "cannot finish reading opened %s: %s", name,
                           strerror(native_error));
        } else {
            (void)snprintf(message, message_capacity,
                           "opened %s grew during its bounded read", name);
        }
        free(bytes);
        (void)close(descriptor);
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED;
    }
    bytes[expected] = 0U;
    (void)close(descriptor);
    *bytes_out = bytes;
    *size_out = expected;
    return (tp_format_diagnostic_code)0;
}

static tp_format_diagnostic_code enumerate_package(
    int package_fd, char *message, size_t message_capacity,
    bool *out_oom) {
    *out_oom = false;
    int directory_copy = duplicate_cloexec(package_fd);
    if (directory_copy < 0) {
        *out_oom = errno == ENOMEM;
        (void)snprintf(message, message_capacity,
                       "cannot duplicate the package directory handle: %s",
                       strerror(errno));
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED;
    }
    DIR *directory = fdopendir(directory_copy);
    if (!directory) {
        const int native_error = errno;
        *out_oom = native_error == ENOMEM;
        (void)close(directory_copy);
        (void)snprintf(message, message_capacity,
                       "cannot enumerate the package directory: %s",
                       strerror(native_error));
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED;
    }
    bool saw_descriptor = false;
    bool saw_source = false;
    int enumeration_error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            enumeration_error = errno;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (strcmp(entry->d_name, "format.json") == 0) {
            saw_descriptor = true;
        } else if (strcmp(entry->d_name, "export.lua") == 0) {
            saw_source = true;
        } else {
            (void)snprintf(message, message_capacity,
                           "format package contains an extra entry");
            (void)closedir(directory);
            return TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY;
        }
    }
    (void)closedir(directory);
    if (enumeration_error != 0) {
        *out_oom = enumeration_error == ENOMEM;
        (void)snprintf(message, message_capacity,
                       "cannot finish package enumeration: %s",
                       strerror(enumeration_error));
        return TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED;
    }
    if (!saw_descriptor) {
        (void)snprintf(message, message_capacity,
                       "format package is missing format.json");
        return TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING;
    }
    if (!saw_source) {
        (void)snprintf(message, message_capacity,
                       "format package is missing export.lua");
        return TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING;
    }
    return (tp_format_diagnostic_code)0;
}

static tp_status read_candidate(int root_fd, const char *native_name,
                                tp_format_discovered_candidate *candidate,
                                tp_error *error) {
    int package_fd;
    do {
        package_fd = openat(root_fd, native_name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (package_fd < 0 && errno == EINTR);
    if (package_fd < 0) {
        const int native_error = errno;
        if (native_error == ENOMEM) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "format package handle allocation failed");
        }
        set_candidate_fault(
            candidate,
            native_error == ELOOP ? TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE
                                  : TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
            native_error == ELOOP
                ? "format package directory is a symlink"
                : "format package directory could not be opened");
        return TP_STATUS_OK;
    }
    struct stat info;
    if (fstat(package_fd, &info) != 0) {
        const int native_error = errno;
        (void)close(package_fd);
        if (native_error == ENOMEM) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "format package identity allocation failed");
        }
        set_candidate_fault(candidate,
                            TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
                            "opened format package could not be inspected");
        return TP_STATUS_OK;
    }
    if (!S_ISDIR(info.st_mode)) {
        set_candidate_fault(candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE,
                            "opened format package is not a real directory");
        (void)close(package_fd);
        return TP_STATUS_OK;
    }

    char message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U] = {0};
    bool oom = false;
    tp_format_diagnostic_code code =
        enumerate_package(package_fd, message, sizeof message, &oom);
    if (oom) {
        (void)close(package_fd);
        return tp_error_set(error, TP_STATUS_OOM,
                            "format package enumeration ran out of memory");
    }
    if (code == TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING) {
        candidate->fault_file = TP_FORMAT_DISCOVERY_FAULT_DESCRIPTOR;
    } else if (code == TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING) {
        candidate->fault_file = TP_FORMAT_DISCOVERY_FAULT_SOURCE;
    }
    if (code == 0) {
        candidate->fault_file = TP_FORMAT_DISCOVERY_FAULT_DESCRIPTOR;
        code = read_package_file(
            package_fd, "format.json", TP_FORMAT_DESCRIPTOR_MAX_BYTES,
            &candidate->descriptor_bytes,
            &candidate->descriptor_byte_count, message, sizeof message,
            &oom);
        if (oom) {
            (void)close(package_fd);
            return tp_error_set(error, TP_STATUS_OOM,
                                "format descriptor snapshot allocation failed");
        }
    }
    if (code == 0) {
        candidate->fault_file = TP_FORMAT_DISCOVERY_FAULT_SOURCE;
        code = read_package_file(
            package_fd, "export.lua", TP_FORMAT_SOURCE_MAX_BYTES,
            &candidate->source_bytes, &candidate->source_byte_count, message,
            sizeof message, &oom);
        if (oom) {
            free(candidate->descriptor_bytes);
            candidate->descriptor_bytes = NULL;
            candidate->descriptor_byte_count = 0U;
            (void)close(package_fd);
            return tp_error_set(error, TP_STATUS_OOM,
                                "format source snapshot allocation failed");
        }
    }
    (void)close(package_fd);
    if (code != 0) {
        free(candidate->descriptor_bytes);
        candidate->descriptor_bytes = NULL;
        candidate->descriptor_byte_count = 0U;
        free(candidate->source_bytes);
        candidate->source_bytes = NULL;
        candidate->source_byte_count = 0U;
        set_candidate_fault(candidate, code, message);
    }
    return TP_STATUS_OK;
}

tp_status tp_format_discovery_read_root(
    const char *root, tp_format_discovery_candidate_visitor visit_candidate,
    void *visit_context, tp_format_discovery_result *out,
    tp_format_discovery_failure *failure, tp_error *error) {
    if (!root || !visit_candidate || !out || !failure || root[0] != '/' ||
        !tp_utf8_is_valid_c_string(root)) {
        return tp_error_set(
            error, TP_STATUS_INVALID_ARGUMENT,
            "format root and visitor require an absolute strict-UTF-8 path");
    }
    memset(out, 0, sizeof *out);
    memset(failure, 0, sizeof *failure);
    out->root = discovery_strdup(root);
    if (!out->root) {
        set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                    "format root allocation failed");
        return tp_error_set(error, TP_STATUS_OOM,
                            "format root allocation failed");
    }

    struct stat path_info;
    if (lstat(root, &path_info) != 0) {
        const int native_error = errno;
        if (native_error == ENOENT) {
            out->root_missing = true;
            return TP_STATUS_OK;
        }
        char message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
        (void)snprintf(message, sizeof message,
                       "cannot inspect format root without following it: %s",
                       strerror(native_error));
        set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO, message);
        tp_format_discovery_result_destroy(out);
        return tp_error_set(
            error,
            native_error == ENOMEM ? TP_STATUS_OOM
                                   : TP_STATUS_PATH_RESOLVE_FAILED,
            "%s", message);
    }
    if (S_ISLNK(path_info.st_mode) || !S_ISDIR(path_info.st_mode)) {
        const tp_format_diagnostic_code code =
            S_ISLNK(path_info.st_mode)
                ? TP_FORMAT_DIAGNOSTIC_ROOT_REPARSE
                : TP_FORMAT_DIAGNOSTIC_ROOT_NOT_DIRECTORY;
        const char *message = S_ISLNK(path_info.st_mode)
                                  ? "format root is a symlink"
                                  : "format root is not a directory";
        set_failure(failure, code, message);
        tp_format_discovery_result_destroy(out);
        return tp_error_set(error, TP_STATUS_PATH_RESOLVE_FAILED, "%s",
                            message);
    }

    int root_fd;
    do {
        root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (root_fd < 0 && errno == EINTR);
    if (root_fd < 0) {
        const int native_error = errno;
        const tp_format_diagnostic_code code =
            native_error == ELOOP ? TP_FORMAT_DIAGNOSTIC_ROOT_REPARSE
                                  : native_error == ENOTDIR
                                        ? TP_FORMAT_DIAGNOSTIC_ROOT_NOT_DIRECTORY
                                        : TP_FORMAT_DIAGNOSTIC_ROOT_IO;
        char message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
        (void)snprintf(message, sizeof message,
                       "cannot open format root through a no-follow handle: %s",
                       strerror(native_error));
        set_failure(failure, code, message);
        tp_format_discovery_result_destroy(out);
        return tp_error_set(
            error,
            native_error == ENOMEM ? TP_STATUS_OOM
                                   : TP_STATUS_PATH_RESOLVE_FAILED,
            "%s", message);
    }
    struct stat root_info;
    if (fstat(root_fd, &root_info) != 0) {
        const int native_error = errno;
        (void)close(root_fd);
        set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                    "opened format root could not be inspected");
        tp_format_discovery_result_destroy(out);
        return tp_error_set(
            error,
            native_error == ENOMEM ? TP_STATUS_OOM
                                   : TP_STATUS_PATH_RESOLVE_FAILED,
            "opened format root could not be inspected (%d)", native_error);
    }
    if (!S_ISDIR(root_info.st_mode)) {
        (void)close(root_fd);
        set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_NOT_DIRECTORY,
                    "opened format root is not a real directory");
        tp_format_discovery_result_destroy(out);
        return tp_error_set(error, TP_STATUS_PATH_RESOLVE_FAILED,
                            "opened format root is not a real directory");
    }

    int directory_copy = duplicate_cloexec(root_fd);
    DIR *directory = directory_copy >= 0 ? fdopendir(directory_copy) : NULL;
    if (!directory) {
        const int native_error = errno;
        if (directory_copy >= 0) {
            (void)close(directory_copy);
        }
        (void)close(root_fd);
        set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                    "format root enumeration could not start");
        tp_format_discovery_result_destroy(out);
        return tp_error_set(
            error,
            native_error == ENOMEM ? TP_STATUS_OOM
                                   : TP_STATUS_PATH_RESOLVE_FAILED,
            "format root enumeration could not start: %s",
            strerror(native_error));
    }

    size_t entries_examined = 0U;
    int enumeration_error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            enumeration_error = errno;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ++entries_examined;
        if (entries_examined > TP_FORMAT_ROOT_ENTRY_MAX) {
            out->limit_fail_closed = true;
            break;
        }

        struct stat entry_info;
        if (fstatat(root_fd, entry->d_name, &entry_info,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            const int native_error = errno;
            (void)closedir(directory);
            (void)close(root_fd);
            set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                        "format root entry identity changed during enumeration");
            tp_format_discovery_result_destroy(out);
            return tp_error_set(
                error, native_error == ENOMEM
                           ? TP_STATUS_OOM
                           : TP_STATUS_PATH_RESOLVE_FAILED,
                "cannot inspect a format root entry: %s",
                strerror(native_error));
        }
        if (!S_ISDIR(entry_info.st_mode) && !S_ISLNK(entry_info.st_mode)) {
            continue;
        }
        if (out->candidate_count >= TP_FORMAT_PACKAGE_MAX) {
            out->limit_fail_closed = true;
            break;
        }
        const bool portable_name =
            tp_format_package_name_is_portable(entry->d_name);
        tp_format_discovered_candidate candidate;
        tp_status status = candidate_initialize(
            entry->d_name, portable_name, &candidate, error);
        if (status != TP_STATUS_OK) {
            (void)closedir(directory);
            (void)close(root_fd);
            set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                        "format candidate materialization failed");
            tp_format_discovery_result_destroy(out);
            return status;
        }
        out->candidate_count++;
        if (!portable_name) {
            set_candidate_fault(
                &candidate, TP_FORMAT_DIAGNOSTIC_PACKAGE_NAME_INVALID,
                "format package directory name is not portable API-v1 UTF-8");
        } else if (S_ISLNK(entry_info.st_mode)) {
            set_candidate_fault(&candidate,
                                TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE,
                                "format package directory is a symlink");
        } else {
            status = read_candidate(root_fd, entry->d_name, &candidate, error);
            if (status != TP_STATUS_OK) {
                tp_format_discovered_candidate_destroy(&candidate);
                (void)closedir(directory);
                (void)close(root_fd);
                set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                            "format candidate read failed");
                tp_format_discovery_result_destroy(out);
                return status;
            }
        }
        const tp_format_discovery_visit_result visit =
            visit_candidate(visit_context, &candidate, error);
        bool stop_success = false;
        status = tp_format_discovery_visit_resolve(
            visit, &stop_success, error);
        tp_format_discovered_candidate_destroy(&candidate);
        if (status != TP_STATUS_OK) {
            (void)closedir(directory);
            (void)close(root_fd);
            tp_format_discovery_result_destroy(out);
            return status;
        }
        if (stop_success) {
            break;
        }
    }
    (void)closedir(directory);
    (void)close(root_fd);
    if (!out->limit_fail_closed && enumeration_error != 0) {
        set_failure(failure, TP_FORMAT_DIAGNOSTIC_ROOT_IO,
                    "format root enumeration failed");
        tp_format_discovery_result_destroy(out);
        return tp_error_set(
            error,
            enumeration_error == ENOMEM ? TP_STATUS_OOM
                                        : TP_STATUS_PATH_RESOLVE_FAILED,
            "format root enumeration failed: %s",
            strerror(enumeration_error));
    }
    return TP_STATUS_OK;
}
