#include "tp_c0/tp_c0_path.h"

#include <string.h>

tp_c0_host tp_c0_host_native(void) {
#if defined(_WIN32)
    return TP_C0_HOST_WINDOWS;
#else
    return TP_C0_HOST_POSIX;
#endif
}

static bool is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Append n bytes of `s` to out at *pos, guarding cap (leaves room for a later
 * NUL by requiring *pos + n < cap). Returns false if it would overflow. */
static bool append(char *out, size_t cap, size_t *pos, const char *s, size_t n) {
    if (*pos + n + 1U > cap) {
        return false;
    }
    memcpy(out + *pos, s, n);
    *pos += n;
    return true;
}

tp_c0_detail tp_c0_project_path_canonical(const char *input, tp_c0_host host, char *out, size_t cap,
                                          tp_error *err) {
    if (!input) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "input path is NULL");
    }
    if (!out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "out buffer is NULL");
    }
    if (input[0] == '\0') {
        return tp_c0_fail(err, TP_C0_ERR_EMPTY, "empty path");
    }

    char work[TP_C0_PATH_MAX];
    size_t ilen = strlen(input);
    if (ilen >= sizeof work) {
        return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "path exceeds %zu bytes", sizeof work);
    }
    memcpy(work, input, ilen + 1U);
    if (host == TP_C0_HOST_WINDOWS) {
        for (size_t i = 0; i < ilen; i++) {
            if (work[i] == '\\') {
                work[i] = '/';
            }
        }
    }

    /* ----- root detection ------------------------------------------------ */
    char rootbuf[TP_C0_PATH_MAX];
    size_t rootlen = 0;
    const char *rest = NULL;

    if (host == TP_C0_HOST_POSIX) {
        if (work[0] != '/') {
            return tp_c0_fail(err, TP_C0_ERR_PATH_NOT_ABSOLUTE, "POSIX identity path must start with '/'");
        }
        rootbuf[0] = '/';
        rootlen = 1;
        rest = work + 1;
    } else {
        /* Windows device / verbatim namespace: "\\?\..." and "\\.\..." (already
         * '/'-rewritten to "//?/..." / "//./..."). Decided policy
         * (docs/decisions/0006-windows-device-paths.md): "\\?\" is a transparent
         * lexical alias for the drive form ("//?/X:...") and the UNC form
         * ("//?/UNC/server/share..."); every OTHER "//?/..." form and ALL "//./..."
         * device paths are rejected -- a device path is never a project file, and
         * one file must have one identity. Rewrites `work` in place, then falls
         * through to the normal drive/UNC detection below. */
        if (work[0] == '/' && work[1] == '/' && (work[2] == '?' || work[2] == '.') && work[3] == '/') {
            if (work[2] == '.') {
                return tp_c0_fail(err, TP_C0_ERR_PATH_DEVICE,
                                  "Windows device path '\\\\.\\...' is not a project identity");
            }
            const char *after = work + 4; /* past "//?/" */
            if (is_alpha(after[0]) && after[1] == ':') {
                /* "//?/X:..." -> "X:..." (canonicalize as a drive path). */
                memmove(work, after, strlen(after) + 1U);
            } else if (ascii_upper(after[0]) == 'U' && ascii_upper(after[1]) == 'N' &&
                       ascii_upper(after[2]) == 'C' && after[3] == '/') {
                /* "//?/UNC/server/share..." -> "//server/share..." (UNC). work+7
                 * is the '/' before the server, so prefixing one '/' yields "//". */
                memmove(work + 1, work + 7, strlen(work + 7) + 1U);
                work[0] = '/';
            } else {
                return tp_c0_fail(err, TP_C0_ERR_PATH_DEVICE,
                                  "unsupported Windows verbatim path '\\\\?\\...'");
            }
        }

        if (work[0] == '/' && work[1] == '/') {
            /* UNC //server/share -- runs of separators inside the head collapse
             * (extra leading '/', and doubled '/' between server and share). */
            const char *p = work + 2;
            while (*p == '/') {
                p++; /* collapse extra separators before the server */
            }
            const char *server = p;
            while (*p && *p != '/') {
                p++;
            }
            size_t slen = (size_t)(p - server);
            if (slen == 0) {
                return tp_c0_fail(err, TP_C0_ERR_PATH_BAD_UNC, "UNC path needs //server/share");
            }
            while (*p == '/') {
                p++; /* collapse separators between server and share */
            }
            const char *share = p;
            while (*p && *p != '/') {
                p++;
            }
            size_t shlen = (size_t)(p - share);
            if (shlen == 0) {
                return tp_c0_fail(err, TP_C0_ERR_PATH_BAD_UNC, "UNC path needs a share name");
            }
            rootbuf[0] = '/';
            rootbuf[1] = '/';
            memcpy(rootbuf + 2, server, slen);
            rootbuf[2 + slen] = '/';
            memcpy(rootbuf + 3 + slen, share, shlen);
            rootlen = 3 + slen + shlen; /* "//" + server + "/" + share */
            rest = (*p == '/') ? p + 1 : p;
        } else if (is_alpha(work[0]) && work[1] == ':') {
            if (work[2] == '/') {
                rootbuf[0] = ascii_upper(work[0]);
                rootbuf[1] = ':';
                rootbuf[2] = '/';
                rootlen = 3;
                rest = work + 3;
            } else if (work[2] == '\0') {
                rootbuf[0] = ascii_upper(work[0]);
                rootbuf[1] = ':';
                rootbuf[2] = '/';
                rootlen = 3;
                rest = work + 2; /* "" -> drive root */
            } else {
                return tp_c0_fail(err, TP_C0_ERR_PATH_DRIVE_REL, "drive-relative 'X:foo' is not a canonical identity");
            }
        } else {
            return tp_c0_fail(err, TP_C0_ERR_PATH_NOT_ABSOLUTE,
                              "Windows identity path must be 'X:/...' or UNC '//server/share/...'");
        }
    }

    /* ----- tokenize `rest`, resolving '.'/'..'/empty ---------------------- */
    /* Bounded by path length; each kept component is a (start,len) into work. */
    const char *comp_ptr[TP_C0_PATH_MAX / 2 + 1];
    size_t comp_len[TP_C0_PATH_MAX / 2 + 1];
    size_t ncomp = 0;

    const char *p = rest;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (*p == '/') {
            p++;
        }
        if (len == 0) {
            continue; /* repeated or trailing separator */
        }
        if (len == 1 && start[0] == '.') {
            continue; /* current dir */
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (ncomp > 0) {
                ncomp--; /* pop */
            }
            continue; /* clamp at root */
        }
        comp_ptr[ncomp] = start;
        comp_len[ncomp] = len;
        ncomp++;
    }

    /* ----- assemble ------------------------------------------------------ */
    size_t pos = 0;
    if (!append(out, cap, &pos, rootbuf, rootlen)) {
        return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "canonical path exceeds out buffer");
    }
    for (size_t i = 0; i < ncomp; i++) {
        if (pos > 0 && out[pos - 1] != '/') {
            if (!append(out, cap, &pos, "/", 1U)) {
                return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "canonical path exceeds out buffer");
            }
        }
        if (!append(out, cap, &pos, comp_ptr[i], comp_len[i])) {
            return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "canonical path exceeds out buffer");
        }
    }
    out[pos] = '\0';
    return TP_C0_OK;
}

bool tp_c0_project_path_equal(const char *canon_a, const char *canon_b, tp_c0_host host) {
    if (!canon_a || !canon_b) {
        return false;
    }
    if (host == TP_C0_HOST_POSIX) {
        return strcmp(canon_a, canon_b) == 0;
    }
    for (;;) {
        char ca = ascii_lower(*canon_a);
        char cb = ascii_lower(*canon_b);
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
