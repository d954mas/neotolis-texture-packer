#include "nt_utf8_argv.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t capacity, const char *message) {
    if (error && capacity > 0U) {
        (void)snprintf(error, capacity, "%s", message ? message : "");
    }
}

bool nt_win_utf16_to_utf8(const wchar_t *wide, char *out,
                          size_t output_capacity, char *error,
                          size_t error_capacity) {
    if (!wide || !out || output_capacity == 0U ||
        output_capacity > (size_t)INT_MAX) {
        set_error(error, error_capacity, "invalid UTF-16 conversion output");
        return false;
    }
    out[0] = '\0';
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        set_error(error, error_capacity, "Windows path contains invalid UTF-16");
        return false;
    }
    if ((size_t)needed > output_capacity) {
        set_error(error, error_capacity, "Windows UTF-8 path exceeds output capacity");
        return false;
    }
    const int converted = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, needed, NULL, NULL);
    if (converted != needed) {
        out[0] = '\0';
        set_error(error, error_capacity, "Windows path conversion changed unexpectedly");
        return false;
    }
    return true;
}

static bool convert_and_free(wchar_t *wide, char *out,
                             size_t output_capacity, char *error,
                             size_t error_capacity) {
    if (!wide) {
        if (out && output_capacity > 0U) {
            out[0] = '\0';
        }
        return false;
    }
    const bool ok = nt_win_utf16_to_utf8(
        wide, out, output_capacity, error, error_capacity);
    free(wide);
    return ok;
}

bool nt_win_current_directory_utf8(char *out, size_t output_capacity,
                                   char *error, size_t error_capacity) {
    if (!out || output_capacity == 0U) {
        set_error(error, error_capacity, "invalid current-directory output");
        return false;
    }
    out[0] = '\0';
    const DWORD needed = GetCurrentDirectoryW(0U, NULL);
    if (needed == 0U || (size_t)needed > SIZE_MAX / sizeof(wchar_t)) {
        set_error(error, error_capacity, "Windows could not query the current directory");
        return false;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)needed * sizeof *wide);
    if (!wide) {
        set_error(error, error_capacity, "out of memory reading the current directory");
        return false;
    }
    const DWORD copied = GetCurrentDirectoryW(needed, wide);
    if (copied == 0U || copied >= needed) {
        free(wide);
        set_error(error, error_capacity, "Windows current directory changed while reading");
        return false;
    }
    return convert_and_free(wide, out, output_capacity, error, error_capacity);
}

bool nt_win_temp_path_utf8(char *out, size_t output_capacity, char *error,
                           size_t error_capacity) {
    if (!out || output_capacity == 0U) {
        set_error(error, error_capacity, "invalid temporary-path output");
        return false;
    }
    out[0] = '\0';
    const DWORD needed = GetTempPathW(0U, NULL);
    if (needed == 0U || (size_t)needed > SIZE_MAX / sizeof(wchar_t)) {
        set_error(error, error_capacity, "Windows could not query the temporary directory");
        return false;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)needed * sizeof *wide);
    if (!wide) {
        set_error(error, error_capacity, "out of memory reading the temporary directory");
        return false;
    }
    const DWORD copied = GetTempPathW(needed, wide);
    if (copied == 0U || copied >= needed) {
        free(wide);
        set_error(error, error_capacity, "Windows temporary directory changed while reading");
        return false;
    }
    return convert_and_free(wide, out, output_capacity, error, error_capacity);
}

bool nt_win_module_path_utf8(char *out, size_t output_capacity, char *error,
                             size_t error_capacity) {
    if (!out || output_capacity == 0U) {
        set_error(error, error_capacity, "invalid module-path output");
        return false;
    }
    out[0] = '\0';
    DWORD capacity = 512U;
    for (;;) {
        wchar_t *wide = (wchar_t *)malloc((size_t)capacity * sizeof *wide);
        if (!wide) {
            set_error(error, error_capacity, "out of memory reading the executable path");
            return false;
        }
        SetLastError(ERROR_SUCCESS);
        const DWORD copied = GetModuleFileNameW(NULL, wide, capacity);
        if (copied == 0U) {
            free(wide);
            set_error(error, error_capacity, "Windows could not read the executable path");
            return false;
        }
        if (copied < capacity - 1U) {
            return convert_and_free(wide, out, output_capacity, error,
                                    error_capacity);
        }
        free(wide);
        if (capacity >= 32768U) {
            set_error(error, error_capacity, "Windows executable path exceeds the supported limit");
            return false;
        }
        capacity *= 2U;
        if (capacity > 32768U) {
            capacity = 32768U;
        }
    }
}

