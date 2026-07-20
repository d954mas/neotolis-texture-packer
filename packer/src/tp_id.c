/* rand_s (Windows) needs _CRT_RAND_S before <stdlib.h>. */
#define _CRT_RAND_S

#include "tp_core/tp_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/random.h> /* getentropy: Linux glibc >= 2.25, macOS >= 10.12 */
#endif

/* ======================================================================== */
/* RNG seam (no engine-private API, no extra link deps). A short read /     */
/* failure is surfaced upstream as a structured tp_status, never an abort.  */
/* ======================================================================== */

static int rng_os_fill(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
#if defined(_WIN32)
    size_t done = 0;
    while (done < len) {
        unsigned int r = 0;
        if (rand_s(&r) != 0) {
            return -1;
        }
        size_t n = len - done;
        if (n > sizeof r) {
            n = sizeof r;
        }
        memcpy(out + done, &r, n);
        done += n;
    }
    return (int)len;
#elif defined(__linux__) || defined(__APPLE__)
    /* getentropy is all-or-nothing per call and caps at 256 bytes; loop in
     * chunks. No per-ID fopen of /dev/urandom. A failure returns -1 (surfaced
     * as TP_STATUS_RNG_FAILED); getentropy never short-reads. */
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > 256U) {
            n = 256U;
        }
        if (getentropy(out + done, n) != 0) {
            return -1;
        }
        done += n;
    }
    return (int)len;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) {
        return -1;
    }
    size_t got = fread(out, 1U, len, f);
    fclose(f);
    return (got == len) ? (int)len : (int)got;
#endif
}

tp_rng tp_rng_os(void) {
    tp_rng rng = {rng_os_fill, NULL};
    return rng;
}

/* ======================================================================== */
/* id128 core                                                                */
/* ======================================================================== */

tp_id128 tp_id128_nil(void) {
    tp_id128 id;
    memset(id.bytes, 0, sizeof id.bytes);
    return id;
}

bool tp_id128_eq(tp_id128 a, tp_id128 b) {
    return memcmp(a.bytes, b.bytes, sizeof a.bytes) == 0;
}

bool tp_id128_is_nil(tp_id128 id) {
    for (size_t i = 0; i < sizeof id.bytes; i++) {
        if (id.bytes[i] != 0U) {
            return false;
        }
    }
    return true;
}

tp_status tp_id128_generate(const tp_rng *rng, tp_id128 *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "id128 out pointer is NULL");
    }
    *out = tp_id128_nil();
    if (!rng || !rng->fill) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "rng or rng->fill is NULL");
    }
    uint8_t buf[16];
    int n = rng->fill(rng->ctx, buf, sizeof buf);
    if (n < 0) {
        return tp_error_set(err, TP_STATUS_RNG_FAILED, "rng reported failure (return %d)", n);
    }
    if ((size_t)n != sizeof buf) {
        return tp_error_set(err, TP_STATUS_RNG_FAILED, "rng produced %d of 16 bytes", n);
    }
    tp_id128 generated;
    memcpy(generated.bytes, buf, sizeof buf);
    if (tp_id128_is_nil(generated)) {
        return tp_error_set(err, TP_STATUS_RNG_FAILED,
                            "rng produced the reserved nil id");
    }
    *out = generated;
    return TP_STATUS_OK;
}

uint64_t tp_id128_bucket(tp_id128 id) {
    /* FNV-1a 64 over the 16 bytes -- an in-memory bucket key, not persistent. */
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < sizeof id.bytes; i++) {
        h ^= (uint64_t)id.bytes[i];
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

/* ======================================================================== */
/* versioned stable hash: FNV-1a/128 (portable, no __int128 extension)       */
/* ======================================================================== */

#define TP_FNV128_PRIME_HI 0x0000000001000000ULL
#define TP_FNV128_PRIME_LO 0x000000000000013BULL

/* (a * b) mod 2^128, a=(ahi:alo), b=(bhi:blo). Only the low 128 bits are kept. */
static void mul128(uint64_t ahi, uint64_t alo, uint64_t bhi, uint64_t blo, uint64_t *rhi, uint64_t *rlo) {
    uint64_t a0 = alo & 0xFFFFFFFFULL, a1 = alo >> 32;
    uint64_t b0 = blo & 0xFFFFFFFFULL, b1 = blo >> 32;
    uint64_t p00 = a0 * b0;
    uint64_t p01 = a0 * b1;
    uint64_t p10 = a1 * b0;
    uint64_t p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    uint64_t lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    hi += ahi * blo + alo * bhi; /* cross terms contribute to the high word */
    *rhi = hi;
    *rlo = lo;
}

tp_hasher tp_hasher_init(void) {
    tp_hasher s = {0x6c62272e07bb0142ULL, 0x62b821756295c58dULL};
    return s;
}

void tp_hasher_update(tp_hasher *h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h->lo ^= (uint64_t)p[i];
        mul128(h->hi, h->lo, TP_FNV128_PRIME_HI, TP_FNV128_PRIME_LO, &h->hi, &h->lo);
    }
}

