#ifndef TP_CORE_TP_EXPORT_H
#define TP_CORE_TP_EXPORT_H

/*
 * Pure export layer over the canonical tp_result (ROADMAP Phase 2, SUMMARY.md
 * §5b/§5d/§5h). Everything here lives in tp_core (GUI-linkable, NO nt_builder):
 *   - capability flags (what a target FORMAT can hold),
 *   - the exporter registry (data + one write fn over the canonical model),
 *   - the capability -> pack-settings clamp (per-target packing, §5h),
 *   - the normalization pass ("prepareData": final names, scale, animations),
 *   - metadata-loss notices (never a hard error for a capability gap).
 *
 * Per-target ORCHESTRATION (pack per target with effective settings) needs the
 * builder and lives in tp_build (tp_export_run.h), not here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_arena;
struct tp_project;

/* Stable exporter id for the full-fidelity reference target. Frontends seed and
 * reference targets through this constant, never a bare string literal (review
 * §4 boundary gate: no exporter-id literals in apps/). */
#define TP_EXPORTER_ID_JSON_NEOTOLIS "json-neotolis"

/* ------------------------------------------------------------------ */
/* Capability flags: what a target FORMAT can hold (SUMMARY.md §5b).    */
/* ------------------------------------------------------------------ */

/* caps describe the OUTPUT FORMAT's expressiveness, independent of the packer.
 * The clamp (tp_export_effective_settings) maps them onto what nt_builder can
 * actually restrict to; the writer uses them to decide what to emit and where
 * to raise a metadata-loss notice. */
typedef struct tp_export_caps {
    bool rotate90;  /* format can encode a 90-degree rotation */
    bool flips;     /* format can encode flips / the full D4 orientation set */
    bool polygons;  /* format stores polygon verts + indices (else quad only) */
    bool pivot;     /* format stores a per-sprite pivot */
    bool slice9;    /* format stores 9-slice borders */
    bool multipage; /* format supports multiple pages */
    bool aliases;   /* format links aliased names to their shared frame */
    /* room to grow: append new flags, never reorder. */
} tp_export_caps;

/* All-true caps (the json-neotolis reference target holds everything). */
tp_export_caps tp_export_caps_full(void);

/* ------------------------------------------------------------------ */
/* Metadata-loss notices (SUMMARY.md §5h): informational, never fatal.  */
/* ------------------------------------------------------------------ */

/* Structured notice classification (ai-first.md item 4, review §3.4): a notice
 * carries WHICH axis degraded and WHY, so consumers (CLI --json, GUI chip)
 * render from data, not by re-parsing prose. Append-only: never reorder or
 * renumber an existing value. */
typedef enum tp_notice_field {
    TP_NOTICE_FIELD_NONE = 0,
    TP_NOTICE_FIELD_TRANSFORM, /* rotate/flip dropped (format can't hold the full D4) */
    TP_NOTICE_FIELD_POLYGON,   /* polygon hull flattened to a rect */
    TP_NOTICE_FIELD_SLICE9,    /* 9-slice borders dropped */
    TP_NOTICE_FIELD_PIVOT,     /* per-sprite pivot dropped */
    TP_NOTICE_FIELD_ALIAS,     /* alias link dropped */
    TP_NOTICE_FIELD_MULTIPAGE, /* multi-page atlas against a single-page target */
} tp_notice_field;

typedef enum tp_notice_reason {
    TP_NOTICE_REASON_NONE = 0,
    TP_NOTICE_REASON_CAPS_UNSUPPORTED, /* the target FORMAT cannot represent this */
} tp_notice_reason;

typedef struct tp_export_notice {
    const char *sprite; /* affected sprite (borrowed); NULL for an atlas-wide notice */
    const char *target; /* exporter id (borrowed); NULL when the producer does not know it */
    int field_id;       /* tp_notice_field */
    int reason_id;      /* tp_notice_reason */
    char msg[256];      /* human prose (derived from the structured fields) */
} tp_export_notice;

/* malloc-owned growable list; aggregated across every target of a run. */
typedef struct tp_export_notices {
    tp_export_notice *items;
    int count;
    int cap;
} tp_export_notices;

void tp_export_notices_init(tp_export_notices *n);
/* Appends a prose-only notice (structured fields zeroed). TP_STATUS_OOM if it cannot grow. */
tp_status tp_export_notice_addf(tp_export_notices *n, const char *fmt, ...) TP_PRINTF_ATTR(2, 3);
/* Appends a structured notice: the degraded axis + reason + affected sprite/target
 * (both nullable, borrowed) alongside the prose. TP_STATUS_OOM if it cannot grow. */
tp_status tp_export_notice_add_ex(tp_export_notices *n, int field_id, int reason_id, const char *sprite,
                                  const char *target, const char *fmt, ...) TP_PRINTF_ATTR(6, 7);
