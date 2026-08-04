#include "tp_core/tp_format.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "tp_core/tp_identity.h"
#include "tp_fs_internal.h"

#define TP_RUNTIME_PATH_CAP (TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U)
#define TP_WINDOWS_MODULE_PATH_CAP 32768U

static void clear_output(char *out, size_t out_cap) {
    if (out && out_cap > 0U) {
        out[0] = '\0';
    }
}
static tp_status module_path_utf8(char raw[TP_RUNTIME_PATH_CAP],
                                  tp_error *err) {
    DWORD capacity = 512U;
    for (;;) {
        wchar_t *wide =
            (wchar_t *)malloc((size_t)capacity * sizeof *wide);
        if (!wide) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "executable image path allocation failed");
        }

        SetLastError(ERROR_SUCCESS);
        const DWORD copied = GetModuleFileNameW(NULL, wide, capacity);
        if (copied == 0U) {
            const DWORD native_error = GetLastError();
            free(wide);
            return tp_error_set(
                err, TP_STATUS_PATH_RESOLVE_FAILED,
                "GetModuleFileNameW failed (%lu)",
                (unsigned long)native_error);
        }
        /* The conservative capacity-1 check also handles the historical
         * truncation behavior without accepting an ambiguous final code unit. */
        if (copied < capacity - 1U) {
            SetLastError(ERROR_SUCCESS);
            const int required = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
            if (required <= 0) {
                const DWORD native_error = GetLastError();
                free(wide);
                return tp_error_set(
                    err, TP_STATUS_INVALID_UTF8,
                    "executable image path is not valid UTF-16 (%lu)",
                    (unsigned long)native_error);
            }
            if ((size_t)required > TP_RUNTIME_PATH_CAP) {
                free(wide);
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "executable image path exceeds %u bytes",
                                    TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES);
            }
            if (!tp_fs_win32_utf16_to_utf8(wide, raw,
                                           TP_RUNTIME_PATH_CAP)) {
                free(wide);
                return tp_error_set(
                    err, TP_STATUS_PATH_RESOLVE_FAILED,
                    "executable image path UTF-8 conversion failed");
            }
            free(wide);
            return TP_STATUS_OK;
        }

        free(wide);
        if (capacity >= TP_WINDOWS_MODULE_PATH_CAP) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "Windows executable image path is too long");
        }
        capacity *= 2U;
        if (capacity > TP_WINDOWS_MODULE_PATH_CAP) {
            capacity = TP_WINDOWS_MODULE_PATH_CAP;
        }
    }
}

tp_status tp_executable_path(char *out, size_t out_cap, tp_error *err) {
    if (!out || out_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "executable path requires a non-empty output buffer");
    }
    out[0] = '\0';

    char raw[TP_RUNTIME_PATH_CAP];
    const tp_status read_status = module_path_utf8(raw, err);
    if (read_status != TP_STATUS_OK) {
        return read_status;
    }

    /* Resolve the loaded module through an opened handle so junction/symlink
     * launch paths converge on the real executable image. The canonicalizer
     * also converts separators and enforces strict UTF-8. */
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
    if (directory_length == 2U && executable[1] == ':') {
        directory_length = 3U; /* executable directly below a drive root */
    }
    if (directory_length >= out_cap) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "executable directory exceeds output capacity");
    }
    memcpy(out, executable, directory_length);
    out[directory_length] = '\0';
    return TP_STATUS_OK;
}
