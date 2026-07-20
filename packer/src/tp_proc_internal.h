#ifndef TP_PROC_INTERNAL_H
#define TP_PROC_INTERNAL_H

/* Minimal portable child-process transport for the private build worker
 * (decision 0018, ROADMAP H0.3-b). A parent spawns a child whose stdin is a
 * pipe the parent writes and whose stdout is a pipe the parent reads; stderr is
 * inherited from the parent. Only those two pipe ends are handed to the child.
 *
 * This is the minimal spawn+stream+wait+kill needed to run one bounded request
 * through the worker and read one bounded reply. Windows Job Object tree-kill
 * and cancel/timeout wiring are H0.4 -- clear TODO(H0.4) seams are noted in the
 * implementations (tp_proc_win32.c / tp_proc_posix.c). */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_proc tp_proc;

/* How a finished child terminated. EXITED means a normal process exit and
 * `code` is the exit status; ABNORMAL means it died on a signal / crashed /
 * could not be reaped and `code` is the signal number (POSIX) or a best-effort
 * native code (Windows). The two are deliberately distinct so the worker
 * boundary can tell "the builder returned a status" from "the process died". */
typedef enum tp_proc_end {
    TP_PROC_END_EXITED = 0,
    TP_PROC_END_ABNORMAL
} tp_proc_end;

typedef struct tp_proc_result {
    tp_proc_end how;
    int code;
} tp_proc_result;

/* Resolve the current executable's own path as UTF-8 (Windows
 * GetModuleFileNameW; Linux /proc/self/exe; macOS _NSGetExecutablePath). On
 * success writes a NUL-terminated path and returns true; on failure or if it
 * does not fit `out_cap`, sets out[0]='\0' and returns false. */
bool tp_proc_self_path(char *out, size_t out_cap);

/* Spawn `exe_utf8` with argv = { exe, arg1, NULL }. The child's stdin is wired
 * to a pipe the parent writes (tp_proc_write_stdin) and its stdout to a pipe the
 * parent reads (tp_proc_read_stdout); stderr is inherited. Returns NULL on
 * failure. */
tp_proc *tp_proc_spawn(const char *exe_utf8, const char *arg1);

/* Write all `size` bytes to the child's stdin, then close stdin so the child
 * observes EOF. Returns false on a short write / broken pipe (the child died
 * mid-request); stdin is closed either way. */
bool tp_proc_write_stdin(tp_proc *proc, const void *data, size_t size);

/* Read the child's stdout until EOF into `buf` (at most `cap` bytes). Stores the
 * byte count in *out_len and whether EOF was reached within `cap` in *out_eof
 * (false => the child produced more than `cap`, i.e. an oversized reply the
 * caller treats as malformed). Returns false only on a hard read error. */
bool tp_proc_read_stdout(tp_proc *proc, void *buf, size_t cap, size_t *out_len,
                         bool *out_eof);

/* Wait for the child to finish and report how it terminated. Returns false only
 * if the child could not be waited on at all. */
bool tp_proc_wait(tp_proc *proc, tp_proc_result *out);

/* Best-effort force-kill of a still-running child. TODO(H0.4): on Windows this
 * terminates only the direct child; a Job Object will tree-kill any grandchild
 * the builder ever spawns. NULL-safe. */
void tp_proc_kill(tp_proc *proc);

/* Kill the child if still running, reap it, and release all resources.
 * NULL-safe. */
void tp_proc_destroy(tp_proc *proc);

#ifdef __cplusplus
}
#endif

#endif /* TP_PROC_INTERNAL_H */