bool nt_win_environment_utf8(const wchar_t *name, char *out,
                             size_t output_capacity, bool *found, char *error,
                             size_t error_capacity) {
    if (!name || !out || output_capacity == 0U || !found) {
        set_error(error, error_capacity, "invalid Windows environment request");
        return false;
    }
    out[0] = '\0';
    *found = false;
    SetLastError(ERROR_SUCCESS);
    const DWORD needed = GetEnvironmentVariableW(name, NULL, 0U);
    if (needed == 0U) {
        const DWORD query_error = GetLastError();
        if (query_error == ERROR_ENVVAR_NOT_FOUND) {
            return true;
        }
        if (query_error == ERROR_SUCCESS) {
            *found = true; /* present with an empty value */
            return true;
        }
        set_error(error, error_capacity, "Windows could not query an environment path");
        return false;
    }
    if ((size_t)needed > SIZE_MAX / sizeof(wchar_t)) {
        set_error(error, error_capacity, "Windows environment path is too large");
        return false;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)needed * sizeof *wide);
    if (!wide) {
        set_error(error, error_capacity, "out of memory reading an environment path");
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const DWORD copied = GetEnvironmentVariableW(name, wide, needed);
    if (copied == 0U) {
        const DWORD read_error = GetLastError();
        free(wide);
        if (read_error == ERROR_SUCCESS) {
            *found = true; /* present with an empty value */
            return true;
        }
        set_error(error, error_capacity, "Windows environment changed while reading");
        return false;
    }
    if (copied >= needed) {
        free(wide);
        set_error(error, error_capacity, "Windows environment changed while reading");
        return false;
    }
    *found = true;
    if (!convert_and_free(wide, out, output_capacity, error,
                          error_capacity)) {
        *found = false;
        return false;
    }
    return true;
}

void nt_utf8_argv_dispose(nt_utf8_argv *args) {
    if (!args) {
        return;
    }
    if (args->argv) {
        for (int i = 0; i < args->argc; ++i) {
            free(args->argv[i]);
        }
        free(args->argv);
    }
    memset(args, 0, sizeof *args);
}

bool nt_utf8_argv_convert(int argc, wchar_t *const *wide_argv,
                          nt_utf8_argv *out, char *error,
                          size_t error_capacity) {
    if (!out || argc < 0 || (argc > 0 && !wide_argv)) {
        set_error(error, error_capacity, "invalid Windows argument vector");
        return false;
    }
    memset(out, 0, sizeof *out);
    if ((size_t)argc > (SIZE_MAX / sizeof(char *)) - 1U) {
        set_error(error, error_capacity, "Windows argument vector is too large");
        return false;
    }
    out->argv = (char **)calloc((size_t)argc + 1U, sizeof *out->argv);
    if (!out->argv) {
        set_error(error, error_capacity, "out of memory converting Windows arguments");
        return false;
    }
    out->argc = argc;
    for (int i = 0; i < argc; ++i) {
        if (!wide_argv[i]) {
            set_error(error, error_capacity, "Windows argument is NULL");
            nt_utf8_argv_dispose(out);
            return false;
        }
        const int bytes = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL, 0, NULL,
            NULL);
        if (bytes <= 0) {
            set_error(error, error_capacity,
                      "Windows command line contains invalid UTF-16");
            nt_utf8_argv_dispose(out);
            return false;
        }
        out->argv[i] = (char *)malloc((size_t)bytes);
        if (!out->argv[i]) {
            set_error(error, error_capacity,
                      "out of memory converting Windows arguments");
            nt_utf8_argv_dispose(out);
            return false;
        }
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i],
                                -1, out->argv[i], bytes, NULL, NULL) != bytes) {
            set_error(error, error_capacity,
                      "Windows argument conversion changed unexpectedly");
            nt_utf8_argv_dispose(out);
            return false;
        }
    }
    return true;
}

bool nt_utf8_argv_from_command_line(nt_utf8_argv *out, char *error,
                                    size_t error_capacity) {
    int argc = 0;
    wchar_t **wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide_argv) {
        set_error(error, error_capacity,
                  "Windows could not parse the process command line");
        return false;
    }
    const bool converted = nt_utf8_argv_convert(
        argc, wide_argv, out, error, error_capacity);
    (void)LocalFree(wide_argv);
    return converted;
}

#endif
