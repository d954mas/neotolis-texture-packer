#include "tp_core/tp_identity.h"

#include <stdio.h>
#include <string.h>

#include "tp_identity_internal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7+: GetFinalPathNameByHandleW */
#endif
#include <windows.h>
#else
#include <errno.h>
#include <stdlib.h> /* realpath */
#endif

/* ======================================================================== */
/* (a) LEXICAL canonicalization -- promoted/adapted from the accepted C0-01  */
/*     tp_c0_path.c. Host-parameterized so both rule sets run on every OS in  */
/*     the production tests; the spike's tp_c0_detail tokens become tp_status. */
/* ======================================================================== */

tp_host tp_host_native(void) {
#if defined(_WIN32)
    return TP_HOST_WINDOWS;
#else
    return TP_HOST_POSIX;
#endif
}

/* Append n bytes of `s` to out at *pos, guarding cap (leaves room for a later
 * NUL by requiring *pos + n + 1 <= cap). Returns false if it would overflow. */
static bool append(char *out, size_t cap, size_t *pos, const char *s, size_t n) {
    if (*pos + n + 1U > cap) {
        return false;
    }
    memcpy(out + *pos, s, n);
    *pos += n;
    return true;
}

tp_status tp_path_canonical_lexical(const char *input, tp_host host, char *out, size_t cap, tp_error *err) {
    if (!input) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "input path is NULL");
    }
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "out buffer is NULL");
    }
    if (input[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "empty path");
    }

    char work[TP_IDENTITY_PATH_MAX];
    size_t ilen = strlen(input);
    if (ilen >= sizeof work) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "path exceeds %zu bytes", sizeof work);
    }
    memcpy(work, input, ilen + 1U);
    if (host == TP_HOST_WINDOWS) {
        for (size_t i = 0; i < ilen; i++) {
            if (work[i] == '\\') {
                work[i] = '/';
            }
        }
    }

    /* Root detection + in-place canonical assembly. Components are emitted into
     * `out`; '..' pops by scanning `out` back to the previous '/', clamped at
     * `rootlen` -- no component arrays, so the stack frame stays ~work[]. */
    size_t pos = 0;
    size_t rootlen = 0;
    const char *rest = NULL;

    if (host == TP_HOST_POSIX) {
        if (work[0] != '/') {
            return tp_error_set(err, TP_STATUS_PATH_NOT_ABSOLUTE, "POSIX identity path must start with '/'");
        }
        if (!append(out, cap, &pos, "/", 1U)) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "canonical path exceeds out buffer");
        }
        rest = work + 1;
    } else {
        /* Windows device / verbatim namespace ("\\?\..." / "\\.\...", already
         * '/'-rewritten). Decision 0006: "\\?\" is a transparent lexical alias
         * for the drive form ("//?/X:...") and the UNC form ("//?/UNC/..."); every
         * OTHER "//?/..." form and ALL "//./..." device paths are rejected -- a
         * device path is never a project file. Rewrites `work` in place, then
         * falls through to normal drive/UNC detection. */
        if (work[0] == '/' && work[1] == '/' && (work[2] == '?' || work[2] == '.') && work[3] == '/') {
            if (work[2] == '.') {
                return tp_error_set(err, TP_STATUS_PATH_DEVICE,
                                    "Windows device path '\\\\.\\...' is not a project identity");
            }
            const char *after = work + 4; /* past "//?/" */
            if (tp_ident_is_alpha(after[0]) && after[1] == ':') {
                /* "//?/X:..." -> "X:..." */
                memmove(work, after, strlen(after) + 1U);
            } else if (tp_ident_ascii_upper(after[0]) == 'U' && tp_ident_ascii_upper(after[1]) == 'N' &&
                       tp_ident_ascii_upper(after[2]) == 'C' && after[3] == '/') {
                /* "//?/UNC/server/share..." -> "//server/share..." */
                memmove(work + 1, work + 7, strlen(work + 7) + 1U);
                work[0] = '/';
            } else {
                return tp_error_set(err, TP_STATUS_PATH_DEVICE, "unsupported Windows verbatim path '\\\\?\\...'");
            }
        }

        if (work[0] == '/' && work[1] == '/') {
            /* UNC //server/share -- runs of separators inside the head collapse. */
            const char *p = work + 2;
            while (*p == '/') {
                p++;
            }
            const char *server = p;
            while (*p && *p != '/') {
                p++;
            }
            size_t slen = (size_t)(p - server);
            if (slen == 0) {
                return tp_error_set(err, TP_STATUS_PATH_BAD_UNC, "UNC path needs //server/share");
            }
            while (*p == '/') {
                p++;
            }
            const char *share = p;
            while (*p && *p != '/') {
                p++;
            }
            size_t shlen = (size_t)(p - share);
            if (shlen == 0) {
                return tp_error_set(err, TP_STATUS_PATH_BAD_UNC, "UNC path needs a share name");
            }
            if (!append(out, cap, &pos, "//", 2U) || !append(out, cap, &pos, server, slen) ||
                !append(out, cap, &pos, "/", 1U) || !append(out, cap, &pos, share, shlen)) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "canonical path exceeds out buffer");
            }
            rest = (*p == '/') ? p + 1 : p;
        } else if (tp_ident_is_alpha(work[0]) && work[1] == ':') {
            if (work[2] == '/') {
                rest = work + 3;
            } else if (work[2] == '\0') {
                rest = work + 2; /* "" -> drive root */
            } else {
                return tp_error_set(err, TP_STATUS_PATH_DRIVE_RELATIVE,
                                    "drive-relative 'X:foo' is not a canonical identity");
            }
            char root[3] = {tp_ident_ascii_upper(work[0]), ':', '/'};
            if (!append(out, cap, &pos, root, sizeof root)) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "canonical path exceeds out buffer");
            }
        } else {
            return tp_error_set(err, TP_STATUS_PATH_NOT_ABSOLUTE,
                                "Windows identity path must be 'X:/...' or UNC '//server/share/...'");
        }
    }
    rootlen = pos; /* the '..'-pop clamp boundary */

    /* Tokenize `rest`, emitting '.'/'..'/plain components into `out`. */
    tp_ident_lex it = tp_ident_lex_begin(rest, false); /* '\' already rewritten to '/' */
    const char *start;
    size_t len;
    while (tp_ident_lex_next(&it, &start, &len)) {
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (pos > rootlen) {
                size_t q = pos;
                while (q > rootlen && out[q - 1] != '/') {
                    q--;
                }
                pos = (q > rootlen) ? q - 1U : rootlen;
            }
            continue; /* clamp at root */
        }
        if (pos > 0 && out[pos - 1] != '/') {
            if (!append(out, cap, &pos, "/", 1U)) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "canonical path exceeds out buffer");
            }
        }
        if (!append(out, cap, &pos, start, len)) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "canonical path exceeds out buffer");
        }
    }
    out[pos] = '\0';
    return TP_STATUS_OK;
}

