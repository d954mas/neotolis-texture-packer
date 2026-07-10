#ifndef TP_CORE_TP_MODEL_H
#define TP_CORE_TP_MODEL_H

/*
 * Canonical sprite/atlas model (docs/research/SUMMARY.md §5d, plan §2.6).
 * Every pointer here is arena-owned (tp_arena.h) -- destroying the arena that
 * produced a tp_result frees the whole thing in one shot.
 *
 * Coordinate spaces (do not mix these up -- see plan §2.6/§2.7):
 *   - frame / spriteSourceSize / sourceSize / pivot are all y-down PNG space
 *     (origin top-left, y increases downward), NOT the engine's y-up runtime
 *     space.
 *   - tp_page.rgba is y-down, top row first.
 *   - In the source .ntpack atlas format, atlas_v is likewise y-down (the
 *     builder bakes this at write time), which is why page pixels can be
 *     indexed straight from atlas UVs with no extra flip.
 *   - verts are TRIM-LOCAL y-down: (0,0) is the top-left of the trimmed
 *     sprite content, independent of where the sprite landed on the page.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_point {
    int32_t x, y;
} tp_point;

/*
 * D4 transform mask (plan §2.4, nt_atlas_format.h transform byte).
 * Apply order is diagonal -> flipH -> flipV; when bit2 (diagonal) is set the
 * trim dimensions (w,h) are swapped before flipping. 0 = identity.
 */
typedef enum tp_transform {
    TP_TRANSFORM_FLIP_H = 1,
    TP_TRANSFORM_FLIP_V = 2,
    TP_TRANSFORM_DIAGONAL = 4
} tp_transform;

typedef struct tp_sprite {
    const char *name; /* resolved via tp_name_map from the region's name_hash */
    int page;         /* multipack page index */

    struct {
        int x, y, w, h; /* placed rect on page, y-down; w/h UNROTATED (pre-D4) */
    } frame;

    uint8_t transform; /* raw D4 mask, see tp_transform; 0 = identity */
    bool trimmed;

    struct {
        int x, y, w, h; /* trimmed rect inside the original source image, y-down */
    } spriteSourceSize;

    struct {
        int w, h; /* original untrimmed source size */
    } sourceSize;

    struct {
        float x, y; /* normalized over sourceSize; default 0.5,0.5; may exceed [0,1] */
    } pivot;

    uint16_t slice9_lrtb[4]; /* [left,right,top,bottom]; all-zero = none */

    tp_point *verts; /* trim-local y-down hull vertices, arena-owned */
    int vert_count;

    uint16_t *indices; /* triangle list, local indices 0..vert_count-1, arena-owned */
    int index_count;

    int alias_of; /* -1 if this is the original placement, else index of the original */
} tp_sprite;

typedef struct tp_page {
    const char *image_name; /* tool-chosen page identifier, e.g. for export filenames */
    int w, h;
    const uint8_t *rgba; /* y-down, top row first, arena-owned */
    bool premultiplied;
} tp_page;

typedef struct tp_result {
    const char *atlas_name;
    float pixels_per_unit;

    tp_page *pages;
    int page_count;

    tp_sprite *sprites; /* sorted by name for deterministic output */
    int sprite_count;
} tp_result;

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_MODEL_H */
