#include "tp_c0/tp_c0_srckey.h"

#include <stdlib.h>
#include <string.h>

#include "utf8proc.h"

static bool is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Structure-normalize (byte level) `input` into `joined`: split on '/' and '\\',
 * drop empty/'.' components, reject '..'/absolute. Result is a '/'-joined key
 * with preserved bytes (NFC applied by the caller). */
static tp_c0_detail structure_normalize(const char *input, char *joined, size_t cap, tp_error *err) {
    if (input[0] == '/' || input[0] == '\\') {
        return tp_c0_fail(err, TP_C0_ERR_KEY_ABSOLUTE, "source key must be relative (leading separator)");
    }
    if (is_alpha(input[0]) && input[1] == ':') {
        return tp_c0_fail(err, TP_C0_ERR_KEY_ABSOLUTE, "source key must be relative (drive prefix)");
    }

    size_t pos = 0;
    bool any = false;
    const char *p = input;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (*p) {
            p++; /* consume separator */
        }
        if (len == 0) {
            continue; /* repeated or trailing separator */
        }
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return tp_c0_fail(err, TP_C0_ERR_KEY_TRAVERSAL, "'..' component would escape the source root");
        }
        if (any) {
            if (pos + 1U >= cap) {
                return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "source key exceeds %zu bytes", cap);
            }
            joined[pos++] = '/';
        } else if (len >= 2 && is_alpha(start[0]) && start[1] == ':') {
            /* Drive prefix revealed only after '.'-stripping: the normalized key
             * would begin "X:", an absolute path. Reject so normalize is
             * idempotent (e.g. "./C:/x" must not become the accepted "C:/x"). A
             * drive-looking spelling in a LATER component is not absolute -- the
             * portability scan flags its ':' instead. */
            return tp_c0_fail(err, TP_C0_ERR_KEY_ABSOLUTE, "source key must be relative (drive prefix)");
        }
        if (pos + len >= cap) {
            return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "source key exceeds %zu bytes", cap);
        }
        memcpy(joined + pos, start, len);
        pos += len;
        any = true;
    }
    joined[pos] = '\0';
    if (!any) {
        return tp_c0_fail(err, TP_C0_ERR_EMPTY, "source key normalizes to empty");
    }
    return TP_C0_OK;
}

/* Run utf8proc_map with `options` over `in`, returning utf8proc's freshly
 * malloc'd NUL-terminated result in *out_buf (the CALLER frees it, inside this
 * TU/CRT) and its byte length in *out_len. Classifies failure as OOM /
 * invalid_utf8. */
static tp_c0_detail utf8_map_alloc(const char *in, utf8proc_option_t options, utf8proc_uint8_t **out_buf,
                                   size_t *out_len, tp_error *err) {
    utf8proc_uint8_t *buf = NULL;
    utf8proc_ssize_t n = utf8proc_map((const utf8proc_uint8_t *)in, -1, &buf,
                                      (utf8proc_option_t)(UTF8PROC_NULLTERM | UTF8PROC_STABLE | options));
    if (n < 0) {
        if (n == UTF8PROC_ERROR_NOMEM) {
            return tp_c0_fail(err, TP_C0_ERR_OOM, "utf8proc out of memory");
        }
        return tp_c0_fail(err, TP_C0_ERR_INVALID_UTF8, "not well-formed UTF-8 (%s)", utf8proc_errmsg(n));
    }
    *out_buf = buf;
    *out_len = (size_t)n;
    return TP_C0_OK;
}

/* Map `in` and copy the NUL-terminated result into caller buffer `out`, freeing
 * utf8proc's buffer here. buffer_too_small if it will not fit (case-fold can
 * expand up to ~3x, so callers must size `out` accordingly). */
static tp_c0_detail utf8_map_into(const char *in, utf8proc_option_t options, char *out, size_t cap, tp_error *err) {
    utf8proc_uint8_t *buf = NULL;
    size_t outlen = 0;
    tp_c0_detail d = utf8_map_alloc(in, options, &buf, &outlen, err);
    if (d != TP_C0_OK) {
        return d;
    }
    if (outlen + 1U > cap) {
        free(buf);
        return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "normalized key exceeds %zu bytes", cap);
    }
    memcpy(out, buf, outlen + 1U); /* includes the NUL utf8proc wrote */
    free(buf);
    return TP_C0_OK;
}