bool tp_path_equal_host(const char *canon_a, const char *canon_b, tp_host host) {
    if (!canon_a || !canon_b) {
        return false;
    }
    if (host == TP_HOST_POSIX) {
        return strcmp(canon_a, canon_b) == 0;
    }
    for (;;) {
        char ca = tp_ident_ascii_lower(*canon_a);
        char cb = tp_ident_ascii_lower(*canon_b);
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
        canon_a++;
        canon_b++;
    }
}

/* --- native-host public wrappers ---------------------------------------- */

tp_status tp_identity_path_lexical(const char *input, char *out, size_t cap, tp_error *err) {
    return tp_path_canonical_lexical(input, tp_host_native(), out, cap, err);
}

bool tp_identity_path_equal(const char *canon_a, const char *canon_b) {
    return tp_path_equal_host(canon_a, canon_b, tp_host_native());
}

/* ======================================================================== */
/* (b) FILESYSTEM realpath/symlink resolution layered on the lexical result. */
/*     This half is genuinely OS-dependent (guarded by #if defined(_WIN32)).  */
/* ======================================================================== */

/* Join an already-resolved parent directory with a final component and re-run
 * the lexical canonicalizer -- collapses the join separator (incl. a root parent
 * like "/" or "C:/") uniformly across OSes. */
