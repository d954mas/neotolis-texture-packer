#include "tp_proc_internal.h"

#ifdef TP_ENABLE_TEST_SEAMS
static unsigned int s_test_destroy_kill_calls;
static unsigned int s_test_destroy_blocking_wait_calls;
static bool s_test_fail_next_kill;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7+: Job Objects + PROC_THREAD_ATTRIBUTE_HANDLE_LIST */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct tp_proc {
    HANDLE process;
    HANDLE job;      /* KILL_ON_JOB_CLOSE tree-kill container; NULL if unavailable */
    HANDLE stdin_w;  /* parent writes the request here; NULL once closed */
    HANDLE stdout_r; /* parent reads the reply here; NULL once closed */
    HANDLE stdin_event;
    OVERLAPPED stdin_overlapped;
    const void *stdin_pending_data;
    void *stdin_pending_buffer;
    size_t stdin_pending_size;
    bool stdin_pending;
    bool waited;
    bool owned_tree;
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

/* UTF-8 cwd -> inheritable-free wide path with backslash separators. The caller
 * guarantees the staging dir is short enough to be a process current directory
 * (Windows caps lpCurrentDirectory at MAX_PATH), so no extended prefix is added
 * -- CreateProcessW rejects a \\?\ current directory. NULL on failure. */
static wchar_t *cwd_to_wide(const char *cwd_utf8) {
    wchar_t *wide = utf8_to_wide(cwd_utf8);
    if (!wide) {
        return NULL;
    }
    for (wchar_t *p = wide; *p; p++) {
        if (*p == L'/') {
            *p = L'\\';
        }
    }
    return wide;
}

/* Best-effort Job Object with KILL_ON_JOB_CLOSE: the child (and any grandchild)
 * is torn down when the parent drops the job handle (crash, cancel, exit). A
 * constrained host that forbids nested jobs simply runs without it. */
static HANDLE create_kill_on_close_job(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) {
        return NULL;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    memset(&info, 0, sizeof info);
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info)) {
        (void)CloseHandle(job);
        return NULL;
    }
    return job;
}

/* Create the stdin half with an overlapped parent writer. CreatePipe cannot
 * request FILE_FLAG_OVERLAPPED, so use the documented named-pipe substrate
 * behind anonymous pipes. The unique name is never exposed outside this call. */
static bool create_overlapped_stdin_pipe(HANDLE *out_child_read,
                                         HANDLE *out_parent_write) {
    static volatile LONG serial = 0;
    wchar_t name[128];
    const LONG id = InterlockedIncrement(&serial);
    if (_snwprintf_s(name, _countof(name), _TRUNCATE,
                     L"\\\\.\\pipe\\ntpacker-proc-%lu-%ld",
                     (unsigned long)GetCurrentProcessId(), (long)id) < 0) {
        return false;
    }

    HANDLE writer = CreateNamedPipeW(
        name, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1U, 1U << 16, 1U << 16,
        0U, NULL);
    if (writer == INVALID_HANDLE_VALUE) {
        return false;
    }

    SECURITY_ATTRIBUTES child_sa;
    child_sa.nLength = sizeof child_sa;
    child_sa.lpSecurityDescriptor = NULL;
    child_sa.bInheritHandle = TRUE;
    HANDLE reader = CreateFileW(name, GENERIC_READ, 0U, &child_sa, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (reader == INVALID_HANDLE_VALUE) {
        (void)CloseHandle(writer);
        return false;
    }

    OVERLAPPED connected;
    memset(&connected, 0, sizeof connected);
    connected.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!connected.hEvent) {
        (void)CloseHandle(reader);
        (void)CloseHandle(writer);
        return false;
    }
    BOOL connected_now = ConnectNamedPipe(writer, &connected);
    DWORD connect_error = connected_now ? ERROR_SUCCESS : GetLastError();
    bool ok = connected_now || connect_error == ERROR_PIPE_CONNECTED;
    if (!ok && connect_error == ERROR_IO_PENDING) {
        DWORD ignored = 0U;
        ok = GetOverlappedResult(writer, &connected, &ignored, TRUE) != 0;
    }
    (void)CloseHandle(connected.hEvent);
    if (!ok) {
        (void)CloseHandle(reader);
        (void)CloseHandle(writer);
        return false;
    }
    *out_child_read = reader;
    *out_parent_write = writer;
    return true;
}

