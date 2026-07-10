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

#ifdef __cplusplus
extern "C" {
#endif

struct tp_pack_settings;

/* Bump when the on-disk schema changes; add a migration case in the loader. */
#define TP_PROJECT_SCHEMA_VERSION 1

/* Per-sprite override. Sparse: an entry exists only when at least one field is
 * non-default. Defaults: origin (0.5,0.5), slice9 all-zero (see the *_DEFAULT
 * constants below). */
typedef struct tp_project_sprite {
    char *name; /* atlas-relative sprite name; the override key */
    float origin_x;
    float origin_y;
    uint16_t slice9_lrtb[4]; /* [left,right,top,bottom] px; all-zero = none */
} tp_project_sprite;

#define TP_PROJECT_ORIGIN_DEFAULT 0.5F

/* Flipbook metadata over sprite names, orthogonal to placement (SUMMARY.md §5a).
 * `frames` are atlas-relative sprite names in explicit playback order. */
typedef struct tp_project_anim {
    char *id;
    char **frames;
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
    char *exporter_id; /* stable id, e.g. "json-neotolis", "defold" */
    char *out_path;    /* project-relative output path/prefix */
    bool enabled;
} tp_project_target;

/* One atlas: packing knobs (mirror tp_pack_settings) + live-linked sources +
 * sparse per-sprite overrides + animations + export targets. All arrays are
 * malloc-owned dynamic vectors; use the helpers below to mutate them. */
typedef struct tp_project_atlas {
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

    char **sources; /* folder/file paths, project-relative, '/'-normalized */
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

/* --- atlas mutation --- */

/* Appends an atlas with default knobs. On success writes its index to
 * *out_index (if non-NULL). Returns TP_STATUS_INVALID_ARGUMENT / TP_STATUS_OOM. */
tp_status tp_project_add_atlas(tp_project *p, const char *name, int *out_index);

/* Removes atlas `index` (frees its owned data). Out-of-range -> OUT_OF_BOUNDS. */
tp_status tp_project_remove_atlas(tp_project *p, int index);

/* Bounds-checked accessor; NULL if index is out of range. */
tp_project_atlas *tp_project_get_atlas(tp_project *p, int index);

/* Resets `a`'s packing knobs to the shared defaults (tp_pack_settings_defaults).
 * The name and lists are left untouched. */
void tp_project_atlas_set_defaults(tp_project_atlas *a);

/* --- source mutation --- */

/* Appends a source path (stored verbatim; save normalizes/relativizes it). */
tp_status tp_project_atlas_add_source(tp_project_atlas *a, const char *path);

/* Removes source `index`. Out-of-range -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_source(tp_project_atlas *a, int index);

/* --- sprite-override mutation --- */

/* Returns the override for `name`, or NULL if none exists. */
tp_project_sprite *tp_project_atlas_find_sprite(tp_project_atlas *a, const char *name);

/* Returns the existing override for `name`, or appends a new default one. The
 * entry is written to *out (if non-NULL). */
tp_status tp_project_atlas_add_sprite(tp_project_atlas *a, const char *name, tp_project_sprite **out);

/* Removes the override for `name`. Absent -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_sprite(tp_project_atlas *a, const char *name);

/* --- animation mutation --- */

/* Appends an animation (default fps/playback/flips, no frames). Written to *out
 * (if non-NULL). Use tp_project_anim_add_frame to populate frames in order. */
tp_status tp_project_atlas_add_animation(tp_project_atlas *a, const char *id, tp_project_anim **out);

/* Removes the animation with `id`. Absent -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_animation(tp_project_atlas *a, const char *id);

/* Appends a frame (sprite name) to an animation, preserving order. */
tp_status tp_project_anim_add_frame(tp_project_anim *anim, const char *frame_name);

/* --- target mutation --- */

/* Appends an export target (enabled by default). Written to *out (if non-NULL). */
tp_status tp_project_atlas_add_target(tp_project_atlas *a, const char *exporter_id, const char *out_path,
                                      tp_project_target **out);

/* Removes target `index`. Out-of-range -> OUT_OF_BOUNDS. */
tp_status tp_project_atlas_remove_target(tp_project_atlas *a, int index);

/* --- load / save --- */

/* Parses `path` into a new project (*out). project_dir is set to path's absolute
 * directory. Errors: TP_STATUS_BAD_VERSION (newer schema), TP_STATUS_BAD_PROJECT
 * (open/parse/type error). On any error *out is set to NULL and `err` is filled. */
tp_status tp_project_load(const char *path, tp_project **out, tp_error *err);

/* Writes `p` deterministically to `path` (see the serialization contract above),
 * updating p->project_dir to path's absolute directory and relativizing absolute
 * source paths against it. `p` is non-const because Save/Save-As updates the
 * in-memory project_dir + path forms (mutation-friendly, GUI live edit). */
tp_status tp_project_save(tp_project *p, const char *path, tp_error *err);

/* --- path helpers --- */

/* Joins a project-relative `rel` onto p->project_dir into `out_abs` (capacity
 * `cap`, '/'-normalized). Absolute `rel` is copied through (normalized). Returns
 * TP_STATUS_OUT_OF_BOUNDS if the result would not fit. */
tp_status tp_project_resolve_path(const tp_project *p, const char *rel, char *out_abs, size_t cap);

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
