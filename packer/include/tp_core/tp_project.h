#ifndef TP_CORE_TP_PROJECT_H
#define TP_CORE_TP_PROJECT_H

/*
 * In-memory project model + `.ntpacker_project` JSON load/save (ROADMAP Phase 3,
 * SUMMARY.md §5a). This is the single source of truth the GUI edits live and the
 * CLI runs -- so the model is malloc-owned and mutation-friendly (NOT arena: one
 * long-lived project is edited in place), and every list has add/remove helpers.
 *
 * Serialization contract (ux.md §1 principle 7 -- all mandatory, test-pinned):
 *   - Portable: paths stored PROJECT-RELATIVE, normalized to '/'. Absolute paths
 *     are accepted on load and relativized on save (ux.md §3.6.3).
 *   - Sparse: fields equal to their default are NEVER written; defaults come from
 *     one place (tp_pack_settings_defaults for the packing knobs).
 *   - Deterministic: "version" first, then all other object keys in ascending
 *     ASCII order; 2-space indent; LF line endings; a trailing newline; floats
 *     formatted "%.9g" (round-trip stable, unlike "%g"). Re-saving an unmodified
 *     loaded project is byte-identical (memcmp-pinned).
 *
 * Load rules (ux.md §3.6/§3.7):
 *   - version > current  -> TP_STATUS_BAD_VERSION ("needs a newer ntpacker").
 *   - version < current  -> chained migration hook (a switch; empty at v1).
 *   - unknown keys        -> ignored (forward-compat for minor additions).
 *   - malformed JSON / wrong types -> TP_STATUS_BAD_PROJECT (with context).
 *   - missing source files on disk are NOT load errors (they are model states).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h" /* tp_id128: persistent structural IDs (schema v2) */

