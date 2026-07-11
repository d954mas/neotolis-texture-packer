#include "gui_scan.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

// #region cache
/* Per-directory scan-result memoization, NOT a watched-folder registry: the folder SET lives in the
 * tp_project model (sources), never here. This cache only avoids re-walking a directory that was
 * scanned recently. Eviction is therefore benign -- a dropped entry costs one re-scan on next access,
 * never a lost folder. When full, the oldest slot is round-robin evicted (s_evict_cursor); every caller
 * (assemble/build_rows/fp_collect) fully consumes a gui_scan_result before the next gui_scan_get can
 * evict it (strings are copied out immediately -- no pointer survives an eviction), so eviction can
 * never dangle a live result either. A many-folder project (>32 dirs) only pays repeated re-scans
 * (perf), with no correctness or silent-drop risk. */
#define GUI_SCAN_CACHE_CAP 32

typedef struct gui_scan_cache_entry {
    char dir[512];
    gui_scan_result result;
    bool used;
} gui_scan_cache_entry;

static gui_scan_cache_entry s_cache[GUI_SCAN_CACHE_CAP];
static uint32_t s_evict_cursor; /* round-robin eviction when the cache is full (benign -- see the region note) */
// #endregion

// #region public API
const gui_scan_result *gui_scan_get(const char *abs_dir) {
    static const gui_scan_result empty = {NULL, 0};
    if (!abs_dir || abs_dir[0] == '\0') {
        return &empty;
    }
    for (int i = 0; i < GUI_SCAN_CACHE_CAP; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].dir, abs_dir) == 0) {
            return &s_cache[i].result;
        }
    }
    /* miss: scan (tp_scan_dir walks + sorts), then insert into a free (or round-robin
     * evicted) slot */
    tp_scan_result scanned;
    tp_scan_dir(abs_dir, &scanned);

    int slot = -1;
    for (int i = 0; i < GUI_SCAN_CACHE_CAP; i++) {
        if (!s_cache[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = (int)(s_evict_cursor++ % GUI_SCAN_CACHE_CAP);
        tp_scan_free(&s_cache[slot].result);
    }
    (void)snprintf(s_cache[slot].dir, sizeof s_cache[slot].dir, "%s", abs_dir);
    s_cache[slot].result = scanned;
    s_cache[slot].used = true;
    return &s_cache[slot].result;
}

bool gui_scan_is_dir(const char *abs) { return tp_scan_is_dir(abs); }

bool gui_scan_exists(const char *abs) { return tp_scan_exists(abs); }

bool gui_scan_stat(const char *abs, long long *out_size, long long *out_mtime) {
    if (!abs || abs[0] == '\0') {
        return false;
    }
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(abs, GetFileExInfoStandard, &fad)) {
        return false;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return false;
    }
    if (out_size) {
        *out_size = ((long long)fad.nFileSizeHigh << 32) | (long long)fad.nFileSizeLow;
    }
    if (out_mtime) {
        *out_mtime = ((long long)fad.ftLastWriteTime.dwHighDateTime << 32) | (long long)fad.ftLastWriteTime.dwLowDateTime;
    }
    return true;
#else
    struct stat st;
    if (stat(abs, &st) != 0 || S_ISDIR(st.st_mode)) {
        return false;
    }
    if (out_size) {
        *out_size = (long long)st.st_size;
    }
    if (out_mtime) {
        *out_mtime = (long long)st.st_mtime;
    }
    return true;
#endif
}

void gui_scan_invalidate_all(void) {
    for (int i = 0; i < GUI_SCAN_CACHE_CAP; i++) {
        if (s_cache[i].used) {
            tp_scan_free(&s_cache[i].result);
            s_cache[i].used = false;
            s_cache[i].dir[0] = '\0';
        }
    }
}

void gui_scan_shutdown(void) { gui_scan_invalidate_all(); }
// #endregion
