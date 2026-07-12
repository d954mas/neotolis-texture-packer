#ifndef TP_HEX_H
#define TP_HEX_H

/*
 * Shared lowercase-hex byte encoder (F1-00). ONE definition used by both the
 * production session-key hex (tp_identity_session.c) and its drift-guard tests
 * (test_identity_id.c, test_identity_session.c) so the two can never silently
 * diverge -- a bug in the encoding fails the pinned golden vectors.
 *
 * DELIBERATELY generic (raw bytes -> hex), NOT a tp_id128 formatter: F1-01 owns
 * the full tp_id128 parse/format surface (the "atlas_/source_/..." canonical
 * shape-ID text and its hash). This primitive stays orthogonal so F1-01 need not
 * rework it -- it can build on it or ignore it.
 */

#include <stddef.h>
#include <stdint.h>

/* Write exactly 2*n lowercase hex digits of src[0..n) into `out`, then a NUL.
 * `out` must have room for 2*n + 1 bytes. */
static inline void tp_hex_encode_lower(const uint8_t *src, size_t n, char *out) {
    static const char digits[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    for (size_t i = 0; i < n; i++) {
        out[2U * i] = digits[src[i] >> 4];
        out[2U * i + 1U] = digits[src[i] & 0x0FU];
    }
    out[2U * n] = '\0';
}

#endif /* TP_HEX_H */
