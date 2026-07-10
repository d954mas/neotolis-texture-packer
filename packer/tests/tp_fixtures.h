#ifndef TP_FIXTURES_H
#define TP_FIXTURES_H

/* Procedural fixture library for the fixture-driven golden round-trip test
 * (plan §3.1/§3.2, task 6). Links against nt_builder + tp_core; lives in the
 * test tree, NOT in the tp_core lib.
 *
 * Each fixture CASE is one atlas built into its own .ntpack via nt_builder,
 * using the §5 export profile (premultiplied=false, compress=NULL,
 * gen_mipmaps=false, debug_png=false) plus per-case overrides. A case carries
 * the EXPECTED canonical values its generator fed in so the golden test can
 * assert the parse-back without re-deriving from the same UVs (which would be
 * circular). Colours are position-identifying so pixel probes detect a wrong
 * frame origin AND diagnose transform orientation.
 *
 * Assertion mode (plan R1 amendment, VERIFIED 2026-07-10): the builder INFLATES
 * non-RECT hulls via clipper2 (~1px), so exact trim/frame golden assertions are
 * valid ONLY for RECT sprites. Polygon sprites use tolerance mode: parse
 * succeeds, §2.6 invariants hold, vertex_count <= max_vertices, recovered trim
 * within a few px of known trim, and pixel probes hit known content. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tp_name_map;

#define TP_FIX_MAX_SPRITES 6
#define TP_FIX_MAX_PROBES 6

/* RECT sprites recover the exact trim rect; polygon sprites over-report trim by
 * the clipper2 inflation, so they assert within tolerance only. */
typedef enum {
    TP_FIX_EXACT = 0,
    TP_FIX_TOLERANCE = 1,
} tp_fix_mode;

/* Pixel generator kind. RECT paints a filled trim rect with 4 distinct corner
 * markers; the polygon kinds paint a silhouette on a transparent field so the
 * alpha bbox equals the declared trim rect. */
typedef enum {
    TP_GEN_RECT = 0,
    TP_GEN_L = 1,
    TP_GEN_DIAMOND = 2,
    TP_GEN_PLUS = 3,
} tp_gen_kind;

/* A probe: a source-space (y-down, original-image) pixel and the RGBA the page
 * must show once the sprite is placed. The test maps src -> trim -> page via the
 * recovered frame + D4 transform, then samples. */
typedef struct {
    int32_t src_x;
    int32_t src_y;
    uint8_t rgba[4];
} tp_fix_probe;

typedef struct {
    const char *name;
    int32_t src_w;
    int32_t src_h;

    /* Known tight alpha bbox (the trim rect the builder computes), y-down. */
    int32_t trim_x;
    int32_t trim_y;
    int32_t trim_w;
    int32_t trim_h;

    /* Pivot as passed to nt_atlas_sprite_opts_t (y-down source space). The
     * reader must recover pivot == this exactly (the two y-flips cancel). */
    float pivot_x;
    float pivot_y;

    uint16_t slice9_lrtb[4]; /* [left,right,top,bottom]; all zero = none */

    tp_fix_mode mode;
    bool expect_trimmed;

    /* Index (within this case's sprite list) of the sprite this one dedups to,
     * or -1 if original / not an alias. */
    int alias_to;

    tp_gen_kind gen_kind;
    uint8_t base[4];       /* fill colour */
    uint8_t corners[4][4]; /* TL,TR,BL,BR markers (RECT only) */

    tp_fix_probe probes[TP_FIX_MAX_PROBES];
    int probe_count;
} tp_fixture_sprite;

typedef struct {
    const char *name; /* case id AND atlas name (slash-free, §5) */

    /* Atlas opts overrides on top of the §5 export profile. */
    uint32_t max_size;
    uint8_t max_vertices;
    uint32_t padding;
    uint32_t margin;
    int shape; /* nt_atlas_shape_t */
    bool allow_transform;
    bool power_of_two;
    float pixels_per_unit;

    /* Page-dimension expectations. */
    int expect_min_pages;       /* assert page_count >= this (0 => >= 1) */
    bool expect_page_eq_sprite; /* page dims == sprites[0] source dims */
    bool expect_page_non_pot;   /* assert at least one page dim is non-POT */

    tp_fixture_sprite sprites[TP_FIX_MAX_SPRITES];
    int sprite_count;
} tp_fixture_case;

/* Returns the shared, lazily-built fixture case table (plan §3.1). */
const tp_fixture_case *tp_fixtures_all(int *out_count);

/* Builds one case to "<out_dir>/<case.name>.ntpack" via nt_builder using the §5
 * export profile + the case overrides. Writes the full path into out_path.
 * Returns true on NT_BUILD_OK. Deterministic (single-threaded). */
bool tp_fixture_build(const tp_fixture_case *c, const char *out_dir, char *out_path, size_t cap);

/* Registers every atlas + sprite name across all cases into the map (so the
 * reader can resolve region name_hash -> name). Returns false on collision/OOM. */
bool tp_fixtures_register_names(struct tp_name_map *map);

#ifdef __cplusplus
}
#endif

#endif /* TP_FIXTURES_H */
