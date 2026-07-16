#ifndef TP_CORE_TP_SPRITE_INDEX_H
#define TP_CORE_TP_SPRITE_INDEX_H

/*
 * Resolved sprite index (F1-03, master spec §5.2, §5.4). A RUNTIME structure --
 * never serialized -- produced by scanning an atlas's sources and deriving, for
 * every resolved sprite, its canonical identity:
 *
 *   source_local_key = tp_srckey_normalize(source-root-relative raw path)   (§5.3)
 *   sprite_id        = tp_sprite_id(owning source_id, source_local_key)     (§5.2)
 *
 * The index mirrors tp_pack_input_build's iteration EXACTLY (sources in array
 * order; a folder's entries sorted by rel path; a file source contributes its
 * basename), so ref[i] corresponds to pack desc[i]. This is deliberate: it makes
 * the index the identity view of the SAME sprite set the pack/export path builds,
 * which is why re-keying overrides through it preserves pack/export byte-identity.
 *
 * A logical/export rename does NOT change sprite_id (it derives from source+key,
 * not the display name). An external source-file rename changes the key and is
 * therefore a different sprite (old key missing, new key present) -- §5.2.
 *
 * The index is READ-ONLY over the project (it never mutates the model). It DOES
 * touch disk (folder scan) -- callers that must not scan on load do not build it.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_project;
struct tp_session_snapshot;

/* One resolved sprite: the canonical identity + the human/bridge labels. All
 * strings are malloc-owned by the index and freed by tp_sprite_index_free. */
typedef struct tp_sprite_ref {
    tp_id128 sprite_id;    /* derived = tp_sprite_id(source_id, source_key) */
    tp_id128 source_id;    /* owning source's structural id */
    int source_index;      /* owning source's array index within the atlas (for labels) */
    char *source_key;      /* normalized source-local key (NFC, '/'-sep, EXTENSION KEPT) */
    char *export_key;      /* strip_ext(raw): the override/frame bridge key (folder kept) */
    char *raw_name;        /* source-relative raw name (extension kept) -- the pack desc name */
    char *abs_path;        /* absolute decode path on disk */
} tp_sprite_ref;

typedef struct tp_sprite_index {
    tp_sprite_ref *refs;
    int count;
    int cap;
} tp_sprite_index;

/* "sprite_" + 32 lowercase hex + NUL == 40 == TP_ID_TEXT_CAP (same as source_). */
#define TP_SPRITE_ID_PREFIX "sprite_"

/* Canonical sprite-id text: "sprite_" + 32 lowercase hex. `cap` must be >=
 * TP_ID_TEXT_CAP. Never fails on a valid buffer; a too-small buffer writes "". */
void tp_sprite_id_format(tp_id128 sprite_id, char *out, size_t cap);

/* Parse a "sprite_<32hex>" selector token into *out_id. Prefix is case-sensitive
 * lowercase; hex accepts both cases. Any bad prefix/hex/length -> TP_STATUS_ID_MALFORMED;
 * NULL text/out -> TP_STATUS_INVALID_ARGUMENT. out_id may be NULL (validation only). */
tp_status tp_sprite_id_parse(const char *text, tp_id128 *out_id, tp_error *err);

/* Build the resolved sprite index for atlas[atlas_index]. Deterministic order ==
 * the pack input order. Missing / unresolvable sources contribute nothing (not an
 * error: a missing source is a model state). Read-only over `p`. On success *out
 * owns its refs; free with tp_sprite_index_free even on the empty result. */
tp_status tp_sprite_index_build(const struct tp_project *p, int atlas_index, tp_sprite_index *out, tp_error *err);
/* Snapshot-owned frontend seam. The model pointer remains component-private. */
tp_status tp_sprite_index_build_snapshot(const struct tp_session_snapshot *snapshot,
                                         int atlas_index, tp_sprite_index *out,
                                         tp_error *err);

/* Frees every ref's strings + the refs array and zeroes *idx. NULL-safe. */
void tp_sprite_index_free(tp_sprite_index *idx);

/* --- lookups (all O(n); n is small) --- */

/* By derived sprite_id. NULL if absent / nil id. */
const tp_sprite_ref *tp_sprite_index_by_id(const tp_sprite_index *idx, tp_id128 sprite_id);

/* By the export-key bridge (ext-stripped, folder-kept). Returns the FIRST match and
 * writes the total match count to *out_matches (if non-NULL): >1 means the name is
 * ambiguous ACROSS sources (§5.4/§5.6) and must be disambiguated by a compound
 * selector -- callers never silently pick the first. NULL if zero matches. */
const tp_sprite_ref *tp_sprite_index_by_export_key(const tp_sprite_index *idx, const char *export_key,
                                                   int *out_matches);

/* By owning (source_id, source_key) -- the canonical persisted identity. Exactly one
 * sprite has a given (source, key). NULL if absent. This is the compound-selector
 * disambiguation seam: two sources with the same filename resolve here to distinct
 * refs even though tp_sprite_index_by_export_key reports them as ambiguous. */
const tp_sprite_ref *tp_sprite_index_by_source_key(const tp_sprite_index *idx, tp_id128 source_id,
                                                   const char *source_key);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SPRITE_INDEX_H */
