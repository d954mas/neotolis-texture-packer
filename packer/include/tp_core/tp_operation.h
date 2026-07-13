#ifndef TP_CORE_TP_OPERATION_H
#define TP_CORE_TP_OPERATION_H

/*
 * Typed semantic operation engine (F2-01, master spec §4.1-4.2, §6-6.2, §7).
 * Promoted from the accepted, 3-OS-green C0-02 spike (packer/spike/c0/tp_c0_op*)
 * into the production tp_core model + tp_status error contract.
 *
 * THE ONE mutation vocabulary all clients share (AGENTS.md tool-parity invariant):
 * every persistent model change is a typed operation from the APPEND-ONLY catalog
 * below, addressing entities BY STABLE ID (never array index or mutable name),
 * with a CLOSED per-op field vocabulary -- there is deliberately NO raw JSON/field
 * patch escape hatch (§6.2). Each op is exactly one effect class
 * CREATE / REMOVE / MOVE / SET.
 *
 * F2-01 / F2-05 BOUNDARY (honest scope -- see docs/decisions):
 *   F2-01 builds and CORE-TESTS this engine (per-op forward/error fixtures + a
 *   parity test proving an op applies == the existing tp_project mutator). It does
 *   NOT wire the shipping CLI/GUI mutators through it -- that FRONTEND CUTOVER is
 *   F2-05. The current apps/cli/cli_mutate.c and apps/gui/gui_project.c mutators are
 *   untouched. Nothing here claims a frontend is routed through the engine.
 *   Transactions / revision / dirty are F2-02: this applies ONE validated op
 *   (validate -> stage -> commit); apply is shaped so F2-02 can wrap it in a batch.
 *
 * Errors are the production tp_status model (never an abort on caller input): a
 * bad payload / dangling reference / out-of-range value yields a structured status
 * + offending field, never a crash. Payloads are bounds-checked, size_t math.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"
#include "tp_core/tp_project.h"  /* tp_project + tp_source_kind + override sentinels */
#include "tp_core/tp_selector.h" /* tp_selector_kind/result/candidates (builder seam) */

