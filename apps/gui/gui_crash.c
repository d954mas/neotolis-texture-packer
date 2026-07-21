#include "gui_crash.h"

/* libc/system headers FIRST (before the gui/engine headers) -- matches gui_pack.c / gui_log_file.c.
 * On macOS clang a vendored/engine header pulling <stdio.h> first otherwise leaves snprintf & friends
 * implicitly declared here, which clang treats as a HARD error (this exact bug bit D1 on macOS CI). */
#include <stdatomic.h> /* POSIX handler's cross-thread reentrancy guard (same header gui_pack.c uses) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nt_utf8_fs.h"

#ifdef _WIN32
#include <windows.h> /* must precede dbghelp.h (it uses Win32 types) */
#include <dbghelp.h> /* MiniDumpWriteDump, MINIDUMP_* */
#else
#include <execinfo.h> /* backtrace, backtrace_symbols_fd (both async-signal-safe on glibc/macOS) */
#include <fcntl.h>    /* open, O_WRONLY/O_CREAT/O_TRUNC */
#include <signal.h>   /* signal, raise, SIG_DFL, sig_atomic_t */
#include <unistd.h>   /* write, close, _exit */
#endif

#include "tinyfiledialogs.h" /* D3 next-launch report prompt (yes/no messageBox); vendored, apps/gui/deps */
#include "gui_paths.h"        /* <app-data> root + ensure-dir (shared with D1) */
#include "gui_shell_quote.h"  /* shared POSIX shell-word quoting (reveal path in the file manager) */

#include "log/nt_log.h" /* used by gui_crash_selftest + gui_crash_report_prompt (main thread, after the
                         * log is up); NEVER by the crash handler (it must stay async-signal-safe). */

// #region state
/* Everything the handler touches is pre-resolved here at install time (main thread) into static
 * buffers: the handler must not format paths (snprintf/strftime are not async-signal-safe). Empty
 * strings mean "could not resolve" -> the handler degrades to writing nothing. */
static bool s_installed;
static char s_report_root[GUI_PATHS_MAX];   /* <app-data> (D3 opens it: contains both crash/ and logs/) */
static char s_crash_dir[GUI_PATHS_MAX];     /* <app-data>/crash (handler output lives here) */
static char s_marker_path[GUI_PATHS_MAX];   /* <app-data>/crash/last-run.crashed (fixed name; D3 reads it) */
static char s_marker_prefix[128];           /* pre-rendered marker text (session-start timestamp) */
static size_t s_marker_prefix_len;
#ifdef _WIN32
static char s_dump_path[GUI_PATHS_MAX]; /* <app-data>/crash/crash-<ts>.dmp */
/* Exception-filter paths are converted before the handler is installed. The
 * extra cells cover the controlled \\?\ / \\?\UNC\ long-path prefix. */
static wchar_t s_marker_path_w[GUI_PATHS_MAX + 8];
static wchar_t s_dump_path_w[GUI_PATHS_MAX + 8];
#else
static char s_backtrace_path[GUI_PATHS_MAX]; /* <app-data>/crash/crash-<ts>.txt */
#endif
// #endregion

#ifndef _WIN32
// #region posix handler (async-signal-safe ONLY)
/* The reentrancy claim below must be lock-free to be async-signal-safe AND to serialize two encode-
 * worker threads faulting within microseconds (a plain sig_atomic_t is only same-thread safe). */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "gui_crash: atomic_int must be always-lock-free for the signal-handler claim");

/* Render a small non-negative int to decimal into buf (>= 16 bytes), return the digit count. Pure
 * arithmetic + array stores -- no libc call, so it is safe to use inside a signal handler. */
