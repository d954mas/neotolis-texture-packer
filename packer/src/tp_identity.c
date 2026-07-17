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
#include <fcntl.h>
#include <stdlib.h>   /* realpath */
#include <sys/stat.h> /* lstat: tell a dangling symlink apart from an absent node */
#include <unistd.h>
#endif

/* ======================================================================== */
/* (a) LEXICAL canonicalization. Host-parameterized so both rule sets run */
/*     on every OS in the production tests and report tp_status errors.   */
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

tp_status tp_identity_bytes_fingerprint(const void *bytes, size_t len, tp_id128 *out, tp_error *err) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if ((!bytes && len != 0U) || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "byte fingerprint requires bytes and output");
    }
    tp_hasher hasher = tp_hasher_init();
    if (len != 0U) {
        tp_hasher_update(&hasher, bytes, len);
    }
    *out = tp_hasher_final(hasher);
    return TP_STATUS_OK;
}

tp_status tp_identity_file_fingerprint(const char *path, tp_id128 *out, tp_error *err) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!path || path[0] == '\0' || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "file fingerprint requires path and output");
    }

    tp_hasher hasher = tp_hasher_init();
    tp_id128 stable_fingerprint = {{0}};
    uint8_t buf[64U * 1024U];
#if defined(_WIN32)
    /* Deny write/delete sharing while the bytes are sampled. Besides making the two-pass
     * stability check below deterministic, this prevents a cooperating Windows writer from
     * changing or rename-replacing the destination during the fingerprint operation. */
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "cannot open '%s' for fingerprinting", path);
    }
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER initial_size;
    if (!GetFileInformationByHandle(h, &info) || GetFileType(h) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
        !GetFileSizeEx(h, &initial_size) || initial_size.QuadPart < 0) {
        (void)CloseHandle(h);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' is not a directly-opened regular file", path);
    }
    if ((uint64_t)initial_size.QuadPart > (uint64_t)TP_IDENTITY_FILE_MAX_BYTES) {
        (void)CloseHandle(h);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "'%s' exceeds the %u-byte fingerprint limit", path,
                            (unsigned int)TP_IDENTITY_FILE_MAX_BYTES);
    }
    uint64_t remaining = (uint64_t)initial_size.QuadPart;
    while (remaining != 0U) {
        const DWORD want = remaining < (uint64_t)sizeof buf ? (DWORD)remaining : (DWORD)sizeof buf;
        DWORD got = 0;
        if (!ReadFile(h, buf, want, &got, NULL) || got == 0U) {
            (void)CloseHandle(h);
            return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                                "cannot read '%s' for fingerprinting", path);
        }
        tp_hasher_update(&hasher, buf, (size_t)got);
        remaining -= (uint64_t)got;
    }
    DWORD extra = 0;
    LARGE_INTEGER final_size;
    if (!ReadFile(h, buf, 1U, &extra, NULL) || !GetFileSizeEx(h, &final_size) ||
        final_size.QuadPart != initial_size.QuadPart || extra != 0U) {
        (void)CloseHandle(h);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' changed while it was fingerprinted", path);
    }
    const tp_id128 first_fingerprint = tp_hasher_final(hasher);
    LARGE_INTEGER begin;
    begin.QuadPart = 0;
    if (!SetFilePointerEx(h, begin, NULL, FILE_BEGIN)) {
        (void)CloseHandle(h);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "cannot rewind '%s' while fingerprinting", path);
    }
    tp_hasher second_hasher = tp_hasher_init();
    remaining = (uint64_t)initial_size.QuadPart;
    while (remaining != 0U) {
        const DWORD want = remaining < (uint64_t)sizeof buf ? (DWORD)remaining : (DWORD)sizeof buf;
        DWORD got = 0;
        if (!ReadFile(h, buf, want, &got, NULL) || got == 0U) {
            (void)CloseHandle(h);
            return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                                "cannot re-read '%s' for a stable fingerprint", path);
        }
        tp_hasher_update(&second_hasher, buf, (size_t)got);
        remaining -= (uint64_t)got;
    }
    BY_HANDLE_FILE_INFORMATION final_info;
    extra = 0;
    if (!ReadFile(h, buf, 1U, &extra, NULL) || extra != 0U ||
        !GetFileInformationByHandle(h, &final_info) ||
        final_info.nFileSizeHigh != info.nFileSizeHigh || final_info.nFileSizeLow != info.nFileSizeLow ||
        CompareFileTime(&final_info.ftLastWriteTime, &info.ftLastWriteTime) != 0) {
        (void)CloseHandle(h);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' changed while it was fingerprinted", path);
    }
    const tp_id128 second_fingerprint = tp_hasher_final(second_hasher);
    if (!tp_id128_eq(first_fingerprint, second_fingerprint)) {
        (void)CloseHandle(h);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' changed while it was fingerprinted", path);
    }
    stable_fingerprint = second_fingerprint;
    /* A concurrent atomic replace does not mutate `h`: it moves the pathname to a new file while this
     * handle keeps reading the old one. Re-open the current pathname while `h` is still alive and require
     * the same volume+file index, shrinking the remaining race to the final identity-check->publish gap. */
    HANDLE current = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION current_info;
    if (current == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(current, &current_info) ||
        (current_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
        current_info.dwVolumeSerialNumber != info.dwVolumeSerialNumber ||
        current_info.nFileIndexHigh != info.nFileIndexHigh || current_info.nFileIndexLow != info.nFileIndexLow) {
        if (current != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(current);
        }
        (void)CloseHandle(h);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' was replaced while it was fingerprinted", path);
    }
    (void)CloseHandle(current);
    (void)CloseHandle(h);
#else
    /* O_NONBLOCK is ignored for regular files, but prevents open() itself from
     * waiting forever on a FIFO before fstat can reject the non-regular node. */
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#else
    struct stat lst;
    if (lstat(path, &lst) != 0 || S_ISLNK(lst.st_mode)) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' is not a directly-opened regular file", path);
    }
