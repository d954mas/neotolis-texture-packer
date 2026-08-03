#include "tp_core/tp_format.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <unistd.h>

#include "tp_core/tp_identity.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define TP_RUNTIME_PATH_CAP (TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U)

static void clear_output(char *out, size_t out_cap) {
    if (out && out_cap > 0U) {
        out[0] = '\0';
    }
}
static tp_status executable_image_path(char raw[TP_RUNTIME_PATH_CAP],
                                       tp_error *err) {
#if defined(__APPLE__)
    uint32_t capacity = (uint32_t)TP_RUNTIME_PATH_CAP;
    if (_NSGetExecutablePath(raw, &capacity) != 0) {
        if (capacity > (uint32_t)TP_RUNTIME_PATH_CAP) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "executable image path exceeds %u bytes",
                                TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES);
        }
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "macOS could not resolve the executable image path");
    }
    return TP_STATUS_OK;
#else
    ssize_t length;
    do {
        length = readlink("/proc/self/exe", raw, TP_RUNTIME_PATH_CAP);
    } while (length < 0 && errno == EINTR);
    if (length < 0) {
        const int native_error = errno;
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "cannot resolve /proc/self/exe: %s",
                            strerror(native_error));
    }
    if (length == 0) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "/proc/self/exe resolved to an empty path");
    }
    if ((size_t)length >= TP_RUNTIME_PATH_CAP) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "executable image path exceeds %u bytes",
                            TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES);
    }
    raw[(size_t)length] = '\0';
    return TP_STATUS_OK;
#endif
}

tp_status tp_executable_path(char *out, size_t out_cap, tp_error *err) {
    if (!out || out_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "executable path requires a non-empty output buffer");
    }
    out[0] = '\0';

    char raw[TP_RUNTIME_PATH_CAP];
    const tp_status read_status = executable_image_path(raw, err);
    if (read_status != TP_STATUS_OK) {
        return read_status;
    }

    /* Canonicalization follows the executable image to its real target and
     * enforces the shared strict UTF-8/absolute-path policy. It deliberately
     * has no argv, PATH, or process-CWD fallback. */
    const tp_status status =
        tp_identity_path_canonical(raw, out, out_cap, err);
    if (status != TP_STATUS_OK) {
        clear_output(out, out_cap);
    }
    return status;
}

tp_status tp_executable_directory(char *out, size_t out_cap, tp_error *err) {
    if (!out || out_cap == 0U) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "executable directory requires a non-empty output buffer");
    }
    out[0] = '\0';

    char executable[TP_RUNTIME_PATH_CAP];
    tp_status status =
        tp_executable_path(executable, sizeof executable, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    const char *separator = strrchr(executable, '/');
    if (!separator) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "resolved executable path has no directory");
    }
    size_t directory_length = (size_t)(separator - executable);
    if (directory_length == 0U) {
        directory_length = 1U; /* executable directly below POSIX root */
    }
    if (directory_length >= out_cap) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "executable directory exceeds output capacity");
    }
    memcpy(out, executable, directory_length);
    out[directory_length] = '\0';
    return TP_STATUS_OK;
}
