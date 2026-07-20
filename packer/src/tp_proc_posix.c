/* pipe2(O_CLOEXEC) on Linux needs the GNU feature set; harmless elsewhere and the
 * macOS/other path falls back to pipe()+fcntl. Guarded to avoid a redefinition. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tp_proc_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

struct tp_proc {
    pid_t pid;
    int stdin_w;  /* parent writes the request here; -1 once closed */
    int stdout_r; /* parent reads the reply here; -1 once closed */
    bool reaped;
    tp_proc_result result;
};

/* A dead child closes its stdin read end, so writing the request to a crashed
 * worker would raise SIGPIPE (default: terminate the host). The whole tool must
 * never crash on a worker fault, so ignore SIGPIPE process-wide once and detect
 * the broken pipe via EPIPE instead. Lock-free once-guard: no extra link dep. */
static void ignore_sigpipe_once(void) {
    static atomic_flag done = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&done)) {
        (void)signal(SIGPIPE, SIG_IGN);
    }
}

bool tp_proc_self_path(char *out, size_t out_cap) {
    if (!out || out_cap == 0U) {
        return false;
    }
    out[0] = '\0';
#if defined(__APPLE__)
    uint32_t size = (uint32_t)(out_cap > 0xFFFFFFFFU ? 0xFFFFFFFFU : out_cap);
    if (_NSGetExecutablePath(out, &size) != 0) {
        out[0] = '\0';
        return false;
    }
    return true;
#else
    /* Linux (and compatible /proc): the canonical, argv-independent self path. */
    ssize_t n = readlink("/proc/self/exe", out, out_cap - 1U);
    if (n <= 0 || (size_t)n >= out_cap) {
        out[0] = '\0';
        return false;
    }
    out[n] = '\0';
    return true;
#endif
    /* No argv[0]/PATH fallback: every supported host resolves its own image
     * without argv (Windows GetModuleFileNameW, Linux /proc/self/exe, macOS
     * _NSGetExecutablePath). An argv[0] search would be strictly less reliable
     * (a caller can spawn us with any argv[0]) and only matters on platforms this
     * tool does not target, so containment fails closed there instead. */
}

/* Create a pipe with BOTH ends close-on-exec, so only the two fds the child dup2's
 * onto stdin/stdout survive exec -- a sibling worker's transport ends (and every
 * other host fd) are dropped at exec, matching the Windows explicit inherit list
 * and closing the sibling-pins-stdin-open deadlock. Returns 0 on success. */
