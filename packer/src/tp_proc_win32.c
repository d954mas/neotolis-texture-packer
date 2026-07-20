#include "tp_proc_internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct tp_proc {
    HANDLE process;
    HANDLE stdin_w;  /* parent writes the request here; NULL once closed */
    HANDLE stdout_r; /* parent reads the reply here; NULL once closed */
    bool waited;
    tp_proc_result result;
};

/* UTF-8 -> freshly allocated UTF-16 (module/self path). Core code, not a
 * frontend, so a direct decode here does not cross the R21 filesystem-policy
 * boundary (which scans apps/ only). NULL on failure. */
static wchar_t *utf8_to_wide(const char *utf8) {
    if (!utf8) {
        return NULL;
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (count <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)count * sizeof *wide);
    if (!wide) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, count) == 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

bool tp_proc_self_path(char *out, size_t out_cap) {
    if (!out || out_cap == 0U) {
        return false;
    }
    out[0] = '\0';
    DWORD capacity = 512U;
    for (;;) {
        wchar_t *wide = (wchar_t *)malloc((size_t)capacity * sizeof *wide);
        if (!wide) {
            return false;
        }
        SetLastError(ERROR_SUCCESS);
        DWORD copied = GetModuleFileNameW(NULL, wide, capacity);
        if (copied == 0U) {
            free(wide);
            return false;
        }
        if (copied < capacity - 1U) {
            int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
            bool ok = bytes > 0 && (size_t)bytes <= out_cap &&
                      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, bytes, NULL, NULL) == bytes;
            free(wide);
            if (!ok) {
                out[0] = '\0';
            }
            return ok;
        }
        free(wide);
        if (capacity >= 32768U) {
            return false;
        }
        capacity *= 2U;
    }
}

/* Build a quoted mutable command line: "<exe>" <arg1>. CreateProcessW may write
 * into lpCommandLine, so it must be a writable buffer. */
static wchar_t *build_command_line(const wchar_t *exe_wide, const char *arg1) {
    wchar_t *arg_wide = utf8_to_wide(arg1 ? arg1 : "");
    if (!arg_wide) {
        return NULL;
    }
    size_t exe_len = wcslen(exe_wide);
    size_t arg_len = wcslen(arg_wide);
    /* 2 quotes + space + NUL. */
    size_t total = exe_len + arg_len + 4U;
    wchar_t *cmd = (wchar_t *)malloc(total * sizeof *cmd);
    if (!cmd) {
        free(arg_wide);
        return NULL;
    }
    size_t off = 0U;
    cmd[off++] = L'"';
    memcpy(cmd + off, exe_wide, exe_len * sizeof *cmd);
    off += exe_len;
    cmd[off++] = L'"';
    cmd[off++] = L' ';
    memcpy(cmd + off, arg_wide, arg_len * sizeof *cmd);
    off += arg_len;
    cmd[off] = L'\0';
    free(arg_wide);
    return cmd;
}

