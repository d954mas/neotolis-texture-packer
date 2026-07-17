#ifndef TP_CORE_TP_SRCKEY_H
#define TP_CORE_TP_SRCKEY_H

/*
 * Persistent source-local key normalization + portability validation (master spec
 * §5.3, §5.6, §59 item 8).
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
 * and "button.png" are distinct keys but a reported cross-platform collision
 * (§5.3: never silently merged).
 *
 * These are pure text primitives (no disk access): they take ARBITRARY bytes and
 * never abort on caller input -- every fault is a structured tp_status. All
 * outputs go into caller buffers (no cross-CRT malloc handoff): the utf8proc
 * allocation is consumed and freed inside the translation unit.
 *
 * SCOPE: this header promotes these primitives and validates source paths with them;
 * full sprite-key resolution (tp_sprite_id wiring) happens separately.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max normalized-key / fold-key length accepted. NFC/casefold can expand a string
 * up to ~3x, so this leaves headroom over a path-length input. #define (never a
 * const size_t: macos -Wgnu-folding-constant rejects that as a VLA bound). */
#define TP_SRCKEY_MAX 4096

/* Normalize `input` (a source-root-relative path) into a canonical NFC key in
 * `out` (capacity `cap`). Faults:
 *   TP_STATUS_INVALID_ARGUMENT  NULL input/out, or empty result (e.g. "." / "///")
 *   TP_STATUS_KEY_ABSOLUTE      absolute input (leading separator / drive prefix)
 *   TP_STATUS_KEY_TRAVERSAL     a '..' component would escape the source root
 *   TP_STATUS_INVALID_UTF8      input is not well-formed UTF-8
 *   TP_STATUS_OUT_OF_BOUNDS     the result does not fit in `cap`
 *   TP_STATUS_OOM               utf8proc allocation failed */
tp_status tp_srckey_normalize(const char *input, char *out, size_t cap, tp_error *err);

/* Case-fold (Unicode casefold + NFC) a NORMALIZED key into `out`, for use as a
 * collision-map key. Full case-fold can expand the input up to ~3x, so size `out`
 * accordingly or expect TP_STATUS_OUT_OF_BOUNDS. (tp_srckey_collides folds
 * internally and is NOT limited by this.) */
tp_status tp_srckey_casefold(const char *normalized_key, char *out, size_t cap, tp_error *err);

/* Cross-platform collision: two normalized keys whose case-folds are equal but
 * which are not byte-identical. *out_collides is set on success. Handles keys
 * whose fold expands past TP_SRCKEY_MAX (folds via a heap buffer, freed here). */
tp_status tp_srckey_collides(const char *key_a, const char *key_b, bool *out_collides, tp_error *err);

/* Portability findings on a NORMALIZED key (a bitmask; 0 == portable). These are
 * validation warnings, not normalization errors. */
enum {
    TP_SRCKEY_PORT_OK = 0,
    TP_SRCKEY_PORT_RESERVED_NAME = 1 << 0,     /* CON/PRN/AUX/NUL/COM1-9/LPT1-9 */
    TP_SRCKEY_PORT_INVALID_CHAR = 1 << 1,      /* one of <>:"|?* or a control char */
    TP_SRCKEY_PORT_TRAILING_DOT_SPACE = 1 << 2 /* a component ends with '.' or ' ' */
};

tp_status tp_srckey_portability(const char *normalized_key, unsigned *out_flags, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SRCKEY_H */
