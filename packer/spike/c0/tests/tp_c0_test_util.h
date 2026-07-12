#ifndef TP_C0_TEST_UTIL_H
#define TP_C0_TEST_UTIL_H

/* Shared C0-01 test helpers (test-only, not shipped). */

#include "tp_c0/tp_c0_id.h"

/* Lowercase 32-hex text of a 128-bit ID (big-endian: bytes[0] first). */
static inline void id_hex(tp_c0_id128 id, char out[33]) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = h[id.bytes[i] >> 4];
        out[2 * i + 1] = h[id.bytes[i] & 0x0F];
    }
    out[32] = '\0';
}

#endif /* TP_C0_TEST_UTIL_H */
