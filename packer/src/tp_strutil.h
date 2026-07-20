#ifndef TP_STRUTIL_H
#define TP_STRUTIL_H

/*
 * Shared owned-string + path primitives for the tp_core src/ TUs: ONE definition
 * each so tp_project.c, tp_input.c, tp_sprite_index.c, and project identity code
 * don't re-implement them. Header-only static inline (like tp_hex.h) -- a private
 * packer/src header, NOT a public tp_core API; frontends keep their own copies.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_error.h" /* tp_status (tp_set_owned_dup) */

/* OOM-safe owned-string field swap (dup-then-commit at field granularity). Duplicate `src`
 * through `dup` -- which MUST return NULL only on an allocation failure -- and only once that
 * succeeds free the old *slot and store the copy, so a failed dup leaves *slot (and the whole
 * enclosing model) BYTE-UNCHANGED. Returns TP_STATUS_OK, else TP_STATUS_OOM.
 *
 * `dup` is a parameter (not a fixed allocator) so each caller routes this swap through its own
 * OOM fault seam while sharing ONE dup->check->free->assign definition; error-message reporting
 * stays at the call site. */
static inline tp_status tp_set_owned_dup(char **slot, const char *src, char *(*dup)(const char *)) {
    char *copy = dup(src);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    free(*slot);
    *slot = copy;
    return TP_STATUS_OK;
}

/* strdup without the _strdup / POSIX strdup portability split. NULL on NULL input or OOM. */
static inline char *tp_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

/* Last path component of a '/'- or '\\'-separated path (a file source's raw name).
 * Returns a pointer INTO `p` (not a copy); `p` must outlive the result. */
static inline const char *tp_path_basename(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') {
            b = q + 1;
        }
    }
    return b;
}

#endif /* TP_STRUTIL_H */
