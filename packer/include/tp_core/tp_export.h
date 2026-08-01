#ifndef TP_CORE_TP_EXPORT_H
#define TP_CORE_TP_EXPORT_H

/*
 * Pure export layer over the canonical tp_result
 * (docs/architecture/engine-and-client-boundaries.md). Everything here lives
 * in tp_core (GUI-linkable, NO nt_builder):
 *   - capability flags (what a target FORMAT can hold),
 *   - built-in descriptors + memory-only native serializers,
 *   - the capability -> pack-settings clamp (per-target packing, §5h),
 *   - versioned Export IR materialization, artifact planning, and publication,
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

#define TP_EXPORT_TRANSFORM_BIT(value) TP_PACK_TRANSFORM_BIT(value)
#define TP_EXPORT_TRANSFORMS_IDENTITY TP_PACK_TRANSFORMS_IDENTITY
#define TP_EXPORT_TRANSFORMS_ALL TP_PACK_TRANSFORMS_ALL

/* caps describe the OUTPUT FORMAT's expressiveness, independent of the packer.
 * The clamp (tp_export_effective_settings) maps them onto what nt_builder can
 * actually restrict to; the writer uses them to decide what to emit and where
 * to raise a metadata-loss notice. */
typedef struct tp_export_caps {
    uint8_t transform_mask; /* exact set of representable tp_sprite.transform values */
    bool polygons;  /* format stores polygon verts + indices (else quad only) */
    bool pivot;     /* format stores a per-sprite pivot */
    bool slice9;    /* format stores 9-slice borders */
    bool multipage; /* format supports multiple pages */
    bool aliases;   /* format links aliased names to their shared frame */
    /* room to grow: append new flags, never reorder. */
} tp_export_caps;

/* All-true caps (the json-neotolis reference target holds everything). */
tp_export_caps tp_export_caps_full(void);
/* Coarse presentation values derived from transform_mask; never policy inputs. */
bool tp_export_caps_supports_rotate90(const tp_export_caps *caps);
bool tp_export_caps_supports_flips(const tp_export_caps *caps);

/* ------------------------------------------------------------------ */
/* Metadata-loss semantics are informational. Failure to allocate or append a
 * required notice is still an export error: diagnostics are part of the
 * machine-readable contract and are never silently dropped. */
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

#ifdef TP_ENABLE_TEST_SEAMS
/* Makes exactly the next notice-list growth fail with TP_STATUS_OOM. */
void tp_export_notices__test_fail_next_reserve(void);
#endif

/* ------------------------------------------------------------------ */
/* Immutable Export IR v1. */
/* ------------------------------------------------------------------ */

#define TP_EXPORT_IR_VERSION 1U

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

/* Options for Export IR materialization. NULL uses tp_export_ir_opts_defaults. */
typedef struct tp_export_ir_opts {
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
} tp_export_ir_opts;

/* Seeds `out` with the documented defaults. */
void tp_export_ir_opts_defaults(tp_export_ir_opts *out);

/* A serializer-visible page descriptor. Concrete output names live in the
 * artifact plan, not here. The raw RGBA buffer remains core-private. */
typedef struct tp_export_page {
    int artifact_id; /* logical page artifact id, currently equal to page index */
    int w, h;
    bool premultiplied;
} tp_export_page;

/* One fully materialized IR sprite. `data` and all of its pointer fields are
 * arena-owned copies; no serializer observes the source tp_result lifetime. */