#endif
    const int fd = open(path, flags);
    if (fd < 0) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "cannot open '%s' for fingerprinting: %s", path,
                            strerror(errno));
    }
    struct stat initial;
    if (fstat(fd, &initial) != 0 || !S_ISREG(initial.st_mode) || initial.st_size < 0) {
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' is not a directly-opened regular file", path);
    }
    if ((uint64_t)initial.st_size > (uint64_t)TP_IDENTITY_FILE_MAX_BYTES) {
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "'%s' exceeds the %u-byte fingerprint limit", path,
                            (unsigned int)TP_IDENTITY_FILE_MAX_BYTES);
    }
    uint64_t remaining = (uint64_t)initial.st_size;
    while (remaining != 0U) {
        const size_t want = remaining < (uint64_t)sizeof buf ? (size_t)remaining : sizeof buf;
        const ssize_t got = read(fd, buf, want);
        if (got <= 0) {
            (void)close(fd);
            return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                                "cannot read '%s' for fingerprinting", path);
        }
        tp_hasher_update(&hasher, buf, (size_t)got);
        remaining -= (uint64_t)got;
    }
    struct stat final;
    const ssize_t extra = read(fd, buf, 1U);
    if (extra < 0 || fstat(fd, &final) != 0 || final.st_dev != initial.st_dev || final.st_ino != initial.st_ino ||
        final.st_size != initial.st_size || extra != 0) {
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' changed while it was fingerprinted", path);
    }
    const tp_id128 first_fingerprint = tp_hasher_final(hasher);
    if (lseek(fd, 0, SEEK_SET) < 0) {
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "cannot rewind '%s' while fingerprinting: %s", path, strerror(errno));
    }
    tp_hasher second_hasher = tp_hasher_init();
    remaining = (uint64_t)initial.st_size;
    while (remaining != 0U) {
        const size_t want = remaining < (uint64_t)sizeof buf ? (size_t)remaining : sizeof buf;
        const ssize_t got = read(fd, buf, want);
        if (got <= 0) {
            (void)close(fd);
            return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                                "cannot re-read '%s' for a stable fingerprint", path);
        }
        tp_hasher_update(&second_hasher, buf, (size_t)got);
        remaining -= (uint64_t)got;
    }
    struct stat stable;
    const ssize_t stable_extra = read(fd, buf, 1U);
    if (stable_extra < 0 || fstat(fd, &stable) != 0 || stable.st_dev != initial.st_dev ||
        stable.st_ino != initial.st_ino || stable.st_size != initial.st_size || stable_extra != 0) {
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' changed while it was fingerprinted", path);
    }
    const tp_id128 second_fingerprint = tp_hasher_final(second_hasher);
    if (!tp_id128_eq(first_fingerprint, second_fingerprint)) {
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' changed while it was fingerprinted", path);
    }
    stable_fingerprint = second_fingerprint;
    /* Like Windows, rename-over leaves `fd` on the old inode. Re-open the current pathname and require
     * that it still resolves directly to the inode whose bytes were hashed. */
    const int current_fd = open(path, flags);
    struct stat current;
    if (current_fd < 0 || fstat(current_fd, &current) != 0 || !S_ISREG(current.st_mode) ||
        current.st_dev != initial.st_dev || current.st_ino != initial.st_ino) {
        if (current_fd >= 0) {
            (void)close(current_fd);
        }
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' was replaced while it was fingerprinted", path);
    }
    (void)close(current_fd);
    (void)close(fd);
