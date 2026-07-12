#include "tp_c0/tp_c0_path.h"

#include <string.h>

#include "tp_c0_lex.h"

tp_c0_host tp_c0_host_native(void) {
#if defined(_WIN32)
    return TP_C0_HOST_WINDOWS;
#else
    return TP_C0_HOST_POSIX;
#endif
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

    /* ----- root detection + in-place canonical assembly ------------------- */
    /* Components are emitted straight into `out`; '..' pops by scanning `out`
     * back to the previous '/', clamped at `rootlen`. No component arrays and no
     * separate rootbuf -- keeps the stack frame to ~work[] instead of ~40KB. */
    size_t pos = 0;
    size_t rootlen = 0;
    const char *rest = NULL;

    if (host == TP_C0_HOST_POSIX) {
        if (work[0] != '/') {
            return tp_c0_fail(err, TP_C0_ERR_PATH_NOT_ABSOLUTE, "POSIX identity path must start with '/'");
        }
        if (!append(out, cap, &pos, "/", 1U)) {
            return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "canonical path exceeds out buffer");
        }
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
            if (tp_c0_is_alpha(after[0]) && after[1] == ':') {
                /* "//?/X:..." -> "X:..." (canonicalize as a drive path). */
                memmove(work, after, strlen(after) + 1U);
            } else if (tp_c0_ascii_upper(after[0]) == 'U' && tp_c0_ascii_upper(after[1]) == 'N' &&
                       tp_c0_ascii_upper(after[2]) == 'C' && after[3] == '/') {
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
            if (!append(out, cap, &pos, "//", 2U) || !append(out, cap, &pos, server, slen) ||
                !append(out, cap, &pos, "/", 1U) || !append(out, cap, &pos, share, shlen)) {
                return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "canonical path exceeds out buffer");
            }
            rest = (*p == '/') ? p + 1 : p;
        } else if (tp_c0_is_alpha(work[0]) && work[1] == ':') {
            if (work[2] == '/') {
                rest = work + 3;
            } else if (work[2] == '\0') {
                rest = work + 2; /* "" -> drive root */
            } else {
                return tp_c0_fail(err, TP_C0_ERR_PATH_DRIVE_REL, "drive-relative 'X:foo' is not a canonical identity");
            }
            char root[3] = {tp_c0_ascii_upper(work[0]), ':', '/'};
            if (!append(out, cap, &pos, root, sizeof root)) {
                return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "canonical path exceeds out buffer");
            }
        } else {
            return tp_c0_fail(err, TP_C0_ERR_PATH_NOT_ABSOLUTE,
                              "Windows identity path must be 'X:/...' or UNC '//server/share/...'");
        }
    }
    rootlen = pos; /* the '..'-pop clamp boundary */

    /* ----- tokenize `rest`, emitting '.'/'..'/plain components into `out` -- */
    tp_c0_lex it = tp_c0_lex_begin(rest, false); /* '\' already rewritten to '/' */
    const char *start;
    size_t len;
    while (tp_c0_lex_next(&it, &start, &len)) {
        if (len == 1 && start[0] == '.') {
            continue; /* current dir */
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* Pop the last emitted component together with its leading '/', by
             * scanning back to the previous separator -- clamped at the root. */
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
                return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "canonical path exceeds out buffer");
            }
        }
        if (!append(out, cap, &pos, start, len)) {
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
        char ca = tp_c0_ascii_lower(*canon_a);
        char cb = tp_c0_ascii_lower(*canon_b);
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
