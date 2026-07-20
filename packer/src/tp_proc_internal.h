#ifndef TP_PROC_INTERNAL_H
#define TP_PROC_INTERNAL_H

/* Minimal portable child-process transport for the private build worker
 * (decision 0018, ROADMAP H0.3-b). A parent spawns a child whose stdin is a
 * pipe the parent writes and whose stdout is a pipe the parent reads; stderr is
 * inherited from the parent. Only those two pipe ends are handed to the child.
 *
 * The child is spawned with a caller-chosen working directory so the request can
 * carry a bare relative ASCII output name (the builder opens a narrow path); the
 * parent owns the real UTF-8 destination. On Windows the child is confined to a
 * Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (parent death or cancel
 * tree-kills a stranded worker) and inherits ONLY the two pipe ends via
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST. Cancel/timeout are driven by the caller via
 * tp_proc_wait_slice + tp_proc_kill. */

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

/* Spawn `exe_utf8` with argv = { exe, arg1, NULL } and, when `cwd_utf8` is
 * non-NULL, the child's current directory set to it (CreateProcessW
 * lpCurrentDirectory as UTF-16; POSIX chdir between fork and exec). The child's
 * stdin is wired to a pipe the parent writes (tp_proc_write_stdin) and its stdout
 * to a pipe the parent reads (tp_proc_read_stdout); stderr is inherited. Returns
 * NULL on failure. `cwd_utf8`, when given, must be a real directory short enough
 * to be a process current directory (Windows caps that at MAX_PATH regardless of
 * long-path awareness); the caller guarantees this. */
tp_proc *tp_proc_spawn(const char *exe_utf8, const char *arg1, const char *cwd_utf8);

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

/* Bounded wait: block up to `slice_ms` for the child. On return *finished is true
 * iff the child has terminated (and *out is filled + the child reaped); false if
 * the slice elapsed with the child still running. Returns false only on a hard
 * wait error. Lets the caller poll a cancel flag / enforce a timeout between
 * slices without a blocking read of a hung child. */
bool tp_proc_wait_slice(tp_proc *proc, int slice_ms, tp_proc_result *out, bool *finished);

/* Force-kill a still-running child and, on Windows, its whole Job Object tree so a
 * grandchild the builder ever spawns cannot be stranded. NULL-safe. */
void tp_proc_kill(tp_proc *proc);

/* Kill the child if still running, reap it, and release all resources.
 * NULL-safe. */
void tp_proc_destroy(tp_proc *proc);

#ifdef __cplusplus
}
#endif

#endif /* TP_PROC_INTERNAL_H */