#endif
    *out = stable_fingerprint;
    return TP_STATUS_OK;
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
    int rp_errno = errno;
    if (rp_errno != ENOENT) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "cannot resolve '%s': %s", lex, strerror(rp_errno));
    }
    /* realpath failed with ENOENT. Distinguish a genuinely-absent final component
     * (a legitimate not-yet-created Save-As destination) from a path that EXISTS as
     * an unresolvable node -- classically a dangling symlink whose target is
     * missing. lstat() does NOT follow the final symlink, so it succeeds on the
     * link itself; that node must NOT get the phantom <parent>/<linkname> identity,
     * because once the target is created realpath would follow the link and the
     * SAME file would canonicalize to a DIFFERENT identity (two identities / life). */
    struct stat lst;
    if (lstat(lex, &lst) == 0) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' exists but cannot be canonically resolved (dangling symlink?)", lex);
    }
    if (errno != ENOENT) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "cannot stat '%s': %s", lex, strerror(errno));
    }
    /* Genuinely not-yet-created: resolve the existing PARENT and append the final
     * component lexically (it need not exist). `lex` is canonical + absolute, so a
     * '/' is always present and never trailing. */
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
    /* `path_slash` is ALWAYS a canonical '/'-form path (drive "X:/..." or UNC
     * "//server/share/..."), NEVER a "\\?\" verbatim path -- the lexical
     * canonicalizer strips that prefix before we get here. CreateFileW normalizes
     * forward slashes in ordinary (non-verbatim) paths, so we widen the '/'-form
     * directly; the old copy into a '\'-rewritten buffer was dead work (it would
     * only ever matter for a verbatim path, which never reaches this function). */
    wchar_t wpath[TP_IDENTITY_PATH_MAX];
    if (fs_widen(path_slash, wpath, (int)(sizeof wpath / sizeof wpath[0]), NULL) != TP_STATUS_OK) {
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

/* True iff the final component of `path_slash` genuinely does NOT exist (a real
 * "name not found"). GetFileAttributesW does NOT follow the final reparse point,
 * so a dangling symlink/junction (target missing) reports its OWN attributes and
 * is correctly seen as EXISTING -- it must not be mistaken for a not-yet-created
 * destination (which would hand it the phantom <parent>/<linkname> identity that
 * flips once the target appears). Fails closed (reports the node present) when the
 * name can't be widened. */
static bool fs_path_is_absent(const char *path_slash) {
    wchar_t wpath[TP_IDENTITY_PATH_MAX];
    if (fs_widen(path_slash, wpath, (int)(sizeof wpath / sizeof wpath[0]), NULL) != TP_STATUS_OK) {
        return false;
    }
    if (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) {
        return false; /* node exists (possibly a dangling reparse point) */
    }
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
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
    /* CreateFileW FOLLOWS reparse points, so a dangling symlink/junction (target
     * missing) also reports the name as not-found here. Only a genuinely-absent
     * final component is a legitimate not-yet-created destination; a node that
     * EXISTS but cannot be resolved is a real failure -- never the phantom
     * <parent>/<linkname> identity that would flip once the target appears (this is
     * the exact distinction the POSIX lstat() branch above makes). */
    if (!fs_path_is_absent(lex)) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "'%s' exists but cannot be canonically resolved (dangling reparse point?)", lex);
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
    if (plen == 0 || parent[plen - 1U] == ':') {
        /* root parent: drive root "C:" -> "C:/", or a bare "/" head. The '||'
         * short-circuits, so parent[plen - 1U] is read only when plen != 0. */
        parent[plen] = '/';
        parent[plen + 1U] = '\0';
    }
    HANDLE ph = fs_open_query(parent);
    if (ph == INVALID_HANDLE_VALUE) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "destination parent '%s' does not exist", parent);
    }
    /* Symmetric with the POSIX sibling (which feeds realpath's resolved parent
     * straight into fs_join_and_canon): pass the raw "\\?\" parent directly. The
     * lexical canonicalizer strips the "\\?\" alias and is idempotent, so the extra
     * intermediate buffer + second canon pass were redundant -- output is
     * byte-identical to canonicalizing praw first. */
    char praw[TP_IDENTITY_PATH_MAX];
    tp_status st = fs_final_path(ph, praw, sizeof praw, err);
    CloseHandle(ph);
    if (st != TP_STATUS_OK) {
        return st;
    }
    return fs_join_and_canon(praw, final_comp, TP_HOST_WINDOWS, out, cap, err);
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

