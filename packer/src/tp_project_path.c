#include "tp_project_path_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define tp_getcwd _getcwd
#else
#include <unistd.h>
#define tp_getcwd getcwd
#endif

/* ======================================================================== */
/* path helpers                                                             */
/* ======================================================================== */

void tp_normalize_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

bool tp_path_is_absolute(const char *p) {
    if (!p || !p[0]) {
        return false;
    }
    if (p[0] == '/' || p[0] == '\\') {
        return true; /* POSIX root or Windows UNC / drive-relative root */
    }
    if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
        return true; /* Windows drive path */
    }
    return false;
}

/* Absolute, '/'-normalized directory that contains `path`. */
tp_status tp_abs_dir_of(const char *path, char *out, size_t cap) {
    char abs[TP_PATH_MAX];
    if (tp_path_is_absolute(path)) {
        if ((size_t)snprintf(abs, sizeof abs, "%s", path) >= sizeof abs) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
    } else {
        char cwd[TP_PATH_MAX];
        if (!tp_getcwd(cwd, (int)sizeof cwd)) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        if ((size_t)snprintf(abs, sizeof abs, "%s/%s", cwd, path) >= sizeof abs) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
    }
    tp_normalize_slashes(abs);

    char *slash = strrchr(abs, '/');
    if (slash) {
        *slash = '\0';
    } else {
        abs[0] = '.';
        abs[1] = '\0';
    }
    if ((size_t)snprintf(out, cap, "%s", abs) >= cap) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    return TP_STATUS_OK;
}

typedef enum tp_path_root_kind {
    TP_PATH_ROOT_RELATIVE = 0,
    TP_PATH_ROOT_POSIX,
    TP_PATH_ROOT_DRIVE,
    TP_PATH_ROOT_UNC,
    TP_PATH_ROOT_INVALID_UNC
} tp_path_root_kind;

typedef struct tp_path_root {
    tp_path_root_kind kind;
    size_t components;
    size_t first;
    size_t first_len;
    size_t second;
    size_t second_len;
} tp_path_root;

typedef struct tp_path_component_iter {
    const char *path;
    size_t pos;
} tp_path_component_iter;

typedef struct tp_path_component {
    size_t start;
    size_t len;
} tp_path_component;

static size_t tp_skip_path_separators(const char *path, size_t pos) {
    while (path[pos] == '/') {
        pos++;
    }
    return pos;
}

static tp_path_root tp_classify_path_root(const char *path) {
    tp_path_root root = {0};
    if (isalpha((unsigned char)path[0]) && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\0')) {
        root.kind = TP_PATH_ROOT_DRIVE;
        root.first = 0U;
        root.first_len = 2U;
        root.components = tp_skip_path_separators(path, 2U);
        return root;
    }
    if (path[0] == '/' && path[1] == '/') {
        size_t pos = tp_skip_path_separators(path, 2U);
        root.kind = TP_PATH_ROOT_INVALID_UNC;
        root.first = pos;
        while (path[pos] != '\0' && path[pos] != '/') {
            pos++;
        }
        root.first_len = pos - root.first;
        pos = tp_skip_path_separators(path, pos);
        root.second = pos;
        while (path[pos] != '\0' && path[pos] != '/') {
            pos++;
        }
        root.second_len = pos - root.second;
        if (root.first_len != 0U && root.second_len != 0U) {
            root.kind = TP_PATH_ROOT_UNC;
            root.components = tp_skip_path_separators(path, pos);
        }
        return root;
    }
    if (path[0] == '/') {
        root.kind = TP_PATH_ROOT_POSIX;
        root.components = tp_skip_path_separators(path, 0U);
        return root;
    }
    root.kind = TP_PATH_ROOT_RELATIVE;
    root.components = 0U;
    return root;
}

static bool tp_path_span_eq(const char *a, size_t a_start, size_t a_len,
                            const char *b, size_t b_start, size_t b_len,
                            bool insensitive) {
    if (a_len != b_len) {
        return false;
    }
    for (size_t i = 0U; i < a_len; i++) {
        const unsigned char ac = (unsigned char)a[a_start + i];
        const unsigned char bc = (unsigned char)b[b_start + i];
        if (insensitive ? tolower(ac) != tolower(bc) : ac != bc) {
            return false;
        }
    }
    return true;
}

static bool tp_path_roots_eq(const char *a, const tp_path_root *ar,
                             const char *b, const tp_path_root *br) {
    if (ar->kind != br->kind || ar->kind == TP_PATH_ROOT_INVALID_UNC) {
        return false;
    }
    if (ar->kind == TP_PATH_ROOT_DRIVE) {
        return tp_path_span_eq(a, ar->first, ar->first_len, b, br->first,
                               br->first_len, true);
    }
    if (ar->kind == TP_PATH_ROOT_UNC) {
        return tp_path_span_eq(a, ar->first, ar->first_len, b, br->first,
                               br->first_len, true) &&
               tp_path_span_eq(a, ar->second, ar->second_len, b, br->second,
                               br->second_len, true);
    }
    return true;
}

