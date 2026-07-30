#ifndef TP_CORE_TP_EXPORT_H
#define TP_CORE_TP_EXPORT_H

/*
 * Pure export layer over the canonical tp_result
 * (docs/architecture/engine-and-client-boundaries.md). Everything here lives
 * in tp_core (GUI-linkable, NO nt_builder):
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
#include "tp_core/tp_id.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_pack_result.h"
#include "tp_core/tp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_arena;
struct tp_project;
struct tp_session_snapshot;

/* Stable exporter id for the full-fidelity reference target. Frontends seed and
 * reference targets through this constant, never a bare string literal (review
 * §4 boundary gate: no exporter-id literals in apps/). */
#define TP_EXPORTER_ID_JSON_NEOTOLIS "json-neotolis"

/* Canonical format/exporter identifier capacity, including NUL. Package ids
 * are machine tokens (typically reverse-DNS), not unbounded display names. */
#define TP_EXPORTER_ID_MAX 256

/* Validates non-empty, strict UTF-8, and the shared byte bound. */
tp_status tp_exporter_id_validate(const char *id, tp_error *err);

/* ------------------------------------------------------------------ */
/* Capability flags: what a target FORMAT can hold. */
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
/* Metadata-loss notices: informational, never fatal. */
/* ------------------------------------------------------------------ */

/* Structured notice classification (docs/spec/product.md): a notice
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
/* Normalization pass ("prepareData"). */
/* ------------------------------------------------------------------ */

/* Per-sprite export-name override (owner requirement: GUI rename). The file on
 * disk is unchanged; only the exported name changes. An override IS the final
 * name VERBATIM -- no ext-strip / folder munging is applied on top of it. */
typedef struct tp_export_name_override {
    const char *raw_name;   /* matches a tp_sprite.name produced by the packer */
    const char *final_name; /* verbatim final export name */
} tp_export_name_override;

/* One explicit animation from the project. Frames are FINAL
 * export names in explicit playback order. Animations are assembled EXPLICITLY
 * (docs/formats/json-neotolis.md) -- there is no numeric-suffix auto-grouping. */
typedef struct tp_export_frame_ref {
    tp_id128 source_id;
    const char *source_key;
} tp_export_frame_ref;

typedef struct tp_export_anim_in {
    const char *id;
    const tp_export_frame_ref *frames;
    int frame_count;
    float fps;
    int playback;
    bool flip_h;
    bool flip_v;
} tp_export_anim_in;

typedef struct tp_export_sprite_ref_in {
    const char *raw_name;
    tp_id128 source_id;
    const char *source_key;
} tp_export_sprite_ref_in;

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
    const tp_export_sprite_ref_in *sprite_refs;
    int sprite_ref_count;
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
/* Capability -> pack-settings clamp (per-target packing). */
/* ------------------------------------------------------------------ */

/* Restricts `in` to what `caps` can represent, writing `out` (may alias `in`).
 *
 * v1 reality (nt_builder has a single allow_transform bool = all-8-D4 vs
 * identity; there is NO rotate-only mode -- see the future NONE/ROT90/D4
 * transform-policy engine PR): transforms stay ON only when the
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
/* Degradation prediction (the flagship "what will this               */
/* format cost you" feedback the CLI dry-run and GUI chip both need).  */
/* ------------------------------------------------------------------ */

/* Enumerates every metadata/pack degradation exporting atlas[atlas_index] to a
 * target with `caps` would cause, appending a structured notice per axis to
 * `out` (init'd by the caller). This is the ONE enumeration both frontends read
 * (there is no GUI-side duplicate).
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
tp_status tp_export_predict_loss_snapshot(const struct tp_session_snapshot *snapshot,
                                          tp_id128 atlas_id,
                                          const tp_export_caps *caps,
                                          const char *target_id,
                                          const tp_export_prepared *opt_prep,
                                          tp_export_notices *out,
                                          tp_error *err);

/* ------------------------------------------------------------------ */
/* Page PNG writer (shared helper used by every exporter).              */
/* ------------------------------------------------------------------ */

/* Writes each page of `result` to "<write_path_base>-<page>.png". Pages are
 * straight-alpha by default; `premultiply` premultiplies RGB by alpha first
 * when requested. The parent directory of write_path_base must already exist
 * (tp_core has no dir-creation opinion). Deterministic.
 *
 * Plain writes: under the set publication below this base is the private staging
 * directory, whose whole point is that nothing there is observable until the set
 * is promoted, so a per-file temp+replace would buy nothing. */
