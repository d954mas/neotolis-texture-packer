#ifndef TP_CORE_TP_ID_H
#define TP_CORE_TP_ID_H

/*
 * 128-bit runtime ID primitive + injectable RNG seam (F1-00 task 2), promoted
 * from the accepted C0-01 `tp_c0_id` contract (packer/spike/c0/src/tp_c0_id.c).
 *
 * SCOPE / F1-00 <-> F1-01 BOUNDARY (architecture decision, lead review):
 *   F1-00 introduces ONLY what an unsaved-session identity needs: the 16-byte
 *   value, nil/equality, and random generation through an injectable RNG seam.
 *   F1-01 owns the FULL id128 surface and EXTENDS this same header/TU -- it adds
 *   parse/format (the "atlas_/source_/..." shape IDs), the versioned stable hash
 *   (tp_hash128 / sprite_id), and the persistent schema-v2 ID fields. F1-01 must
 *   NOT redefine `tp_id128` or the `tp_rng` seam; it builds on them. Keeping this
 *   minimal here is deliberate so the two packets do not duplicate the primitive.
 *
 * Errors are the production `tp_status` model (never an abort on caller input):
 * an RNG that fails or short-reads yields TP_STATUS_RNG_FAILED, and *out is left
 * nil so a failed identity is never mistaken for a real one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical binary shape: 16 raw bytes. bytes[0] is the most-significant octet
 * in the hex text, so textual and binary orderings agree (big-endian text). */
typedef struct tp_id128 {
    uint8_t bytes[16];
} tp_id128;

/* ----- RNG seam ---------------------------------------------------------- */

/* Fill exactly `len` bytes into `out`; return the count actually produced.
 * A return != len (short read) or < 0 (failure) is surfaced as a structured
 * TP_STATUS_RNG_FAILED by tp_id128_generate -- never an abort. `ctx` is caller
 * state. Tests inject a deterministic fill; production uses tp_rng_os(). */
typedef int (*tp_rng_fill_fn)(void *ctx, uint8_t *out, size_t len);

typedef struct tp_rng {
    tp_rng_fill_fn fill;
    void *ctx;
} tp_rng;

/* Default OS entropy source (Windows rand_s / POSIX getentropy or /dev/urandom).
 * Uses no engine-private API and adds no link dependency. */
tp_rng tp_rng_os(void);

/* ----- id128 core -------------------------------------------------------- */

tp_id128 tp_id128_nil(void);
bool tp_id128_eq(tp_id128 a, tp_id128 b);
bool tp_id128_is_nil(tp_id128 id);

/* Generate a random 128-bit ID through `rng`. On any RNG fault returns
 * TP_STATUS_RNG_FAILED (fills `err`) and leaves *out nil. NULL rng/out ->
 * TP_STATUS_INVALID_ARGUMENT. A healthy RNG never yields the reserved nil. */
tp_status tp_id128_generate(const tp_rng *rng, tp_id128 *out, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_ID_H */
