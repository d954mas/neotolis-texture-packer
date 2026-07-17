#ifndef TP_CORE_TP_PROJECT_MIGRATE_H
#define TP_CORE_TP_PROJECT_MIGRATE_H

/*
 * Schema-v2 identity migration + writable promotion (master spec §5, §5.5).
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
 *      every NIL id AND re-randomizes every loader-SYNTHESIZED id (id_synthetic) via
 *      the injected RNG -- so a migrated legacy project persists fresh random ids on
 *      its first writable save, while a real loaded id (v3/v4, or a v2 file's
 *      atlas/anim/target id) is preserved. Idempotent (once nothing is nil or
 *      synthetic, a second call is a no-op) and atomic (an RNG fault leaves every ID
 *      unchanged).
 *
 * The low-level deterministic assigner (tp_legacy_*) is exposed for reuse and for
 * the collision-fallback tests; project code calls the tp_project_* wrappers.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"
#include "tp_core/tp_project.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_sprite_index;

/* ----- low-level deterministic legacy assigner ------ *
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

/* ----- project-level identity migration / promotion ---------------------- *
 * Fill every NIL structural ID (atlas/animation/target) with a DETERMINISTIC
 * synthetic ID derived from entity kind + stable legacy structure (atlas: index;
 * animation: "atlasIdx|name"; target: "atlasIdx|exporter_id|out_path"). Entities
 * that already carry an ID are left untouched. Repeated calls on the same model
 * produce identical IDs (master spec §5.5). The loader applies this so a read-only
 * consumer sees stable IDs; TP_STATUS_ID_COLLISION_EXHAUSTED is unreachable with
 * the default hash. */
tp_status tp_project_assign_legacy_ids(tp_project *p, tp_error *err);

/* Sources-only variant: synthesize deterministic ids for NIL source ids
 * ONLY, leaving atlas/animation/target ids untouched. Used by the v2->v3 migration
 * path -- a v2 file already carries atlas/anim/target ids but no source ids, so a
 * nil atlas/anim/target id there stays a genuine anomaly for validate to reject
 * (decision 0008). Source tuple = "<atlasIdx>|<path>". Idempotent. */
tp_status tp_project_assign_legacy_source_ids(tp_project *p, tp_error *err);

/* Writable promotion: give a fresh RANDOM ID (via `rng`) to every structural entity
 * whose id is NIL (freshly created) OR was SYNTHESIZED by the loader for a legacy gap
 * (id_synthetic), then clear the flag and freeze. So a migrated legacy project's first
 * writable save persists fresh random ids, not the loader's stable synthetic ones
 * (master spec §5.5). A REAL loaded id -- a v3/v4 id, or a v2 file's atlas/anim/target
 * id -- has id_synthetic == false and is PRESERVED (per-entity granularity: a v2 file
 * re-randomizes only its synthesized source ids). ATOMIC -- an RNG fault
 * (TP_STATUS_RNG_FAILED) leaves every ID and flag unchanged. IDEMPOTENT -- once no id
 * is nil or synthetic this is a no-op, so a writable session that promotes before its
 * first mutation never re-changes an ID. */
tp_status tp_project_promote_ids(tp_project *p, const tp_rng *rng, tp_error *err);

/* Validate structural-ID integrity: no two atlas/animation/target entities may
 * share a persistent ID, and no required ID may be nil. -> TP_STATUS_DUPLICATE_ID
 * / TP_STATUS_ID_MALFORMED with context, else TP_STATUS_OK. Run by the loader
 * after IDs are resolved; also usable by validate tooling. */
tp_status tp_project_validate_ids(const tp_project *p, tp_error *err);

/* ----- lazy v3->v4 sprite re-keying (master spec §5.2, decision 0009) ---- *
 * Rewrite PENDING sprite overrides (a v3 name-keyed record, or one added by name
 * before a scan: source_ref nil / src_key NULL) onto their canonical {source_ref,
 * src_key} using the CURRENT resolved sprite index `idx` (which the caller built by
 * scanning). Load can NOT do this (the export-key name has no extension; the
 * source-local key does), so it happens here at first successful resolution and the
 * next save persists the v4 form.
 *
 * Per pending record, matched by its export-key bridge against the index:
 *   - exactly one match  -> filled in (record becomes migrated); the derived
 *     sprite_id is tp_sprite_id(source_id, src_key);
 *   - zero matches       -> left pending (a soft orphan: it applies to nothing now
 *     and re-keys automatically if the file returns);
 *   - more than one match -> TP_STATUS_INVALID_ARGUMENT, NEVER guessed (an
 *     ambiguous legacy reference across sources -- §5.6).
 * A record that is ALREADY migrated is untouched (so a stored orphan keeps its
 * identity and reactivates only if that canonical source/key returns). Mutation is
 * atomic for the selected atlas: ambiguity or OOM leaves every record unchanged. */
tp_status tp_project_resolve_atlas_sprites(tp_project *p, int atlas_index, const struct tp_sprite_index *idx,
                                           tp_error *err);

/* Project-level owner for lazy legacy sprite/frame reference migration. The
 * operation is atomic across every atlas: it resolves on a clone and swaps only
 * after every scan/allocation succeeds. */
bool tp_project_has_pending_sprite_refs(const tp_project *project);
tp_status tp_project_migrate_sprite_refs(tp_project *project, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PROJECT_MIGRATE_H */