tp_status tp_export_write_pages(const tp_result *result, const char *write_path_base, bool premultiply, tp_error *err);

/* Sink for enumerating an exporter's output files (for the structured export
 * report / introspection). Each call receives one output path; `ud` is the
 * caller's context. */
typedef void (*tp_export_path_sink)(void *ud, const char *path);

/* Checked constructors for exporter-owned suffixes. `out` is always bounded by
 * the canonical path contract, including the NUL terminator. */
tp_status tp_export_output_path(const char *out_path_base, const char *suffix,
                                char out[TP_IDENTITY_PATH_MAX], tp_error *err);
tp_status tp_export_page_path(const char *out_path_base, int page,
                              char out[TP_IDENTITY_PATH_MAX], tp_error *err);

/* Appends every page-PNG path ("<out_path_base>-<N>.png", N in [0,page_count)) to
 * `sink`. Single source of the page-file naming tp_export_write_pages uses, so an
 * exporter's list_outputs and the actual writer never drift. All paths are
 * validated before the first sink call; overflow is a structured error, never a
 * silently omitted report entry. */
tp_status tp_export_list_page_files(const tp_result *result, const char *out_path_base,
                                    tp_export_path_sink sink, void *ud, tp_error *err);

/* ------------------------------------------------------------------ */
/* Exporter registry (data + one write fn over the canonical model).    */
/* ------------------------------------------------------------------ */

/* Everything a writer is given. The two bases are deliberately separate:
 *
 *   write_path_base -- WHERE the files go. Every file a writer creates must be
 *     "<write_path_base><suffix>" and nothing else. Under the set publication
 *     below this is a private staging directory, so the writes are plain (the
 *     directory is unobservable until the whole set is promoted).
 *   out_path_base   -- WHERE the files will end up. Read-only: a writer consults
 *     it when its CONTENT must reference the published location (the Defold
 *     exporter resolves its project-relative .tpatlas ref by walking up from the
 *     final directory). Never write here.
 *
 * The two share a basename, so "<base>.<ext>" naming is identical either way. A
 * direct caller that wants the files where they land -- a golden test, a tool --
 * passes the same base for both.
 *
 * `caps` is the target's capability set: the writer emits only what caps allows
 * and raises a metadata-loss notice for genuine drops. Never a hard error for a
 * capability gap. `notices` is nullable. */
typedef struct tp_export_write_ctx {
    const tp_export_prepared *prep;
    const tp_export_caps *caps;
    const char *write_path_base;
    const char *out_path_base;
    tp_export_notices *notices;
} tp_export_write_ctx;

typedef tp_status (*tp_export_write_fn)(const tp_export_write_ctx *ctx, tp_error *err);

/* Optional: enumerate the files write() produces for `prep` rooted at
 * out_path_base, via `sink`. Lets the run layer report every written file honestly
 * without re-encoding each writer's naming. NULL => the run layer assumes the
 * common shape "<base>.<extension>" + the page PNGs. Defold sets it because it also
 * writes a .tpatlas sibling the single primary `extension` cannot express.
 * Implementations must reject unrepresentable paths through status/error before
 * invoking the sink. */
typedef tp_status (*tp_export_list_outputs_fn)(const tp_export_prepared *prep, const char *out_path_base,
                                               tp_export_path_sink sink, void *ud, tp_error *err);

typedef struct tp_exporter {
    const char *id;           /* stable id, e.g. "json-neotolis" */
    const char *display_name; /* human label for GUI dropdowns */
    const char *extension;    /* primary output extension, no dot, e.g. "json" */
    tp_export_caps caps;
    tp_export_write_fn write;
    tp_export_list_outputs_fn list_outputs; /* nullable; NULL => "<base>.<ext>" + page PNGs */
} tp_exporter;