static bool tp_path_component_next(tp_path_component_iter *iter,
                                   tp_path_component *component) {
    size_t pos = tp_skip_path_separators(iter->path, iter->pos);
    if (iter->path[pos] == '\0') {
        iter->pos = pos;
        return false;
    }
    component->start = pos;
    while (iter->path[pos] != '\0' && iter->path[pos] != '/') {
        pos++;
    }
    component->len = pos - component->start;
    iter->pos = pos;
    return true;
}

static tp_status tp_relativize_append(char *out, size_t cap, size_t *used,
                                      const char *text, size_t length) {
    const size_t separator = *used == 0U ? 0U : 1U;
    if (separator > cap - *used || length >= cap - *used - separator) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    if (separator != 0U) {
        out[(*used)++] = '/';
    }
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return TP_STATUS_OK;
}

/* Rewrites `abs` (an absolute, '/'-normalized path) relative to `base_dir`
 * (likewise). Falls back to copying `abs` when the two live on different roots
 * (different Windows drives / drive-vs-rootless) -- the caller's notice concern
 * (ux.md §3.6.3). Result is '/'-normalized. */
tp_status tp_relativize(const char *abs, const char *base_dir, char *out,
                               size_t cap) {
    if (!abs || !base_dir || !out || cap == 0U) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    char abuf[TP_PATH_MAX];
    char bbuf[TP_PATH_MAX];
    if ((size_t)snprintf(abuf, sizeof abuf, "%s", abs) >= sizeof abuf ||
        (size_t)snprintf(bbuf, sizeof bbuf, "%s", base_dir) >= sizeof bbuf) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_normalize_slashes(abuf);
    tp_normalize_slashes(bbuf);

    const tp_path_root ar = tp_classify_path_root(abuf);
    const tp_path_root br = tp_classify_path_root(bbuf);
    if (!tp_path_roots_eq(abuf, &ar, bbuf, &br)) {
        if ((size_t)snprintf(out, cap, "%s", abuf) >= cap) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        return TP_STATUS_OK;
    }

    tp_path_component_iter ai = {abuf, ar.components};
    tp_path_component_iter bi = {bbuf, br.components};
    bool component_insensitive =
        ar.kind == TP_PATH_ROOT_DRIVE || ar.kind == TP_PATH_ROOT_UNC;
#ifdef _WIN32
    component_insensitive = true;
#endif
    for (;;) {
        const tp_path_component_iter ai_before = ai;
        const tp_path_component_iter bi_before = bi;
        tp_path_component ac = {0};
        tp_path_component bc = {0};
        const bool have_a = tp_path_component_next(&ai, &ac);
        const bool have_b = tp_path_component_next(&bi, &bc);
        if (!have_a || !have_b ||
            !tp_path_span_eq(abuf, ac.start, ac.len, bbuf, bc.start, bc.len,
                             component_insensitive)) {
            if (have_a || have_b) {
                ai = ai_before;
                bi = bi_before;
            }
            break;
        }
    }

    out[0] = '\0';
    size_t used = 0U;
    tp_path_component component = {0};
    while (tp_path_component_next(&bi, &component)) {
        const tp_status status =
            tp_relativize_append(out, cap, &used, "..", 2U);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    while (tp_path_component_next(&ai, &component)) {
        const tp_status status = tp_relativize_append(
            out, cap, &used, abuf + component.start, component.len);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    if (used == 0U) {
        if (cap < 2) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        out[0] = '.';
        out[1] = '\0';
    }
    return TP_STATUS_OK;
}

tp_status tp_project__test_relativize(const char *abs, const char *base_dir,
                                      char *out, size_t cap) {
    return tp_relativize(abs, base_dir, out, cap);
}

/* ======================================================================== */
/* path resolve + packing bridge                                            */
/* ======================================================================== */

tp_status tp_project_resolve_path(const tp_project *p, const char *rel, char *out_abs, size_t cap) {
    if (!p || !rel || !out_abs || cap == 0) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (tp_path_is_absolute(rel)) {
        if ((size_t)snprintf(out_abs, cap, "%s", rel) >= cap) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        tp_normalize_slashes(out_abs);
        return TP_STATUS_OK;
    }
    if (!p->project_dir) {
        return TP_STATUS_INVALID_ARGUMENT; /* no base for a relative path */
    }
    if ((size_t)snprintf(out_abs, cap, "%s/%s", p->project_dir, rel) >= cap) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_normalize_slashes(out_abs);
    return TP_STATUS_OK;
}

tp_status tp_project_resolve_source_path(const tp_project *p, const char *rel,
                                         char *out_abs, size_t cap) {
    if (!p || !rel || !out_abs || cap == 0U) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (tp_path_is_absolute(rel)) {
        return tp_project_resolve_path(p, rel, out_abs, cap);
    }
    const char *base = p->source_base_dir ? p->source_base_dir : p->project_dir;
    if (!base) {
        return TP_STATUS_PATH_NOT_ABSOLUTE;
    }
    if ((size_t)snprintf(out_abs, cap, "%s/%s", base, rel) >= cap) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_normalize_slashes(out_abs);
    return TP_STATUS_OK;
}