#ifdef __cplusplus
extern "C" {
#endif

struct tp_sprite_index;

/* ---- append-only operation catalog (task 1) ------------------------------ *
 * New kinds are added before TP_OP_KIND_COUNT, NEVER reordered or removed (a
 * journaled/persisted op record must not shift). Mirrors the C0-02 catalog. */
typedef enum tp_op_kind {
    TP_OP_INVALID = 0,

    TP_OP_ATLAS_CREATE,
    TP_OP_ATLAS_REMOVE,
    TP_OP_ATLAS_RENAME,
    TP_OP_ATLAS_SETTINGS_SET,

    TP_OP_SOURCE_ADD,
    TP_OP_SOURCE_REMOVE,
    TP_OP_SOURCE_REPLACE, /* reserved: linked-source repath (Epic B); no current CLI verb */

    TP_OP_SPRITE_OVERRIDE_SET,
    TP_OP_SPRITE_OVERRIDE_CLEAR,
    TP_OP_SPRITE_NAME_SET,

    TP_OP_ANIMATION_CREATE,
    TP_OP_ANIMATION_REMOVE,
    TP_OP_ANIMATION_SETTINGS_SET,
    TP_OP_ANIMATION_FRAMES_SET, /* reserved: bulk whole-list set (MCP); CLI uses granular frame ops */
    TP_OP_ANIMATION_FRAME_ADD,
    TP_OP_ANIMATION_FRAME_REMOVE,
    TP_OP_ANIMATION_FRAME_MOVE,

    TP_OP_TARGET_CREATE,
    TP_OP_TARGET_REMOVE,
    TP_OP_TARGET_SET,

    TP_OP_KIND_COUNT
} tp_op_kind;

/* The four before/after diff classes (task 1, spec §9). Every op is exactly one. */
typedef enum tp_op_class {
    TP_OP_CLASS_CREATE = 0,
    TP_OP_CLASS_REMOVE,
    TP_OP_CLASS_MOVE,
    TP_OP_CLASS_SET
} tp_op_class;

/* One catalog row. `target_kind` is the ID kind of the op's PRIMARY target;
 * atlas-scoped ops also carry a parent atlas id in the payload. `cli_verb` is the
 * current ntpacker verb that lowers to this op, or NULL for a reserved op. */
typedef struct tp_op_info {
    tp_op_kind kind;
    const char *wire;          /* canonical "op" string, e.g. "atlas.create" */
    tp_op_class effect;        /* before/after diff class */
    tp_id_kind target_kind;    /* primary target ID kind (INVALID = project-level) */
    const char *cli_verb;      /* current CLI verb, or NULL if reserved */
} tp_op_info;

const tp_op_info *tp_op_info_by_kind(tp_op_kind kind);
const tp_op_info *tp_op_info_by_wire(const char *wire);
tp_op_kind tp_op_kind_from_wire(const char *wire); /* TP_OP_INVALID for unknown */
const char *tp_op_wire(tp_op_kind kind);           /* "" for INVALID/out-of-range */
const char *tp_op_class_name(tp_op_class cls);     /* "create"/"remove"/"move"/"set" */
bool tp_op_catalog_selfcheck(void);                /* row order == enum; pinned by a test */

/* The CLOSED canonical field vocabulary of an op (addressing `*_id` keys + typed
 * payload keys; excludes the "op" discriminator). Makes "no raw field patch"
 * (§6.2) executable: a key outside this set is unknown. Writes *count (>=0). */
const char *const *tp_op_fields(tp_op_kind kind, int *count);
bool tp_op_field_allowed(tp_op_kind kind, const char *key);

/* ---- field presence masks (SET ops apply a SUBSET of fields) ------------- */

/* atlas.settings.set: which of the 10 packing knobs the op sets. */
enum tp_atlas_field_mask {
    TP_AF_MAX_SIZE = 1u << 0,
    TP_AF_PADDING = 1u << 1,
    TP_AF_MARGIN = 1u << 2,
    TP_AF_EXTRUDE = 1u << 3,
    TP_AF_ALPHA_THRESHOLD = 1u << 4,
    TP_AF_MAX_VERTICES = 1u << 5,
    TP_AF_SHAPE = 1u << 6,
    TP_AF_ALLOW_TRANSFORM = 1u << 7,
    TP_AF_POWER_OF_TWO = 1u << 8,
    TP_AF_PIXELS_PER_UNIT = 1u << 9
};

/* sprite.override.set / .clear: which override fields the op sets / clears.
 * ORIGIN groups (origin_x,origin_y); SLICE9 groups (l,r,t,b) -- the CLI sets each
 * as a unit -- but the canonical encoder emits the underlying scalar keys. */
enum tp_sprite_field_mask {
    TP_SPF_ORIGIN = 1u << 0,
    TP_SPF_SLICE9 = 1u << 1,
    TP_SPF_SHAPE = 1u << 2,
    TP_SPF_ALLOW_ROTATE = 1u << 3,
    TP_SPF_MAX_VERTICES = 1u << 4,
    TP_SPF_MARGIN = 1u << 5,
    TP_SPF_EXTRUDE = 1u << 6,
    TP_SPF_ALL = 0x7Fu /* every override field (sprite unset) */
};

/* animation.settings.set: which animation knobs the op sets. */
enum tp_anim_field_mask {
    TP_ANF_FPS = 1u << 0,
    TP_ANF_PLAYBACK = 1u << 1,
    TP_ANF_FLIP_H = 1u << 2,
    TP_ANF_FLIP_V = 1u << 3
};

/* ---- typed payload arms (tagged union) ----------------------------------- *
 * Canonical operations store IDs only (§5.4). A human selector resolves to one ID
 * at the request edge (tp_op_build_*), before validate/apply ever see the op.
 * Strings are malloc-owned; free the whole op with tp_operation_free. */

typedef struct tp_op_atlas_create { char *name; } tp_op_atlas_create;      /* atlas_id = the NEW atlas id */
typedef struct tp_op_atlas_rename { char *name; } tp_op_atlas_rename;      /* atlas_id addresses the atlas */

/* atlas.settings.set: the knob values + a presence mask (tp_atlas_field_mask). */
typedef struct tp_op_atlas_settings {
    uint32_t mask;
    int max_size, padding, margin, extrude, alpha_threshold, max_vertices, shape;
    bool allow_transform, power_of_two;
    float pixels_per_unit;
} tp_op_atlas_settings;

typedef struct tp_op_source_add {  /* atlas_id = parent atlas */
    tp_id128 source_id;            /* the NEW source id */
    tp_source_kind kind;
    char *key;                     /* source path (project-relative, '/'-normalized) */
} tp_op_source_add;

typedef struct tp_op_source_ref {  /* source.remove / .replace address by source_id */
    tp_id128 source_id;
    char *key;                     /* source.replace new path; NULL for source.remove */
} tp_op_source_ref;

/* Sprite ops address by CANONICAL identity {source_id, src_key} (§5.2/§5.4); the
 * derived sprite_id = tp_sprite_id(source_id, src_key) is emitted for reference.
 * atlas_id = parent atlas. See docs/decisions for how apply maps this to the
 * existing export-key-bridge sprite-override storage (byte-identical to the CLI). */
typedef struct tp_op_sprite_set {
    tp_id128 source_id;
    char *src_key;                 /* normalized source-local key (NFC, ext KEPT) */
    uint32_t mask;                 /* tp_sprite_field_mask: which fields to set */
    float origin_x, origin_y;
    uint16_t slice9[4];            /* [l,r,t,b] */
    int16_t ov_shape, ov_allow_rotate, ov_max_vertices, ov_margin, ov_extrude;
} tp_op_sprite_set;

typedef struct tp_op_sprite_clear {
    tp_id128 source_id;
    char *src_key;
    uint32_t mask;                 /* tp_sprite_field_mask: which fields to clear (ALL = sprite unset) */
} tp_op_sprite_clear;

typedef struct tp_op_sprite_name { /* the logical/export rename is a distinct reference target (§5.2) */
    tp_id128 source_id;
    char *src_key;
    char *name;                    /* new export name; NULL clears back to file-derived */
} tp_op_sprite_name;

typedef struct tp_op_anim_create { /* atlas_id = parent atlas */
    tp_id128 anim_id;              /* the NEW animation id */
    char *name;                    /* logical/display name */
    float fps;
    int playback;
    bool flip_h, flip_v;
    char **frames;                 /* initial frame references (export-name bridge), in order */
    int frame_count;
} tp_op_anim_create;

typedef struct tp_op_anim_ref { tp_id128 anim_id; } tp_op_anim_ref; /* animation.remove */

typedef struct tp_op_anim_settings {
    tp_id128 anim_id;
    uint32_t mask;                 /* tp_anim_field_mask */
    float fps;
    int playback;
    bool flip_h, flip_v;
} tp_op_anim_settings;

typedef struct tp_op_anim_frames_set { /* reserved bulk set: replace the whole list */
    tp_id128 anim_id;
    char **frames;
    int frame_count;
} tp_op_anim_frames_set;

typedef struct tp_op_anim_frame_add {
    tp_id128 anim_id;
    char *frame;                   /* frame reference (export-name bridge) */
    int index;                     /* insert position; < 0 = append */
} tp_op_anim_frame_add;

typedef struct tp_op_anim_frame_rm {
    tp_id128 anim_id;
    int index;                     /* 0-based frame slot */
} tp_op_anim_frame_rm;

typedef struct tp_op_anim_frame_move {
    tp_id128 anim_id;
    int from_index, to_index;
} tp_op_anim_frame_move;

typedef struct tp_op_target_create { /* atlas_id = parent atlas */
    tp_id128 target_id;            /* the NEW target id */
    char *exporter_id;
    char *out_path;
    bool enabled;
} tp_op_target_create;

typedef struct tp_op_target_ref { tp_id128 target_id; } tp_op_target_ref; /* target.remove */

typedef struct tp_op_target_set {
    tp_id128 target_id;
    char *exporter_id;
    char *out_path;
    bool enabled;
} tp_op_target_set;

/* The tagged operation. `kind` is the tag; `atlas_id` is the parent atlas for
 * atlas-scoped ops (and the primary target for atlas.* ops). Zero-initialize, set
 * kind, fill the matching arm. */
typedef struct tp_operation {
    tp_op_kind kind;
    tp_id128 atlas_id;
    union {
        tp_op_atlas_create atlas_create;
        tp_op_atlas_rename atlas_rename;
        tp_op_atlas_settings atlas_settings;
        tp_op_source_add source_add;
        tp_op_source_ref source_ref;
        tp_op_sprite_set sprite_set;
        tp_op_sprite_clear sprite_clear;
        tp_op_sprite_name sprite_name;
        tp_op_anim_create anim_create;
        tp_op_anim_ref anim_ref;
        tp_op_anim_settings anim_settings;
        tp_op_anim_frames_set anim_frames_set;
        tp_op_anim_frame_add anim_frame_add;
        tp_op_anim_frame_rm anim_frame_rm;
        tp_op_anim_frame_move anim_frame_move;
        tp_op_target_create target_create;
        tp_op_target_ref target_ref;
        tp_op_target_set target_set;
    } u;
} tp_operation;

/* Frees the malloc-owned strings/arrays the op's active arm holds, then zeroes it.
 * NULL-safe. A zero-initialized op (no strings set) frees nothing. */
void tp_operation_free(tp_operation *op);

/* ---- structured rejection (task 5) --------------------------------------- *
 * Every rejection carries a machine status id (tp_status_id) + the offending
 * closed-vocabulary field + human context -- not only prose. */
typedef struct tp_op_reject {
    tp_status status;   /* TP_STATUS_OK == no rejection */
    char field[64];     /* offending field key, or "" */
    char message[192];  /* human context */
} tp_op_reject;

/* ---- validation (task 4) ------------------------------------------------- *
 * Validate the op's payload (closed vocabulary, numeric RANGES, non-empty names,
 * exporter-id against the live registry) and its ID REFERENCES against `p` (parent
 * atlas exists; addressed entity exists). Pure: never mutates `p`. Returns
 * TP_STATUS_OK, else fills *rej (may be NULL) with status + field + message. This
 * is the range/name/exporter/reference logic moved OUT of the CLI/GUI into core. */
tp_status tp_operation_validate(const tp_project *p, const tp_operation *op, tp_op_reject *rej);

/* ---- id-only apply (task 2) ---------------------------------------------- *
 * Validate then apply ONE operation to `p`, addressing entities by ID. STAGE-THEN-
 * COMMIT: any allocator failure before the commit point leaves the model
 * BYTE-UNCHANGED (a failed apply never half-mutates). On rejection fills *rej (may
 * be NULL) and returns the structured status; the model is untouched. */
tp_status tp_operation_apply(tp_project *p, const tp_operation *op, tp_op_reject *rej);

/* ---- canonical byte-stable encoders (determinism) ------------------------ *
 * 2-space indent, LF, %.9g floats, keys ASCENDING (the "op" discriminator first),
 * trailing newline -- identical conventions to the tp_project writer. Return a
 * freshly malloc'd NUL-terminated buffer (caller frees), or NULL on OOM / a NULL
 * or INVALID-kind op. */
char *tp_operation_encode(const tp_operation *op);

/* Encode the apply RESULT object: committed -> {"op":...,"status":"committed"};
 * rejected (rej->status != OK) -> {"code":...,"field":...(sparse),"message":...,
 * "op":...,"status":"rejected"}. */
char *tp_op_result_encode(const tp_operation *op, const tp_op_reject *rej);

/* ---- selector -> operation builders (task 3) ----------------------------- *
 * The CLI/MCP convenience layer: a human selector + already-typed args -> a typed,
 * ID-only operation, or a structured ambiguity/not-found. The builders resolve the
 * target via the production tp_selector (F1-03) so every frontend selects the same
 * entity without guessing. `cand` (may be NULL) receives the stable candidate list
 * ONLY on TP_STATUS_AMBIGUOUS_SELECTOR. On success *out owns its strings (free with
 * tp_operation_free). See tp_op_build.c for the full per-verb set. */

/* Resolve a human `selector` to exactly one entity id of the wanted kind, for op
 * building. Wraps tp_selector_resolve + a kind check. */
tp_status tp_op_resolve_target(const tp_project *p, const struct tp_sprite_index *sprites, int sprite_atlas_index,
                               tp_selector_kind want, const char *selector, tp_selector_result *out,
                               tp_selector_candidates *cand, tp_error *err);

/* atlas.create: no selector (creates a new entity). `new_id` is the atlas's id. */
tp_status tp_op_build_atlas_create(tp_id128 new_id, const char *name, tp_operation *out);
/* atlas.rename: resolve `atlas_sel` -> atlas_id, set new name. */
tp_status tp_op_build_atlas_rename(const tp_project *p, const char *atlas_sel, const char *new_name, tp_operation *out,
                                   tp_selector_candidates *cand, tp_error *err);
/* atlas.remove: resolve `atlas_sel` -> atlas_id. */
tp_status tp_op_build_atlas_remove(const tp_project *p, const char *atlas_sel, tp_operation *out,
                                   tp_selector_candidates *cand, tp_error *err);
/* target.set: resolve `target_sel` within atlas `atlas_sel` -> target_id. */
tp_status tp_op_build_target_set(const tp_project *p, const char *atlas_sel, const char *target_sel,
                                 const char *exporter_id, const char *out_path, bool enabled, tp_operation *out,
                                 tp_selector_candidates *cand, tp_error *err);
/* animation.remove: resolve `anim_sel` within atlas `atlas_sel` -> anim_id. */
tp_status tp_op_build_anim_remove(const tp_project *p, const char *atlas_sel, const char *anim_sel, tp_operation *out,
                                  tp_selector_candidates *cand, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_OPERATION_H */