static int make_cloexec_pipe(int fd[2]) {
#if defined(__linux__)
    return pipe2(fd, O_CLOEXEC);
#else
    /* macOS/other lack pipe2: set FD_CLOEXEC right after creation. The tiny
     * pipe()->fcntl window is only material to a concurrent fork+exec, which the
     * spawn path does not do. */
    if (pipe(fd) != 0) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(fd[i], F_GETFD);
        if (flags < 0 || fcntl(fd[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            (void)close(fd[0]);
            (void)close(fd[1]);
            return -1;
        }
    }
    return 0;
#endif
}

tp_proc *tp_proc_spawn(const char *exe_utf8, const char *arg1, const char *cwd_utf8) {
    if (!exe_utf8 || exe_utf8[0] == '\0') {
        return NULL;
    }
    ignore_sigpipe_once();

    int in_fd[2] = {-1, -1};  /* [0] child stdin read, [1] parent write */
    int out_fd[2] = {-1, -1}; /* [0] parent read, [1] child stdout write */
    if (make_cloexec_pipe(in_fd) != 0) {
        return NULL;
    }
    if (make_cloexec_pipe(out_fd) != 0) {
        (void)close(in_fd[0]);
        (void)close(in_fd[1]);
        return NULL;
    }

    tp_proc *proc = (tp_proc *)calloc(1U, sizeof *proc);
    if (!proc) {
        (void)close(in_fd[0]);
        (void)close(in_fd[1]);
        (void)close(out_fd[0]);
        (void)close(out_fd[1]);
        return NULL;
    }

    /* execv wants a NULL-terminated char* const[]; the strings are borrowed. */
    char *argv[3];
    argv[0] = (char *)exe_utf8;
    argv[1] = (char *)(arg1 ? arg1 : "");
    argv[2] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        (void)close(in_fd[0]);
        (void)close(in_fd[1]);
        (void)close(out_fd[0]);
        (void)close(out_fd[1]);
        free(proc);
        return NULL;
    }
    if (pid == 0) {
        /* Child: only async-signal-safe calls between fork and execv (chdir, dup2,
         * fcntl, close are all on the POSIX async-signal-safe list). Enter the
         * staging dir first so the request's relative output name resolves there,
         * wire the two pipe ends to stdin/stdout, drop every other raw pipe fd
         * (CLOEXEC drops any that slip through at exec), keep stderr. */
        if (cwd_utf8 && cwd_utf8[0] != '\0' && chdir(cwd_utf8) != 0) {
            _exit(127); /* cannot enter staging: parent maps the non-zero exit */
        }
        if (dup2(in_fd[0], STDIN_FILENO) < 0 || dup2(out_fd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        /* dup2 clears CLOEXEC on the new fd EXCEPT when oldfd==newfd (a no-op that
         * keeps the flag), which happens when the host was started with fd 0/1
         * closed and pipe() handed that number to a pipe end. Clear it explicitly
         * so the wired stdin/stdout are not auto-closed at exec. */
        (void)fcntl(STDIN_FILENO, F_SETFD, 0);
        (void)fcntl(STDOUT_FILENO, F_SETFD, 0);
        /* Guard each close: a raw pipe fd that dup2 landed on the wired STDIN/STDOUT
         * slot (host launched with fd 0/1 closed) must not be closed out from under
         * the child. Every genuine duplicate (fd >= 2) is still dropped; CLOEXEC
         * drops the rest at exec. */
        if (in_fd[0] != STDIN_FILENO && in_fd[0] != STDOUT_FILENO) {
            (void)close(in_fd[0]);
        }
        if (in_fd[1] != STDIN_FILENO && in_fd[1] != STDOUT_FILENO) {
            (void)close(in_fd[1]);
        }
        if (out_fd[0] != STDIN_FILENO && out_fd[0] != STDOUT_FILENO) {
            (void)close(out_fd[0]);
        }
        if (out_fd[1] != STDIN_FILENO && out_fd[1] != STDOUT_FILENO) {
            (void)close(out_fd[1]);
        }
        (void)execv(exe_utf8, argv);
        _exit(127); /* exec failed: parent sees a non-zero exit + no reply */
    }

    /* Parent: keep the write-to-child and read-from-child ends only. */
    (void)close(in_fd[0]);
    (void)close(out_fd[1]);
    proc->pid = pid;
    proc->stdin_w = in_fd[1];
    proc->stdout_r = out_fd[0];
    proc->reaped = false;
    return proc;
}

bool tp_proc_write_stdin(tp_proc *proc, const void *data, size_t size) {
    if (!proc || proc->stdin_w < 0) {
        return false;
    }
    const unsigned char *bytes = (const unsigned char *)data;
    size_t off = 0U;
    bool ok = true;
    while (off < size) {
        ssize_t n = write(proc->stdin_w, bytes + off, size - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false; /* EPIPE (child gone) or a real write error */
            break;
        }
        off += (size_t)n;
    }
    (void)close(proc->stdin_w); /* EOF for the child either way */
    proc->stdin_w = -1;
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
    if (!proc || proc->stdout_r < 0 || (!buf && cap != 0U)) {
        return false;
    }
    unsigned char *bytes = (unsigned char *)buf;
    size_t total = 0U;
    for (;;) {
        if (total == cap) {
            /* Buffer full: peek one more byte to tell EOF from an oversized
             * reply (an oversized reply is a malformed, fail-closed outcome). */
            unsigned char probe;
            ssize_t extra = read(proc->stdout_r, &probe, 1U);
            if (extra < 0 && errno == EINTR) {
                continue;
            }
            if (out_len) {
                *out_len = total;
            }
            if (out_eof) {
                *out_eof = (extra == 0);
            }
            return true;
        }
        ssize_t n = read(proc->stdout_r, bytes + total, cap - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            if (out_len) {
                *out_len = total;
            }
            if (out_eof) {
                *out_eof = true;
            }
            return true;
        }
        total += (size_t)n;
    }
}

bool tp_proc_wait_slice(tp_proc *proc, int slice_ms, tp_proc_result *out, bool *finished) {
    if (finished) {
        *finished = false;
    }
    if (!proc) {
        return false;
    }
    if (proc->reaped) {
        if (finished) {
            *finished = true;
        }
        if (out) {
            *out = proc->result;
        }
        return true;
    }
    int status = 0;
    pid_t r;
    do {
        r = waitpid(proc->pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        return false;
    }
    if (r == 0) {
        /* Still running: sleep out the slice so the caller polls at a bounded
         * cadence rather than spinning. */
        if (slice_ms > 0) {
            struct timespec ts = {slice_ms / 1000, (long)(slice_ms % 1000) * 1000000L};
            (void)nanosleep(&ts, NULL);
        }
        return true;
    }
    if (WIFEXITED(status)) {
        proc->result.how = TP_PROC_END_EXITED;
        proc->result.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        proc->result.how = TP_PROC_END_ABNORMAL;
        proc->result.code = WTERMSIG(status);
    } else {
        proc->result.how = TP_PROC_END_ABNORMAL;
        proc->result.code = -1;
    }
    proc->reaped = true;
    if (finished) {
        *finished = true;
    }
    if (out) {
        *out = proc->result;
    }
    return true;
}

void tp_proc_kill(tp_proc *proc) {
    if (!proc || proc->reaped) {
        return;
    }
    (void)kill(proc->pid, SIGKILL);
}

void tp_proc_destroy(tp_proc *proc) {
    if (!proc) {
        return;
    }
    if (proc->stdin_w >= 0) {
        (void)close(proc->stdin_w);
    }
    if (proc->stdout_r >= 0) {
        (void)close(proc->stdout_r);
    }
    if (!proc->reaped) {
        (void)kill(proc->pid, SIGKILL);
        int status = 0;
        pid_t r;
        do {
            r = waitpid(proc->pid, &status, 0);
        } while (r < 0 && errno == EINTR);
        proc->reaped = true; /* reaped to avoid a zombie */
    }
    free(proc);
}