tp_proc *tp_proc_spawn(const char *exe_utf8, const char *arg1) {
    if (!exe_utf8 || exe_utf8[0] == '\0') {
        return NULL;
    }
    wchar_t *exe_wide = utf8_to_wide(exe_utf8);
    if (!exe_wide) {
        return NULL;
    }
    wchar_t *cmd = build_command_line(exe_wide, arg1);
    if (!cmd) {
        free(exe_wide);
        return NULL;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE; /* the child ends must be inheritable */

    HANDLE stdin_r = NULL;  /* child reads the request */
    HANDLE stdin_w = NULL;  /* parent writes the request */
    HANDLE stdout_r = NULL; /* parent reads the reply */
    HANDLE stdout_w = NULL; /* child writes the reply */
    if (!CreatePipe(&stdin_r, &stdin_w, &sa, 0) ||
        !CreatePipe(&stdout_r, &stdout_w, &sa, 0)) {
        if (stdin_r) {
            (void)CloseHandle(stdin_r);
        }
        if (stdin_w) {
            (void)CloseHandle(stdin_w);
        }
        free(exe_wide);
        free(cmd);
        return NULL;
    }
    /* Keep only the two child-facing ends inheritable; the parent's ends must
     * not leak into the child (they would keep EOF from ever arriving). */
    (void)SetHandleInformation(stdin_w, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_r;
    si.hStdOutput = stdout_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE); /* inherit parent stderr */

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    /* bInheritHandles=TRUE with only the two pipe ends marked inheritable.
     * TODO(H0.4): tighten to an explicit PROC_THREAD_ATTRIBUTE_HANDLE_LIST and
     * add a Job Object so a parent death or cancel cannot strand a worker. */
    BOOL ok = CreateProcessW(exe_wide, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(exe_wide);
    free(cmd);
    /* The child owns its ends now; the parent must drop them so EOF propagates. */
    (void)CloseHandle(stdin_r);
    (void)CloseHandle(stdout_w);
    if (!ok) {
        (void)CloseHandle(stdin_w);
        (void)CloseHandle(stdout_r);
        return NULL;
    }
    (void)CloseHandle(pi.hThread);

    tp_proc *proc = (tp_proc *)calloc(1U, sizeof *proc);
    if (!proc) {
        (void)CloseHandle(stdin_w);
        (void)CloseHandle(stdout_r);
        (void)TerminateProcess(pi.hProcess, 1U);
        (void)CloseHandle(pi.hProcess);
        return NULL;
    }
    proc->process = pi.hProcess;
    proc->stdin_w = stdin_w;
    proc->stdout_r = stdout_r;
    proc->waited = false;
    return proc;
}

bool tp_proc_write_stdin(tp_proc *proc, const void *data, size_t size) {
    if (!proc || !proc->stdin_w) {
        return false;
    }
    const unsigned char *bytes = (const unsigned char *)data;
    size_t off = 0U;
    bool ok = true;
    while (off < size) {
        size_t remaining = size - off;
        DWORD chunk = remaining > 0x40000000U ? 0x40000000U : (DWORD)remaining;
        DWORD written = 0U;
        if (!WriteFile(proc->stdin_w, bytes + off, chunk, &written, NULL) || written == 0U) {
            ok = false; /* broken pipe (child gone) or a real write error */
            break;
        }
        off += written;
    }
    (void)CloseHandle(proc->stdin_w); /* EOF for the child either way */
    proc->stdin_w = NULL;
    return ok && off == size;
}

bool tp_proc_read_stdout(tp_proc *proc, void *buf, size_t cap, size_t *out_len,
                         bool *out_eof) {
    if (out_len) {
        *out_len = 0U;
    }
    if (out_eof) {
        *out_eof = false;
    }
    if (!proc || !proc->stdout_r || (!buf && cap != 0U)) {
        return false;
    }
    unsigned char *bytes = (unsigned char *)buf;
    size_t total = 0U;
    for (;;) {
        if (total == cap) {
            unsigned char probe;
            DWORD extra = 0U;
            BOOL r = ReadFile(proc->stdout_r, &probe, 1U, &extra, NULL);
            bool eof = (!r && GetLastError() == ERROR_BROKEN_PIPE) || (r && extra == 0U);
            if (out_len) {
                *out_len = total;
            }
            if (out_eof) {
                *out_eof = eof; /* not eof => oversized reply */
            }
            return true;
        }
        size_t remaining = cap - total;
        DWORD chunk = remaining > 0x40000000U ? 0x40000000U : (DWORD)remaining;
        DWORD got = 0U;
        BOOL r = ReadFile(proc->stdout_r, bytes + total, chunk, &got, NULL);
        if (!r) {
            /* The child closing its write end reports a broken pipe = clean EOF. */
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                if (out_len) {
                    *out_len = total;
                }
                if (out_eof) {
                    *out_eof = true;
                }
                return true;
            }
            return false;
        }
        if (got == 0U) {
            if (out_len) {
                *out_len = total;
            }
            if (out_eof) {
                *out_eof = true;
            }
            return true;
        }
        total += got;
    }
}

bool tp_proc_wait(tp_proc *proc, tp_proc_result *out) {
    if (!proc || !proc->process) {
        return false;
    }
    if (!proc->waited) {
        if (WaitForSingleObject(proc->process, INFINITE) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD code = 0U;
        if (!GetExitCodeProcess(proc->process, &code)) {
            return false;
        }
        /* Windows has no separate "signaled" state; a crash surfaces as the
         * exception exit code. Report it as a normal exit and let the worker
         * boundary treat a non-zero code with no valid reply as a crash. */
        proc->result.how = TP_PROC_END_EXITED;
        proc->result.code = (int)code;
        proc->waited = true;
    }
    if (out) {
        *out = proc->result;
    }
    return true;
}

void tp_proc_kill(tp_proc *proc) {
    if (!proc || !proc->process || proc->waited) {
        return;
    }
    (void)TerminateProcess(proc->process, 1U);
}

void tp_proc_destroy(tp_proc *proc) {
    if (!proc) {
        return;
    }
    if (proc->stdin_w) {
        (void)CloseHandle(proc->stdin_w);
    }
    if (proc->stdout_r) {
        (void)CloseHandle(proc->stdout_r);
    }
    if (proc->process) {
        if (!proc->waited) {
            (void)TerminateProcess(proc->process, 1U);
            (void)WaitForSingleObject(proc->process, 2000U);
        }
        (void)CloseHandle(proc->process);
    }
    free(proc);
}
