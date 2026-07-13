#ifndef TP_STRUTIL_H
#define TP_STRUTIL_H

/*
 * Shared owned-string + path primitives for the tp_core src/ TUs (fix [8]). ONE
 * definition of each, so tp_project.c, tp_input.c, tp_sprite_index.c, and
 * tp_project_migrate.c stop re-implementing the same three helpers (they had drifted
 * into dup_str / base_name / slash_norm / mig_strdup / tp_strdup copies). Header-only
 * static inline, same pattern as tp_hex.h -- a private packer/src header, NOT a public
 * tp_core API (frontends link only the public headers and keep their own copies).
 *
 * Behaviour is byte-for-byte identical to the copies these replace: no allocation
 * strategy, iteration order, or output changes.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

/* Tolerant fallback normalizer: copy `src` into `out` (capacity `cap`) replacing '\\'
 * with '/', always NUL-terminated. Used when the real canonicalizer (tp_srckey_normalize)
 * rejects a path, so the value still flows through instead of vanishing/aborting. */
static inline void tp_slash_norm(const char *src, char *out, size_t cap) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1U < cap; i++) {
        out[i] = (src[i] == '\\') ? '/' : src[i];
    }
    out[i] = '\0';
}

#endif /* TP_STRUTIL_H */
