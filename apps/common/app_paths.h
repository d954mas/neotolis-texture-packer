#ifndef NTPACKER_APP_PATHS_H
#define NTPACKER_APP_PATHS_H

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_id.h"

/* Resolve the shared persistent app-data root without creating it. GUI keeps
 * its executable-relative fallback; headless callers must pass false. */
bool app_paths_data_root(char *out, size_t capacity, bool allow_exe_fallback);

/* Stable recovery-sidecar key shared by GUI and headless hosts. */
tp_id128 app_recovery_key(void);

#endif
