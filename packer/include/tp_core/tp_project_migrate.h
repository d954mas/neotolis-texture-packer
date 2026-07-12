#ifndef TP_CORE_TP_PROJECT_MIGRATE_H
#define TP_CORE_TP_PROJECT_MIGRATE_H

/*
 * F1-01 schema-v2 identity migration + writable promotion (master spec §5, §5.5).
 * Kept in its own TU so tp_project.c's loader/serializer is not bloated by the
 * id-assignment policy.
 *
 * Two id-assignment strategies live here:
 *   1. Deterministic LEGACY synthesis (read-only / temporary): a v1 (id-less)
 *      project loaded for inspection gets synthetic IDs derived from entity kind
 *      + stable legacy structure. Repeated loads before save produce identical
 *      IDs (master spec §5.5). This is what tp_project_load applies.
 *   2. Writable PROMOTION: a writable session assigns FINAL random IDs before the
 *      first mutation and never re-changes them (master spec §5.5: "the next
 *      successful save persists normal random IDs"). tp_project_promote_ids fills
 *      any nil structural ID via the injected RNG; it is idempotent (a second
 *      call is a no-op) and atomic (an RNG fault leaves every ID unchanged).
 *
 * The low-level deterministic assigner (tp_legacy_*) is exposed for reuse and for
 * the collision-fallback tests; project code calls the tp_project_* wrappers.
 */

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----- low-level deterministic legacy assigner (promoted from C0-01) ------ *
 * One legacy entity to receive a synthetic ID. `tuple` is the caller-built
 * canonical discriminator derived from the v1 file (e.g. atlas index, or
 * "atlasIdx|exporter_id|out_path" for a target); it must be stable across loads
 * of the same file. `id` is filled by tp_legacy_assign. */
typedef struct tp_legacy_entry {
    tp_id_kind kind;
    const char *tuple;
    tp_id128 id; /* OUT */
} tp_legacy_entry;

/* Base-hash seam: (kind, tuple, salt) -> 128-bit base ID. salt==0 is the primary
 * position; the assigner bumps salt on collision. Injectable so tests can force
 * collisions; NULL selects tp_legacy_hash_default. */
typedef tp_id128 (*tp_legacy_hash_fn)(void *ctx, tp_id_kind kind, const char *tuple, uint32_t salt);

/* Default seam: versioned stable hash ("lid1" tag) over kind, tuple, and salt. */
tp_id128 tp_legacy_hash_default(void *ctx, tp_id_kind kind, const char *tuple, uint32_t salt);

/* Assign deterministic, unique, non-nil synthetic IDs to entries[0..n) in array
 * order; on a collision with an already-assigned entry (or a nil hash) the salt
 * is bumped and the base hash re-evaluated until unique. `hash`==NULL uses
 * tp_legacy_hash_default. TP_STATUS_ID_COLLISION_EXHAUSTED only if the bounded
 * salt sweep cannot disambiguate (unreachable with the default hash). */
tp_status tp_legacy_assign(tp_legacy_entry *entries, size_t n, tp_legacy_hash_fn hash, void *ctx, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PROJECT_MIGRATE_H */