static tp_status fs_join_and_canon(const char *resolved_parent, const char *final_comp, tp_host host, char *out,
                                   size_t cap, tp_error *err) {
    char joined[TP_IDENTITY_PATH_MAX];
    int n = snprintf(joined, sizeof joined, "%s/%s", resolved_parent, final_comp);
    if (n < 0 || (size_t)n >= sizeof joined) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "resolved destination path too long");
    }
    return tp_path_canonical_lexical(joined, host, out, cap, err);
}

#if !defined(_WIN32)

static tp_status fs_resolve_posix(const char *lex, char *out, size_t cap, tp_error *err) {
    char *res = realpath(lex, NULL); /* POSIX.1-2008: NULL -> malloc exact size */
    if (res) {
        tp_status st = TP_STATUS_OK;
        size_t rlen = strlen(res);
        if (rlen >= cap) {
            st = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "resolved path exceeds out buffer");
        } else {
            memcpy(out, res, rlen + 1U);
        }
        free(res);
        return st;
    }
    if (errno != ENOENT) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "cannot resolve '%s': %s", lex, strerror(errno));
    }
    /* Not-yet-created destination: resolve the existing PARENT and append the
     * final component lexically (its final component need not exist). `lex` is
     * canonical + absolute, so a '/' is always present and never trailing. */
    const char *slash = strrchr(lex, '/');
    if (!slash) {
        /* Unreachable: a canonical POSIX path always has a leading '/'. Guarded so
         * the pointer subtraction below is never UB (UBSan pointer-overflow). */
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "cannot split '%s'", lex);
    }
    const char *final_comp = slash + 1;
    char parent[TP_IDENTITY_PATH_MAX];
    size_t plen = (size_t)(slash - lex);
    if (plen == 0) {
        parent[0] = '/'; /* root parent */
        parent[1] = '\0';
    } else {
        if (plen >= sizeof parent) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "destination parent path too long");
        }
        memcpy(parent, lex, plen);
        parent[plen] = '\0';
    }
    char *pres = realpath(parent, NULL);
    if (!pres) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "destination parent '%s' does not exist", parent);
    }
    tp_status st = fs_join_and_canon(pres, final_comp, TP_HOST_POSIX, out, cap, err);
    free(pres);
    return st;
}

#else /* _WIN32 */

/* UTF-8 -> UTF-16 into a fixed wide buffer. */
static tp_status fs_widen(const char *utf8, wchar_t *wout, int wcap, tp_error *err) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wout, wcap);
    if (n == 0) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "UTF-8 to UTF-16 conversion failed");
    }
    return TP_STATUS_OK;
}

/* UTF-16 -> UTF-8 into `out`. */
static tp_status fs_narrow(const wchar_t *w, char *out, size_t cap, tp_error *err) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    if (n == 0) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "resolved path exceeds out buffer");
    }
    return TP_STATUS_OK;
}

/* Open an existing file OR directory just to query its identity (no access
 * rights requested; FILE_FLAG_BACKUP_SEMANTICS lets us open directories, and NO
 * FILE_FLAG_OPEN_REPARSE_POINT means symlinks/junctions are FOLLOWED to their
 * target -- which is exactly the identity we want). */
