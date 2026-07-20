#ifndef TP_IDENTITY_INTERNAL_H
#define TP_IDENTITY_INTERNAL_H

/*
 * tp_identity internal seam -- lives in src/ (NOT installed). It exposes the
 * host-PARAMETERIZED lexical canonicalizer + the tiny ASCII/lex helpers.
 *
 * WHY a host parameter here when the PUBLIC API (tp_identity.h) is native-host
 * (#if defined(_WIN32))?  The public surface is native-host as required,
 * but the lexical rules are byte-deterministic for BOTH POSIX and Windows on
 * every OS. Keeping a host-parameterized core lets the production tests run the
 * full cross-OS golden corpus on a single CI runner -- preserving the spike's
 * "no platform-specific golden output" property for the lexical half. The
 * realpath half is genuinely OS-dependent and is NOT parameterized.
 *
 * PROMOTE-vs-SHARE (architecture decision, lead review): the spike stays as the
 * frozen contract fixture; production got its OWN adapted copy of the lexical
 * logic (spike is never linked into tp_core -- it is deliberately out of the
 * tp_core/tp_build/apps link closure). Drift between the two copies is caught by
 * porting the identical golden vectors into packer/tests: if the
 * production copy ever diverges from the pinned rules, a ported vector fails.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_error.h"

/* Lexical rule set. Both are exercised on every OS by the production tests. */
typedef enum tp_host {
    TP_HOST_POSIX = 0,
    TP_HOST_WINDOWS = 1
} tp_host;

/* The host rule set of the current build (#if defined(_WIN32)). */
tp_host tp_host_native(void);

/* Lexically canonicalize an ABSOLUTE project path under `host` (touches no
 * filesystem). Implements master spec §5.1 and decision 0006; tp_status errors:
 *   - not absolute        -> TP_STATUS_PATH_NOT_ABSOLUTE
 *   - Windows "C:foo"     -> TP_STATUS_PATH_DRIVE_RELATIVE
 *   - malformed UNC       -> TP_STATUS_PATH_BAD_UNC
 *   - Windows device path -> TP_STATUS_PATH_DEVICE
 *   - empty / NULL        -> TP_STATUS_INVALID_ARGUMENT
 *   - too long for `cap`  -> TP_STATUS_OUT_OF_BOUNDS
 * Output uses '/'; the Windows drive letter is upper-cased; all other case is
 * preserved. */
tp_status tp_path_canonical_lexical(const char *input, tp_host host, char *out, size_t cap, tp_error *err);

/* Identity equality of two ALREADY-canonical paths under host case policy:
 * POSIX byte-exact; Windows folds ASCII case. */
bool tp_path_equal_host(const char *canon_a, const char *canon_b, tp_host host);

/* ----- tiny ASCII + path-component helpers ------------------------------- */

static inline bool tp_ident_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline char tp_ident_ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static inline char tp_ident_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Component iterator over a path-like string; skips empty components (runs of
 * separators). Each caller layers its own '.'/'..'/emit policy on top. */
typedef struct tp_ident_lex {
    const char *p;
    bool backslash_sep; /* treat '\\' as a separator in addition to '/' */
} tp_ident_lex;

static inline tp_ident_lex tp_ident_lex_begin(const char *s, bool backslash_sep) {
    tp_ident_lex it;
    it.p = s;
    it.backslash_sep = backslash_sep;
    return it;
}

static inline bool tp_ident_lex_is_sep(const tp_ident_lex *it, char c) {
    return c == '/' || (it->backslash_sep && c == '\\');
}

static inline bool tp_ident_lex_next(tp_ident_lex *it, const char **out_start, size_t *out_len) {
    const char *p = it->p;
    while (*p != '\0' && tp_ident_lex_is_sep(it, *p)) {
        p++;
    }
    if (*p == '\0') {
        it->p = p;
        return false;
    }
    const char *start = p;
    while (*p != '\0' && !tp_ident_lex_is_sep(it, *p)) {
        p++;
    }
    *out_start = start;
    *out_len = (size_t)(p - start);
    it->p = p;
    return true;
}

#endif /* TP_IDENTITY_INTERNAL_H */
