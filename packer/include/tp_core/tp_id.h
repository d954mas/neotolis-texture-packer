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

/* 64-bit bucket hash for in-memory maps/sets (NOT the persistent ID). */
uint64_t tp_id128_bucket(tp_id128 id);

/* ----- shape ID (F1-01, promoted from C0-01 tp_c0_id) -------------------- *
 * The entity kind carried by the textual prefix. Binary IDs do not embed the
 * kind -- the storing field decides it -- so the prefix is a presentation and
 * validation affordance; the persistent binary ID is the 16 bytes. SOURCE is
 * kept in the enum (append-only) for F1-02 even though F1-01 attaches no source
 * id field yet. */
typedef enum tp_id_kind {
    TP_ID_KIND_INVALID = 0,
    TP_ID_KIND_ATLAS,
    TP_ID_KIND_SOURCE,
    TP_ID_KIND_ANIM,
    TP_ID_KIND_TARGET
} tp_id_kind;

/* Longest text: "source_"/"target_" (7) + 32 hex + NUL = 40. Fixed via #define
 * (never a const size_t: macos -Wgnu-folding-constant rejects that as a VLA). */
#define TP_ID_TEXT_CAP 40

/* "atlas_"/"source_"/"anim_"/"target_", or "" for INVALID. */
const char *tp_id_kind_prefix(tp_id_kind kind);

/* Canonical text: <prefix> + 32 LOWERCASE hex. `cap` must be >= TP_ID_TEXT_CAP.
 * Rejects TP_ID_KIND_INVALID -> TP_STATUS_ID_MALFORMED; a too-small buffer ->
 * TP_STATUS_OUT_OF_BOUNDS; NULL out -> TP_STATUS_INVALID_ARGUMENT. Formats the
 * nil ID as "<prefix>0...0" (callers that require a real ID reject nil first). */
tp_status tp_id_format(tp_id_kind kind, tp_id128 id, char *out, size_t cap, tp_error *err);

/* Parse a shape ID. Prefix is case-SENSITIVE lowercase; hex digits accept both
 * cases and re-emit lowercase. The nil (all-zero) value parses OK -- callers
 * that require a real ID reject it via tp_id128_is_nil. out_kind/out_id may be
 * NULL. Any bad prefix/hex/length/trailing/empty input -> TP_STATUS_ID_MALFORMED
 * (prose carries the specific reason); NULL text -> TP_STATUS_INVALID_ARGUMENT. */
tp_status tp_id_parse(const char *text, tp_id_kind *out_kind, tp_id128 *out_id, tp_error *err);

/* ----- versioned stable hash (FNV-1a/128; endian-stable, no __int128) ----- *
 * Deterministic, platform-independent 128-bit hash: same input => same output on
 * every OS (byte-at-a-time mixing, big-endian output packing -- no reinterpret).
 * This is the primitive under sprite/legacy IDs. */
tp_id128 tp_hash128(const void *data, size_t len);

/* Streaming form of the same hash, for composing a hash from several pieces
 * without concatenating them into one buffer (used by sprite/legacy IDs). */
typedef struct tp_hasher {
    uint64_t hi, lo;
} tp_hasher;

tp_hasher tp_hasher_init(void);
void tp_hasher_update(tp_hasher *h, const void *data, size_t len);
tp_id128 tp_hasher_final(tp_hasher h);

/* sprite_id = stable_hash("sid1" tag + source_id bytes + 0x00 + normalized_key).
 * The "sid1" algorithm tag is versioned: changing the mix is a visible change. A
 * logical/export rename does not change this; a source-local key change does.
 * PROMOTED-BUT-UNUSED in F1-01: it needs a real source_id, introduced in F1-02
 * (tagged sources) and wired into sprite resolution in F1-03. */
tp_id128 tp_sprite_id(tp_id128 source_id, const char *normalized_key);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_ID_H */
