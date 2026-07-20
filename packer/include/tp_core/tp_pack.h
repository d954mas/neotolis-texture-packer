#ifndef TP_CORE_TP_PACK_H
#define TP_CORE_TP_PACK_H

/* Runs one atlas through nt_builder from a minimal settings input and returns an
 * owned tp_result (plan §4 task 9, ROADMAP 1b).
 *
 * Flow: drive nt_builder (start_pack -> begin_atlas -> add sprites -> end_atlas
 * -> finish_pack) using the §5 export-friendly profile, writing a session
 * .ntpack to "<work_dir>/<atlas_name>.ntpack", then parse that pack back with
 * tp_pack_read to recover the canonical model. Phase 1b packs ONE atlas per
 * call; multi-atlas orchestration is a later layer.
 *
 * The "settings input" here is the Phase-1b minimal struct, NOT the Phase-3
 * project file -- Phase 3 maps the project file onto this struct.
 *
 * Determinism: the builder is left single-threaded, so a fixed settings input
 * yields a byte-identical session .ntpack and a field-identical tp_result. */

#include <stdbool.h>
#include <float.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_arena;
struct tp_result;

/* Canonical packing-knob contract shared by operation admission, saved-project
 * validation, and the defensive Pack boundary. Keep one owner so clients cannot
 * drift from the engine/build cap. */
#ifndef NT_BUILD_MAX_TEXTURE_SIZE
#define TP_PACK_MAX_PAGE_DIM 4096
#else
#define TP_PACK_MAX_PAGE_DIM NT_BUILD_MAX_TEXTURE_SIZE
#endif
#define TP_PACK_ALPHA_MAX 255
#define TP_PACK_MAX_VERTICES 16
#define TP_PACK_SHAPE_MIN 0
#define TP_PACK_SHAPE_MAX 2
/* Current nt_builder vector-packer hard limit.  Keep this public because it is
 * a product capability boundary, not an implementation accident: admission
 * must reject a provably larger job before the assertion-based backend sees it. */
#define TP_PACK_MAX_PAGES 64

static inline bool tp_pack_max_size_valid(int value) {
    return value >= 1 && value <= TP_PACK_MAX_PAGE_DIM;
}
static inline bool tp_pack_nonnegative_valid(int value) { return value >= 0; }
static inline bool tp_pack_alpha_threshold_valid(int value) {
    return value >= 0 && value <= TP_PACK_ALPHA_MAX;
}
static inline bool tp_pack_max_vertices_valid(int value) {
    return value >= 1 && value <= TP_PACK_MAX_VERTICES;
}
static inline bool tp_pack_shape_valid(int value) {
    return value >= TP_PACK_SHAPE_MIN && value <= TP_PACK_SHAPE_MAX;
}
static inline bool tp_pack_pixels_per_unit_valid(float value) {
    return value > 0.0F && value <= FLT_MAX;
}
static inline bool tp_pack_extrude_shape_valid(int extrude, int shape) {
    return extrude == 0 || shape == TP_PACK_SHAPE_MIN;
}

/* tp_pack_sprite_desc.ov_mask bits: which atlas packing knobs a sprite overrides.
 * A zero ov_mask = inherit everything (zero-init safe -- existing desc builders and
 * other agents' desc arrays keep working with no per-sprite overrides). */
#define TP_PACK_OV_SHAPE ((uint8_t)(1u << 0))
#define TP_PACK_OV_ROTATE ((uint8_t)(1u << 1))
#define TP_PACK_OV_MAXVERT ((uint8_t)(1u << 2))
#define TP_PACK_OV_MARGIN ((uint8_t)(1u << 3))
#define TP_PACK_OV_EXTRUDE ((uint8_t)(1u << 4))
#define TP_PACK_OV_ALL                                                         \
    ((uint8_t)(TP_PACK_OV_SHAPE | TP_PACK_OV_ROTATE | TP_PACK_OV_MAXVERT |    \
               TP_PACK_OV_MARGIN | TP_PACK_OV_EXTRUDE))

/* Desc per-sprite override VALUES mirror the engine nt_atlas_sprite_opts_t encoding
 * (tp_pack.c static-asserts they match). shape/allow_rotate carry explicit non-zero
 * constants; margin/extrude/max_vertices carry the raw value (an explicit 0 is
 * rejected by validate_settings -- the engine's 0-means-inherit makes it
 * unrepresentable downstream). */
#define TP_PACK_SPRITE_SHAPE_RECT 1
#define TP_PACK_SPRITE_SHAPE_CONVEX 2
#define TP_PACK_SPRITE_SHAPE_CONCAVE 3
#define TP_PACK_SPRITE_ROTATE_NO 1

/* One sprite. Either `path` (strict-UTF-8 file input, decoded through the
 * shared bounded tp_image ingress and submitted to the builder as raw RGBA8)
 * OR raw pixels (`rgba` + `w` + `h`) when `path == NULL`. */