#ifdef __cplusplus
extern "C" {
#endif

struct tp_pack_settings;

/* Bump when the on-disk schema changes; add a migration case in the loader.
 * v2 (F1-01): atlas/animation/target carry a persistent tp_id128 `id`; the
 * animation's old string `id` became its logical `name` (id/name split).
 * v3 (F1-02): the bare `sources` string array becomes an array of tagged source
 * OBJECTS {id, kind, path}; each source carries a persistent tp_id128 `id`.
 * A v2 bare-string source migrates to kind=folder (decision 0008).
 * v4 (F1-03): a sparse sprite override is keyed by its owning source + source-local
 * KEY ({source, key}) instead of the mutable atlas-relative `name`, and an animation
 * frame reference likewise carries {source, key} -- so a logical/export rename never
 * moves an override or a frame, and the derived sprite_id survives reorder/reload
 * (decision 0009). The re-key needs a disk scan (name has no extension; the key does)
 * and load MUST NOT scan, so a v3 name-keyed record loads as a "pending" record and
 * is rewritten to {source, key} lazily at first successful resolution; a record whose
 * key never resolves stays orphaned and reactivates when the key returns. A record
 * still in pending form is a valid v4 state (serialized with `name`). */
#define TP_PROJECT_SCHEMA_VERSION 4

/* Per-sprite override. Sparse: an entry exists only when at least one field is
 * non-default. Defaults: origin (0.5,0.5), slice9 all-zero, rename NULL (see the
 * *_DEFAULT constants below).
 *
 * Identity (schema v4, F1-03): the CANONICAL key is the owning source + source-local
 * key, from which sprite_id derives (tp_sprite_id(source_id, src_key)); this survives
 * a logical/export rename and source reorder.
 *   - `source_ref` / `src_key`: the persisted v4 identity. source_ref is nil and
 *     src_key NULL while the record is PENDING (loaded from a v3 file, or added by
 *     name before any scan) -- it cannot be keyed to (source, key) without a disk
 *     scan, which load never does. Lazy resolution fills them (tp_project_migrate).
 *   - `name`: the export-KEY bridge (ext-stripped, folder-kept). ALWAYS populated for
 *     an active record and the key the name-based pack/export path still matches on,
 *     so re-keying does NOT change which override applies to which packed sprite. For
 *     a migrated (v4) record it is derived on load = strip_ext(src_key) -- no scan.
 * A migrated record whose (source_ref, src_key) resolves to no current sprite is an
 * ORPHAN: stored verbatim, inactive (the name bridge naturally matches nothing), and
 * reactivating when the key returns. */
typedef struct tp_project_sprite {
    char *name;          /* export-key bridge (ext-stripped, folder-kept); the name-based apply key */
    tp_id128 source_ref; /* owning source's structural id; nil = pending (unresolved) */
    char *src_key;       /* normalized source-local key (NFC, ext KEPT); NULL = pending */
    float origin_x;
    float origin_y;
    uint16_t slice9_lrtb[4]; /* [left,right,top,bottom] px; all-zero = none */
    char *rename;            /* final export name override (NULL = file-derived); consumed by tp_normalize overrides */

    /* Optional per-sprite packing overrides (owner scope 2026-07-10). Sparse:
     * TP_PROJECT_OV_INHERIT (-1) = inherit the atlas value (never serialized).
     * shape uses atlas-shape semantics (0=RECT,1=CONVEX_HULL,2=CONCAVE_CONTOUR);
     * allow_rotate 0 = force no-rotate (the engine has no force-rotate). margin/
     * extrude/max_vertices carry the raw value; an explicit 0 is unrepresentable
     * engine-side and is rejected by tp_pack (documented follow-up). */
    int16_t ov_shape;
    int16_t ov_allow_rotate;
    int16_t ov_max_vertices;
    int16_t ov_margin;
    int16_t ov_extrude;
} tp_project_sprite;

#define TP_PROJECT_ORIGIN_DEFAULT 0.5F
#define TP_PROJECT_OV_INHERIT (-1)

/* One animation frame reference: a sprite in the same atlas, in playback order.
 * Schema v4 (F1-03): keyed by its owning source + source-local key, exactly like a
 * sprite override, so a frame reference targets the derived sprite_id and survives
 * reorder/reload/logical-rename (decision 0009). Mirror of tp_project_sprite's
 * identity fields:
 *   - `source_ref` / `src_key`: the canonical v4 identity; nil / NULL while PENDING
 *     (a v3 name-keyed frame, or one added by name before a scan) -- re-keyed lazily.
 *   - `name`: the export-key bridge (the frame's human/display reference); ALWAYS set
 *     for an active frame and what the name-based export path (tp_normalize) resolves
 *     against, so re-keying does not change which sprite a frame resolves to. */
typedef struct tp_project_frame {
    char *name;          /* export-key bridge (display reference); the name-based resolve key */
    tp_id128 source_ref; /* owning source's structural id; nil = pending */
    char *src_key;       /* normalized source-local key (NFC, ext KEPT); NULL = pending */
} tp_project_frame;

/* Flipbook metadata over sprite references, orthogonal to placement (SUMMARY.md §5a).
 * `frames` are frame references in explicit playback order (schema v4: {source, key},
 * see tp_project_frame).
 *
 * id/name split (F1-01, schema v2): `id` is the persistent structural ID (survives
 * rename/reorder/save/reload); `name` is the logical/display name and the human
 * reference key. The v1 string `id` migrated into `name`. */
typedef struct tp_project_anim {
    tp_id128 id;       /* persistent structural ID (schema v2); nil until assigned/promoted */
    bool id_synthetic; /* TRANSIENT (never serialized): the loader synthesized this id for a
                        * legacy gap -- the first writable promote re-randomizes it (§5.5, decision 0007) */
    char *name;    /* logical/display name; the name-keyed reference (was v1 `id`) */
    tp_project_frame *frames;
    int frame_count;
    int frame_cap; /* internal: allocation capacity of `frames` */
    float fps;     /* default 30 */
    int playback;  /* stable id; 0 = once-forward (default) */
    bool flip_h;
    bool flip_v;
} tp_project_anim;

#define TP_PROJECT_ANIM_FPS_DEFAULT 30.0F
#define TP_PROJECT_ANIM_PLAYBACK_DEFAULT 0

/* One export target: a pluggable exporter id + its output path. `enabled`
 * defaults true (sparse: only written when false). */
typedef struct tp_project_target {
    tp_id128 id;       /* persistent structural ID (schema v2); nil until assigned/promoted */
    bool id_synthetic; /* TRANSIENT (never serialized): the loader synthesized this id for a
                        * legacy gap -- the first writable promote re-randomizes it (§5.5, decision 0007) */
    char *exporter_id; /* exporter kind, e.g. "json-neotolis", "defold" (NOT the structural id) */
    char *out_path;    /* project-relative output path/prefix */
    bool enabled;
} tp_project_target;

/* Source kind (schema v3, F1-02). Master spec §11 models a source as
 * `kind: path | atlas`, where a "path" source is either a scanned folder or a
 * single image file; this enum makes that file-vs-folder sub-distinction explicit.
 * APPEND-ONLY (the value is the stored classification): TP_SOURCE_KIND_ATLAS
 * (foreign atlas descriptor) is reserved for Epic B1. `folder` is the zero/default
 * value -- a migrated v2 bare-string source and any add_source without a kind
 * become `folder` (decision 0008). Serialized as a string token ("folder" is the
 * omitted sparse default; "file" is written). */
typedef enum tp_source_kind {
    TP_SOURCE_KIND_FOLDER = 0, /* recursively scanned folder (default) */
    TP_SOURCE_KIND_FILE = 1    /* single image file */
    /* TP_SOURCE_KIND_ATLAS = 2  -- reserved for Epic B1; do NOT use before then */
} tp_source_kind;

/* One tagged source (schema v3): a persistent structural id + its kind + the
 * folder/file path (project-relative, '/'-normalized; stored verbatim, save
 * relativizes absolute forms). `id` starts nil until assigned/promoted. Scan
 * classifies file-vs-folder at runtime by stat, so a stored kind that disagrees
 * with disk still packs correctly; kind is authoritative only where disk cannot
 * be consulted (a missing source, for F1-03 sprite-id derivation). */
typedef struct tp_project_source {
    tp_id128 id;         /* persistent structural ID (schema v3); nil until assigned/promoted */
    bool id_synthetic;   /* TRANSIENT (never serialized): the loader synthesized this id for a legacy
                          * gap (a v2 file's bare-string source) -- the first writable promote
                          * re-randomizes it, while a real v3/v4 loaded source id is left untouched */
    tp_source_kind kind; /* folder (default) / file */
    char *path;          /* folder/file path, project-relative, '/'-normalized */
} tp_project_source;

/* One atlas: packing knobs (mirror tp_pack_settings) + live-linked sources +
 * sparse per-sprite overrides + animations + export targets. All arrays are
 * malloc-owned dynamic vectors; use the helpers below to mutate them. */
typedef struct tp_project_atlas {
    tp_id128 id;       /* persistent structural ID (schema v2); nil until assigned/promoted */
    bool id_synthetic; /* TRANSIENT (never serialized): the loader synthesized this id for a
                        * legacy gap -- the first writable promote re-randomizes it (§5.5, decision 0007) */
    char *name;

    /* Packing knobs -- mirror tp_pack_settings; seeded by tp_pack_settings
     * defaults (single source of truth). Written sparsely (only if != default). */
    int max_size;
    int padding;
    int margin;
    int extrude;
    int alpha_threshold;
    int max_vertices;
    int shape; /* nt_atlas_shape_t: 0=RECT, 1=CONVEX_HULL, 2=CONCAVE_CONTOUR */
    bool allow_transform;
    bool power_of_two;
    float pixels_per_unit;

    tp_project_source *sources; /* tagged source records (schema v3) */
    int source_count;
    int source_cap;

    tp_project_sprite *sprites; /* sparse overrides */
    int sprite_count;
    int sprite_cap;

    tp_project_anim *animations;
    int animation_count;
    int animation_cap;

    tp_project_target *targets;
    int target_count;
    int target_cap;
} tp_project_atlas;

/* The whole project. `project_dir` is the absolute directory of the project file
 * (set on load/save; NULL while unsaved) and the base for path resolution. */
typedef struct tp_project {
    int schema_version;
    char *project_dir; /* absolute; NULL if never saved */
    tp_project_atlas *atlases;
    int atlas_count;
    int atlas_cap;
} tp_project;

/* --- lifecycle --- */

/* New project with one default atlas ("atlas1", default knobs). NULL on OOM. */
tp_project *tp_project_create(void);

/* Frees the project and everything it owns. NULL-safe. */
void tp_project_destroy(tp_project *p);

/* Deep-clones `src` into a fresh independent project (every malloc-owned field
 * duplicated: project_dir, atlas name/knobs/ids, sources, sparse sprite overrides,
 * animations + frames, targets). OOM-SAFE: on any allocation failure returns NULL
 * and frees the partial clone (no leak). The clone is byte-identical under
 * tp_project_save_buffer (test-pinned). This is the atomicity primitive the F2-02
 * transaction engine clones the model with before applying a batch (§7). NULL src
 * -> NULL. */
tp_project *tp_project_clone(const tp_project *src);

/* --- atlas mutation --- */

/* Appends an atlas with default knobs. On success writes its index to
 * *out_index (if non-NULL). Returns TP_STATUS_INVALID_ARGUMENT / TP_STATUS_OOM. */
tp_status tp_project_add_atlas(tp_project *p, const char *name, int *out_index);

/* Removes atlas `index` (frees its owned data). Out-of-range -> OUT_OF_BOUNDS. */
tp_status tp_project_remove_atlas(tp_project *p, int index);

/* Bounds-checked accessor; NULL if index is out of range. */
tp_project_atlas *tp_project_get_atlas(tp_project *p, int index);

/* Returns the index of the atlas whose structural id equals `id`, or -1 if none /
 * nil id. Persistent references (and the F2 operation engine) target the id, not
 * the array index, which reorder/remove invalidates (master spec §5.4). */
int tp_project_find_atlas_by_id(const tp_project *p, tp_id128 id);

/* Resets `a`'s packing knobs to the shared defaults (tp_pack_settings_defaults).
 * The name and lists are left untouched. */
void tp_project_atlas_set_defaults(tp_project_atlas *a);

/* Renames atlas `a` in place: frees the old name and dups `name` (must be non-NULL
 * and non-empty -> TP_STATUS_INVALID_ARGUMENT otherwise). OOM leaves the old name
 * intact. The single home for the atlas-name write both frontends need -- the GUI
 * did this inline (gui_project.c set_atlas_name) and the CLI `atlas rename` verb
 * calls it; uniqueness is a frontend policy, not enforced here (mirrors add_atlas). */
tp_status tp_project_set_atlas_name(tp_project_atlas *a, const char *name);

/* --- source mutation --- */

/* Appends a source path with an explicit `kind` (stored verbatim; save
 * normalizes/relativizes it; `id` starts nil -- a writable session assigns it via
 * tp_project_promote_ids). Dedupe: a no-op returning TP_STATUS_OK when the same
 * '/'-normalized path is already present in this atlas (source_count is unchanged
 * -- the caller detects the no-op by comparing the count). The kind of an existing
 * duplicate is NOT changed. Frontends that know the kind (a folder- vs file-picker
 * dialog) call this; migration and kind-agnostic callers use add_source (folder). */
tp_status tp_project_atlas_add_source_kind(tp_project_atlas *a, const char *path, tp_source_kind kind);

/* add_source_kind(a, path, TP_SOURCE_KIND_FOLDER): the kind-agnostic default (the
 * migration default and the safe classification when the caller does not know). */
tp_status tp_project_atlas_add_source(tp_project_atlas *a, const char *path);

/* True when the atlas already holds a source whose '/'-normalized path equals `path`'s
 * -- the exact predicate add_source_kind uses to dedupe. Lets a caller (the op-engine
 * validator) reject a would-be-deduped add BEFORE it strands a new source's id. */
bool tp_project_atlas_has_source_path(const tp_project_atlas *a, const char *path);

/* Removes source `index`. Out-of-range -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_source(tp_project_atlas *a, int index);

/* --- source id addressing (schema v3) --- persistent references target the id,
 * not the array index (which reorder/remove invalidates; master spec §5.4). */

/* Returns the source whose structural id equals `id`, or NULL if none / nil id. */
tp_project_source *tp_project_atlas_find_source_by_id(tp_project_atlas *a, tp_id128 id);

/* Removes the source whose structural id equals `id`. Absent / nil -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_source_by_id(tp_project_atlas *a, tp_id128 id);

/* --- sprite-override mutation --- */

/* Returns the override for `name`, or NULL if none exists. */
tp_project_sprite *tp_project_atlas_find_sprite(tp_project_atlas *a, const char *name);

/* Returns the existing override for `name`, or appends a new default one. The
 * entry is written to *out (if non-NULL). */
tp_status tp_project_atlas_add_sprite(tp_project_atlas *a, const char *name, tp_project_sprite **out);

/* Removes the override for `name`. Absent -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_sprite(tp_project_atlas *a, const char *name);

/* Drops the override entry for `name` IF it now holds only defaults (keeps storage
 * sparse -- the invariant tp_project_sprite documents). A no-op TP_STATUS_OK when the
 * entry is absent or still carries a non-default field. The single home for the
 * sparse-prune both frontends do after clearing a field back to inherit (the CLI
 * `sprite set <field>=inherit` path; the GUI does the same inline after an override
 * edit). Composes existing find + remove -- no new sparse rule. */
tp_status tp_project_atlas_prune_sprite(tp_project_atlas *a, const char *name);

/* Sets (or clears) a sprite's `rename` export-name override. A non-empty `rename`
 * ensures the override entry and stores it verbatim; NULL or "" clears it and
 * removes the entry if it then holds only defaults (keeps storage sparse). */
tp_status tp_project_atlas_set_sprite_rename(tp_project_atlas *a, const char *sprite_name, const char *rename);

/* --- animation mutation --- */

/* Appends an animation with logical `name` (default fps/playback/flips, no frames;
 * `id` starts nil -- a writable session assigns it via tp_project_promote_ids).
 * Written to *out (if non-NULL). Use tp_project_anim_add_frame to populate frames
 * in order. The name-keyed API is retained for F1-03 to migrate to id selectors. */
tp_status tp_project_atlas_add_animation(tp_project_atlas *a, const char *name, tp_project_anim **out);

/* Removes the animation whose logical `name` matches. Absent -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_animation(tp_project_atlas *a, const char *name);

/* --- animation id addressing (schema v2) --- the F2 operation engine addresses
 * an animation by its structural id, not its mutable name/array index (§5.4). */

/* Returns the animation whose structural id equals `id` within `a`, or NULL if
 * none / nil id. */
tp_project_anim *tp_project_atlas_find_animation_by_id(tp_project_atlas *a, tp_id128 id);

/* Removes the animation whose structural id equals `id`. Absent / nil -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_animation_by_id(tp_project_atlas *a, tp_id128 id);

/* Appends a frame (sprite name) to an animation, preserving order. */
tp_status tp_project_anim_add_frame(tp_project_anim *anim, const char *frame_name);

/* Removes frame `index` (0-based), preserving the order of the rest. Out-of-range -> OUT_OF_BOUNDS. */
tp_status tp_project_anim_remove_frame(tp_project_anim *anim, int index);

/* Moves frame `index` by `delta` slots (negative = earlier), clamping the destination into range and
 * preserving every other frame's relative order. A move that changes nothing is an OK no-op.
 * Out-of-range `index` -> OUT_OF_BOUNDS. */
tp_status tp_project_anim_move_frame(tp_project_anim *anim, int index, int delta);

/* --- target mutation --- */

/* Appends an export target (enabled by default). Written to *out (if non-NULL). */
tp_status tp_project_atlas_add_target(tp_project_atlas *a, const char *exporter_id, const char *out_path,
                                      tp_project_target **out);

/* Removes target `index`. Out-of-range -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_target(tp_project_atlas *a, int index);

/* --- target id addressing (schema v2) --- the F2 operation engine addresses a
 * target by its structural id, not its array index (§5.4). */

/* Returns the target whose structural id equals `id` within `a`, or NULL if none /
 * nil id. */
tp_project_target *tp_project_atlas_find_target_by_id(tp_project_atlas *a, tp_id128 id);

/* Removes the target whose structural id equals `id`. Absent / nil -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_target_by_id(tp_project_atlas *a, tp_id128 id);

/* Replaces target `index`'s fields (exporter_id + out_path duped, enabled set).
 * Both strings required + non-empty. Out-of-range -> OUT_OF_BOUNDS; OOM leaves the
 * target unchanged. Deterministic save is preserved (sparse `enabled` rule holds). */
tp_status tp_project_atlas_set_target(tp_project_atlas *a, int index, const char *exporter_id, const char *out_path,
                                      bool enabled);

/* Appends the default export target -- the full-fidelity json-neotolis exporter
 * (TP_EXPORTER_ID_JSON_NEOTOLIS) with out path "out/<atlas_name>" -- so a
 * freshly created project exports something. The single home for default-target
 * seeding both frontends call (review §3.1; L-5 keeps tp_project_create itself
 * target-free). Adds unconditionally; callers that only want to seed an EMPTY
 * atlas guard on target_count first. Out-of-range atlas_index -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_seed_default_target(tp_project *p, int atlas_index);

/* True iff SOME target in the project OTHER than `exclude` has this exact (non-empty)
 * out_path. Scans EVERY atlas's targets (an out_path collision is project-wide: two
 * atlases exporting to one file silently overwrite each other). Only ENABLED targets
 * count -- the exporter skips disabled targets, so a disabled target never overwrites.
 * out_paths are compared SLASH-NORMALIZED (the same tp_normalize_slashes the exporter
 * applies) so "out\x" and "out/x" resolve to one file and DO collide. An empty/NULL
 * `out_path` never "collides" (that is the separate empty-out_path check) -> false.
 * `self` is the caller's own target (excluded by POINTER identity, so this is robust
 * even when target ids are nil -- unpromoted / RNG-fault sessions); a NULL `self`
 * excludes nothing. The shared source of truth for the duplicate check both frontends
 * surface (validate + the GUI target panel). NULL p -> false. */
bool tp_project_out_path_shared(const tp_project *p, const char *out_path, const tp_project_target *self);

/* --- load / save --- */

/* Parses `path` into a new project (*out). project_dir is set to path's absolute
 * directory. Errors: TP_STATUS_BAD_VERSION (newer schema), TP_STATUS_BAD_PROJECT
 * (open/parse/type error). On any error *out is set to NULL and `err` is filled. */
tp_status tp_project_load(const char *path, tp_project **out, tp_error *err);

/* Like tp_project_load, additionally returning the fingerprint of the exact byte
 * buffer successfully parsed. `out_fingerprint` is optional and is cleared on
 * failure. This avoids reopening the path after load (which could hash different
 * bytes after an external replacement). */
tp_status tp_project_load_with_fingerprint(const char *path, tp_project **out, tp_id128 *out_fingerprint,
                                           tp_error *err);

/* Writes `p` deterministically to `path` (see the serialization contract above),
 * updating p->project_dir to path's absolute directory and relativizing absolute
 * source paths against it. `p` is non-const because Save/Save-As updates the
 * in-memory project_dir + path forms (mutation-friendly, GUI live edit).
 * file-save = relativize + tp_project_save_buffer + fwrite. */
tp_status tp_project_save(tp_project *p, const char *path, tp_error *err);

/* Like tp_project_save, additionally returning the fingerprint of the exact
 * serialized buffer successfully written and atomically promoted to `path`.
 * `out_fingerprint` is optional and is cleared on failure. */
tp_status tp_project_save_with_fingerprint(tp_project *p, const char *path, tp_id128 *out_fingerprint,
                                           tp_error *err);

/* Staged optimistic save for an already-bound identity. The original project is not path-normalized
 * unless publish succeeds. After the sibling temp is fully written, the destination's exact bytes are
 * fingerprinted immediately before atomic replace; a missing/different/unreadable destination returns
 * TP_STATUS_FILE_CHANGED_EXTERNALLY and leaves both destination and `p` unchanged. */
tp_status tp_project_save_if_unchanged(tp_project *p, const char *path,
                                       const tp_id128 *expected_fingerprint,
                                       tp_id128 *out_fingerprint, tp_error *err);

/* Serializes `p` to a freshly malloc'd buffer (*out, NUL-terminated; *out_len
 * excludes the NUL), byte-identical to what tp_project_save writes for a project
 * whose sources are already relative. PURE: no relativize, no project_dir change
 * -- so it is the exact snapshot primitive undo/redo needs. Caller frees *out. */
tp_status tp_project_save_buffer(const tp_project *p, char **out, size_t *out_len, tp_error *err);

/* Parses `len` bytes of project JSON at `buf` into a new project (*out). Mirror
 * of tp_project_load minus the file read; project_dir stays NULL (no path). */
tp_status tp_project_load_buffer(const char *buf, size_t len, tp_project **out, tp_error *err);

/* --- path helpers --- */

/* Joins a project-relative `rel` onto p->project_dir into `out_abs` (capacity
 * `cap`, '/'-normalized). Absolute `rel` is copied through (normalized). Returns
 * TP_STATUS_OUT_OF_BOUNDS if the result would not fit. */
tp_status tp_project_resolve_path(const tp_project *p, const char *rel, char *out_abs, size_t cap);

/* --- per-sprite effective rules --- */

/* Effective shape for one sprite in ATLAS-shape semantics (0=RECT, 1=CONVEX_HULL,
 * 2=CONCAVE_CONTOUR): a non-zero slice9 border forces RECT, else the sprite's
 * shape override (`ov_shape`, or TP_PROJECT_OV_INHERIT to inherit), else the atlas
 * shape. The single home for the rule the desc builder (tp_input) and the settings
 * view share (arch review §3.1); tp_pack keeps its own equivalent check as a
 * validation safety net. RECT is the only shape that may extrude, so callers gate
 * an extrude override on a RECT result. */
int tp_project_sprite_effective_shape(int atlas_shape, bool has_slice9, int ov_shape);

/* --- bridge to packing --- */

/* Maps atlas[atlas_index]'s knobs + name onto `out` (a tp_pack_settings). NOTE:
 * it fills the KNOBS and atlas_name only; work_dir and the sprites[] array are
 * the call site's responsibility (folder scanning is a separate Phase 2/8
 * concern). Returns TP_STATUS_OUT_OF_BOUNDS if atlas_index is out of range. */
tp_status tp_project_atlas_to_settings(const tp_project *p, int atlas_index, struct tp_pack_settings *out,
                                       tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PROJECT_H */
