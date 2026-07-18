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
 *   - version != current -> TP_STATUS_BAD_VERSION.
 *   - unknown keys        -> TP_STATUS_BAD_PROJECT (closed canonical schema).
 *   - malformed JSON / wrong types -> TP_STATUS_BAD_PROJECT (with context).
 *   - missing source files on disk are NOT load errors (they are model states).
 */

#include <stdbool.h>
#include <float.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h" /* tp_id128: persistent structural IDs */

#ifdef __cplusplus
extern "C" {
#endif

struct tp_pack_settings;

/* The production loader accepts exactly this schema version. Unsupported older
 * or newer versions are rejected; conversion is intentionally outside tp_core. */
#define TP_PROJECT_SCHEMA_VERSION 5

/* Per-sprite override. Sparse: an entry exists only when at least one field is
 * non-default. Defaults: origin (0.5,0.5), slice9 all-zero, rename NULL (see the
 * *_DEFAULT constants below).
 *
 * The canonical identity is the owning source ID plus normalized source-local
 * key, from which sprite_id derives (tp_sprite_id(source_id, src_key)). `name`
 * is a derived display/export bridge and is never authoritative for lookup.
 *
 * The source entity and source_ref must remain present in the project graph. A
 * record is an orphan only when that physical source is unavailable or the
 * source-local key is absent; it becomes active again when the same key returns. */
typedef struct tp_project_sprite {
    char *name;          /* display/export-key bridge; never an application key */
    tp_id128 source_ref; /* owning source's structural id; always non-nil */
    char *src_key;       /* normalized source-local key (NFC, ext KEPT) */
    float origin_x;
    float origin_y;
    uint16_t slice9_lrtb[4]; /* [left,right,top,bottom] px; all-zero = none */
    char *rename;            /* final export name override (NULL = file-derived); consumed by tp_normalize overrides */

    /* Optional per-sprite packing overrides (owner scope 2026-07-10). Sparse:
     * TP_PROJECT_OV_INHERIT (-1) = inherit the atlas value (never serialized).
     * shape uses atlas-shape semantics (0=RECT,1=CONVEX_HULL,2=CONCAVE_CONTOUR);
     * allow_rotate 0 = force no-rotate (the engine has no force-rotate). margin/
     * extrude/max_vertices carry the raw value; explicit 0 and values that do
     * not fit the builder descriptor are rejected before model adoption. */
    int16_t ov_shape;
    int16_t ov_allow_rotate;
    int16_t ov_max_vertices;
    int16_t ov_margin;
    int16_t ov_extrude;
} tp_project_sprite;

#define TP_PROJECT_ORIGIN_DEFAULT 0.5F
#define TP_PROJECT_OV_INHERIT (-1)

/* One animation frame reference: a sprite in the same atlas, in playback order.
 * It uses the same canonical source ID + source-local key identity as a sprite
 * override. `name` is derived for display; export resolves through the canonical
 * fields and then maps to the packed logical name. */
typedef struct tp_project_frame {
    char *name;          /* display reference for snapshots/human selectors */
    tp_id128 source_ref; /* owning source's structural id; always non-nil */
    char *src_key;       /* normalized source-local key (NFC, ext KEPT) */
} tp_project_frame;

/* Flipbook metadata over sprite references, orthogonal to placement (SUMMARY.md §5a).
 * `frames` are canonical {source, key} references in explicit playback order.
 * `id` is persistent and survives rename/reorder/save/reload; `name` is the
 * logical/display name and human selector. */
typedef struct tp_project_anim {
    tp_id128 id;   /* persistent structural ID; fresh private entities start nil */
    char *name;    /* logical/display name */
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
#define TP_PROJECT_ANIM_PLAYBACK_MIN 0
#define TP_PROJECT_ANIM_PLAYBACK_MAX 6

static inline bool tp_project_anim_fps_valid(float value) {
    return value > 0.0F && value <= FLT_MAX;
}
static inline bool tp_project_anim_playback_valid(int value) {
    return value >= TP_PROJECT_ANIM_PLAYBACK_MIN &&
           value <= TP_PROJECT_ANIM_PLAYBACK_MAX;
}

/* One export target: a pluggable exporter id + its output path. `enabled`
 * defaults true (sparse: only written when false). */
typedef struct tp_project_target {
    tp_id128 id;       /* persistent structural ID; fresh private entities start nil */
    char *exporter_id; /* exporter kind, e.g. "json-neotolis", "defold" (NOT the structural id) */
    char *out_path;    /* project-relative output path/prefix */
    bool enabled;
} tp_project_target;

/* Source kind. Master spec §11 models a source as
 * `kind: path | atlas`, where a "path" source is either a scanned folder or a
 * single image file; this enum makes that file-vs-folder sub-distinction explicit.
 * APPEND-ONLY (the value is the stored classification): TP_SOURCE_KIND_ATLAS
 * (foreign atlas descriptor) is reserved for Epic B1. `folder` is the zero/default
 * value. add_source without an explicit kind creates a folder source. Serialized
 * as a string token ("folder" is the omitted sparse default; "file" is written). */
typedef enum tp_source_kind {
    TP_SOURCE_KIND_FOLDER = 0, /* recursively scanned folder (default) */
    TP_SOURCE_KIND_FILE = 1    /* single image file */
    /* TP_SOURCE_KIND_ATLAS = 2  -- reserved for Epic B1; do NOT use before then */
} tp_source_kind;

/* One tagged source: a persistent structural id + its kind + the
 * folder/file path (project-relative, '/'-normalized; stored verbatim, save
 * relativizes absolute forms). A fresh private entity starts with a nil ID until
 * session adoption assigns it. Scan
 * classifies file-vs-folder at runtime by stat, so a stored kind that disagrees
 * with disk still packs correctly; kind is authoritative only where disk cannot
 * be consulted (a missing source, for sprite-id derivation). */
typedef struct tp_project_source {
    tp_id128 id;         /* persistent structural ID */
    tp_source_kind kind; /* folder (default) / file */
    char *path;          /* folder/file path, project-relative, '/'-normalized */
} tp_project_source;

/* One atlas: packing knobs (mirror tp_pack_settings) + live-linked sources +
 * sparse per-sprite overrides + animations + export targets. All arrays are
 * malloc-owned dynamic vectors; use the helpers below to mutate them. */
typedef struct tp_project_atlas {
    tp_id128 id;       /* persistent structural ID; fresh private entities start nil */
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

    tp_project_source *sources; /* tagged source records */
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

/* The whole project. `project_dir` is the absolute directory of the current
 * project file (set on load/save; NULL while unsaved). `source_base_dir` owns
 * the resolution base of stable live source spellings; Save As can update the
 * former without retargeting sources or rewriting history-owned records. */
typedef struct tp_project {
    char *project_dir; /* absolute; NULL if never saved */
    char *source_base_dir; /* absolute; NULL until relative sources acquire a base */
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
 * duplicated: project/source base dirs, atlas name/knobs/ids, sources, sparse sprite overrides,
 * animations + frames, targets). OOM-SAFE: on any allocation failure returns NULL
 * and frees the partial clone (no leak). The clone is byte-identical under
 * tp_project_save_buffer (test-pinned). This is the atomicity primitive the
 * transaction engine clones the model with before applying a batch (§7). NULL src
 * -> NULL. */
tp_project *tp_project_clone(const tp_project *src);

/* --- read-only project queries --- */

/* Returns the index of the atlas whose structural id equals `id`, or -1 if none /
 * nil id. Persistent references (and the F2 operation engine) target the id, not
 * the array index, which reorder/remove invalidates (master spec §5.4). */
int tp_project_find_atlas_by_id(const tp_project *p, tp_id128 id);

/* True when the atlas already holds a source whose '/'-normalized path equals `path`'s
 * -- the exact predicate add_source_kind uses to dedupe. Lets a caller (the op-engine
 * validator) reject a would-be-deduped add BEFORE it strands a new source's id. */
bool tp_project_atlas_has_source_path(const tp_project_atlas *a, const char *path);

/* True iff SOME target in the project OTHER than `exclude` has this exact (non-empty)
 * out_path. Scans EVERY atlas's targets (an out_path collision is project-wide: two
 * atlases exporting to one file silently overwrite each other). Only ENABLED targets
 * count -- the exporter skips disabled targets, so a disabled target never overwrites.
 * out_paths are compared SLASH-NORMALIZED (the same tp_normalize_slashes the exporter
 * applies) so "out\x" and "out/x" resolve to one file and DO collide. An empty/NULL
 * `out_path` never "collides" (that is the separate empty-out_path check) -> false.
 * `self` is the caller's own target (excluded by POINTER identity, so this is robust
 * even when target ids are nil in an unpublished private candidate); a NULL `self`
 * excludes nothing. The shared source of truth for the duplicate check both frontends
 * surface (validate + the GUI target panel). NULL p -> false. */
bool tp_project_out_path_shared(const tp_project *p, const char *out_path, const tp_project_target *self);
/* Chooses the lowest free atlasN/default out path using the same normalized,
 * enabled-target collision rules as validation/export. */
tp_status tp_project_next_atlas_defaults(const tp_project *p, char *name,
                                         size_t name_cap, char *out_path,
                                         size_t out_path_cap,
                                         const char **exporter_id,
                                         bool *target_enabled, tp_error *err);
/* Chooses the first available animation name: `base`, `base2`, ...; an empty
 * base uses `anim1`, `anim2`, ... . Never truncates a candidate. */
tp_status tp_project_next_animation_name(const tp_project *p, tp_id128 atlas_id,
                                         const char *base, char *name,
                                         size_t name_cap, tp_error *err);
/* Canonical defaults for one newly-created target. */
tp_status tp_project_target_defaults(const tp_project *p, tp_id128 atlas_id,
                                     const char **exporter_id, char *out_path,
                                     size_t out_path_cap, bool *enabled,
                                     tp_error *err);

/* --- load / save --- */

/* Parses `path` into a new project (*out). project_dir is set to path's absolute
 * directory. Errors: TP_STATUS_BAD_VERSION (non-v5 schema), TP_STATUS_BAD_PROJECT
 * (open/parse/type error). On any error *out is set to NULL and `err` is filled. */
tp_status tp_project_load(const char *path, tp_project **out, tp_error *err);

/* Like tp_project_load, additionally returning the fingerprint of the exact byte
 * buffer successfully parsed. `out_fingerprint` is optional and is cleared on
 * failure. This avoids reopening the path after load (which could hash different
 * bytes after an external replacement). */
tp_status tp_project_load_with_fingerprint(const char *path, tp_project **out, tp_id128 *out_fingerprint,
                                           tp_error *err);

/* Writes `p` deterministically to `path` (see the serialization contract above),
 * updating p->project_dir to path's absolute directory. Source path normalization
 * is staged for output; live source spellings and source_base_dir remain stable.
 * Publication is sibling-temp + file sync + atomic replace/create + parent sync.
 * TP_STATUS_FILE_IO_FAILED is a typed pre-publication failure: destination,
 * fingerprint, and staged project paths remain unchanged; err->file_io identifies
 * the exact phase, attempted path, and captured native cause. The core does not
 * retry atomic publication.
 * TP_STATUS_FILE_DURABILITY_UNCERTAIN is a post-publication outcome: the new file
 * and staged project directory are authoritative, so callers must surface a
 * warning and must not retry as though no write occurred. */
tp_status tp_project_save(tp_project *p, const char *path, tp_error *err);

/* Like tp_project_save, additionally returning the fingerprint of the exact
 * serialized buffer atomically promoted to `path`. `out_fingerprint` is optional
 * and is cleared on pre-publication failure; it remains populated for
 * TP_STATUS_FILE_DURABILITY_UNCERTAIN because those bytes were published. */
tp_status tp_project_save_with_fingerprint(tp_project *p, const char *path, tp_id128 *out_fingerprint,
                                           tp_error *err);

/* Create-only atomic save. If `path` exists at publish time, returns
 * TP_STATUS_FILE_EXISTS without changing it or the staged project paths. */
tp_status tp_project_save_new_with_fingerprint(
    tp_project *p, const char *path, tp_id128 *out_fingerprint, tp_error *err);

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

/* Runs the canonical serializer in allocation-free counting mode. On success,
 * *out_len is the exact byte count tp_project_save_buffer would produce
 * (excluding NUL). `limit` is inclusive; an oversized project returns
 * TP_STATUS_OUT_OF_BOUNDS before allocating or materializing JSON. */
tp_status tp_project_serialized_size_bounded(const tp_project *p, size_t limit,
                                             size_t *out_len, tp_error *err);

/* Parses `len` bytes of project JSON at `buf` into a new project (*out). Mirror
 * of tp_project_load minus the file read; project_dir stays NULL (no path). */
tp_status tp_project_load_buffer(const char *buf, size_t len, tp_project **out, tp_error *err);

/* --- path helpers --- */

/* Joins a project-relative `rel` onto p->project_dir into `out_abs` (capacity
 * `cap`, '/'-normalized). Absolute `rel` is copied through (normalized). Returns
 * TP_STATUS_OUT_OF_BOUNDS if the result would not fit. */
tp_status tp_project_resolve_path(const tp_project *p, const char *rel, char *out_abs, size_t cap);

/* Resolves a live source spelling against source_base_dir (falling back to
 * project_dir for older/in-memory projects). Export targets continue to use
 * tp_project_resolve_path and therefore follow Save As. A relative source with
 * neither base returns TP_STATUS_PATH_NOT_ABSOLUTE; callers may use that exact
 * status to opt into request-edge CWD resolution. */
tp_status tp_project_resolve_source_path(const tp_project *p, const char *rel,
                                         char *out_abs, size_t cap);

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
