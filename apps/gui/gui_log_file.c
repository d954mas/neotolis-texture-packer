#include "gui_log_file.h"

#include "gui_paths.h"

#include "log/nt_log.h"

#include "tinycthread.h" /* C11 mtx_t (vendored, same as gui_pack.c) -- the sink runs on pack threads */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// #region state
/* ~2 MB active cap, 3 rotated generations kept (ntpacker.log + .1/.2/.3 = <=4 files, bounded). */
#define GUI_LOG_CAP_BYTES (2 * 1024 * 1024)
#define GUI_LOG_KEEP 3

/* THREAD-SAFETY: nt_log's sink registry is lockless and the sink is NOT single-threaded -- during a
 * GUI pack, tp_pack's parallel-encode workers call nt_log on several threads, so gui_log_file_sink
 * runs concurrently. s_lock serializes the WHOLE sink body (the s_active/s_file check, rotation,
 * fwrite, fflush, s_bytes) so a rotation on one thread can't fclose the FILE another is writing.
 * Initialized in install() (single-threaded, before add_sink); taken by shutdown() too. Nothing under
 * the lock ever calls nt_log -> no re-entrant deadlock. */
static mtx_t s_lock;
static FILE *s_file;
static bool s_active; /* master flag: true only while fully installed; also gates late calls into no-ops */
static long s_bytes;  /* running size of the active log, so rotation is a compare not a stat per line */
static char s_log_dir[GUI_PATHS_MAX];  /* <app-data>/logs */
static char s_log_path[GUI_PATHS_MAX]; /* <app-data>/logs/ntpacker.log */
// #endregion

// #region rotation
/* idx 0 == the active log; 1..GUI_LOG_KEEP == rotated generations. Always fits: install() proved
 * s_log_dir leaves room for the longest of these before opening anything. */
static void build_slot_path(char *out, size_t n, int idx) {
    if (idx == 0) {
        (void)snprintf(out, n, "%s/ntpacker.log", s_log_dir);
    } else {
        (void)snprintf(out, n, "%s/ntpacker.%d.log", s_log_dir, idx);
    }
}

/* Roll ntpacker.log -> .1 -> .2 -> .3, dropping the oldest. Caller must have closed s_file first
 * (Windows can't rename an open file). Highest-index first so every rename target is already vacated;
 * a missing source just makes rename a no-op. */
static void rotate_files(void) {
    char from[GUI_PATHS_MAX];
    char to[GUI_PATHS_MAX];
    build_slot_path(to, sizeof to, GUI_LOG_KEEP);
    (void)remove(to);
    for (int i = GUI_LOG_KEEP - 1; i >= 0; i--) {
        build_slot_path(from, sizeof from, i);
        build_slot_path(to, sizeof to, i + 1);
        (void)rename(from, to);
    }
}

/* Open the active log for append (binary: exact byte accounting + no CRLF translation) and seed
 * s_bytes from its current size. Returns false if it can't be opened. */
static bool open_active(void) {
    s_file = fopen(s_log_path, "ab");
    if (s_file == NULL) {
        return false;
    }
    s_bytes = 0;
    if (fseek(s_file, 0, SEEK_END) == 0) {
        long pos = ftell(s_file);
        if (pos > 0) {
            s_bytes = pos;
        }
    }
    return true;
}

/* The close+rotate+reopen dance, shared by install() and the sink. On return true the active file is
 * open and under cap. On return false file logging is DEAD (s_file == NULL): either the reopen failed,
 * or -- the P-finding -- the rename didn't actually shrink the active file (e.g. a tailing editor holds
 * it open on Windows), so we refuse to spin re-rotating a still-full file every line. */
static bool rotate_and_reopen(void) {
    if (s_file != NULL) {
        (void)fclose(s_file);
        s_file = NULL;
    }
    rotate_files();
    if (!open_active()) {
        return false;
    }
    if (s_bytes >= GUI_LOG_CAP_BYTES) {
        (void)fclose(s_file); /* rotation left us still over cap -> give up, don't thrash */
        s_file = NULL;
        return false;
    }
    return true;
}
// #endregion

// #region sink
/* Receives the already-formatted line (nt_log did the vsnprintf + truncation). We only prepend a
 * timestamp and reproduce the console's "LEVEL [domain] msg" shape, then fflush so a crash keeps the
 * tail. Entire body under s_lock (see the state note). NT_LOG_BUF_SIZE caps msg; +128 covers framing. */