static int crash_utoa(unsigned v, char *buf) {
    char tmp[16];
    int i = 0;
    do {
        tmp[i++] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while (v != 0U && i < (int)sizeof tmp);
    int len = 0;
    while (i > 0) {
        buf[len++] = tmp[--i];
    }
    return len;
}

/* Best-effort write() in a fault path: async-signal-safe, and there is nothing to do on a short or
 * failed write, so the result is consumed and dropped. */
static void crash_write(int fd, const char *buf, size_t len) {
    ssize_t r = write(fd, buf, len);
    (void)r;
}

/* Fatal-signal handler. EVERY call here must be async-signal-safe (open/write/close/_exit, backtrace/
 * backtrace_symbols_fd, signal/raise) and it must NOT take D1's log mutex or call nt_log -- a pack
 * worker may hold that mutex at the crash point, which would deadlock. */
static void crash_signal_handler(int sig) {
    /* Claim the handler exactly once, ACROSS THREADS: a lock-free atomic exchange is async-signal-safe
     * and stops two workers faulting near-simultaneously from both O_TRUNC'ing + interleaving into the
     * same backtrace file. The first arrival (old value 0) proceeds; any later/nested one bails. */
    static atomic_int s_in_handler; /* zero-initialized */
    if (atomic_exchange_explicit(&s_in_handler, 1, memory_order_acq_rel) != 0) {
        _exit(128 + sig); /* re-faulted / concurrent fault -> bail without recursing or racing the file */
    }

    /* (1) Marker FIRST, so a fault while writing the backtrace still leaves D3 the crash flag. */
    if (s_marker_path[0] != '\0') {
        int fd = open(s_marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            if (s_marker_prefix_len > 0) {
                crash_write(fd, s_marker_prefix, s_marker_prefix_len);
            }
            char num[20];
            int nl = crash_utoa((unsigned)sig, num);
            num[nl++] = '\n';
            crash_write(fd, "signal ", 7);
            crash_write(fd, num, (size_t)nl);
            (void)close(fd);
        }
    }

    /* (2) Backtrace to the pre-resolved per-run file. backtrace_symbols_fd writes straight to the fd
     * and (unlike backtrace_symbols) does not malloc -> async-signal-safe; backtrace() was warmed up
     * at install time so it will not hit the dynamic loader here. */
    if (s_backtrace_path[0] != '\0') {
        int fd = open(s_backtrace_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            void *frames[64];
            int n = backtrace(frames, (int)(sizeof frames / sizeof frames[0]));
            backtrace_symbols_fd(frames, n, fd);
            (void)close(fd);
        }
    }

    /* (3) Restore the default disposition + re-raise: the OS produces its normal core/report exactly
     * as if we had never intercepted the fault. */
    (void)signal(sig, SIG_DFL);
    (void)raise(sig);
    _exit(128 + sig); /* only reached if raise() unexpectedly returns */
}
// #endregion
#else
// #region windows handler
/* Render a 32-bit value as 8 hex digits into buf[8]. Pure arithmetic -- no CRT/heap -- so it is safe
 * in the exception filter even if the fault corrupted the heap. */
static void crash_hex32(unsigned long v, char *buf) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        buf[i] = digits[v & 0xFUL];
        v >>= 4;
    }
}

/* Top-level exception filter: more latitude than a POSIX signal handler, but still kept minimal +
 * reentrancy-shy, and it likewise NEVER touches D1's log (a pack worker may hold its mutex here). */
static LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS *info) {
    static volatile LONG s_in_filter = 0;
    if (InterlockedCompareExchange(&s_in_filter, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH; /* nested fault -> don't recurse into dump writing */
    }

    /* Marker FIRST (even a failed dump still tells the next launch we crashed). */
    if (s_marker_path_w[0] != L'\0') {
        HANDLE h = CreateFileW(s_marker_path_w, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            (void)WriteFile(h, s_marker_prefix, (DWORD)s_marker_prefix_len, &wrote, NULL);
            /* Append the faulting exception code as hex -- mirrors POSIX's "signal N". Built with the
             * heap-light hex writer (no CRT formatting) since the heap may be corrupt here. */
            char line[24];
            memcpy(line, "exception 0x", 12);
            unsigned long code =
                (info != NULL && info->ExceptionRecord != NULL) ? (unsigned long)info->ExceptionRecord->ExceptionCode : 0UL;
            crash_hex32(code, line + 12);
            line[20] = '\n';
            (void)WriteFile(h, line, 21, &wrote, NULL);
            (void)CloseHandle(h);
        }
    }

    /* MiniDump: MiniDumpNormal|MiniDumpWithDataSegs = small file that still carries the thread stacks
     * + global/static data segments (enough to symbolize a crash without a huge full-memory dump). */
    if (s_dump_path_w[0] != L'\0') {
        HANDLE h = CreateFileW(s_dump_path_w, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = info;
            mei.ClientPointers = FALSE;
            (void)MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                                    (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs), &mei, NULL, NULL);
            (void)CloseHandle(h);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH; /* let the OS default handler (WER / a debugger) still run */
}
// #endregion
#endif

// #region lifecycle
void gui_crash_install(void) {
    if (s_installed) {
        return;
    }
    /* Headless CI (GUI selftest #50) runs under ASan+UBSan, which install their OWN SIGSEGV/SIGABRT
     * handlers to print their reports -- overriding those would mask sanitizer diagnostics. Headless
     * also must not need a writable app-data dir. So skip entirely, exactly like gui_log_file_install. */
    if (getenv("NTPACKER_GUI_HEADLESS") != NULL) {
        return;
    }

    /* Resolve <app-data>/crash and pre-build every path/string the handler will need. A failure here
     * leaves the marker/dump paths empty -> the handler degrades to a no-op writer, but we still
     * install the handler below so a crash re-raises the OS default cleanly. */
    char root[GUI_PATHS_MAX];
    char crash_dir[GUI_PATHS_MAX];
    int nd = gui_paths_app_data_root(root, sizeof root)
                 ? snprintf(crash_dir, sizeof crash_dir, "%s/crash", root)
                 : -1;
    if (nd > 0 && (size_t)nd < sizeof crash_dir && gui_paths_ensure_dir(crash_dir)) {
        /* Retain both the report root for D3 and the crash dir for handler-owned paths. Opening the
         * root is intentional: logs/ is a sibling of crash/, not inside it. */
        (void)snprintf(s_report_root, sizeof s_report_root, "%s", root);
        memcpy(s_crash_dir, crash_dir, (size_t)nd + 1);
        int nm = snprintf(s_marker_path, sizeof s_marker_path, "%s/last-run.crashed", crash_dir);
        if (nm <= 0 || (size_t)nm >= sizeof s_marker_path) {
            s_marker_path[0] = '\0';
        }
#ifdef _WIN32
        if (s_marker_path[0] != '\0' &&
            !nt_utf8_path_to_utf16(
                s_marker_path, s_marker_path_w,
                sizeof s_marker_path_w / sizeof s_marker_path_w[0])) {
            s_marker_path[0] = '\0';
            s_marker_path_w[0] = L'\0';
        }
#endif
        /* Per-run dump/backtrace path: SESSION-START timestamp + PID (both resolved here, never in the
         * handler -- strftime/snprintf/getpid are not needed on the signal path). The PID disambiguates
         * two instances that crash in the SAME second, so a dump never overwrites another run's. */
        time_t now = time(NULL);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        char stamp[24];
        (void)strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
#ifdef _WIN32
        int np = snprintf(s_dump_path, sizeof s_dump_path, "%s/crash-%s-%lu.dmp", crash_dir, stamp,
                          (unsigned long)GetCurrentProcessId());
        if (np <= 0 || (size_t)np >= sizeof s_dump_path) {
            s_dump_path[0] = '\0';
        } else if (!nt_utf8_path_to_utf16(
                       s_dump_path, s_dump_path_w,
                       sizeof s_dump_path_w / sizeof s_dump_path_w[0])) {
            s_dump_path[0] = '\0';
            s_dump_path_w[0] = L'\0';
        }
#else
        int np = snprintf(s_backtrace_path, sizeof s_backtrace_path, "%s/crash-%s-%ld.txt", crash_dir, stamp,
                          (long)getpid());
        if (np <= 0 || (size_t)np >= sizeof s_backtrace_path) {
            s_backtrace_path[0] = '\0';
        }
#endif
        char human[32];
        (void)strftime(human, sizeof human, "%Y-%m-%d %H:%M:%S", &tmv);
        int npr = snprintf(s_marker_prefix, sizeof s_marker_prefix, "ntpacker crashed -- session started %s\n", human);
        s_marker_prefix_len = (npr > 0 && (size_t)npr < sizeof s_marker_prefix) ? (size_t)npr : 0;
    }

#ifdef _WIN32
    (void)SetUnhandledExceptionFilter(crash_exception_filter);
#else
    /* Warm up backtrace() so its first call -- which may malloc via the dynamic loader to bring in
     * libgcc's unwinder -- happens NOW on the main thread, not inside the signal handler. */
    void *warm[1];
    (void)backtrace(warm, 1);
    static const int sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        (void)signal(sigs[i], crash_signal_handler);
    }
#endif
    /* Deliberately NO nt_log here: gui_crash_install runs before nt_engine_init, so the log subsystem
     * may not be up yet. The install is silent. */
    s_installed = true;
}

void gui_crash_clear_marker(void) {
    if (s_marker_path[0] == '\0') {
        return; /* never installed / no crash dir -> nothing to clear */
    }
    (void)nt_utf8_remove(
        s_marker_path); /* clean exit: drop marker so D3 only fires after a real crash */
}

/* True iff the marker file is present on disk. D3 runs long after any fault (next launch), so plain
 * stdio is fine here -- none of the signal-handler async-safety rules apply on this path. */
static bool crash_marker_exists(void) {
    FILE *f = nt_utf8_fopen(s_marker_path, "rb");
    if (f == NULL) {
        return false;
    }
    (void)fclose(f);
    return true;
}

/* Open `dir` in the OS file explorer, best-effort; returns true if the open was dispatched. Mirrors
 * gui_view_chrome.c's gui_open_url: Windows ShellExecuteW (shell32 -- already linked for
 * tinyfiledialogs), POSIX open/xdg-open. A failed open is non-fatal (the prompt already showed the
 * path, so the user can still reach the folder by hand) -- the caller logs the miss. */
static bool crash_open_folder(const char *dir) {
#ifdef _WIN32
    wchar_t wide[GUI_PATHS_MAX + 8];
    if (!nt_utf8_path_to_utf16(dir, wide, sizeof wide / sizeof wide[0])) {
        return false;
    }
    HINSTANCE rc =
        ShellExecuteW(NULL, L"open", wide, NULL, NULL, SW_SHOWNORMAL);
    return (INT_PTR)rc > 32; /* returns > 32 on success (path parameter, not shell) */
#else
    char quoted[GUI_PATHS_MAX * 4 + 4]; /* worst case: every byte an escaped ' -> 4x, + the two quotes */
    if (!gui_shell_squote(dir, quoted, sizeof quoted)) {
        return false;
    }
    char cmd[sizeof quoted + 32];
#if defined(__APPLE__)
    (void)snprintf(cmd, sizeof cmd, "open %s >/dev/null 2>&1 &", quoted);
#else
    (void)snprintf(cmd, sizeof cmd, "xdg-open %s >/dev/null 2>&1 &", quoted);
#endif
    /* Backgrounded (&) so startup never blocks on the file manager -> this only catches a failed shell
     * spawn (system() == -1 or non-zero), not a missing xdg-open; that is fine for a best-effort open. */
    return system(cmd) == 0;
#endif
}

void gui_crash_report_prompt(void) {
    /* Nothing to do when the crash dir never resolved: not installed, headless (gui_crash_install
     * no-ops there so s_marker_path stays empty), or the app-data dir could not be created. This
     * single empty-path guard is what keeps the GUI selftest / CI silent -- no dialog, no marker
     * churn -- so it must come first. */
    if (s_marker_path[0] == '\0') {
        return;
    }
    /* A clean previous run had its marker removed by gui_crash_clear_marker; a real crash left it. So
     * the marker's mere presence is the "the last run crashed" signal. */
    if (!crash_marker_exists()) {
        return;
    }
    /* Include every relevant path in the message so a failed open still tells the user where to look. */
    char msg[GUI_PATHS_MAX * 3 + 320];
    (void)snprintf(msg, sizeof msg,
                   "ntpacker closed unexpectedly last time.\n\n"
                   "Open the application-data folder so you can send the diagnostics to the developer?\n\n"
                   "Crash dump: %s\nLogs: %s/logs",
                   s_crash_dir, s_report_root);
    /* yesno / question, default YES. tinyfd returns 1 for yes, 0 for no. NOTE: with NO graphical dialog
     * backend (e.g. Linux without zenity/kdialog) tinyfd falls back to a BLOCKING stdin console prompt.
     * Accepted as a rare edge for a GL desktop app (which normally has a dialog backend); it never bites
     * CI, which is headless (guarded out above) or the selftest build (guarded out at the call site). */
    if (tinyfd_messageBox("ntpacker -- crash report", msg, "yesno", "question", 1) == 1) {
        if (!crash_open_folder(s_report_root)) {
            /* The prompt already showed the path, so this is non-fatal -- just record the miss. Safe to
             * log here: the report prompt runs on the main thread after the log is up (unlike the handler). */
            nt_log_error("ntpacker-gui: could not open the diagnostics folder '%s'", s_report_root);
        }
    }
    /* Clear on EITHER choice: the report offer is a one-shot, not a recurring nag. */
    gui_crash_clear_marker();
}

void gui_crash_selftest(void) {
    if (getenv("NTPACKER_GUI_HEADLESS") != NULL) {
        return; /* never fault under headless/CI, even if the hidden arg leaks in */
    }
    nt_log_error("ntpacker-gui: --selftest-crash -- deliberately faulting to exercise the crash handler");
    /* Deref a volatile null -> SIGSEGV (POSIX) / EXCEPTION_ACCESS_VIOLATION (Windows). volatile so the
     * optimizer cannot elide the store under -O2. */
    volatile int *p = NULL;
    *p = 0xC0FFEE;
    _Exit(3); /* unreachable in practice; makes the "must fault" intent explicit */
}
// #endregion
