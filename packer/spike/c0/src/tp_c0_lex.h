#ifndef TP_C0_LEX_H
#define TP_C0_LEX_H

/*
 * C0-01 internal lexing helpers -- NOT installed (lives in src/, not include/).
 * Shared by tp_c0_srckey.c and tp_c0_path.c: ASCII case/class helpers and a
 * next-component iterator that splits a path-like string on '/' (and, when
 * `backslash_sep`, also '\\'), skipping empty components (runs of separators).
 * Each caller layers its own '.'/'..'/emit policy on top. These `static inline`
 * helpers replace three hand-rolled split loops with zero behavior change.
 */

#include <stdbool.h>
#include <stddef.h>

static inline bool tp_c0_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline char tp_c0_ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static inline char tp_c0_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Component iterator over a path-like string. */
typedef struct tp_c0_lex {
    const char *p;
    bool backslash_sep; /* treat '\\' as a separator in addition to '/' */
} tp_c0_lex;

static inline tp_c0_lex tp_c0_lex_begin(const char *s, bool backslash_sep) {
    tp_c0_lex it;
    it.p = s;
    it.backslash_sep = backslash_sep;
    return it;
}

static inline bool tp_c0_lex_is_sep(const tp_c0_lex *it, char c) {
    return c == '/' || (it->backslash_sep && c == '\\');
}

/* Advance to the next NON-EMPTY component (runs of separators are skipped). On
 * success sets out_start and out_len (len >= 1) and returns true; returns false
 * at the end of the string. */
static inline bool tp_c0_lex_next(tp_c0_lex *it, const char **out_start, size_t *out_len) {
    const char *p = it->p;
    while (*p != '\0' && tp_c0_lex_is_sep(it, *p)) {
        p++;
    }
    if (*p == '\0') {
        it->p = p;
        return false;
    }
    const char *start = p;
    while (*p != '\0' && !tp_c0_lex_is_sep(it, *p)) {
        p++;
    }
    *out_start = start;
    *out_len = (size_t)(p - start);
    it->p = p;
    return true;
}

#endif /* TP_C0_LEX_H */
