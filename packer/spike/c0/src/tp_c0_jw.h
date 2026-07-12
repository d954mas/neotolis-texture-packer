#ifndef TP_C0_JW_H
#define TP_C0_JW_H

/*
 * C0-02 internal deterministic JSON writer -- NOT installed (src/, not include/).
 * This is a DELIBERATE spike-local copy of tp_core's private packer/src/tp_sb.h
 * writer (2-space indent, LF, integral numbers without a decimal point, "%.9g"
 * for fractional, ascending-key object order enforced by the caller). tp_sb.h is
 * a tp_core-private header the spike cannot include from its public-only path,
 * and the spike is a throwaway that must not create a cross-module dependency, so
 * the writer is restated here rather than shared. The initial buffer capacity is
 * intentionally reconciled to tp_sb.h's 1024 (a txn document is comfortably
 * larger than a handful of bytes, so a 1 KB first alloc avoids the early
 * doublings); everything else matches tp_sb.h byte-for-byte.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
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
        size_t nc = (w->cap == 0) ? 1024U : w->cap; /* matches tp_sb.h's initial cap */
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

/* PRId64 (not "%ld") so integral output is byte-identical on 32-bit-long Windows
 * and 64-bit-long Linux/macOS -- the cross-OS determinism pin (contract §3). */
static inline void tp_c0_jw_int(tp_c0_jw *w, int64_t v) {
    char tmp[32];
    (void)snprintf(tmp, sizeof tmp, "%" PRId64, v);
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