static void gui_log_file_sink(nt_log_level_t level, const char *domain, const char *msg, void *user) {
    (void)user;
    mtx_lock(&s_lock);
    if (!s_active || s_file == NULL) {
        mtx_unlock(&s_lock);
        return;
    }
    static const char *const level_names[] = {"INFO", "WARN", "ERROR"};
    size_t li = (size_t)level;
    const char *lvl = (li < 3) ? level_names[li] : "LOG";

    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    (void)strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    char line[NT_LOG_BUF_SIZE + 128];
    int nprint;
    if (domain != NULL && domain[0] != '\0') {
        nprint = snprintf(line, sizeof line, "%s %s [%s] %s\n", ts, lvl, domain, msg);
    } else {
        nprint = snprintf(line, sizeof line, "%s %s %s\n", ts, lvl, msg);
    }
    if (nprint < 0) {
        mtx_unlock(&s_lock);
        return;
    }
    size_t len = (nprint < (int)sizeof line) ? (size_t)nprint : (sizeof line - 1); /* snprintf truncated an over-long line */

    /* Rotate before appending if this line would cross the cap (s_bytes>0 so a lone giant line --
     * impossible given the caps -- can't spin on an empty file). A dead rotation disables us. */
    if (s_bytes > 0 && s_bytes + (long)len >= GUI_LOG_CAP_BYTES) {
        if (!rotate_and_reopen()) {
            s_active = false; /* rotation is stuck -> stop file logging; console still works */
            mtx_unlock(&s_lock);
            return;
        }
    }
    size_t wrote = fwrite(line, 1, len, s_file);
    s_bytes += (long)wrote; /* credit only what actually landed, so the counter never drifts */
    if (wrote < len) {
        /* short write (e.g. full disk): stop rather than keep emitting torn lines + a wrong counter */
        (void)fclose(s_file);
        s_file = NULL;
        s_active = false;
        mtx_unlock(&s_lock);
        return;
    }
    (void)fflush(s_file);
    mtx_unlock(&s_lock);
}
// #endregion

// #region lifecycle
void gui_log_file_install(void) {
    if (s_active) {
        return;
    }
    /* Headless CI (GUI selftest #50) must not need a writable app-data dir -> no file logging at all. */
    if (getenv("NTPACKER_GUI_HEADLESS") != NULL) {
        return;
    }
    char root[GUI_PATHS_MAX];
    if (!gui_paths_app_data_root(root, sizeof root)) {
        return;
    }
    int nd = snprintf(s_log_dir, sizeof s_log_dir, "%s/logs", root);
    if (nd < 0 || (size_t)nd >= sizeof s_log_dir) {
        return; /* path truncated -> don't open a wrong/truncated file */
    }
    /* Need room for the LONGEST derived path ("<dir>/ntpacker.3.log"), else a rotated slot truncates. */
    if (strlen(s_log_dir) + strlen("/ntpacker.3.log") + 1 > sizeof s_log_path) {
        return;
    }
    if (!gui_paths_ensure_dir(s_log_dir)) {
        return; /* no writable dir -> degrade to console-only */
    }
    (void)snprintf(s_log_path, sizeof s_log_path, "%s/ntpacker.log", s_log_dir); /* guaranteed to fit */

    if (!open_active()) {
        return;
    }
    if (s_bytes >= GUI_LOG_CAP_BYTES) {  /* already over cap from a previous run -> rotate at startup */
        if (!rotate_and_reopen()) {
            return; /* couldn't rotate (s_file already NULL) -> console-only */
        }
    }
    /* Init the lock BEFORE registering the sink (install runs single-threaded; the sink can only fire
     * once it is registered, by which point the lock is live). */
    if (mtx_init(&s_lock, mtx_plain) != thrd_success) {
        (void)fclose(s_file);
        s_file = NULL;
        return;
    }
    s_active = true;
    nt_log_add_sink(gui_log_file_sink, NULL);
    /* Not under s_lock: this logs, which fans into the sink (which takes s_lock) -> would self-deadlock. */
    nt_log_info("ntpacker-gui: file log -> %s", s_log_path);
}

void gui_log_file_shutdown(void) {
    if (!s_active) {
        return; /* never installed / already down -> mutex was never initialized, don't touch it */
    }
    /* Under the lock: any in-flight worker sink call finishes first, then all later ones no-op. */
    mtx_lock(&s_lock);
    s_active = false;
    if (s_file != NULL) {
        (void)fclose(s_file);
        s_file = NULL;
    }
    mtx_unlock(&s_lock);
    nt_log_remove_sink(gui_log_file_sink, NULL); /* no more fan-out into the sink after this */
    mtx_destroy(&s_lock);
}
// #endregion
