#ifndef TP_C0_OP_H
#define TP_C0_OP_H

/*
 * C0-02 task 1+2: the append-only semantic operation catalog and the
 * selector-resolution boundary.
 *
 * Master spec §6 (all persistent mutation is a typed semantic operation targeting
 * stable IDs, never array indexes or mutable names), §5.4 (a selector resolves to
 * exactly one ID before validation; ambiguity is an error + candidate list), §6.2
 * (no raw JSON Patch: every mutation is a named op with a closed field vocabulary),
 * §59 items 6, 9-19, 52. No production schema and no mutation engine here -- this
 * pins the wire-neutral vocabulary and the request-edge -> canonical-ID boundary.
 *
 * Every current CLI mutation verb maps to a catalog kind below (see the info
 * table's cli_verb column and C0-02-contract.md §1); there is deliberately no
 * "raw field patch" escape hatch.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_c0/tp_c0_error.h"
#include "tp_c0/tp_c0_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Append-only: new kinds are added before TP_C0_OP_KIND_COUNT, never reordered
 * or removed (renumbering would break journaled/persisted operation records). */
typedef enum tp_c0_op_kind {
    TP_C0_OP_INVALID = 0,

    TP_C0_OP_ATLAS_CREATE,
    TP_C0_OP_ATLAS_REMOVE,
    TP_C0_OP_ATLAS_RENAME,
    TP_C0_OP_ATLAS_SETTINGS_SET,

    TP_C0_OP_SOURCE_ADD,
    TP_C0_OP_SOURCE_REMOVE,
    TP_C0_OP_SOURCE_REPLACE,

    TP_C0_OP_SPRITE_OVERRIDE_SET,
    TP_C0_OP_SPRITE_OVERRIDE_CLEAR,
    TP_C0_OP_SPRITE_NAME_SET,

    TP_C0_OP_ANIMATION_CREATE,
    TP_C0_OP_ANIMATION_REMOVE,
    TP_C0_OP_ANIMATION_SETTINGS_SET,
    TP_C0_OP_ANIMATION_FRAMES_SET,
    TP_C0_OP_ANIMATION_FRAME_ADD,
    TP_C0_OP_ANIMATION_FRAME_REMOVE,
    TP_C0_OP_ANIMATION_FRAME_MOVE,

    TP_C0_OP_TARGET_CREATE,
    TP_C0_OP_TARGET_REMOVE,
    TP_C0_OP_TARGET_SET,

    TP_C0_OP_KIND_COUNT
} tp_c0_op_kind;

/* The four before/after diff classes every operation falls into (task 6, spec
 * §9). The class fixes what a committed op must record for exact reverse apply:
 *   CREATE - after = full new entity + ordering position; before = nothing.
 *   REMOVE - before = full removed entity + ordering position; after = nothing.
 *   MOVE   - before index/position, after index/position; identity unchanged.
 *   SET    - before field value(s), after field value(s). */
typedef enum tp_c0_op_class {
    TP_C0_OP_CLASS_CREATE = 0,
    TP_C0_OP_CLASS_REMOVE,
    TP_C0_OP_CLASS_MOVE,
    TP_C0_OP_CLASS_SET
} tp_c0_op_class;

/* One catalog row. `target_kind` is the ID kind of the op's PRIMARY target
 * (the entity it creates/removes/moves/sets); atlas-scoped ops also carry a
 * parent atlas ID in their payload but the primary target is the row's kind.
 * `cli_verb` is the current ntpacker verb that lowers to this op, or NULL for a
 * spec-listed op with no current verb (reserved -- see contract §1). */
typedef struct tp_c0_op_info {
    tp_c0_op_kind kind;
    const char *wire;          /* canonical "op" string, e.g. "atlas.create" */
    tp_c0_op_class effect;     /* before/after diff class */
    tp_c0_id_kind target_kind; /* primary target ID kind (INVALID = project-level) */
    const char *cli_verb;      /* current CLI verb, or NULL if reserved */
} tp_c0_op_info;