typedef struct tp_export_sprite {
    const char *final_name;
    tp_sprite data;
    bool is_solid; /* derived once from the packed page footprint */
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

/* Versioned normalized export model. Every visible pointer is arena-owned;
 * serializers never observe tp_result or its raw page pixels. */
typedef struct tp_export_ir {
    uint32_t version;
    const char *target_id;
    const char *atlas_name;
    float pixels_per_unit;
    tp_export_page *pages;
    int page_count;
    tp_export_sprite *sprites;
    int sprite_count; /* sorted ascending by final_name */
    tp_export_anim *animations;
    int animation_count; /* sorted ascending by id */
    float scale;         /* emitted-geometry scale (default 1.0) */
} tp_export_ir;

/* Builds `out` from `result` + `opts`. Final names are computed (override ->
 * folder-strip -> ext-strip), sprites are sorted by final name (determinism
 * key), aliases keep their link, and animations are the explicit project
 * animations (sorted by id). A final-name collision after munging (e.g. a.png
 * + a.jpg -> "a", or an override colliding with another final name) is a
 * TP_STATUS_INVALID_ARGUMENT with a clear message. */
tp_status tp_export_ir_build(const tp_result *result, const tp_export_ir_opts *opts,
                             const char *target_id, struct tp_arena *arena,
                             tp_export_ir *out, tp_error *err);
tp_status tp_export_ir_validate(const tp_export_ir *ir, tp_error *err);

/* ------------------------------------------------------------------ */
/* Capability -> pack-settings clamp (per-target packing). */
/* ------------------------------------------------------------------ */

/* Restricts `in` to what `caps` can represent, writing `out` (may alias `in`).
 *
 * The saved project keeps its simple allow_transform boolean, while the pack
 * layer carries an exact D4 value mask. Export intersects that mask with the
 * format mask before grouping and packing. Polygon shape is clamped to RECT
 * when the format cannot store polygons, so the target
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
 *   - TRANSFORM: the requested and effective D4 masks differ;
 *   - POLYGON:   a polygon atlas shape a non-polygon target flattens to rect;
 *   - SLICE9 / PIVOT: the atlas carries a 9-slice / pivot a target can't store.
 * `opt_ir` (nullable) adds PACK-DEPENDENT axes that only exist once packed --
 *   ALIAS and MULTIPAGE. The GUI chip passes NULL (project-only preview); the
 *   CLI dry-run passes the Export IR for the full picture. `target_id` (nullable)
 *   is recorded on each emitted notice. */
tp_status tp_export_predict_loss(const struct tp_project *project, int atlas_index, const tp_export_caps *caps,
                                 const char *target_id, const tp_export_ir *opt_ir, tp_export_notices *out,
                                 tp_error *err);
tp_status tp_export_predict_loss_snapshot(const struct tp_session_snapshot *snapshot,
                                          tp_id128 atlas_id,
                                          const tp_export_caps *caps,
                                          const char *target_id,
                                          const tp_export_ir *opt_ir,
                                          tp_export_notices *out,
                                          tp_error *err);

/* Checked constructors for exporter-owned suffixes. `out` is always bounded by
 * the canonical path contract, including the NUL terminator. */
tp_status tp_export_output_path(const char *out_path_base, const char *suffix,
                                char out[TP_IDENTITY_PATH_MAX], tp_error *err);
tp_status tp_export_page_path(const char *out_path_base, int page,
                              char out[TP_IDENTITY_PATH_MAX], tp_error *err);

/* ------------------------------------------------------------------ */
/* Native format descriptor, artifact plan, and serializer documents. */
/* ------------------------------------------------------------------ */

typedef struct tp_format_artifact_decl {
    const char *id;     /* stable logical id, e.g. "metadata" */
    const char *suffix; /* appended to the target output base, including dot */
} tp_format_artifact_decl;

typedef struct tp_format_descriptor {
    const char *id;
    const char *display_name;
    tp_export_caps caps;
    const tp_format_artifact_decl *artifacts;
    int artifact_count;
} tp_format_descriptor;

typedef enum tp_export_artifact_kind {
    TP_EXPORT_ARTIFACT_DOCUMENT = 1,
    TP_EXPORT_ARTIFACT_PAGE = 2,
} tp_export_artifact_kind;

typedef struct tp_export_artifact {
    tp_export_artifact_kind kind;
    int logical_id; /* descriptor artifact index or Export IR page artifact id */
    const char *id;
    const char *path;
} tp_export_artifact;

typedef struct tp_export_artifact_plan {
    const char *out_path_base;
    tp_export_artifact *artifacts;
    int artifact_count;
    int document_count;
} tp_export_artifact_plan;

/* Serializer output is memory-only. The serializer transfers each malloc-owned
 * buffer to the caller; the common publisher frees it on every path. */
typedef struct tp_export_document {
    void *data;
    size_t size;
} tp_export_document;

typedef struct tp_export_serialize_ctx {
    const tp_export_ir *ir;
    const tp_format_descriptor *format;
    const tp_export_artifact_plan *plan;
    tp_export_notices *notices;
} tp_export_serialize_ctx;

typedef tp_status (*tp_export_serialize_fn)(const tp_export_serialize_ctx *ctx,
                                            tp_export_document *documents,
                                            int document_count,
                                            tp_error *err);

typedef struct tp_exporter {
    const tp_format_descriptor *format;
    tp_export_serialize_fn serialize;
} tp_exporter;

tp_status tp_export_artifact_plan_build(const tp_exporter *exp,
                                        const tp_export_ir *ir,
                                        const char *out_path_base,
                                        struct tp_arena *arena,
                                        tp_export_artifact_plan *out,
                                        tp_error *err);

/* Common-core PNG sink for one already-planned page artifact. The caller owns
 * the exact path; this function never derives a filename. */
tp_status tp_export_write_page_artifact(const tp_page *page, int page_id,
                                        const char *path, bool premultiply,
                                        tp_error *err);

/* Publishes a target's whole output SET or none of it.
 *
 * `plan` is the complete output contract, checked before serialization:
 *
 *   1. Every entry must be a direct child of out_path_base's own directory.
 *      A path outside that directory (including any deeper subdirectory) or one
 *      whose staged form would exceed the path limit is a structured error --
 *      the serializer never runs and nothing on disk is touched. Arbitrarily placed
 *      outputs are out of scope, and publishing part of a set is not an
 *      acceptable substitute for saying so.
 *   2. No two entries may name the same file, comparing ASCII case-INSENSITIVELY.
 *      Windows and macOS resolve "Atlas.png" and "atlas.png" to one file, so a
 *      list that collides on case cannot describe a set on those hosts and is
 *      rejected everywhere, keeping the contract host-independent. Non-ASCII
 *      case collisions (Turkish dotted I, full-width forms) are out of scope.
 *      The error names both colliding entries.
 *
 * Then a private staging directory is created as a SIBLING of the output
 * directory (same volume, so every publish step is a pure rename). The serializer
 * returns one memory document per declared format artifact; the core writes them
 * and every planned page PNG, then verifies the complete staged set. The scan is
 * part of the guarantee, so a staging
 * directory that cannot be opened or cannot be enumerated to its end fails
 * CLOSED -- an unverified set is exactly what this rejects.
 *
 * Publication itself is a two-phase swap with rollback. Phase one renames each
 * existing destination aside to a ".tp-old-" sibling; phase two renames each
 * staged file onto its destination. Any failure returned by either phase rolls
 * the whole thing back -- every displaced file returns to its destination and
 * every already-promoted file is removed -- so a completed call sees either all
 * current artifact-plan entries or the previous contents of those same
 * destinations, never a mix. Files absent from the current plan are never
 * scanned, inferred, or removed. On success the displaced copies are deleted.
 * If rollback itself cannot complete, the error reports publication uncertainty
 * instead of claiming a clean failure.
 *
 * Abrupt process termination is outside that rollback guarantee. It can leave
 * private staging/backup names and a partially swapped planned set. Later
 * exports do not scan, infer ownership of, restore, or delete those leftovers;
 * doing so safely would require an explicit transaction record.
 *
 * `out_serializer_ran` (nullable) reports whether `exp->serialize` was invoked,
 * so a caller's report can distinguish a rejected output list from a failed
 * writer. `out_publication_uncertain` (nullable) is true only if rollback could
 * not prove that the previous destination set was restored. The staging dir is
 * removed on every path. */
tp_status tp_export_publish(const tp_exporter *exp,
                            const tp_export_ir *ir,
                            const tp_result *packed,
                            const tp_export_artifact_plan *plan,
                            tp_export_notices *notices,
                            bool *out_serializer_ran,
                            bool *out_publication_uncertain,
                            tp_error *err);

#ifdef TP_ENABLE_TEST_SEAMS
/* Fails the `nth` (0-based) rename the two-phase swap performs, counting the
 * displace and promote phases together in execution order, so the rollback path
 * is constructible without a real filesystem fault. Negative disarms. */
void tp_export_publish__test_fail_rename_at(int nth);
#endif

/* Lookup by id in the fixed built-in native registry. NULL on miss. */
const tp_exporter *tp_exporter_find(const char *id);

/* Enumeration for GUI/CLI dropdowns. */
int tp_exporter_count(void);
const tp_exporter *tp_exporter_at(int index);

#ifdef TP_ENABLE_TEST_SEAMS
tp_status tp_exporter_register(const tp_exporter *e);
#endif

tp_status tp_export_json_neotolis_serialize(const tp_export_serialize_ctx *ctx,
                                            tp_export_document *documents,
                                            int document_count,
                                            tp_error *err);

/* json-neotolis schema version emitted in the "version" field. */
#define TP_JSON_NEOTOLIS_SCHEMA_VERSION 1

/* defold-tpinfo format version emitted in the .tpinfo "version" field (contract:
 * docs/formats/defold-tpinfo.md). Public so the CLI version manifest can report
 * it from one source instead of a duplicated literal. */
#define TP_DEFOLD_TPINFO_VERSION "2.0"

/* Defold memory serializer for extension-texturepacker `.tpinfo` + `.tpatlas`.
 * Page PNGs and all filenames remain common-core responsibilities. */
tp_status tp_export_defold_serialize(const tp_export_serialize_ctx *ctx,
                                     tp_export_document *documents,
                                     int document_count,
                                     tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_EXPORT_H */
