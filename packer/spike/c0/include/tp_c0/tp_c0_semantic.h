#ifndef TP_C0_SEMANTIC_H
#define TP_C0_SEMANTIC_H

/*
 * C0-02 task 4: the semantic-state identity partition.
 *
 * Master spec §8: `dirty = current semantic state identity != saved baseline
 * identity`, and dirty is NOT derived from revision numbers. §9 (Undo stores
 * semantic diffs), §59 items 13-14, 22, 29-32. This pins which PERSISTENT fields
 * feed that identity (and thus revision/Undo/dirty) and which RUNTIME state is
 * excluded. It is a table, not an engine: the golden test pins the two sets and
 * their disjointness; F2 computes the actual identity hash from this partition.
 *
 * Ordering rule (contract §4): the identity is computed over the persistent model
 * with each entity collection keyed by its stable ID and order-normalized
 * (atlases/sources/sprites/animations/targets are ID-addressed and have no
 * reorder operation), EXCEPT an animation's `frames`, whose order IS semantic
 * content (playback order). `ordered == true` marks that single exception.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A persistent field (or collection) that participates in semantic-state
 * identity. `ordered` is meaningful only for a collection field and is true only
 * where element order is itself semantic. */
typedef struct tp_c0_semantic_field {
    const char *entity; /* "project"/"atlas"/"source"/"sprite"/"animation"/"target" */
    const char *field;  /* field or collection name */
    bool ordered;       /* collection whose element order is semantic (anim frames) */
} tp_c0_semantic_field;

int tp_c0_semantic_field_count(void);
const tp_c0_semantic_field *tp_c0_semantic_field_at(int i); /* NULL if out of range */

/* Runtime state categories explicitly EXCLUDED from semantic-state identity.
 * Changing any of these never marks the project dirty and is never an Undo
 * entry (spec §8, §9.3, §10-11, §59 items 22, 31-32). */
int tp_c0_runtime_excluded_count(void);
const char *tp_c0_runtime_excluded_at(int i); /* NULL if out of range */

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_SEMANTIC_H */