tp_status tp_identity_path_absolute_lexical(const char *input, char *out,
                                            size_t cap, tp_error *err) {
    tp_status status = tp_identity_path_lexical(input, out, cap, err);
    if (status != TP_STATUS_PATH_NOT_ABSOLUTE) {
        return status;
    }
    if (!input || input[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "project path is empty");
    }
    char cwd[TP_IDENTITY_PATH_MAX];
#if defined(_WIN32)
    wchar_t wide[TP_IDENTITY_PATH_MAX];
    const DWORD length = GetCurrentDirectoryW(
        (DWORD)(sizeof wide / sizeof wide[0]), wide);
    if (length == 0U || length >= (DWORD)(sizeof wide / sizeof wide[0]) ||
        fs_narrow(wide, cwd, sizeof cwd, err) != TP_STATUS_OK) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "current directory cannot be resolved");
    }
#else
    if (!getcwd(cwd, sizeof cwd)) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "current directory cannot be resolved: %s",
                            strerror(errno));
    }
#endif
    char absolute[TP_IDENTITY_PATH_MAX];
    const int written = snprintf(absolute, sizeof absolute, "%s/%s", cwd,
                                 input);
    if (written < 0 || (size_t)written >= sizeof absolute) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "project path exceeds the supported limit");
    }
    return tp_identity_path_lexical(absolute, out, cap, err);
}

tp_status tp_identity_project_path_canonical(const char *input, char *out,
                                             size_t cap, tp_error *err) {
    char absolute[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_absolute_lexical(
        input, absolute, sizeof absolute, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    return tp_identity_path_canonical(absolute, out, cap, err);
}
