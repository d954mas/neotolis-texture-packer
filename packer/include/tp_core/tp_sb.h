#ifndef TP_CORE_TP_SB_H
#define TP_CORE_TP_SB_H

/* The one deterministic JSON/text string builder for tp_core and its clients
 * (2-space indent, LF, %.9g floats, sorted keys). Header-only static inline so
 * every writer -- project file, exporters, operation/transaction encoders, CLI
 * payloads -- reuses these without a separate TU. */

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_utf8.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Test-only allocation-failure seam, deliberately narrower than
 * TP_ENABLE_TEST_SEAMS (same per-binary shape as the CLI's inspect/arena fault
 * seams): only a TU compiled with TP_SB_ALLOC_FAULT_SEAM gains the check, and
 * that TU's own binary supplies the predicate. tp_core and every shipping
 * target are built without the macro, so a shipped build contains neither the
 * branch nor an undefined symbol. It exists because tp_sb is header-only static
 * inline: without it, a fault binary can only pre-poison the individual
 * builders it can name, which silently narrows an "every allocation fails"
 * fault to a handful of call sites. */
#if defined(TP_SB_ALLOC_FAULT_SEAM)
bool tp_sb_alloc_fault_active(void);
#endif

typedef struct tp_sb {
    char *buf;
    size_t len;
    size_t cap;
    size_t limit; /* 0 = unlimited; otherwise maximum bytes excluding NUL */
    bool count_only; /* exact allocation-free sizing pass */
    size_t *allocation_count; /* optional deterministic test/perf probe */
    bool oom;
    bool limit_exceeded;
} tp_sb;

/* Releases the buffer; the sticky oom/limit_exceeded verdict is preserved so a
 * caller can free first and report afterwards. */
static inline void tp_sb_free(tp_sb *sb) {
    if (sb) {
        free(sb->buf);
        sb->buf = NULL;
        sb->len = 0U;
        sb->cap = 0U;
    }
}

static inline void tp_sb_write(tp_sb *sb, const char *s, size_t n) {
    if (sb->oom || sb->limit_exceeded) {
        return;
    }
    if (sb->len == SIZE_MAX || n > SIZE_MAX - sb->len - 1U) {
        sb->oom = true;
        return;
    }
    if (sb->limit != 0U && (sb->len > sb->limit || n > sb->limit - sb->len)) {
        sb->limit_exceeded = true;
        return;
    }
    if (sb->count_only) {
        sb->len += n;
        return;
    }
    const size_t needed = sb->len + n + 1U;
    if (needed > sb->cap) {
        size_t new_cap = (sb->cap == 0) ? 1024U : sb->cap;
        while (needed > new_cap) {
            if (new_cap > SIZE_MAX / 2U) {
                new_cap = needed;
                break;
            }
            new_cap *= 2U;
        }
        if (sb->limit != 0U && sb->limit != SIZE_MAX &&
            new_cap > sb->limit + 1U) {
            new_cap = sb->limit + 1U;
        }
#if defined(TP_SB_ALLOC_FAULT_SEAM)
        if (tp_sb_alloc_fault_active()) {
            sb->oom = true;
            return;
        }
#endif
        char *nb = (char *)realloc(sb->buf, new_cap);
        if (!nb) {
            sb->oom = true;
            return;
        }
        if (sb->allocation_count) {
            (*sb->allocation_count)++;
        }
        sb->buf = nb;
        sb->cap = new_cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static inline void tp_sb_str(tp_sb *sb, const char *s) { tp_sb_write(sb, s, strlen(s)); }

static inline void tp_sb_char(tp_sb *sb, char c) { tp_sb_write(sb, &c, 1U); }

static inline void tp_sb_indent(tp_sb *sb, int depth) {
    for (int i = 0; i < depth; i++) {
        tp_sb_str(sb, "  ");
    }
}

static inline void tp_sb_int(tp_sb *sb, long v) {
    char tmp[32];
    (void)snprintf(tmp, sizeof tmp, "%ld", v);
    tp_sb_str(sb, tmp);
}

static inline void tp_sb_uint(tp_sb *sb, unsigned long v) {
    char tmp[32];
    (void)snprintf(tmp, sizeof tmp, "%lu", v);
    tp_sb_str(sb, tmp);
}

/* "%zu": a size_t above ULONG_MAX must not truncate on 32-bit-long Windows. */
static inline void tp_sb_size(tp_sb *sb, size_t v) {
    char tmp[32];
    (void)snprintf(tmp, sizeof tmp, "%zu", v);
    tp_sb_str(sb, tmp);
}

/* 64-bit integral emit via PRId64 (not "%ld") so a value like 5000000000 is
 * byte-identical on 32-bit-long Windows and 64-bit-long Linux/macOS -- the
 * cross-OS determinism pin the transaction JSON contract needs. */
static inline void tp_sb_i64(tp_sb *sb, int64_t v) {
    char tmp[32];
    (void)snprintf(tmp, sizeof tmp, "%" PRId64, v);
    tp_sb_str(sb, tmp);
}

/* "%.9g" round-trips a float exactly (unlike "%g"); locale is "C" in tp_core. */
static inline void tp_sb_num(tp_sb *sb, double v) {
    char tmp[64];
    (void)snprintf(tmp, sizeof tmp, "%.9g", v);
    tp_sb_str(sb, tmp);
}

/* Sanitization is a property of machine OUTPUT: a malformed byte becomes U+FFFD
 * so an emitted document is always decodable rather than one a parser rejects
 * wholesale. Well-formed input passes through byte-identical, so this never
 * fires on validated data. The persisted .ntpacker_project writer is the
 * deliberate exception -- its names are validated upstream and NT_ASSERTed at
 * its own emit wrapper, so corruption is never laundered into the user's file. */
static inline void tp_sb_json_string(tp_sb *sb, const char *s) {
    if (!s) {
        s = "";
    }
    const size_t length = strlen(s);
    tp_sb_char(sb, '"');
    for (size_t offset = 0U;
         offset < length && !sb->oom && !sb->limit_exceeded;) {
        const unsigned char c = (unsigned char)s[offset];
        if (c >= 0x80U) {
            const size_t width =
                tp_utf8_codepoint_width(s + offset, length - offset);
            if (width == 0U) {
                tp_sb_str(sb, "\\ufffd");
                offset++;
            } else {
                tp_sb_write(sb, s + offset, width);
                offset += width;
            }
            continue;
        }
        switch (c) {
            case '"': tp_sb_str(sb, "\\\""); break;
            case '\\': tp_sb_str(sb, "\\\\"); break;
            case '\b': tp_sb_str(sb, "\\b"); break;
            case '\f': tp_sb_str(sb, "\\f"); break;
            case '\n': tp_sb_str(sb, "\\n"); break;
            case '\r': tp_sb_str(sb, "\\r"); break;
            case '\t': tp_sb_str(sb, "\\t"); break;
            default:
                if (c < 0x20U) {
                    char esc[8];
                    (void)snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
                    tp_sb_str(sb, esc);
                } else {
                    tp_sb_char(sb, (char)c);
                }
                break;
        }
        offset++;
    }
    tp_sb_char(sb, '"');
}

/* Opens the next "key": slot at `keydepth`, handling the leading comma. */
static inline void tp_obj_key(tp_sb *sb, int keydepth, bool *first, const char *key) {
    tp_sb_str(sb, *first ? "\n" : ",\n");
    *first = false;
    tp_sb_indent(sb, keydepth);
    tp_sb_json_string(sb, key);
    tp_sb_str(sb, ": ");
}

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SB_H */