void tp_export_notices_free(tp_export_notices *n);

/* ------------------------------------------------------------------ */
/* Normalization pass ("prepareData", ROADMAP Phase 2 deliverable).     */
/* ------------------------------------------------------------------ */

/* Per-sprite export-name override (owner requirement: GUI rename). The file on
 * disk is unchanged; only the exported name changes. An override IS the final
 * name VERBATIM -- no ext-strip / folder munging is applied on top of it. */
typedef struct tp_export_name_override {
    const char *raw_name;   /* matches a tp_sprite.name produced by the packer */
    const char *final_name; /* verbatim final export name */
} tp_export_name_override;

/* One explicit animation from the project (SUMMARY.md §5a). Frames are FINAL
 * export names in explicit playback order. Animations are assembled EXPLICITLY
 * (docs/design/ux.md 3.7b) -- there is no numeric-suffix auto-grouping. */
typedef struct tp_export_anim_in {
    const char *id;
    const char *const *frames;
    int frame_count;
    float fps;
    int playback;
    bool flip_h;
    bool flip_v;
} tp_export_anim_in;

/* Options for tp_normalize. NULL is equivalent to tp_normalize_opts_defaults. */
typedef struct tp_normalize_opts {
    bool strip_extension; /* default true: drop a trailing ".ext" from the name */
    bool strip_folders;   /* default false: keep only the basename after '/'    */
    float scale;          /* default 1.0: multiplier applied to emitted geometry */

    /* Per-sprite rename overrides (applied BEFORE any munging; verbatim). */
    const tp_export_name_override *overrides;
    int override_count;

    /* Explicit project animations (assembled verbatim; no auto-grouping). */
    const tp_export_anim_in *animations;
    int animation_count;
} tp_normalize_opts;

/* Seeds `out` with the documented defaults. */
void tp_normalize_opts_defaults(tp_normalize_opts *out);

/* One prepared sprite: the final export name + a borrow of the canonical
 * sprite. alias_of indexes into the prepared (final-name-sorted) sprite list. */
typedef struct tp_export_sprite {
    const char *final_name;
    const tp_sprite *src;
    int alias_of; /* -1, else index into tp_export_prepared.sprites */
} tp_export_sprite;

/* One prepared animation: id + ordered FINAL frame names. */
typedef struct tp_export_anim {
    const char *id;
    const char **frames;
    int frame_count;
    float fps;
    int playback;
    bool flip_h;
    bool flip_v;
} tp_export_anim;

/* The normalized, export-ready view of a tp_result. Every pointer is arena
 * owned (the arena passed to tp_normalize). The source tp_result is borrowed. */
typedef struct tp_export_prepared {
    const tp_result *result; /* pages, atlas_name, pixels_per_unit */
    tp_export_sprite *sprites;
    int sprite_count; /* sorted ascending by final_name */
    tp_export_anim *animations;
    int animation_count; /* sorted ascending by id */
    float scale;         /* emitted-geometry scale (default 1.0) */
} tp_export_prepared;

/* Builds `out` from `result` + `opts`. Final names are computed (override ->
 * folder-strip -> ext-strip), sprites are sorted by final name (determinism
 * key), aliases keep their link, and animations are the explicit project
 * animations (sorted by id). A final-name collision after munging (e.g. a.png
 * + a.jpg -> "a", or an override colliding with another final name) is a
 * TP_STATUS_INVALID_ARGUMENT with a clear message. */
tp_status tp_normalize(const tp_result *result, const tp_normalize_opts *opts, struct tp_arena *arena,
                       tp_export_prepared *out, tp_error *err);

/* ------------------------------------------------------------------ */
/* Capability -> pack-settings clamp (per-target packing, SUMMARY §5h). */
/* ------------------------------------------------------------------ */

/* Restricts `in` to what `caps` can represent, writing `out` (may alias `in`).
 *
 * v1 reality (nt_builder has a single allow_transform bool = all-8-D4 vs
 * identity; there is NO rotate-only mode -- see the future NONE/ROT90/D4
 * transform-policy engine PR, SUMMARY.md §5g): transforms stay ON only when the
 * target can hold the FULL D4 the builder would bake, i.e. rotate90 AND flips.
 * A rotate90-only target (flips == false) therefore packs IDENTITY-ONLY in v1
 * (TODO: rotation-only once the builder gains a transform-policy knob). Polygon
 * shape is clamped to RECT when the format cannot store polygons, so the target
 * packs rectangles instead of tight hulls it would only flatten. Pivot/slice9
 * are metadata (not pack settings) -- the writer drops them with a notice. */
tp_status tp_export_effective_settings(const tp_pack_settings *in, const tp_export_caps *caps, tp_pack_settings *out);

/* True when two settings would produce the same pack (so the run is shared). */
bool tp_export_settings_equal(const tp_pack_settings *a, const tp_pack_settings *b);