tp_id128 tp_hasher_final(tp_hasher s) {
    tp_id128 out;
    for (int i = 0; i < 8; i++) {
        unsigned sh = (unsigned)(56 - 8 * i);
        out.bytes[i] = (uint8_t)(s.hi >> sh);
        out.bytes[8 + i] = (uint8_t)(s.lo >> sh);
    }
    return out;
}

tp_id128 tp_hash128(const void *data, size_t len) {
    tp_hasher s = tp_hasher_init();
    tp_hasher_update(&s, data, len);
    return tp_hasher_final(s);
}

tp_id128 tp_sprite_id(tp_id128 source_id, const char *normalized_key) {
    static const char tag[4] = {'s', 'i', 'd', '1'}; /* versioned algorithm tag */
    static const uint8_t sep = 0x00U;
    tp_hasher s = tp_hasher_init();
    tp_hasher_update(&s, tag, sizeof tag);
    tp_hasher_update(&s, source_id.bytes, sizeof source_id.bytes);
    tp_hasher_update(&s, &sep, 1U);
    if (normalized_key) {
        tp_hasher_update(&s, normalized_key, strlen(normalized_key));
    }
    return tp_hasher_final(s);
}

/* ======================================================================== */
/* shape ID (prefix + hex)                                                   */
/* ======================================================================== */

const char *tp_id_kind_prefix(tp_id_kind kind) {
    switch (kind) {
        case TP_ID_KIND_ATLAS: return "atlas_";
        case TP_ID_KIND_SOURCE: return "source_";
        case TP_ID_KIND_ANIM: return "anim_";
        case TP_ID_KIND_TARGET: return "target_";
        case TP_ID_KIND_INVALID: return "";
    }
    return "";
}

tp_status tp_id_format(tp_id_kind kind, tp_id128 id, char *out, size_t cap, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_id_format: out pointer is NULL");
    }
    if (kind == TP_ID_KIND_INVALID) {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "tp_id_format: cannot format an INVALID kind");
    }
    const char *prefix = tp_id_kind_prefix(kind);
    size_t plen = strlen(prefix);
    size_t need = plen + 32U + 1U;
    if (cap < need) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_id_format: need %zu bytes, have %zu", need, cap);
    }
    memcpy(out, prefix, plen);
    static const char hexd[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    for (size_t i = 0; i < 16U; i++) {
        out[plen + 2U * i] = hexd[id.bytes[i] >> 4];
        out[plen + 2U * i + 1U] = hexd[id.bytes[i] & 0x0FU];
    }
    out[plen + 32U] = '\0';
    return TP_STATUS_OK;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

tp_status tp_id_parse(const char *text, tp_id_kind *out_kind, tp_id128 *out_id, tp_error *err) {
    if (out_kind) {
        *out_kind = TP_ID_KIND_INVALID;
    }
    if (out_id) {
        *out_id = tp_id128_nil();
    }
    if (!text) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_id_parse: text is NULL");
    }
    if (text[0] == '\0') {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "tp_id_parse: empty shape ID");
    }

    /* Reuse tp_id_kind_prefix() as the single source of the prefix strings
     * (INVALID has none, so it is not a candidate). */
    static const tp_id_kind kinds[] = {
        TP_ID_KIND_ATLAS,
        TP_ID_KIND_SOURCE,
        TP_ID_KIND_ANIM,
        TP_ID_KIND_TARGET,
    };
    tp_id_kind kind = TP_ID_KIND_INVALID;
    const char *body = NULL;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        const char *prefix = tp_id_kind_prefix(kinds[i]);
        size_t plen = strlen(prefix);
        if (strncmp(text, prefix, plen) == 0) {
            kind = kinds[i];
            body = text + plen;
            break;
        }
    }
    if (kind == TP_ID_KIND_INVALID || !body) {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "tp_id_parse: unknown or missing kind prefix");
    }

    tp_id128 id = tp_id128_nil();
    for (size_t i = 0; i < 16U; i++) {
        char c0 = body[2U * i];
        if (c0 == '\0') {
            return tp_error_set(err, TP_STATUS_ID_MALFORMED, "tp_id_parse: body shorter than 32 hex digits");
        }
        char c1 = body[2U * i + 1U]; /* safe: c0 != NUL, so index 2i is in-string */
        if (c1 == '\0') {
            return tp_error_set(err, TP_STATUS_ID_MALFORMED, "tp_id_parse: body shorter than 32 hex digits");
        }
        int hi = hexval(c0);
        int lo = hexval(c1);
        if (hi < 0 || lo < 0) {
            return tp_error_set(err, TP_STATUS_ID_MALFORMED, "tp_id_parse: non-hex digit in 128-bit body");
        }
        id.bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    if (body[32] != '\0') {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "tp_id_parse: trailing bytes after 32 hex digits");
    }

    if (out_kind) {
        *out_kind = kind;
    }
    if (out_id) {
        *out_id = id;
    }
    return TP_STATUS_OK;
}
