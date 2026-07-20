#ifndef NTPACKER_GUI_PATHS_H
#define NTPACKER_GUI_PATHS_H

/* App-data location + directory helpers, shared by the crash-diagnostics workstream (D1 log file,
 * D2 dump dir, D3 report prompt) and a future recovery-folder (R5). Deliberately general: it resolves
 * the ntpacker app-data ROOT; callers append their own subdir ("logs", "crash", ...) and ensure it.
 * Platform split (Win %LOCALAPPDATA% / POSIX XDG state dir) lives here so nothing else has to know it. */

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest path this module builds/accepts (root + a couple of appended components). */
#define GUI_PATHS_MAX TP_IDENTITY_PATH_MAX

/* Writes the ntpacker app-data root (NO trailing slash) into out[out_size]; returns true on success,
 * false if it does not fit (truncation) or out is invalid.
 *   Windows: %LOCALAPPDATA%\ntpacker
 *   POSIX:   $XDG_STATE_HOME/ntpacker  else  $HOME/.local/state/ntpacker
 *   fallback (neither env resolves): "<exe-dir>/ntpacker-data"
 * Does NOT create the directory -- call gui_paths_ensure_dir on the final path you build. */
bool gui_paths_app_data_root(char *out, size_t out_size);

/* mkdir -p for `path` (delegates to tp_core's tp_mkdirs -- the single owner of dir creation both
 * frontends share), then verifies it now exists as a directory. Returns true if so. */
bool gui_paths_ensure_dir(const char *path);

/* Checked, atomic path shaping for dialog results. All functions reject
 * malformed UTF-8 and insufficient output capacity without returning a
 * truncated path. */
bool gui_paths_copy_normalized(const char *input, char *out, size_t out_size);
bool gui_paths_project_file(const char *input, char *out, size_t out_size);
bool gui_paths_relativize_to_project(const char *input,
                                     const char *project_file, char *out,
                                     size_t out_size);

/* Resolve the directory the running exe lives in (absolute) into out[out_size]. Windows: the module
 * path's directory; POSIX: the CWD -- an ABSOLUTE base, never a bare "." (a relative base made the
 * headless selftest write scratch to the wrong dir on CI); "." only as a last resort. The single home
 * for this: main.c's s_exe_dir resolves through here. */
void gui_paths_exe_dir(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PATHS_H */
