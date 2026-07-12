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
/* RNG seam (promoted verbatim from C0-01 tp_c0_id.c: no engine-private API,  */
/* no extra link deps). A short read / failure is surfaced upstream as a     */
/* structured tp_status, never an abort.                                     */
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
    memcpy(out->bytes, buf, sizeof buf);
    return TP_STATUS_OK;
}