/* Publishes a target's whole output SET or none of it.
 *
 * `output_files` is the target's complete enumerated output list (the run
 * layer's collect step) and it is the CONTRACT, checked before anything runs:
 *
 *   1. Every entry must be a direct child of out_path_base's own directory.
 *      A path outside that directory (including any deeper subdirectory) or one
 *      whose staged form would exceed the path limit is a structured error --
 *      the writer never runs and nothing on disk is touched. Arbitrarily placed
 *      outputs are out of scope, and publishing part of a set is not an
 *      acceptable substitute for saying so.
 *   2. No two entries may name the same file, comparing ASCII case-INSENSITIVELY.
 *      Windows and macOS resolve "Atlas.png" and "atlas.png" to one file, so a
 *      list that collides on case cannot describe a set on those hosts and is
 *      rejected everywhere, keeping the contract host-independent. Non-ASCII
 *      case collisions (Turkish dotted I, full-width forms) are out of scope.
 *      The error names both colliding entries.
 *
 * Then: a private staging directory is created as a SIBLING of the output
 * directory (same volume, so every publish step is a pure rename), the writer is
 * told to write there, and the produced set is verified before anything is
 * published -- every listed output present, no destination an existing
 * directory, and no staged file the list missed. The leftover match is
 * BYTE-EXACT, including case: an enumerated output name must equal the name the
 * writer produced byte for byte. The scan is part of the guarantee, so a staging
 * directory that cannot be opened or cannot be enumerated to its end fails
 * CLOSED -- an unverified set is exactly what this rejects.
 *
 * Publication itself is a two-phase swap with rollback. Phase one renames each
 * existing destination aside to a ".tp-old-" sibling; phase two renames each
 * staged file onto its destination. ANY failure in either phase rolls the whole
 * thing back -- every displaced file returns to its destination and every
 * already-promoted file is removed -- so the caller sees the complete new set or
 * the complete old set, never a mix. On success the displaced copies are
 * deleted. If the rollback itself cannot complete, the error says so explicitly
 * rather than reporting a clean failure.
 *
 * A process killed mid-swap leaves those private names behind; the sweep at the
 * start of each publish reclaims them by owner liveness (a ".tp-old-" file whose
 * destination exists means the swap completed and is deleted; one whose
 * destination is missing means it was interrupted and is restored).
 *
 * `out_writer_ran` (nullable) reports whether `exp->write` was invoked at all,
 * so a caller's report can distinguish a rejected output list from a failed
 * writer. The staging dir is removed on every path. */
tp_status tp_export_write_and_publish_set(const tp_exporter *exp,
                                          const tp_export_prepared *prep,
                                          const char *out_path_base,
                                          const char *const *output_files,
                                          int output_file_count,
                                          tp_export_notices *notices,
                                          bool *out_writer_ran,
                                          tp_error *err);

#ifdef TP_ENABLE_TEST_SEAMS
/* Fails the `nth` (0-based) rename the two-phase swap performs, counting the
 * displace and promote phases together in execution order, so the rollback path
 * is constructible without a real filesystem fault. Negative disarms. */
void tp_export_publish__test_fail_rename_at(int nth);
#endif

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
tp_status tp_export_json_neotolis_write(const tp_export_write_ctx *ctx, tp_error *err);

/* json-neotolis schema version emitted in the "version" field. */
#define TP_JSON_NEOTOLIS_SCHEMA_VERSION 1

/* defold-tpinfo format version emitted in the .tpinfo "version" field (contract:
 * docs/formats/defold-tpinfo.md). Public so the CLI version manifest can report
 * it from one source instead of a duplicated literal. */
#define TP_DEFOLD_TPINFO_VERSION "2.0"

/* The Defold serializer (extension-texturepacker `.tpinfo` + `.tpatlas` + page
 * PNGs), exposed so tools/tests can drive it directly. Writes three artifacts at
 * ctx->write_path_base: "<base>.tpinfo", "<base>.tpatlas", "<base>-<N>.png"; the
 * .tpatlas `file:` resource ref is resolved from ctx->out_path_base, the location
 * the atlas will actually be loaded from. Contract: docs/formats/defold-tpinfo.md. */
tp_status tp_export_defold_write(const tp_export_write_ctx *ctx, tp_error *err);

/* Enumerates the Defold writer's outputs ("<base>.tpinfo", "<base>.tpatlas", and
 * the page PNGs). Wired into the Defold exporter descriptor's list_outputs so the
 * run layer's report lists the .tpatlas sibling the primary extension omits. */
tp_status tp_export_defold_list_outputs(const tp_export_prepared *prep, const char *out_path_base,
                                        tp_export_path_sink sink, void *ud, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_EXPORT_H */
