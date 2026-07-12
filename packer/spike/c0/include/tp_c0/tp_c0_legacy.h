#ifndef TP_C0_LEGACY_H
#define TP_C0_LEGACY_H

/*
 * C0-01 task 5: deterministic synthetic IDs for legacy v1 (ID-less) projects
 * (master spec §5.5, §59 item 4; F1-01 task 4).
 *
 * A v1 project has no structural IDs. Loading one synthesizes IDs from
 * (entity kind + stable legacy discriminators). Guarantees:
 *   - repeated read-only loads before save produce identical IDs;
 *   - a base-hash collision is disambiguated deterministically by a salt sweep,
 *     so the collision fallback is itself reproducible across loads.
 *
 * The base hash is an injectable seam so tests can force collisions; the default
 * seam is a versioned stable hash and never exhausts in practice.
 */

#include <stddef.h>
#include <stdint.h>

#include "tp_c0/tp_c0_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One legacy entity to receive a synthetic ID. `tuple` is the caller-built
 * canonical discriminator string derived from the v1 file (e.g. atlas index,
 * or "atlasIdx|exporter_id|out_path" for a target) -- it must be stable across
 * loads of the same file. `id` is filled by tp_c0_legacy_assign. */
typedef struct tp_c0_legacy_entry {
    tp_c0_id_kind kind;
    const char *tuple;
    tp_c0_id128 id; /* OUT */
} tp_c0_legacy_entry;

/* Base-hash seam: (kind, tuple, salt) -> 128-bit base ID. salt==0 is the primary
 * position; the assigner bumps salt on collision. */
typedef tp_c0_id128 (*tp_c0_legacy_hash_fn)(void *ctx, tp_c0_id_kind kind, const char *tuple, uint32_t salt);

/* Default seam: versioned stable hash ("lid1" tag) over kind, tuple, and salt. */
tp_c0_id128 tp_c0_legacy_hash_default(void *ctx, tp_c0_id_kind kind, const char *tuple, uint32_t salt);

/* Assign deterministic, unique, non-nil synthetic IDs to entries[0..n). Entries
 * are processed in array order; on a collision with an already-assigned entry
 * (or a nil hash) the salt is incremented and the base hash re-evaluated until
 * unique. `hash`==NULL uses tp_c0_legacy_hash_default. Structured error only if
 * the bounded salt sweep cannot disambiguate (unreachable with the default). */
tp_c0_detail tp_c0_legacy_assign(tp_c0_legacy_entry *entries, size_t n, tp_c0_legacy_hash_fn hash, void *ctx,
                                 tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_LEGACY_H */
