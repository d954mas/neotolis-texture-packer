#ifndef TP_CORE_TP_SELECTOR_H
#define TP_CORE_TP_SELECTOR_H

/*
 * Canonical selector resolver (F1-03, master spec §5.4). Canonical operations,
 * history entries, and persistent references target IDs; a frontend (CLI/MCP) may
 * accept a human-friendly selector for convenience, but it MUST resolve to exactly
 * one id before it is used. This resolver is that single seam -- it lives in core so
 * every frontend selects the same entity without guessing.
 *
 * Grammar (documented, stable):
 *   1. Canonical id text:
 *        atlas_<32hex> | source_<32hex> | anim_<32hex> | target_<32hex> | sprite_<32hex>
 *      -> the entity carrying that id (ids are unique -> exactly one, or not-found).
 *   2. Scoped compound `<scope>:<rest>`:
 *        atlas:<name> | source:<path> | anim:<name> | target:<out_path> | sprite:<key>
 *          -> resolve <rest> within that kind only.
 *        source_<32hex>:<key>
 *          -> the sprite whose key is <key> WITHIN that source (cross-source
 *             disambiguation: two sources sharing a filename resolve here).
 *   3. Bare token <name> -> search every kind (atlas name, source path, anim name,
 *      target out_path, and -- when a sprite index is supplied -- sprite export/source
 *      key). Exactly one match -> resolved; more than one -> AMBIGUOUS + candidate
 *      list; zero -> not-found. The first match is NEVER silently chosen.
 *
 * Structural kinds (atlas/source/anim/target) are searched project-wide with no disk
 * access. Sprites are searched only in the caller-supplied resolved index (one atlas
 * context) -- pass NULL to resolve structural entities only.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_project;
struct tp_sprite_index;

typedef enum tp_selector_kind {
    TP_SEL_NONE = 0,
    TP_SEL_ATLAS,
    TP_SEL_SOURCE,
    TP_SEL_ANIM,
    TP_SEL_TARGET,
    TP_SEL_SPRITE
} tp_selector_kind;

typedef struct tp_selector_result {
    tp_selector_kind kind;
    tp_id128 id;     /* structural id (atlas/source/anim/target) or sprite_id (SPRITE) */
    int atlas_index; /* owning atlas index (for ATLAS: the atlas itself) */
} tp_selector_result;

/* One ambiguous-match candidate. Fixed-size fields (no cross-TU malloc handoff);
 * `idtext` is the canonical id a caller re-submits to disambiguate. */
typedef struct tp_selector_candidate {
    tp_selector_kind kind;
    tp_id128 id;
    int atlas_index;
    char idtext[TP_ID_TEXT_CAP]; /* atlas_/source_/anim_/target_/sprite_ + hex */
    char label[256];             /* stable human label */
} tp_selector_candidate;

typedef struct tp_selector_candidates {
    tp_selector_candidate *v;
    int count;
    int cap;
    bool oom; /* sticky: a failed grow stops collection */
} tp_selector_candidates;

/* Frees the candidate vector and zeroes *c. NULL-safe. */
void tp_selector_candidates_free(tp_selector_candidates *c);

/* Machine token for a selector kind ("atlas"/"source"/"anim"/"target"/"sprite"/""). */
const char *tp_selector_kind_token(tp_selector_kind kind);

/* Resolve `selector` to exactly one entity (see the grammar above). `sprites` (may
 * be NULL) is a resolved sprite index for atlas `sprite_atlas_index`, used for SPRITE
 * matches. `cand` (may be NULL) receives a STABLE-ordered candidate list ONLY on
 * TP_STATUS_AMBIGUOUS_SELECTOR. Returns TP_STATUS_OK (out filled),
 * TP_STATUS_AMBIGUOUS_SELECTOR, TP_STATUS_NOT_FOUND, or TP_STATUS_INVALID_ARGUMENT
 * (NULL/empty selector). */
tp_status tp_selector_resolve(const struct tp_project *p, const char *selector,
                              const struct tp_sprite_index *sprites, int sprite_atlas_index,
                              tp_selector_result *out, tp_selector_candidates *cand, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SELECTOR_H */