static tp_proc *tp_proc_spawn_impl(const char *exe_utf8, const char *arg1,
                                   const char *cwd_utf8, bool require_tree) {
    if (!exe_utf8 || exe_utf8[0] == '\0') {
        return NULL;
    }
    wchar_t *exe_wide = utf8_to_wide(exe_utf8);
    if (!exe_wide) {
        return NULL;
    }
    wchar_t *cmd = build_command_line(exe_wide, arg1);
    wchar_t *cwd_wide = NULL;
    if (!cmd || (cwd_utf8 && !(cwd_wide = cwd_to_wide(cwd_utf8)))) {
        free(exe_wide);
        free(cmd);
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
    /* 64 KiB pipe buffers: the encoder bounds every reply to <= ~8 KiB, so it fits
     * entirely and the child can write it and exit without the parent reading
     * concurrently -- the parent waits (with cancel/timeout) THEN reads, never
     * blocking a read on a hung child. A reply larger than this buffer would block
     * the child and be caught by the safety timeout (builder_crashed), not the
     * over-cap read branch. */
    if (!create_overlapped_stdin_pipe(&stdin_r, &stdin_w) ||
        !CreatePipe(&stdout_r, &stdout_w, &sa, 1u << 16)) {
        if (stdin_r) {
            (void)CloseHandle(stdin_r);
        }
        if (stdin_w) {
            (void)CloseHandle(stdin_w);
        }
        free(exe_wide);
        free(cmd);
        free(cwd_wide);
        return NULL;
    }
    /* Keep only the two child-facing ends inheritable; the parent's ends must
     * not leak into the child (they would keep EOF from ever arriving). */
    (void)SetHandleInformation(stdin_w, HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);

    /* Restrict inheritance to EXACTLY the two pipe ends via an explicit handle
     * list (replaces blanket bInheritHandles): an unrelated inheritable handle
     * can no longer leak into the child and pin a pipe open. stderr is not
     * inherited -- the worker silences logging, so it never writes there. */
    SIZE_T attr_size = 0;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs =
        (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    HANDLE inherit[2] = {stdin_r, stdout_w};
    /* Track init separately: a list that Initialize populated must be handed to
     * DeleteProcThreadAttributeList before free() even if Update fails (MSDN), and
     * an uninitialized buffer must NOT be passed to Delete. */
    bool init_ok = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    bool attrs_ok = init_ok &&
                    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                              inherit, sizeof inherit, NULL, NULL);
    if (!attrs_ok) {
        if (init_ok) {
            DeleteProcThreadAttributeList(attrs);
        }
        if (attrs) {
            free(attrs);
        }
        (void)CloseHandle(stdin_r);
        (void)CloseHandle(stdin_w);
        (void)CloseHandle(stdout_r);
        (void)CloseHandle(stdout_w);
        free(exe_wide);
        free(cmd);
        free(cwd_wide);
        return NULL;
    }

    STARTUPINFOEXW six;
    memset(&six, 0, sizeof six);
    six.StartupInfo.cb = sizeof six;
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdInput = stdin_r;
    six.StartupInfo.hStdOutput = stdout_w;
    six.StartupInfo.hStdError = INVALID_HANDLE_VALUE; /* not inherited: worker is silent */
    six.lpAttributeList = attrs;

    HANDLE job = create_kill_on_close_job();
    if (require_tree && !job) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
        free(exe_wide);
        free(cmd);
        free(cwd_wide);
        (void)CloseHandle(stdin_r);
        (void)CloseHandle(stdin_w);
        (void)CloseHandle(stdout_r);
        (void)CloseHandle(stdout_w);
        return NULL;
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    /* Suspended so the child is assigned to the job (tree-kill) BEFORE it runs. */
    BOOL ok = CreateProcessW(exe_wide, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                             NULL, cwd_wide, &six.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
    free(exe_wide);
    free(cmd);
    free(cwd_wide);
    /* The child owns its ends now; the parent must drop them so EOF propagates. */
    (void)CloseHandle(stdin_r);
    (void)CloseHandle(stdout_w);
    if (!ok) {
        if (job) {
            (void)CloseHandle(job);
        }
        (void)CloseHandle(stdin_w);
        (void)CloseHandle(stdout_r);
        return NULL;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        if (require_tree) {
            (void)TerminateProcess(pi.hProcess, 1U);
            (void)WaitForSingleObject(pi.hProcess, 2000U);
            (void)CloseHandle(job);
            (void)CloseHandle(stdin_w);
            (void)CloseHandle(stdout_r);
            (void)CloseHandle(pi.hProcess);
            (void)CloseHandle(pi.hThread);
            return NULL;
        }
        /* A parent job that forbids nesting rejects the assign; drop our (now
         * empty) job so tp_proc_kill falls back to TerminateProcess on the real
         * child instead of tree-killing an empty job and stranding it. */
        (void)CloseHandle(job);
        job = NULL;
    }
    (void)ResumeThread(pi.hThread);
    (void)CloseHandle(pi.hThread);

    tp_proc *proc = (tp_proc *)calloc(1U, sizeof *proc);
    if (!proc) {
        if (job) {
            (void)TerminateJobObject(job, 1U);
            (void)CloseHandle(job);
        } else {
            (void)TerminateProcess(pi.hProcess, 1U);
        }
        (void)CloseHandle(stdin_w);
        (void)CloseHandle(stdout_r);
        (void)CloseHandle(pi.hProcess);
        return NULL;
    }
    proc->stdin_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!proc->stdin_event) {
        if (job) {
            (void)TerminateJobObject(job, 1U);
            (void)CloseHandle(job);
        } else {
            (void)TerminateProcess(pi.hProcess, 1U);
        }
        (void)CloseHandle(stdin_w);
        (void)CloseHandle(stdout_r);
        (void)CloseHandle(pi.hProcess);
        free(proc);
        return NULL;
    }
    memset(&proc->stdin_overlapped, 0, sizeof proc->stdin_overlapped);
    proc->stdin_overlapped.hEvent = proc->stdin_event;
    proc->process = pi.hProcess;
    proc->job = job;
    proc->stdin_w = stdin_w;
    proc->stdout_r = stdout_r;
    proc->waited = false;
    proc->owned_tree = require_tree;
    return proc;
}

tp_proc *tp_proc_spawn(const char *exe_utf8, const char *arg1, const char *cwd_utf8) {
    return tp_proc_spawn_impl(exe_utf8, arg1, cwd_utf8, false);
}

tp_proc *tp_proc_spawn_owned_tree(const char *exe_utf8, const char *arg1,
                                  const char *cwd_utf8) {
    return tp_proc_spawn_impl(exe_utf8, arg1, cwd_utf8, true);
}

bool tp_proc_write_stdin_keep_open(tp_proc *proc, const void *data, size_t size) {
    if (!proc || !proc->stdin_w || (!data && size != 0U)) {
        return false;
    }
    const unsigned char *bytes = (const unsigned char *)data;
    size_t off = 0U;
    while (off < size) {
        size_t consumed = 0U;
        bool would_block = false;
        if (!tp_proc_try_write_stdin(proc, bytes + off, size - off, &consumed,
                                     &would_block)) {
            return false; /* broken pipe (child gone) or a real write error */
        }
        off += consumed;
        if (would_block && WaitForSingleObject(proc->stdin_event, INFINITE) != WAIT_OBJECT_0) {
            return false;
        }
    }
    return true;
}

bool tp_proc_try_write_stdin(tp_proc *proc, const void *data, size_t size,
                             size_t *out_consumed, bool *out_would_block) {
    if (out_consumed) {
        *out_consumed = 0U;
    }
    if (out_would_block) {
        *out_would_block = false;
    }
    if (!proc || !proc->stdin_w || (!data && size != 0U)) {
        return false;
    }

    if (proc->stdin_pending) {
        if (data != proc->stdin_pending_data || size < proc->stdin_pending_size) {
            return false;
        }
        DWORD written = 0U;
        if (!GetOverlappedResult(proc->stdin_w, &proc->stdin_overlapped,
                                 &written, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) {
                if (out_would_block) {
                    *out_would_block = true;
                }
                return true;
            }
            free(proc->stdin_pending_buffer);
            proc->stdin_pending_buffer = NULL;
            proc->stdin_pending = false;
            proc->stdin_pending_data = NULL;
            proc->stdin_pending_size = 0U;
            return false;
        }
        const size_t completed = proc->stdin_pending_size;
        free(proc->stdin_pending_buffer);
        proc->stdin_pending_buffer = NULL;
        proc->stdin_pending = false;
        proc->stdin_pending_data = NULL;
        proc->stdin_pending_size = 0U;
        if ((size_t)written != completed) {
            return false;
        }
        if (out_consumed) {
            *out_consumed = completed;
        }
        return true;
    }
    if (size == 0U) {
        return true;
    }

    const DWORD chunk = size > 0x40000000U ? 0x40000000U : (DWORD)size;
    void *pending_buffer = malloc((size_t)chunk);
    if (!pending_buffer) {
        return false;
    }
    memcpy(pending_buffer, data, (size_t)chunk);
    (void)ResetEvent(proc->stdin_event);
    memset(&proc->stdin_overlapped, 0, sizeof proc->stdin_overlapped);
    proc->stdin_overlapped.hEvent = proc->stdin_event;
    DWORD written = 0U;
    if (WriteFile(proc->stdin_w, pending_buffer, chunk, &written,
                  &proc->stdin_overlapped)) {
        free(pending_buffer);
        if (out_consumed) {
            *out_consumed = (size_t)written;
        }
        return written != 0U;
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        free(pending_buffer);
        return false;
    }
    proc->stdin_pending = true;
    proc->stdin_pending_data = data;
    proc->stdin_pending_buffer = pending_buffer;
    proc->stdin_pending_size = (size_t)chunk;
    if (out_would_block) {
        *out_would_block = true;
    }
    return true;
}

bool tp_proc_send_cancel(tp_proc *proc) {
    const unsigned char signal = TP_PROC_CANCEL_BYTE;
    return tp_proc_write_stdin_keep_open(proc, &signal, sizeof signal);
}

bool tp_proc_close_stdin(tp_proc *proc) {
    if (!proc || !proc->stdin_w) {
        return false;
    }
    bool ok = true;
    if (proc->stdin_pending) {
        (void)CancelIoEx(proc->stdin_w, &proc->stdin_overlapped);
        DWORD ignored = 0U;
        if (!GetOverlappedResult(proc->stdin_w, &proc->stdin_overlapped,
                                 &ignored, TRUE) &&
            GetLastError() != ERROR_OPERATION_ABORTED) {
            ok = false;
        }
        free(proc->stdin_pending_buffer);
        proc->stdin_pending_buffer = NULL;
        proc->stdin_pending = false;
        proc->stdin_pending_data = NULL;
        proc->stdin_pending_size = 0U;
    }
    ok = CloseHandle(proc->stdin_w) != 0 && ok;
    proc->stdin_w = NULL;
    return ok;
}

bool tp_proc_write_stdin(tp_proc *proc, const void *data, size_t size) {
    const bool wrote = tp_proc_write_stdin_keep_open(proc, data, size);
    const bool closed = tp_proc_close_stdin(proc);
    return wrote && closed;
}

tp_proc_stdin_event tp_proc_child_poll_stdin(void) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE) {
        return TP_PROC_STDIN_EVENT_ERROR;
    }

    DWORD available = 0U;
    if (!PeekNamedPipe(input, NULL, 0U, NULL, &available, NULL)) {
        if (GetLastError() != ERROR_BROKEN_PIPE) {
            return TP_PROC_STDIN_EVENT_ERROR;
        }
        /* A writer can queue the cancel byte and immediately close. Some
         * Windows pipe implementations report BROKEN_PIPE from Peek even while
         * that final byte remains readable, so drain once before declaring EOF. */
        unsigned char trailing = 0U;
        DWORD trailing_got = 0U;
        if (ReadFile(input, &trailing, sizeof trailing, &trailing_got, NULL) &&
            trailing_got == sizeof trailing) {
            return trailing == TP_PROC_CANCEL_BYTE ? TP_PROC_STDIN_EVENT_CANCEL
                                                    : TP_PROC_STDIN_EVENT_ERROR;
        }
        return GetLastError() == ERROR_BROKEN_PIPE ? TP_PROC_STDIN_EVENT_CLOSED
                                                   : TP_PROC_STDIN_EVENT_ERROR;
    }
    if (available == 0U) {
        return TP_PROC_STDIN_EVENT_NONE;
    }

    unsigned char control = 0U;
    DWORD got = 0U;
    if (!ReadFile(input, &control, sizeof control, &got, NULL)) {
        return GetLastError() == ERROR_BROKEN_PIPE ? TP_PROC_STDIN_EVENT_CLOSED
                                                   : TP_PROC_STDIN_EVENT_ERROR;
    }
    if (got == 0U) {
        return TP_PROC_STDIN_EVENT_CLOSED;
    }
    return control == TP_PROC_CANCEL_BYTE ? TP_PROC_STDIN_EVENT_CANCEL
                                          : TP_PROC_STDIN_EVENT_ERROR;
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

