#ifndef TP_C0_SRCKEY_H
#define TP_C0_SRCKEY_H

/*
 * C0-01 task 6: persistent source-local key normalization + portability
 * validation (master spec §5.3, §5.6, §59 item 8).
 *
 * A source key addresses a sprite within a source root. Canonical form is:
 *   - UTF-8, Unicode NFC;
 *   - '/' separator ('\\' is ALSO treated as a separator so a folder authored on
 *     Windows addresses the same sprite on Linux);
 *   - '.' components and repeated separators dropped;
 *   - preserved letter case;
 *   - relative to the source root -- never absolute, never containing '..'.
 *
 * Case-folding is used ONLY for portability collision detection: "Button.png"
 * and "button.png" are distinct keys but a reported cross-platform collision.
 *
 * All outputs go into caller buffers (no cross-CRT malloc handoff): the utf8proc
 * allocation is consumed and freed inside this translation unit.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_c0/tp_c0_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max normalized-key / fold-key length this spike accepts. NFC/casefold can
 * expand a string up to ~3x, so this leaves headroom over a path-length input. */
#define TP_C0_SRCKEY_MAX 4096

/* Normalize `input` (a source-root-relative path) into a canonical NFC key in
 * `out`. Structured error on absolute input, a '..' component, invalid UTF-8, an
 * empty result (e.g. "." or "///"), or overflow. */
tp_c0_detail tp_c0_srckey_normalize(const char *input, char *out, size_t cap, tp_error *err);

/* Case-fold (Unicode casefold + NFC) a NORMALIZED key into `out`, for use as a
 * collision-map key. */
tp_c0_detail tp_c0_srckey_casefold(const char *normalized_key, char *out, size_t cap, tp_error *err);

/* Cross-platform collision: two normalized keys whose case-folds are equal but
 * which are not byte-identical. *out_collides is set on success. */
tp_c0_detail tp_c0_srckey_collides(const char *key_a, const char *key_b, bool *out_collides, tp_error *err);

/* Portability findings on a NORMALIZED key (a bitmask; 0 == portable). These are
 * validation warnings, not normalization errors. */
enum {
    TP_C0_PORT_OK = 0,
    TP_C0_PORT_RESERVED_NAME = 1 << 0,      /* CON/PRN/AUX/NUL/COM1-9/LPT1-9 */
    TP_C0_PORT_INVALID_CHAR = 1 << 1,       /* one of <>:"|?* or a control char */
    TP_C0_PORT_TRAILING_DOT_SPACE = 1 << 2  /* a component ends with '.' or ' ' */
};

tp_c0_detail tp_c0_srckey_portability(const char *normalized_key, unsigned *out_flags, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_SRCKEY_H */