/* ------------------------------------------------------------------ */
/* Degradation prediction (review §3.4; the flagship "what will this   */
/* format cost you" feedback the CLI dry-run and GUI chip both need).  */
/* ------------------------------------------------------------------ */

/* Enumerates every metadata/pack degradation exporting atlas[atlas_index] to a
 * target with `caps` would cause, appending a structured notice per axis to
 * `out` (init'd by the caller). This is the ONE enumeration both frontends read
 * (kills the GUI-side copy in review §3.1).
 *
 * PROJECT-KNOWABLE axes are computed from the project alone (no pack needed):
 *   - TRANSFORM: allow_transform on, but caps can't hold the full D4;
 *   - POLYGON:   a polygon atlas shape a non-polygon target flattens to rect;
 *   - SLICE9 / PIVOT: the atlas carries a 9-slice / pivot a target can't store.
 * `opt_prep` (nullable) adds PACK-DEPENDENT axes that only exist once packed --
 *   ALIAS and MULTIPAGE. The GUI chip passes NULL (project-only preview); the
 *   CLI dry-run passes the packed prep for the full picture. `target_id` (nullable)
 *   is recorded on each emitted notice. */
tp_status tp_export_predict_loss(const struct tp_project *project, int atlas_index, const tp_export_caps *caps,
                                 const char *target_id, const tp_export_prepared *opt_prep, tp_export_notices *out,
                                 tp_error *err);

/* ------------------------------------------------------------------ */
/* Page PNG writer (shared helper used by every exporter).              */
/* ------------------------------------------------------------------ */

/* Writes each page of `result` to "<out_path_base>-<page>.png". Pages are
 * straight-alpha by default; `premultiply` premultiplies RGB by alpha first
 * (ROADMAP toggle). The parent directory of out_path_base must already exist
 * (tp_core has no dir-creation opinion). Deterministic. */
tp_status tp_export_write_pages(const tp_result *result, const char *out_path_base, bool premultiply, tp_error *err);

/* ------------------------------------------------------------------ */
/* Exporter registry (data + one write fn over the canonical model).    */
/* ------------------------------------------------------------------ */

/* Writes `prep` to files rooted at `out_path_base` (no extension; the exporter
 * appends its own). `caps` is the target's capability set: the writer emits
 * only what caps allows and raises a metadata-loss notice for genuine drops.
 * Never a hard error for a capability gap. */
typedef tp_status (*tp_export_write_fn)(const tp_export_prepared *prep, const tp_export_caps *caps,
                                        const char *out_path_base, tp_export_notices *notices, tp_error *err);

typedef struct tp_exporter {
    const char *id;           /* stable id, e.g. "json-neotolis" */
    const char *display_name; /* human label for GUI dropdowns */
    const char *extension;    /* primary output extension, no dot, e.g. "json" */
    tp_export_caps caps;
    tp_export_write_fn write;
} tp_exporter;

/* Lookup by id across built-in + runtime-registered exporters. NULL on miss. */
const tp_exporter *tp_exporter_find(const char *id);

/* Enumeration for GUI/CLI dropdowns (built-ins first, then registered). */
int tp_exporter_count(void);
const tp_exporter *tp_exporter_at(int index);

/* Registers a runtime exporter (Phase 7 templates; tests inject capability-
 * restricted descriptors here). `e` must outlive the process use; the registry
 * borrows the pointer. Duplicate id or a full table -> error. */
tp_status tp_exporter_register(const tp_exporter *e);

/* The json-neotolis serializer, exposed so tools/tests can drive it through a
 * custom capability-restricted descriptor over the same writer. */
tp_status tp_export_json_neotolis_write(const tp_export_prepared *prep, const tp_export_caps *caps,
                                        const char *out_path_base, tp_export_notices *notices, tp_error *err);

/* json-neotolis schema version emitted in the "version" field. */
#define TP_JSON_NEOTOLIS_SCHEMA_VERSION 1

/* defold-tpinfo format version emitted in the .tpinfo "version" field (contract:
 * docs/formats/defold-tpinfo.md). Public so the CLI version manifest can report
 * it from one source instead of a duplicated literal. */
#define TP_DEFOLD_TPINFO_VERSION "2.0"

/* The Defold serializer (extension-texturepacker `.tpinfo` + `.tpatlas` + page
 * PNGs), exposed so tools/tests can drive it directly. Writes three artifacts at
 * out_path_base: "<base>.tpinfo", "<base>.tpatlas", "<base>-<N>.png". Contract:
 * docs/formats/defold-tpinfo.md. */
tp_status tp_export_defold_write(const tp_export_prepared *prep, const tp_export_caps *caps, const char *out_path_base,
                                 tp_export_notices *notices, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_EXPORT_H */
