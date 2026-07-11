#include "gui_scan.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
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

// #region growable entry vector (scan-local)
typedef struct scan_vec {
    gui_scan_entry *data;
    int count;
    int cap;
} scan_vec;

static bool scan_vec_push(scan_vec *v, const char *rel, const char *abs, long long size, long long mtime) {
    if (v->count == v->cap) {
        int ncap = (v->cap == 0) ? 32 : v->cap * 2;
        gui_scan_entry *nd = (gui_scan_entry *)realloc(v->data, (size_t)ncap * sizeof *nd);
        if (!nd) {
            return false;
        }
        v->data = nd;
        v->cap = ncap;
    }
    gui_scan_entry *e = &v->data[v->count];
    (void)snprintf(e->rel, sizeof e->rel, "%s", rel);
    (void)snprintf(e->abs, sizeof e->abs, "%s", abs);
    e->size = size;
    e->mtime = mtime;
    v->count++;
    return true;
}
// #endregion

// #region helpers
static bool has_image_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    /* case-insensitive compare against the accepted set */
    static const char *exts[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga"};
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        const char *a = dot;
        const char *b = exts[i];
        bool eq = true;
        while (*a && *b) {
            char ca = *a;
            char cb = *b;
            if (ca >= 'A' && ca <= 'Z') {
                ca = (char)(ca - 'A' + 'a');
            }
            if (ca != cb) {
                eq = false;
                break;
            }
            a++;
            b++;
        }
        if (eq && *a == '\0' && *b == '\0') {
            return true;
        }
    }
    return false;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const gui_scan_entry *)a)->rel, ((const gui_scan_entry *)b)->rel);
}
// #endregion

// #region platform recursion
/* Recurse `abs_dir` (physical path) accumulating image files; `rel_prefix` is the
 * '/'-normalized path from the scan root (empty at the top). */
static void scan_dir(const char *abs_dir, const char *rel_prefix, scan_vec *out) {
#ifdef _WIN32
    char pattern[512];
    (void)snprintf(pattern, sizeof pattern, "%s\\*", abs_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        char child_abs[512];
        (void)snprintf(child_abs, sizeof child_abs, "%s\\%s", abs_dir, name);
        char child_rel[256];
        if (rel_prefix[0] != '\0') {
            (void)snprintf(child_rel, sizeof child_rel, "%s/%s", rel_prefix, name);
        } else {
            (void)snprintf(child_rel, sizeof child_rel, "%s", name);
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_dir(child_abs, child_rel, out);
        } else if (has_image_ext(name)) {
            const long long sz = ((long long)fd.nFileSizeHigh << 32) | (long long)fd.nFileSizeLow;
            const long long mt =
                ((long long)fd.ftLastWriteTime.dwHighDateTime << 32) | (long long)fd.ftLastWriteTime.dwLowDateTime;
            (void)scan_vec_push(out, child_rel, child_abs, sz, mt);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(abs_dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        char child_abs[512];
        (void)snprintf(child_abs, sizeof child_abs, "%s/%s", abs_dir, name);
        char child_rel[256];
        if (rel_prefix[0] != '\0') {
            (void)snprintf(child_rel, sizeof child_rel, "%s/%s", rel_prefix, name);
        } else {
            (void)snprintf(child_rel, sizeof child_rel, "%s", name);
        }
        struct stat st;
        if (stat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            scan_dir(child_abs, child_rel, out);
        } else if (has_image_ext(name)) {
            (void)scan_vec_push(out, child_rel, child_abs, (long long)st.st_size, (long long)st.st_mtime);
        }
    }
    closedir(d);
#endif
}
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
    /* miss: scan, then insert into a free (or round-robin evicted) slot */
    scan_vec v = {0};
    scan_dir(abs_dir, "", &v);
    if (v.count > 1) {
        qsort(v.data, (size_t)v.count, sizeof *v.data, entry_cmp);
    }

    int slot = -1;
    for (int i = 0; i < GUI_SCAN_CACHE_CAP; i++) {
        if (!s_cache[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = (int)(s_evict_cursor++ % GUI_SCAN_CACHE_CAP);
        free(s_cache[slot].result.entries);
    }
    (void)snprintf(s_cache[slot].dir, sizeof s_cache[slot].dir, "%s", abs_dir);
    s_cache[slot].result.entries = v.data;
    s_cache[slot].result.count = v.count;
    s_cache[slot].used = true;
    return &s_cache[slot].result;
}

bool gui_scan_is_dir(const char *abs) {
    if (!abs || abs[0] == '\0') {
        return false;
    }
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(abs);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(abs, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool gui_scan_exists(const char *abs) {
    if (!abs || abs[0] == '\0') {
        return false;
    }
#ifdef _WIN32
    return GetFileAttributesA(abs) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(abs, &st) == 0;
#endif
}

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
            free(s_cache[i].result.entries);
            s_cache[i].result.entries = NULL;
            s_cache[i].result.count = 0;
            s_cache[i].used = false;
            s_cache[i].dir[0] = '\0';
        }
    }
}

void gui_scan_shutdown(void) { gui_scan_invalidate_all(); }
// #endregion
