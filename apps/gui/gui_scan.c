#include "gui_scan.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char *dir;
    gui_scan_result result;
    bool used;
} gui_scan_cache_entry;

static gui_scan_cache_entry s_cache[GUI_SCAN_CACHE_CAP];
static uint32_t s_evict_cursor; /* round-robin eviction when the cache is full (benign -- see the region note) */
#if defined(NTPACKER_GUI_BENCH)
static gui_scan_bench_counters s_bench_counters;
#endif

static tp_status cache_store_owned(const char *abs_dir,
                                   gui_scan_result *owned,
                                   gui_scan_cache_entry **out_entry,
                                   tp_error *err) {
    *out_entry = NULL;
    int slot = -1;
    for (int i = 0; i < GUI_SCAN_CACHE_CAP; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].dir, abs_dir) == 0) {
            slot = i;
            break;
        }
        if (slot < 0 && !s_cache[i].used) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = (int)(s_evict_cursor++ % GUI_SCAN_CACHE_CAP);
    }
    size_t length = strlen(abs_dir);
    char *dir_copy = (char *)malloc(length + 1U);
    if (!dir_copy) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "GUI scan cache key allocation failed");
    }
    memcpy(dir_copy, abs_dir, length + 1U);
    if (s_cache[slot].used) {
        tp_scan_free(&s_cache[slot].result);
        free(s_cache[slot].dir);
    }
    s_cache[slot].dir = dir_copy;
    s_cache[slot].result = *owned;
    owned->entries = NULL;
    owned->count = 0;
    s_cache[slot].used = true;
    *out_entry = &s_cache[slot];
    return TP_STATUS_OK;
}
// #endregion

// #region public API
tp_status gui_scan_get(const char *abs_dir,
                       const gui_scan_result **out_result, tp_error *err) {
#if defined(NTPACKER_GUI_BENCH)
    s_bench_counters.get_calls++;
#endif
    if (out_result) {
        *out_result = NULL;
    }
    if (!abs_dir || abs_dir[0] == '\0' || !out_result) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "GUI scan requires a path and output");
    }
    for (int i = 0; i < GUI_SCAN_CACHE_CAP; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].dir, abs_dir) == 0) {
            *out_result = &s_cache[i].result;
            return TP_STATUS_OK;
        }
    }
    /* miss: scan (tp_scan_dir walks + sorts), then insert into a free (or round-robin
     * evicted) slot */
    tp_scan_result scanned = {0};
#if defined(NTPACKER_GUI_BENCH)
    s_bench_counters.directory_walks++;
#endif
    tp_status status = tp_scan_dir(abs_dir, &scanned, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    gui_scan_cache_entry *entry = NULL;
    status = cache_store_owned(abs_dir, &scanned, &entry, err);
    if (status != TP_STATUS_OK) {
        tp_scan_free(&scanned);
        return status;
    }
    *out_result = &entry->result;
    return TP_STATUS_OK;
}

bool gui_scan_is_dir(const char *abs) {
#if defined(NTPACKER_GUI_BENCH)
    s_bench_counters.is_dir_fs_calls++;
#endif
    return tp_scan_is_dir(abs);
}

bool gui_scan_exists(const char *abs) {
#if defined(NTPACKER_GUI_BENCH)
    s_bench_counters.exists_fs_calls++;
#endif
    return tp_scan_exists(abs);
}

tp_status gui_scan_classify_checked(const char *abs, tp_scan_kind *out,
                                    tp_error *err) {
#if defined(NTPACKER_GUI_BENCH)
    s_bench_counters.exists_fs_calls++;
#endif
    return tp_scan_classify_checked(abs, out, err);
}

bool gui_scan_stat(const char *abs, long long *out_size, long long *out_mtime) {
    return tp_scan_file_stat(abs, out_size, out_mtime);
}

void gui_scan_invalidate_all(void) {
    for (int i = 0; i < GUI_SCAN_CACHE_CAP; i++) {
        if (s_cache[i].used) {
            tp_scan_free(&s_cache[i].result);
            free(s_cache[i].dir);
            s_cache[i].dir = NULL;
            s_cache[i].used = false;
        }
    }
}

void gui_scan_shutdown(void) { gui_scan_invalidate_all(); }

#if defined(NTPACKER_GUI_BENCH)
bool gui_scan_bench_seed_owned(const char *abs_dir, gui_scan_result *owned_result) {
    if (!abs_dir || abs_dir[0] == '\0' || !owned_result ||
        owned_result->count < 0 || (owned_result->count > 0 && !owned_result->entries)) {
        return false;
    }
    gui_scan_cache_entry *entry = NULL;
    return cache_store_owned(abs_dir, owned_result, &entry, NULL) ==
           TP_STATUS_OK;
}

void gui_scan_bench_reset_counters(void) { memset(&s_bench_counters, 0, sizeof s_bench_counters); }

gui_scan_bench_counters gui_scan_bench_get_counters(void) { return s_bench_counters; }
#endif
// #endregion
