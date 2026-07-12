#ifndef TP_C0_JW_H
#define TP_C0_JW_H

/*
 * C0-02 internal deterministic JSON writer -- NOT installed (src/, not include/).
 * Mirrors the byte-stable conventions proven in packer/src/tp_sb.h (2-space
 * indent, LF, integral numbers without a decimal point, "%.9g" for fractional,
 * ascending-key object order enforced by the caller). The spike cannot include
 * tp_core's private tp_sb.h from its public-only include path, so this small
 * writer restates the same rules for the txn request/result encoders.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct tp_c0_jw {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} tp_c0_jw;

static inline void tp_c0_jw_raw(tp_c0_jw *w, const char *s, size_t n) {
    if (w->oom) {
        return;
    }
    if (w->len + n + 1U > w->cap) {
        size_t nc = (w->cap == 0) ? 512U : w->cap;
        while (w->len + n + 1U > nc) {
            nc *= 2U;
        }
        char *nb = (char *)realloc(w->buf, nc);
        if (!nb) {
            w->oom = true;
            return;
        }
        w->buf = nb;
        w->cap = nc;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static inline void tp_c0_jw_str(tp_c0_jw *w, const char *s) { tp_c0_jw_raw(w, s, strlen(s)); }
static inline void tp_c0_jw_char(tp_c0_jw *w, char c) { tp_c0_jw_raw(w, &c, 1U); }

static inline void tp_c0_jw_indent(tp_c0_jw *w, int depth) {
    for (int i = 0; i < depth; i++) {
        tp_c0_jw_str(w, "  ");
    }
}

static inline void tp_c0_jw_int(tp_c0_jw *w, long v) {
    char tmp[32];
    (void)snprintf(tmp, sizeof tmp, "%ld", v);
    tp_c0_jw_str(w, tmp);
}

/* "%.9g" round-trips a float exactly and drops a trailing ".0" for integral
 * values, matching tp_project.c's byte-stable float formatting. */
static inline void tp_c0_jw_num(tp_c0_jw *w, double v) {
    char tmp[64];
    (void)snprintf(tmp, sizeof tmp, "%.9g", v);
    tp_c0_jw_str(w, tmp);
}

static inline void tp_c0_jw_bool(tp_c0_jw *w, bool v) { tp_c0_jw_str(w, v ? "true" : "false"); }

static inline void tp_c0_jw_json_string(tp_c0_jw *w, const char *s) {
    tp_c0_jw_char(w, '"');
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        switch (*c) {
            case '"': tp_c0_jw_str(w, "\\\""); break;
            case '\\': tp_c0_jw_str(w, "\\\\"); break;
            case '\b': tp_c0_jw_str(w, "\\b"); break;
            case '\f': tp_c0_jw_str(w, "\\f"); break;
            case '\n': tp_c0_jw_str(w, "\\n"); break;
            case '\r': tp_c0_jw_str(w, "\\r"); break;
            case '\t': tp_c0_jw_str(w, "\\t"); break;
            default:
                if (*c < 0x20U) {
                    char esc[8];
                    (void)snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*c);
                    tp_c0_jw_str(w, esc);
                } else {
                    tp_c0_jw_char(w, (char)*c);
                }
                break;
        }
    }
    tp_c0_jw_char(w, '"');
}

/* Opens the next "key": slot at `depth`, handling the leading comma/newline. */
static inline void tp_c0_jw_key(tp_c0_jw *w, int depth, bool *first, const char *key) {
    tp_c0_jw_str(w, *first ? "\n" : ",\n");
    *first = false;
    tp_c0_jw_indent(w, depth);
    tp_c0_jw_json_string(w, key);
    tp_c0_jw_str(w, ": ");
}

#endif /* TP_C0_JW_H */