typedef struct tp_pack_sprite_desc {
    const char *name; /* required, unique within the atlas */
    const char *path; /* file input; if NULL, the raw fields below are used */

    /* Optional canonical project identity. Project-built inputs populate these
     * and use `name` as an internal collision-free packing key; exporters map
     * back to logical_name. Direct raw pack callers may leave them zero/NULL. */
    tp_id128 source_id;
    const char *source_key;
    const char *logical_name;

    const uint8_t *rgba; /* raw RGBA8, w*h*4 bytes, y-down; used when path == NULL */
    int w;
    int h;

    /* Pivot over source size (pre-trim), y-down. 0.5,0.5 = centre; may exceed
     * [0,1]. Not defaulted by tp_pack_settings_defaults (per-sprite) -- set it. */
    float origin_x;
    float origin_y;

    uint16_t slice9_lrtb[4]; /* [left,right,top,bottom] px; all-zero = none */

    /* Per-sprite packing overrides (owner scope 2026-07-10). Present only when the
     * matching ov_mask bit is set; value uses the engine encoding (see the TP_PACK_*
     * macros above). run_builder maps these onto nt_atlas_sprite_opts_t. */
    uint8_t ov_mask;
    uint8_t ov_shape;        /* TP_PACK_SPRITE_SHAPE_* when TP_PACK_OV_SHAPE */
    uint8_t ov_allow_rotate; /* TP_PACK_SPRITE_ROTATE_NO when TP_PACK_OV_ROTATE */
    uint8_t ov_max_vertices; /* [1..16] when TP_PACK_OV_MAXVERT */
    uint8_t ov_margin;       /* [1..255] when TP_PACK_OV_MARGIN */
    uint8_t ov_extrude;      /* [1..255] when TP_PACK_OV_EXTRUDE, effective RECT only */
} tp_pack_sprite_desc;

/* Minimal pack settings. `atlas_name` must be normalization-invariant (no `\\`,
 * `./`, `..`, `//`, no leading/trailing `/`) -- otherwise the atlas blob's raw
 * "<atlas>/texN" hash diverges from the normalized entry-table hash and every
 * page lookup misses (plan §5/R2). `work_dir` is required: tp_core has no
 * temp-dir opinions, so the caller says where the session .ntpack goes. The
 * packing knobs are caller-driven (plan §5); the straight-alpha export profile
 * (premultiplied=false, compress=NULL, gen_mipmaps=false, RGBA8, no debug PNG)
 * is applied by tp_pack and is not tunable here. */
typedef struct tp_pack_settings {
    const char *atlas_name; /* required, normalization-invariant */
    const char *work_dir;   /* required, dir for the session .ntpack */

    const tp_pack_sprite_desc *sprites;
    int sprite_count;

    /* Mirror nt_atlas_opts_t; seed from tp_pack_settings_defaults(). */
    int max_size;        /* max page dimension, [1..4096] */
    int padding;         /* spacing between sprites (>= 0) */
    int margin;          /* atlas edge margin (>= 0) */
    int extrude;         /* AABB edge duplication (>= 0; must be 0 unless shape == RECT) */
    int alpha_threshold; /* alpha >= this = opaque for trimming, [0..255] */
    int max_vertices;    /* max polygon vertices per region, [1..16] */
    int shape;           /* nt_atlas_shape_t: 0=RECT, 1=CONVEX_HULL, 2=CONCAVE_CONTOUR */
    bool allow_transform;
    bool power_of_two;
    float pixels_per_unit; /* > 0 and finite */
} tp_pack_settings;

/* Fills `out` with the nt_atlas_opts_defaults() knobs (max_size=2048, padding=2,
 * shape=CONCAVE_CONTOUR, allow_transform, power_of_two, ppu=1.0, ...). Leaves
 * atlas_name/work_dir/sprites unset (caller must fill). Returns
 * TP_STATUS_INVALID_ARGUMENT if `out` is NULL. */
tp_status tp_pack_settings_defaults(tp_pack_settings *out);

/* Packs one atlas and returns an owned tp_result allocated from `arena`
 * (destroying the arena frees the result). On success returns TP_STATUS_OK and
 * writes *out_result. On any failure returns a tp_status, fills `err` (if
 * non-NULL), and sets *out_result to NULL. Never asserts on caller input:
 * invalid atlas_name / duplicate sprite names / bad dimensions all return
 * TP_STATUS_INVALID_ARGUMENT instead of tripping a builder assert. */
tp_status tp_pack(const tp_pack_settings *settings, struct tp_arena *arena, struct tp_result **out_result,
                  tp_error *err);

/* Cooperative cancellation for a long Pack. `poll(ctx)` returns true when the
 * caller has requested cancellation; the build-worker wait loop checks it while
 * the child runs and, on a true return, kills the worker, removes its staging,
 * publishes nothing, and returns TP_STATUS_OK with *out_result == NULL and no
 * builder error (a cancelled Pack is not a failure). A genuine success always
 * sets *out_result, so "OK with a NULL result" uniquely means cancelled. A NULL
 * poll disables cancellation; tp_pack() is exactly this with no poll. */
typedef bool (*tp_pack_cancel_poll)(void *ctx);

tp_status tp_pack_cancellable(const tp_pack_settings *settings, struct tp_arena *arena,
                              struct tp_result **out_result, tp_pack_cancel_poll cancel_poll,
                              void *cancel_ctx, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PACK_H */
