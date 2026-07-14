#include "gui_paths.h"

#include "tp_core/tp_scan.h" /* tp_mkdirs (shared dir-creation) + tp_scan_is_dir (verify) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// #region public API
void gui_paths_exe_dir(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
#ifdef _WIN32
    char exe[GUI_PATHS_MAX];
    DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (n > 0U && n < (DWORD)sizeof exe) {
        char *slash = strrchr(exe, '\\');
        if (slash != NULL) {
            *slash = '\0';
            (void)snprintf(out, out_size, "%s", exe);
            return;
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
    const char *base = getenv("LOCALAPPDATA");
    if (base != NULL && base[0] != '\0') {
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