bool tp_proc_try_read_stdout(tp_proc *proc, void *buf, size_t cap, size_t *out_len,
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

    DWORD available = 0U;
    if (!PeekNamedPipe(proc->stdout_r, NULL, 0U, NULL, &available, NULL)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) {
            if (out_eof) {
                *out_eof = true;
            }
            return true;
        }
        return false;
    }
    if (available == 0U || cap == 0U) {
        return true;
    }

    size_t bounded = cap < (size_t)available ? cap : (size_t)available;
    DWORD chunk = bounded > 0x40000000U ? 0x40000000U : (DWORD)bounded;
    DWORD got = 0U;
    if (!ReadFile(proc->stdout_r, buf, chunk, &got, NULL)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) {
            if (out_eof) {
                *out_eof = true;
            }
            return true;
        }
        return false;
    }
    if (out_len) {
        *out_len = (size_t)got;
    }
    return true;
}

bool tp_proc_wait_slice(tp_proc *proc, int slice_ms, tp_proc_result *out, bool *finished) {
    if (finished) {
        *finished = false;
    }
    if (!proc || !proc->process) {
        return false;
    }
    if (proc->waited) {
        if (finished) {
            *finished = true;
        }
        if (out) {
            *out = proc->result;
        }
        return true;
    }
    DWORD waited = WaitForSingleObject(proc->process, slice_ms < 0 ? INFINITE : (DWORD)slice_ms);
    if (waited == WAIT_TIMEOUT) {
        return true; /* still running: *finished stays false */
    }
    if (waited != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0U;
    if (!GetExitCodeProcess(proc->process, &code)) {
        return false;
    }
    /* Windows has no separate "signaled" state; a crash surfaces as the exception
     * exit code. Report EXITED and let the worker boundary treat a non-zero code
     * with no valid reply as a crash. */
    proc->result.how = TP_PROC_END_EXITED;
    proc->result.code = (int)code;
    proc->waited = true;
    if (finished) {
        *finished = true;
    }
    if (out) {
        *out = proc->result;
    }
    return true;
}

void tp_proc_kill(tp_proc *proc) {
    if (!proc) {
        return;
    }
#ifdef TP_ENABLE_TEST_SEAMS
    s_test_destroy_kill_calls++;
    if (s_test_fail_next_kill) {
        s_test_fail_next_kill = false;
        return;
    }
#endif
    if (proc->job) {
        /* The direct child may already be waited while a nested worker remains. */
        (void)TerminateJobObject(proc->job, 1U); /* tree-kill the whole worker */
    } else if (!proc->waited && proc->process) {
        (void)TerminateProcess(proc->process, 1U);
    }
}

static DWORD WINAPI tp_proc_reap_owned_tree(LPVOID context) {
    tp_proc *proc = context;
    if (proc->stdin_w) {
        (void)tp_proc_close_stdin(proc);
    }
    if (proc->stdout_r) {
        (void)CloseHandle(proc->stdout_r);
    }
    if (proc->process) {
        (void)WaitForSingleObject(proc->process, INFINITE);
        (void)CloseHandle(proc->process);
    }
    if (proc->job) {
        (void)CloseHandle(proc->job);
    }
    if (proc->stdin_event) {
        (void)CloseHandle(proc->stdin_event);
    }
    free(proc);
    return 0U;
}

void tp_proc_destroy(tp_proc *proc) {
    if (!proc) {
        return;
    }
    if (!proc->waited) {
        tp_proc_kill(proc);
    }
    if (proc->owned_tree && proc->stdin_pending) {
        /* OVERLAPPED storage must outlive its pending request. The host frame
         * cannot wait for cancellation completion, so transfer the complete
         * handle to a detached reaper after terminating the owned tree. */
        (void)CancelIoEx(proc->stdin_w, &proc->stdin_overlapped);
        HANDLE reaper = CreateThread(
            NULL, 0U, tp_proc_reap_owned_tree, proc, 0U, NULL);
        if (reaper) {
            (void)CloseHandle(reaper);
            return;
        }
        /* CreateThread failure must not turn frame teardown into a wait.
         * Closing the kill-on-close job reinforces termination; retain the
         * remaining handle and OVERLAPPED storage until process exit. */
        if (proc->job) {
            (void)CloseHandle(proc->job);
            proc->job = NULL;
        }
        return;
    }
    if (proc->stdin_w) {
        (void)tp_proc_close_stdin(proc);
    }
    if (proc->stdout_r) {
        (void)CloseHandle(proc->stdout_r);
    }
    if (proc->process) {
        if (!proc->waited && !proc->owned_tree) {
            /* The synchronous inner build-worker boundary removes its
             * staging directory immediately after destroy. Wait for the
             * already-terminated direct child so Windows releases its CWD
             * handles first. Outer session workers never take this path. */
#ifdef TP_ENABLE_TEST_SEAMS
            s_test_destroy_blocking_wait_calls++;
#endif
            (void)WaitForSingleObject(proc->process, 2000U);
        }
        (void)CloseHandle(proc->process);
    }
    if (proc->job) {
        (void)CloseHandle(proc->job); /* KILL_ON_JOB_CLOSE reaps any stray tree */
    }
    if (proc->stdin_event) {
        (void)CloseHandle(proc->stdin_event);
    }
    free(proc);
}

#ifdef TP_ENABLE_TEST_SEAMS
void tp_proc__test_reset_destroy_trace(void) {
    s_test_destroy_kill_calls = 0U;
    s_test_destroy_blocking_wait_calls = 0U;
    s_test_fail_next_kill = false;
}

void tp_proc__test_fail_next_kill(void) {
    s_test_fail_next_kill = true;
}

unsigned int tp_proc__test_destroy_kill_calls(void) {
    return s_test_destroy_kill_calls;
}

unsigned int
tp_proc__test_destroy_blocking_wait_calls(void) {
    return s_test_destroy_blocking_wait_calls;
}
#endif
