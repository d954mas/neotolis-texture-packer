#include "tp_proc_internal.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
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
    /* TODO(H0.4): argv[0]/PATH fallback for platforms without /proc or
     * _NSGetExecutablePath. The three CI OSes are covered above. */
}

tp_proc *tp_proc_spawn(const char *exe_utf8, const char *arg1) {
    if (!exe_utf8 || exe_utf8[0] == '\0') {
        return NULL;
    }
    ignore_sigpipe_once();

    int in_fd[2] = {-1, -1};  /* [0] child stdin read, [1] parent write */
    int out_fd[2] = {-1, -1}; /* [0] parent read, [1] child stdout write */
    if (pipe(in_fd) != 0) {
        return NULL;
    }
    if (pipe(out_fd) != 0) {
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
        /* Child: only async-signal-safe calls between fork and execv. Wire the
         * two pipe ends to stdin/stdout, drop every raw pipe fd, keep stderr. */
        if (dup2(in_fd[0], STDIN_FILENO) < 0 || dup2(out_fd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        (void)close(in_fd[0]);
        (void)close(in_fd[1]);
        (void)close(out_fd[0]);
        (void)close(out_fd[1]);
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

bool tp_proc_wait(tp_proc *proc, tp_proc_result *out) {
    if (!proc) {
        return false;
    }
    if (!proc->reaped) {
        int status = 0;
        pid_t r;
        do {
            r = waitpid(proc->pid, &status, 0);
        } while (r < 0 && errno == EINTR);
        if (r != proc->pid) {
            return false;
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