tp_c0_detail tp_c0_srckey_normalize(const char *input, char *out, size_t cap, tp_error *err) {
    if (!input || !out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "input or out is NULL");
    }
    if (input[0] == '\0') {
        return tp_c0_fail(err, TP_C0_ERR_EMPTY, "empty source key");
    }
    char joined[TP_C0_SRCKEY_MAX];
    tp_c0_detail d = structure_normalize(input, joined, sizeof joined, err);
    if (d != TP_C0_OK) {
        return d;
    }
    return utf8_map_into(joined, UTF8PROC_COMPOSE, out, cap, err);
}

tp_c0_detail tp_c0_srckey_casefold(const char *normalized_key, char *out, size_t cap, tp_error *err) {
    if (!normalized_key || !out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "input or out is NULL");
    }
    return utf8_map_into(normalized_key, (utf8proc_option_t)(UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE), out, cap, err);
}

tp_c0_detail tp_c0_srckey_collides(const char *key_a, const char *key_b, bool *out_collides, tp_error *err) {
    if (!key_a || !key_b || !out_collides) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "input or out is NULL");
    }
    *out_collides = false;
    if (strcmp(key_a, key_b) == 0) {
        return TP_C0_OK; /* identical keys are not a collision */
    }
    /* Fold both keys via utf8proc directly and compare the malloc'd results, so a
     * near-limit key whose case-fold expands (up to ~3x) is never rejected by a
     * fixed-size buffer. Both allocations are consumed and freed inside this TU. */
    const utf8proc_option_t fold = (utf8proc_option_t)(UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE);
    utf8proc_uint8_t *fold_a = NULL;
    size_t la = 0;
    tp_c0_detail d = utf8_map_alloc(key_a, fold, &fold_a, &la, err);
    if (d != TP_C0_OK) {
        return d;
    }
    utf8proc_uint8_t *fold_b = NULL;
    size_t lb = 0;
    d = utf8_map_alloc(key_b, fold, &fold_b, &lb, err);
    if (d != TP_C0_OK) {
        free(fold_a);
        return d;
    }
    *out_collides = (la == lb) && memcmp(fold_a, fold_b, la) == 0;
    free(fold_a);
    free(fold_b);
    return TP_C0_OK;
}

static bool is_reserved_component(const char *comp, size_t len) {
    size_t base = 0;
    while (base < len && comp[base] != '.') {
        base++;
    }
    if (base < 3 || base > 4) {
        return false;
    }
    char up[5];
    for (size_t i = 0; i < base; i++) {
        up[i] = ascii_upper(comp[i]);
    }
    up[base] = '\0';
    if (base == 3) {
        return strcmp(up, "CON") == 0 || strcmp(up, "PRN") == 0 || strcmp(up, "AUX") == 0 ||
               strcmp(up, "NUL") == 0;
    }
    /* base == 4: COM1-9 / LPT1-9 */
    bool com_lpt = (strncmp(up, "COM", 3) == 0 || strncmp(up, "LPT", 3) == 0);
    return com_lpt && up[3] >= '1' && up[3] <= '9';
}

tp_c0_detail tp_c0_srckey_portability(const char *normalized_key, unsigned *out_flags, tp_error *err) {
    if (!normalized_key || !out_flags) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "input or out is NULL");
    }
    unsigned flags = TP_C0_PORT_OK;
    const char *p = normalized_key;
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
            continue;
        }
        if (is_reserved_component(start, len)) {
            flags |= TP_C0_PORT_RESERVED_NAME;
        }
        if (start[len - 1] == '.' || start[len - 1] == ' ') {
            flags |= TP_C0_PORT_TRAILING_DOT_SPACE;
        }
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)start[i];
            if (c < 0x20U) {
                flags |= TP_C0_PORT_INVALID_CHAR;
            } else if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') {
                flags |= TP_C0_PORT_INVALID_CHAR;
            }
        }
    }
    *out_flags = flags;
    return TP_C0_OK;
}