/* Catalog accessors. by_wire / kind_from_wire return INVALID/NULL for an unknown
 * wire name (the caller reports TP_C0_ERR_OP_UNKNOWN -- never guesses). */
const tp_c0_op_info *tp_c0_op_info_by_kind(tp_c0_op_kind kind);
const tp_c0_op_info *tp_c0_op_info_by_wire(const char *wire);
tp_c0_op_kind tp_c0_op_kind_from_wire(const char *wire);
const char *tp_c0_op_wire(tp_c0_op_kind kind);      /* "" for INVALID/out-of-range */
const char *tp_c0_op_class_name(tp_c0_op_class cls); /* "create"/"remove"/"move"/"set" */

/* Inverse of tp_c0_op_class_name (single source of truth): maps a class name back
 * to its enum. *ok is false (and SET returned) for an unknown/NULL name. */
tp_c0_op_class tp_c0_op_class_from_name(const char *name, bool *ok);

/* Self-check: every catalog row (k_ops and k_fields) sits at its own kind index,
 * so a reorder can't silently mis-map a kind to the wrong wire/field vocabulary.
 * Returns true when the tables are consistent. Pinned by test_c0_op. */
bool tp_c0_op_catalog_selfcheck(void);

/* The CLOSED canonical field vocabulary of an op (addressing `*_id` keys plus
 * typed payload keys; excludes the "op" discriminator itself). This is what makes
 * "no raw field patch" (spec §6.2) executable: a key outside this set is an
 * TP_C0_ERR_UNKNOWN_FIELD. Returns the array and writes *count (>=0). */
const char *const *tp_c0_op_fields(tp_c0_op_kind kind, int *count);

/* True if `key` is "op" or a member of the op's closed field vocabulary. */
bool tp_c0_op_field_allowed(tp_c0_op_kind kind, const char *key);

/* ---- selector-resolution boundary (task 2) ------------------------------- */

/* How a request addresses an operation target. Selectors exist ONLY at the
 * request edge; the canonical committed operation stores an ID (SEL_ID). The
 * validate step resolves NAME/INDEX selectors to a single ID before any apply. */
typedef enum tp_c0_sel_kind {
    TP_C0_SEL_ID = 0, /* already a resolved shape ID (canonical form) */
    TP_C0_SEL_NAME,   /* a mutable name/key: atlas name, anim id, sprite key, exporter id */
    TP_C0_SEL_INDEX   /* an array index (e.g. `target remove <n>`) */
} tp_c0_sel_kind;

#define TP_C0_SEL_NAME_CAP 256

typedef struct tp_c0_selector {
    tp_c0_sel_kind kind;
    tp_c0_id_kind target_kind;      /* which entity kind is being addressed */
    tp_c0_id128 id;                 /* valid when kind == SEL_ID */
    char name[TP_C0_SEL_NAME_CAP];  /* valid when kind == SEL_NAME (NUL-terminated) */
    int index;                      /* valid when kind == SEL_INDEX */
} tp_c0_selector;

/* A resolvable entity, as the session would expose it to the resolver. Source
 * path selectors are normalized via C0-01 tp_c0_srckey BEFORE this layer sees
 * them, so `name` compares are plain byte-equality here (no path logic). */
typedef struct tp_c0_entity_ref {
    tp_c0_id_kind kind;
    tp_c0_id128 id;
    const char *name; /* mutable name/key/exporter-id; NULL if unnamed */
    int index;        /* stable position within its kind */
} tp_c0_entity_ref;

/* Resolve `sel` against `entities` (n rows) to exactly one ID.
 *   SEL_ID    -> the id must exist in the table, else selector_unresolved.
 *   SEL_NAME  -> exactly one same-kind entity with an equal name, else
 *                selector_unresolved (0 matches) / selector_ambiguous (>1);
 *                on ambiguity `err` prose lists the candidate IDs.
 *   SEL_INDEX -> exactly one same-kind entity whose index matches.
 * On success writes *out_id. Never aborts. */
tp_c0_detail tp_c0_selector_resolve(const tp_c0_selector *sel, const tp_c0_entity_ref *entities, int n,
                                    tp_c0_id128 *out_id, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_OP_H */
