/* rand_s (Windows) needs _CRT_RAND_S before <stdlib.h>. */
#define _CRT_RAND_S

#include "tp_c0/tp_c0_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/random.h> /* getentropy: Linux glibc >= 2.25, macOS >= 10.12 */
#endif

/* ======================================================================== */
/* RNG seam                                                                  */
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
     * chunks. No per-ID fopen of /dev/urandom. A failure is surfaced as rng_fail
     * (return -1) upstream -- getentropy never short-reads. */
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

tp_c0_rng tp_c0_rng_os(void) {
    tp_c0_rng rng = {rng_os_fill, NULL};
    return rng;
}

/* ======================================================================== */
/* id128 core                                                                */
/* ======================================================================== */

tp_c0_id128 tp_c0_id128_nil(void) {
    tp_c0_id128 id;
    memset(id.bytes, 0, sizeof id.bytes);
    return id;
}

bool tp_c0_id128_eq(tp_c0_id128 a, tp_c0_id128 b) {
    return memcmp(a.bytes, b.bytes, sizeof a.bytes) == 0;
}

bool tp_c0_id128_is_nil(tp_c0_id128 id) {
    for (size_t i = 0; i < sizeof id.bytes; i++) {
        if (id.bytes[i] != 0U) {
            return false;
        }
    }
    return true;
}

uint64_t tp_c0_id128_bucket(tp_c0_id128 id) {
    /* FNV-1a 64 over the 16 bytes -- an in-memory bucket key, not persistent. */
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < sizeof id.bytes; i++) {
        h ^= (uint64_t)id.bytes[i];
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

tp_c0_detail tp_c0_id128_generate(const tp_c0_rng *rng, tp_c0_id128 *out, tp_error *err) {
    if (!out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "id128 out pointer is NULL");
    }
    *out = tp_c0_id128_nil();
    if (!rng || !rng->fill) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "rng or rng->fill is NULL");
    }
    uint8_t buf[16];
    int n = rng->fill(rng->ctx, buf, sizeof buf);
    if (n < 0) {
        return tp_c0_fail(err, TP_C0_ERR_RNG_FAIL, "rng reported failure (return %d)", n);
    }
    if ((size_t)n != sizeof buf) {
        return tp_c0_fail(err, TP_C0_ERR_RNG_SHORT, "rng produced %d of 16 bytes", n);
    }
    memcpy(out->bytes, buf, sizeof buf);
    return TP_C0_OK;
}

/* ======================================================================== */
/* versioned stable hash: FNV-1a/128 (portable, no __int128 extension)       */
/* ======================================================================== */

#define TP_C0_FNV128_PRIME_HI 0x0000000001000000ULL
#define TP_C0_FNV128_PRIME_LO 0x000000000000013BULL

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

tp_c0_hasher tp_c0_hasher_init(void) {
    tp_c0_hasher s = {0x6c62272e07bb0142ULL, 0x62b821756295c58dULL};
    return s;
}

void tp_c0_hasher_update(tp_c0_hasher *h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h->lo ^= (uint64_t)p[i];
        mul128(h->hi, h->lo, TP_C0_FNV128_PRIME_HI, TP_C0_FNV128_PRIME_LO, &h->hi, &h->lo);
    }
}

tp_c0_id128 tp_c0_hasher_final(tp_c0_hasher s) {
    tp_c0_id128 out;
    for (int i = 0; i < 8; i++) {
        unsigned sh = (unsigned)(56 - 8 * i);
        out.bytes[i] = (uint8_t)(s.hi >> sh);
        out.bytes[8 + i] = (uint8_t)(s.lo >> sh);
    }
    return out;
}

tp_c0_id128 tp_c0_hash128(const void *data, size_t len) {
    tp_c0_hasher s = tp_c0_hasher_init();
    tp_c0_hasher_update(&s, data, len);
    return tp_c0_hasher_final(s);
}

tp_c0_id128 tp_c0_sprite_id(tp_c0_id128 source_id, const char *normalized_key) {
    static const char tag[4] = {'s', 'i', 'd', '1'}; /* versioned algorithm tag */
    static const uint8_t sep = 0x00U;
    tp_c0_hasher s = tp_c0_hasher_init();
    tp_c0_hasher_update(&s, tag, sizeof tag);
    tp_c0_hasher_update(&s, source_id.bytes, sizeof source_id.bytes);
    tp_c0_hasher_update(&s, &sep, 1U);
    if (normalized_key) {
        tp_c0_hasher_update(&s, normalized_key, strlen(normalized_key));
    }
    return tp_c0_hasher_final(s);
}

