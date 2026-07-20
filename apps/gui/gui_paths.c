#include "gui_paths.h"

/* libc first (before the tp_core/vendored headers) -- matches gui_pack.c; on macOS clang a header
 * pulling <stdio.h> first otherwise leaves snprintf undeclared here (implicit-decl = hard error). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include "nt_utf8_argv.h"
#else
#include <unistd.h>
#endif

#include "tp_core/tp_scan.h" /* tp_mkdirs (shared dir-creation) + tp_scan_is_dir (verify) */
#include "tp_core/tp_utf8.h"

static void clear_output(char *out, size_t out_size) {
    if (out && out_size > 0U) {
        out[0] = '\0';
    }
}

bool gui_paths_copy_normalized(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size == 0U ||
        !tp_utf8_is_valid_c_string(input)) {
        clear_output(out, out_size);
        return false;
    }
    const size_t length = strlen(input);
    if (length >= out_size) {
        clear_output(out, out_size);
        return false;
    }
    memmove(out, input, length + 1U);
    for (size_t i = 0U; i < length; ++i) {
        if (out[i] == '\\') {
            out[i] = '/';
        }
    }
    return true;
}

bool gui_paths_project_file(const char *input, char *out, size_t out_size) {
    static const char extension[] = ".ntpacker_project";
    if (!input || !out || out_size == 0U ||
        !tp_utf8_is_valid_c_string(input)) {
        clear_output(out, out_size);
        return false;
    }
    const char *slash = strrchr(input, '/');
    const char *backslash = strrchr(input, '\\');
    const char *base = !slash ? backslash
                       : (!backslash || slash > backslash) ? slash
                                                            : backslash;
    base = base ? base + 1 : input;
    const bool needs_extension = strrchr(base, '.') == NULL; /* boundary-ok: project filename, not sprite export-key normalization */
    const size_t input_length = strlen(input);
    const size_t extension_length = needs_extension ? sizeof extension - 1U : 0U;
    if (input_length > SIZE_MAX - extension_length ||
        input_length + extension_length >= out_size) {
        clear_output(out, out_size);
        return false;
    }
    memmove(out, input, input_length);
    if (extension_length > 0U) {
        memcpy(out + input_length, extension, extension_length);
    }
    out[input_length + extension_length] = '\0';
    return true;
}

bool gui_paths_relativize_to_project(const char *input,
                                     const char *project_file, char *out,
                                     size_t out_size) {
    char normalized[GUI_PATHS_MAX];
    if (!gui_paths_copy_normalized(input, normalized, sizeof normalized)) {
        clear_output(out, out_size);
        return false;
    }

    char project[GUI_PATHS_MAX];
    const bool have_project =
        project_file && project_file[0] != '\0' &&
        gui_paths_copy_normalized(project_file, project, sizeof project);
    const char *result = normalized;
    if (have_project) {
        char *last = strrchr(project, '/');
        if (last) {
            *last = '\0';
            const size_t directory_length = strlen(project);
            if (directory_length > 0U &&
                strncmp(normalized, project, directory_length) == 0 &&
                normalized[directory_length] == '/') {
                result = normalized + directory_length + 1U;
            }
        }
    }
    return gui_paths_copy_normalized(result, out, out_size);
}

// #region public API
void gui_paths_exe_dir(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
#ifdef _WIN32
    char exe[GUI_PATHS_MAX];
    char error[160] = {0};
    if (nt_win_module_path_utf8(exe, sizeof exe, error, sizeof error)) {
        char *slash = strrchr(exe, '\\');
        char *forward = strrchr(exe, '/');
        if (!slash || (forward && forward > slash)) {
            slash = forward;
        }
        if (slash != NULL) {
            *slash = '\0';
            const int copied = snprintf(out, out_size, "%s", exe);
            if (copied >= 0 && (size_t)copied < out_size) {
                return;
            }
        }
    }
#else
    /* Absolute CWD, not "." -- a relative base made the headless selftest write scratch to the CWD
     * while source paths resolved elsewhere, so sources looked "missing" on Linux CI. */
    if (getcwd(out, out_size) != NULL) {
        return;
    }
#endif
    (void)snprintf(out, out_size, ".");
}

bool gui_paths_app_data_root(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return false;
    }
    int n;
#ifdef _WIN32
    char base[GUI_PATHS_MAX];
    char error[160] = {0};
    bool found = false;
    if (!nt_win_environment_utf8(L"LOCALAPPDATA", base, sizeof base, &found,
                                 error, sizeof error)) {
        return false;
    }
    if (found && base[0] != '\0') {
        n = snprintf(out, out_size, "%s\\ntpacker", base);
        return n >= 0 && (size_t)n < out_size;
    }
#else
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        n = snprintf(out, out_size, "%s/ntpacker", xdg);
        return n >= 0 && (size_t)n < out_size;
    }
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        n = snprintf(out, out_size, "%s/.local/state/ntpacker", home);
        return n >= 0 && (size_t)n < out_size;
    }
#endif
    /* Neither env resolved -> park the data next to the exe so we still have a writable place. */
    char exe_dir[GUI_PATHS_MAX];
    gui_paths_exe_dir(exe_dir, sizeof exe_dir);
    n = snprintf(out, out_size, "%s/ntpacker-data", exe_dir);
    return n >= 0 && (size_t)n < out_size;
}

bool gui_paths_ensure_dir(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    tp_mkdirs(path);              /* tp_core owns mkdir -p (was copy-pasted GUI-side); best-effort */
    return tp_scan_is_dir(path); /* our bool result: did it actually end up a directory? */
}
// #endregion
