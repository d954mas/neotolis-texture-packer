#include "app_paths.h"

#include <stdio.h>
#include <stdlib.h>

#include "tp_core/tp_format.h"
#include "tp_core/tp_identity.h"

#ifdef _WIN32
#include "nt_utf8_argv.h"
#endif

bool app_paths_data_root(char *out, size_t capacity, bool allow_exe_fallback) {
    if (!out || capacity == 0U) {
        return false;
    }
    out[0] = '\0';
    const char *base = NULL;
    const char *suffix = NULL;
#ifdef _WIN32
    char environment[TP_IDENTITY_PATH_MAX];
    char error[160] = {0};
    bool found = false;
    if (!nt_win_environment_utf8(L"LOCALAPPDATA", environment,
                                  sizeof environment, &found, error,
                                  sizeof error)) {
        return false;
    }
    if (found && environment[0] != '\0') {
        base = environment;
        suffix = "\\ntpacker";
    }
#else
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0] != '\0') {
        base = xdg;
        suffix = "/ntpacker";
    } else if (home && home[0] != '\0') {
        base = home;
        suffix = "/.local/state/ntpacker";
    }
#endif
    char executable[TP_IDENTITY_PATH_MAX];
    if (!base) {
        if (!allow_exe_fallback ||
            tp_executable_directory(executable, sizeof executable, NULL) != TP_STATUS_OK) {
            return false;
        }
        base = executable;
        suffix = "/ntpacker-data";
    }
    const int length = snprintf(out, capacity, "%s%s", base, suffix);
    if (length < 0 || (size_t)length >= capacity) {
        out[0] = '\0';
        return false;
    }
    return true;
}

tp_id128 app_recovery_key(void) {
    /* Incompatible journal contracts require an explicit key-version change. */
    const tp_id128 key = {{'n', 't', 'p', 'k', '_', 'r', 'e', 'c',
                           'o', 'v', 'e', 'r', 'y', '_', '0', '1'}};
    return key;
}