/* ======================================================================== */
/* shape ID (prefix + hex)                                                   */
/* ======================================================================== */

const char *tp_c0_id_kind_prefix(tp_c0_id_kind kind) {
    switch (kind) {
        case TP_C0_ID_KIND_ATLAS: return "atlas_";
        case TP_C0_ID_KIND_SOURCE: return "source_";
        case TP_C0_ID_KIND_ANIM: return "anim_";
        case TP_C0_ID_KIND_TARGET: return "target_";
        case TP_C0_ID_KIND_INVALID: return "";
    }
    return "";
}

tp_c0_detail tp_c0_id_format(tp_c0_id_kind kind, tp_c0_id128 id, char *out, size_t cap, tp_error *err) {
    if (!out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "format out pointer is NULL");
    }
    if (kind == TP_C0_ID_KIND_INVALID) {
        return tp_c0_fail(err, TP_C0_ERR_ID_BAD_PREFIX, "cannot format an INVALID kind");
    }
    const char *prefix = tp_c0_id_kind_prefix(kind);
    size_t plen = strlen(prefix);
    size_t need = plen + 32U + 1U;
    if (cap < need) {
        return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "need %zu bytes, have %zu", need, cap);
    }
    memcpy(out, prefix, plen);
    static const char hexd[16] = "0123456789abcdef";
    for (size_t i = 0; i < 16U; i++) {
        out[plen + 2U * i] = hexd[id.bytes[i] >> 4];
        out[plen + 2U * i + 1U] = hexd[id.bytes[i] & 0x0FU];
    }
    out[plen + 32U] = '\0';
    return TP_C0_OK;
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

tp_c0_detail tp_c0_id_parse(const char *text, tp_c0_id_kind *out_kind, tp_c0_id128 *out_id, tp_error *err) {
    if (out_kind) {
        *out_kind = TP_C0_ID_KIND_INVALID;
    }
    if (out_id) {
        *out_id = tp_c0_id128_nil();
    }
    if (!text) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "parse text is NULL");
    }
    if (text[0] == '\0') {
        return tp_c0_fail(err, TP_C0_ERR_EMPTY, "empty shape ID");
    }

    static const struct {
        const char *p;
        tp_c0_id_kind k;
    } kinds[] = {
        {"atlas_", TP_C0_ID_KIND_ATLAS},
        {"source_", TP_C0_ID_KIND_SOURCE},
        {"anim_", TP_C0_ID_KIND_ANIM},
        {"target_", TP_C0_ID_KIND_TARGET},
    };
    tp_c0_id_kind kind = TP_C0_ID_KIND_INVALID;
    const char *body = NULL;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        size_t plen = strlen(kinds[i].p);
        if (strncmp(text, kinds[i].p, plen) == 0) {
            kind = kinds[i].k;
            body = text + plen;
            break;
        }
    }
    if (kind == TP_C0_ID_KIND_INVALID || !body) {
        return tp_c0_fail(err, TP_C0_ERR_ID_BAD_PREFIX, "unknown or missing kind prefix");
    }

    tp_c0_id128 id = tp_c0_id128_nil();
    for (size_t i = 0; i < 16U; i++) {
        char c0 = body[2U * i];
        if (c0 == '\0') {
            return tp_c0_fail(err, TP_C0_ERR_ID_BAD_LENGTH, "body shorter than 32 hex digits");
        }
        char c1 = body[2U * i + 1U]; /* safe: c0 != NUL, so index 2i is in-string */
        if (c1 == '\0') {
            return tp_c0_fail(err, TP_C0_ERR_ID_BAD_LENGTH, "body shorter than 32 hex digits");
        }
        int hi = hexval(c0);
        int lo = hexval(c1);
        if (hi < 0 || lo < 0) {
            return tp_c0_fail(err, TP_C0_ERR_ID_BAD_HEX, "non-hex digit in 128-bit body");
        }
        id.bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    if (body[32] != '\0') {
        return tp_c0_fail(err, TP_C0_ERR_ID_TRAILING, "trailing bytes after 32 hex digits");
    }

    if (out_kind) {
        *out_kind = kind;
    }
    if (out_id) {
        *out_id = id;
    }
    return TP_C0_OK;
}
