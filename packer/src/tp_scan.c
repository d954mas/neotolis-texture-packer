#include "tp_core/tp_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

// #region growable entry vector (scan-local)
typedef struct scan_vec {
    tp_scan_entry *data;
    int count;
    int cap;
} scan_vec;

static bool scan_vec_push(scan_vec *v, const char *rel, const char *abs, long long size, long long mtime) {
    if (v->count == v->cap) {
        int ncap = (v->cap == 0) ? 32 : v->cap * 2;
        tp_scan_entry *nd = (tp_scan_entry *)realloc(v->data, (size_t)ncap * sizeof *nd);
        if (!nd) {
            return false;
        }
        v->data = nd;
        v->cap = ncap;
    }
    tp_scan_entry *e = &v->data[v->count];
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
    return strcmp(((const tp_scan_entry *)a)->rel, ((const tp_scan_entry *)b)->rel);
}

/* R5b-2: append a heap-dup of `name` to the string list, growing the backing array as needed. On any
 * allocation failure the list is left unchanged (the walk skips that entry -- a best-effort listing,
 * mirroring scan_vec_push above). */
static bool str_list_push(tp_str_list *v, const char *name) {
    if (v->count == v->cap) {
        int ncap = (v->cap == 0) ? 16 : v->cap * 2;
        char **nd = (char **)realloc(v->items, (size_t)ncap * sizeof *nd);
        if (!nd) {
            return false;
        }
        v->items = nd;
        v->cap = ncap;
    }
    size_t n = strlen(name) + 1U;
    char *copy = (char *)malloc(n);
    if (!copy) {
        return false;
    }
    memcpy(copy, name, n);
    v->items[v->count++] = copy;
    return true;
}

/* True iff `name` ends with `suffix` (case-sensitive byte compare; "" matches everything). */
static bool name_has_suffix(const char *name, const char *suffix) {
    size_t ln = strlen(name);
    size_t ls = strlen(suffix);
    if (ls == 0) {
        return true;
    }
    if (ln < ls) {
        return false;
    }
    return memcmp(name + (ln - ls), suffix, ls) == 0;
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
void tp_scan_dir(const char *abs_dir, tp_scan_result *out) {
    if (!out) {
        return;
    }
    out->entries = NULL;
    out->count = 0;
    if (!abs_dir || abs_dir[0] == '\0') {
        return;
    }
    scan_vec v = {0};
    scan_dir(abs_dir, "", &v);
    if (v.count > 1) {
        qsort(v.data, (size_t)v.count, sizeof *v.data, entry_cmp);
    }
    out->entries = v.data;
    out->count = v.count;
}

void tp_scan_free(tp_scan_result *out) {
    if (!out) {
        return;
    }
    free(out->entries);
    out->entries = NULL;
    out->count = 0;
}

bool tp_scan_list_dir(const char *dir, const char *suffix, tp_str_list *out) {
    if (!out || !dir || dir[0] == '\0') {
        return false;
    }
    const char *suf = suffix ? suffix : "";
#ifdef _WIN32
    char pattern[1088]; /* app-data recovery folder (< GUI_PATHS_MAX) + "\\*"; truncation -> open fails -> false */
    int np = snprintf(pattern, sizeof pattern, "%s\\*", dir);
    if (np <= 0 || (size_t)np >= sizeof pattern) {
        return false;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false; /* dir-open failure -> out left as-is */
    }
    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue; /* regular files only */
        }
        if (!name_has_suffix(name, suf)) {
            continue;
        }
        if (!str_list_push(out, name)) {
            /* R5b-2 fix [8]: an OOM appending a name would return a PARTIAL listing that could silently
             * miss the newest orphan. Fail CLOSED exactly like a dir-open failure: drop the partial list
             * (no leak) and return false so the recovery scan degrades to "no recovery this launch". */
            tp_str_list_free(out);
            FindClose(h);
            return false;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    DIR *d = opendir(dir);
    if (!d) {
        return false; /* dir-open failure -> out left as-is */
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        char child_abs[1400]; /* dir (< GUI_PATHS_MAX) + '/' + name (<= NAME_MAX); truncation -> stat fails -> skip */
        int nc = snprintf(child_abs, sizeof child_abs, "%s/%s", dir, name);
        if (nc <= 0 || (size_t)nc >= (int)sizeof child_abs) {
            continue;
        }
        struct stat st;
        if (stat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            continue; /* regular files only (skip subdirectories) */
        }
        if (!name_has_suffix(name, suf)) {
            continue;
        }
        if (!str_list_push(out, name)) {
            /* R5b-2 fix [8]: OOM -> fail CLOSED (see the Win32 branch): drop the partial list + return
             * false so the caller treats it as "listing failed -> no recovery this launch". */
            tp_str_list_free(out);
            closedir(d);
            return false;
        }
    }
    closedir(d);
    return true;
#endif
}

void tp_str_list_free(tp_str_list *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->count; i++) {
        free(out->items[i]);
    }
    free(out->items);
    out->items = NULL;
    out->count = 0;
    out->cap = 0;
}

bool tp_scan_is_dir(const char *abs) {
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

bool tp_scan_exists(const char *abs) {
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

#ifdef _WIN32
#define TP_MKDIR_ONE(p) (void)CreateDirectoryA((p), NULL)
#else
#define TP_MKDIR_ONE(p) (void)mkdir((p), 0755)
#endif

void tp_mkdirs(const char *dir) {
    if (!dir || dir[0] == '\0') {
        return;
    }
    char tmp[1024];
    (void)snprintf(tmp, sizeof tmp, "%s", dir);
    /* strip trailing separators (but keep a lone leading '/'). */
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[--len] = '\0';
    }
    /* create each intermediate directory, then the leaf. `q != tmp` skips a
     * leading '/' (POSIX absolute) and never mkdir's the empty string. */
    for (char *q = tmp; *q != '\0'; q++) {
        if ((*q == '/' || *q == '\\') && q != tmp) {
            const char sep = *q;
            *q = '\0';
            TP_MKDIR_ONE(tmp);
            *q = sep;
        }
    }
    TP_MKDIR_ONE(tmp);
}

void tp_mkdirs_parent(const char *file_path) {
    if (!file_path) {
        return;
    }
    char tmp[1024];
    (void)snprintf(tmp, sizeof tmp, "%s", file_path);
    char *last = strrchr(tmp, '/');
    char *lb = strrchr(tmp, '\\');
    if (lb && (!last || lb > last)) {
        last = lb;
    }
    if (!last) {
        return; /* no directory component */
    }
    *last = '\0';
    tp_mkdirs(tmp);
}
// #endregion
