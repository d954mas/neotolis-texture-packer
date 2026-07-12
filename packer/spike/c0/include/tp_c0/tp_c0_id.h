#ifndef TP_C0_ID_H
#define TP_C0_ID_H

/*
 * C0-01 tasks 2 & 4: 128-bit structural ID primitive, canonical binary/string
 * "shape ID" (prefix + hex) parse/format, an injectable RNG seam, and the
 * versioned stable hash used to derive deterministic sprite/legacy IDs.
 *
 * Master spec §5 (random persistent 128-bit IDs; atlas_/source_/anim_/target_
 * shapes), §5.2 (sprite_id = stable_hash(source_id + normalized key)),
 * §5.4/§5.6 (selectors resolve to a single ID; malformed/nil IDs are reported),
 * §59 items 4-6. No production schema is introduced here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_c0/tp_c0_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical binary shape: 16 raw bytes. bytes[0] is the most-significant octet
 * in the hex text, so the textual and binary orderings agree (big-endian text). */
typedef struct tp_c0_id128 {
    uint8_t bytes[16];
} tp_c0_id128;

/* The entity kind carried by the textual prefix. Binary IDs do not embed the
 * kind -- the storing field decides it -- so the prefix is a presentation and
 * validation affordance, and the persistent binary ID is the 16 bytes. */
typedef enum tp_c0_id_kind {
    TP_C0_ID_KIND_INVALID = 0,
    TP_C0_ID_KIND_ATLAS,
    TP_C0_ID_KIND_SOURCE,
    TP_C0_ID_KIND_ANIM,
    TP_C0_ID_KIND_TARGET
} tp_c0_id_kind;

/* Longest text: "source_"/"target_" (7) + 32 hex + NUL = 40. */
#define TP_C0_ID_TEXT_CAP 40

/* ----- RNG seam (task 2) ------------------------------------------------- */

/* Fill exactly `len` bytes into `out`; return the count actually produced.
 * A return != len (short read) or < 0 (failure) is surfaced as a structured
 * error by tp_c0_id128_generate -- never an abort. `ctx` is caller state. */
typedef int (*tp_c0_rng_fill_fn)(void *ctx, uint8_t *out, size_t len);

typedef struct tp_c0_rng {
    tp_c0_rng_fill_fn fill;
    void *ctx;
} tp_c0_rng;

/* Default OS entropy source (Windows rand_s / POSIX /dev/urandom). Uses no
 * engine-private API. Suitable as F1's production generator seed. */
tp_c0_rng tp_c0_rng_os(void);

/* ----- id128 core -------------------------------------------------------- */

tp_c0_id128 tp_c0_id128_nil(void);
bool tp_c0_id128_eq(tp_c0_id128 a, tp_c0_id128 b);
bool tp_c0_id128_is_nil(tp_c0_id128 id);
/* 64-bit bucket hash for in-memory maps (not the persistent ID). */
uint64_t tp_c0_id128_bucket(tp_c0_id128 id);

/* Generate a random 128-bit ID through `rng`. Structured error (rng_short /
 * rng_fail / null_arg) on any RNG fault; on error *out is left as nil. */
tp_c0_detail tp_c0_id128_generate(const tp_c0_rng *rng, tp_c0_id128 *out, tp_error *err);

/* ----- versioned stable hash (tasks 2 & 5 helper) ------------------------ */

/* Deterministic, platform-independent 128-bit hash (FNV-1a/128). Same input =>
 * same output on every OS. This is the primitive under sprite/legacy IDs. */
tp_c0_id128 tp_c0_hash128(const void *data, size_t len);

/* Streaming form of the same hash, for composing a hash from several pieces
 * without concatenating them into one buffer (used by sprite/legacy IDs). */
typedef struct tp_c0_hasher {
    uint64_t hi, lo;
} tp_c0_hasher;

tp_c0_hasher tp_c0_hasher_init(void);
void tp_c0_hasher_update(tp_c0_hasher *h, const void *data, size_t len);
tp_c0_id128 tp_c0_hasher_final(tp_c0_hasher h);

/* sprite_id = stable_hash("sid1" tag + source_id bytes + 0x00 + normalized_key).
 * The "sid1" algorithm tag is versioned: changing the mix is a visible change.
 * A logical/export rename does not change this; a source-local key change does. */
tp_c0_id128 tp_c0_sprite_id(tp_c0_id128 source_id, const char *normalized_key);

/* ----- shape ID (prefix + hex) parse/format (task 4) --------------------- */

/* "atlas_"/"source_"/"anim_"/"target_", or "" for INVALID. */
const char *tp_c0_id_kind_prefix(tp_c0_id_kind kind);

/* Canonical text: <prefix> + 32 LOWERCASE hex. `cap` must be >= TP_C0_ID_TEXT_CAP.
 * Rejects TP_C0_ID_KIND_INVALID. */
tp_c0_detail tp_c0_id_format(tp_c0_id_kind kind, tp_c0_id128 id, char *out, size_t cap, tp_error *err);

/* Parse a shape ID. Prefix is case-SENSITIVE lowercase; hex digits accept both
 * cases and re-emit lowercase. The nil (all-zero) value parses OK -- callers
 * that require a real ID reject it via tp_c0_id128_is_nil. out_kind/out_id may
 * be NULL. Structured error on bad prefix/hex/length/trailing input. */
tp_c0_detail tp_c0_id_parse(const char *text, tp_c0_id_kind *out_kind, tp_c0_id128 *out_id, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_ID_H */