static HANDLE fs_open_query(const char *path_slash) {
    char win[TP_IDENTITY_PATH_MAX];
    size_t i = 0;
    for (; path_slash[i] != '\0' && i + 1U < sizeof win; i++) {
        win[i] = (path_slash[i] == '/') ? '\\' : path_slash[i];
    }
    win[i] = '\0';
    wchar_t wpath[TP_IDENTITY_PATH_MAX];
    if (fs_widen(win, wpath, (int)(sizeof wpath / sizeof wpath[0]), NULL) != TP_STATUS_OK) {
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

/* GetFinalPathNameByHandleW -> UTF-8 "\\?\..."/"\\?\UNC\..." raw form. The
 * lexical canonicalizer strips the "\\?\" alias, so the raw output feeds
 * straight back into it. */
static tp_status fs_final_path(HANDLE h, char *raw, size_t cap, tp_error *err) {
    wchar_t wbuf[TP_IDENTITY_PATH_MAX];
    DWORD cnt = (DWORD)(sizeof wbuf / sizeof wbuf[0]);
    DWORD n = GetFinalPathNameByHandleW(h, wbuf, cnt, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "GetFinalPathNameByHandle failed (%lu)",
                            (unsigned long)GetLastError());
    }
    if (n >= cnt) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "resolved path too long");
    }
    return fs_narrow(wbuf, raw, cap, err);
}

static tp_status fs_resolve_windows(const char *lex, char *out, size_t cap, tp_error *err) {
    HANDLE h = fs_open_query(lex);
    if (h != INVALID_HANDLE_VALUE) {
        char raw[TP_IDENTITY_PATH_MAX];
        tp_status st = fs_final_path(h, raw, sizeof raw, err);
        CloseHandle(h);
        if (st != TP_STATUS_OK) {
            return st;
        }
        return tp_path_canonical_lexical(raw, TP_HOST_WINDOWS, out, cap, err);
    }
    DWORD e = GetLastError();
    if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "cannot open '%s' (%lu)", lex, (unsigned long)e);
    }
    /* Not-yet-created destination: resolve the existing PARENT directory. */
    const char *slash = strrchr(lex, '/');
    if (!slash) {
        /* Unreachable: a canonical Windows drive/UNC path always contains '/'. */
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "cannot split '%s'", lex);
    }
    const char *final_comp = slash + 1;
    char parent[TP_IDENTITY_PATH_MAX];
    size_t plen = (size_t)(slash - lex);
    if (plen >= sizeof parent - 1U) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "destination parent path too long");
    }
    memcpy(parent, lex, plen);
    parent[plen] = '\0';
    if (plen == 0 || (plen > 0 && parent[plen - 1U] == ':')) {
        /* root parent: drive root "C:" -> "C:/", or a bare "/" head */
        parent[plen] = '/';
        parent[plen + 1U] = '\0';
    }
    HANDLE ph = fs_open_query(parent);
    if (ph == INVALID_HANDLE_VALUE) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "destination parent '%s' does not exist", parent);
    }
    char praw[TP_IDENTITY_PATH_MAX];
    tp_status st = fs_final_path(ph, praw, sizeof praw, err);
    CloseHandle(ph);
    if (st != TP_STATUS_OK) {
        return st;
    }
    char pdir[TP_IDENTITY_PATH_MAX];
    st = tp_path_canonical_lexical(praw, TP_HOST_WINDOWS, pdir, sizeof pdir, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    return fs_join_and_canon(pdir, final_comp, TP_HOST_WINDOWS, out, cap, err);
}

#endif /* _WIN32 */

tp_status tp_identity_path_canonical(const char *input, char *out, size_t cap, tp_error *err) {
    if (!out || cap == 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "out buffer is NULL/zero");
    }
    char lex[TP_IDENTITY_PATH_MAX];
    tp_status st = tp_identity_path_lexical(input, lex, sizeof lex, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
#if defined(_WIN32)
    return fs_resolve_windows(lex, out, cap, err);
#else
    return fs_resolve_posix(lex, out, cap, err);
#endif
}
