#include "tp_core/tp_pack_read.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_pack_read_internal.h"

#include "hash/nt_hash.h"
#include "nt_atlas_format.h"
#include "nt_pack_format.h"
#include "nt_texture_format.h"

/* All struct reads go through memcpy into aligned locals: the on-disk structs
 * are #pragma pack(1) and the input buffer's alignment is caller-controlled. */

// #region pure recovery helpers (exposed to tests via tp_pack_read_internal.h)

void tp_transform_decode(int32_t x, int32_t y, uint8_t flags, int32_t tw, int32_t th, int32_t *ox, int32_t *oy) {
    int32_t rx = x;
    int32_t ry = y;
    if (flags & 4u) {
        int32_t t = rx;
        rx = ry;
        ry = t;
    }
    int32_t w = (flags & 4u) ? th : tw;
    int32_t h = (flags & 4u) ? tw : th;
    if (flags & 1u) {
        rx = w - rx;
    }
    if (flags & 2u) {
        ry = h - ry;
    }
    *ox = rx;
    *oy = ry;
}

void tp_transform_out_dims(uint8_t flags, int32_t tw, int32_t th, int32_t *ow, int32_t *oh) {
    if (flags & 4u) {
        *ow = th;
        *oh = tw;
    } else {
        *ow = tw;
        *oh = th;
    }
}

/* Builder's idealized encode: round-half-up over an exact page ratio (plan §2.5).
 * The real builder computes this in float32; the golden tests exercise that path.
 * This double form is the reference the UV property test pins. */
uint16_t tp_px_to_uv(int32_t px, int32_t page_dim) {
    double u = (((double)px * 65535.0) / (double)page_dim) + 0.5;
    if (u < 0.0) {
        u = 0.0;
    }
    if (u > 65535.0) {
        u = 65535.0;
    }
    return (uint16_t)u; /* truncation == floor for u >= 0 */
}

/* Exact inverse while the §2.5 recovery error 0.5*page_dim/65535 stays < 0.5px, i.e.
 * for page_dim up to ~32768 (0.125px at the raised 16384 max). lround absorbs it. */
int32_t tp_uv_to_px(uint16_t u, int32_t page_dim) {
    return (int32_t)lround(((double)u * (double)page_dim) / 65535.0);
}
// #endregion

// #region bounds helpers

static bool in_bounds(size_t off, size_t need, size_t total) {
    return off <= total && need <= total - off;
}

static size_t align4(uint32_t n) {
    return ((size_t)n + 3u) & ~(size_t)3u;
}
// #endregion

/* Parse-time scratch: a sprite plus the fields needed to group aliases before
 * the by-name sort assigns final indices. */
typedef struct {
    tp_sprite sprite;
    uint32_t vertex_start;
    uint16_t first_u;
    uint16_t first_v;
    uint8_t page_index;
} region_parse_t;

static int cmp_region_by_name(const void *a, const void *b) {
    const region_parse_t *ra = (const region_parse_t *)a;
    const region_parse_t *rb = (const region_parse_t *)b;
    return strcmp(ra->sprite.name, rb->sprite.name);
}

/* Resolve texture_resource_ids[p] against the entry table (never recompute a
 * hash, plan R2), validate the texture asset, copy mip0 into the arena. */
static tp_status parse_page(const uint8_t *data, size_t total, const NtAssetEntry *entries, uint16_t asset_count,
                           uint64_t tex_id, int page_no, const char *atlas_dbg, tp_arena *arena, tp_page *out_page,
                           tp_error *err) {
    const NtAssetEntry *te = NULL;
    for (uint16_t i = 0; i < asset_count; i++) {
        if (entries[i].resource_id == tex_id) {
            te = &entries[i];
            break;
        }
    }
    if (!te) {
        return tp_error_set(err, TP_STATUS_PAGE_NOT_FOUND,
                            "atlas '%s' page %d: no pack entry matches texture_resource_id 0x%016llx "
                            "(R2: atlas name needs no normalization?)",
                            atlas_dbg, page_no, (unsigned long long)tex_id);
    }
    if (te->asset_type != NT_ASSET_TEXTURE) {
        return tp_error_set(err, TP_STATUS_PAGE_NOT_FOUND,
                            "atlas '%s' page %d: entry 0x%016llx is asset_type %u, expected TEXTURE(%u)", atlas_dbg,
                            page_no, (unsigned long long)tex_id, (unsigned)te->asset_type, (unsigned)NT_ASSET_TEXTURE);
    }
    if (!in_bounds(te->offset, te->size, total)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s' page %d: texture entry [%u..+%u] overruns pack %zu",
                            atlas_dbg, page_no, (unsigned)te->offset, (unsigned)te->size, total);
    }
    if (te->size < sizeof(NtTextureAssetHeader)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s' page %d: texture entry %u bytes < header %zu",
                            atlas_dbg, page_no, (unsigned)te->size, sizeof(NtTextureAssetHeader));
    }
    NtTextureAssetHeader th;
    memcpy(&th, data + te->offset, sizeof th);
    if (th.magic != NT_TEXTURE_MAGIC) {
        return tp_error_set(err, TP_STATUS_BAD_MAGIC, "atlas '%s' page %d: texture magic 0x%08x != TTEX 0x%08x",
                            atlas_dbg, page_no, (unsigned)th.magic, (unsigned)NT_TEXTURE_MAGIC);
    }
    if (th.version != NT_TEXTURE_VERSION) {
        return tp_error_set(err, TP_STATUS_BAD_VERSION, "atlas '%s' page %d: texture version %u != %u", atlas_dbg,
                            page_no, (unsigned)th.version, (unsigned)NT_TEXTURE_VERSION);
    }
    if (th.compression != NT_TEXTURE_COMPRESSION_RAW || th.format != NT_TEXTURE_FORMAT_RGBA8) {
        return tp_error_set(err, TP_STATUS_UNSUPPORTED_TEXTURE,
                            "atlas '%s' page %d: texture compression=%u format=%u; reader requires RAW(0)+RGBA8(1). "
                            "Re-export with the straight-alpha uncompressed profile (compress=NULL, format=RGBA8).",
                            atlas_dbg, page_no, (unsigned)th.compression, (unsigned)th.format);
    }
    if (th.width == 0 || th.height == 0) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s' page %d: texture has zero dimension %ux%u",
                            atlas_dbg, page_no, (unsigned)th.width, (unsigned)th.height);
    }
    uint64_t pixel_bytes = (uint64_t)th.width * (uint64_t)th.height * 4u;
    uint64_t avail = (uint64_t)te->size - (uint64_t)sizeof(NtTextureAssetHeader);
    if (pixel_bytes > avail) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "atlas '%s' page %d: mip0 %ux%u needs %llu bytes, entry has %llu", atlas_dbg, page_no,
                            (unsigned)th.width, (unsigned)th.height, (unsigned long long)pixel_bytes,
                            (unsigned long long)avail);
    }
    uint8_t *px = (uint8_t *)tp_arena_alloc(arena, (size_t)pixel_bytes);
    if (!px) {
        return tp_error_set(err, TP_STATUS_OOM, "atlas '%s' page %d: arena OOM copying %llu pixel bytes", atlas_dbg,
                            page_no, (unsigned long long)pixel_bytes);
    }
    memcpy(px, data + te->offset + sizeof(NtTextureAssetHeader), (size_t)pixel_bytes);
    out_page->w = (int)th.width;
    out_page->h = (int)th.height;
    out_page->rgba = px;
    out_page->premultiplied = (th.flags & NT_TEXTURE_FLAG_PREMULTIPLIED) != 0u;
    out_page->image_name = NULL; /* set by caller from atlas display name */
    return TP_STATUS_OK;
}

/* Recover one sprite from a region (plan §2.4-2.6) into `out` (parse scratch). */
static tp_status parse_region(const NtAtlasRegion *reg, const uint8_t *blob, const NtAtlasHeader *ah,
                              const tp_page *pages, uint16_t page_count, const struct tp_name_map *names,
                              tp_arena *arena, region_parse_t *out, const char *atlas_dbg, tp_error *err) {
    const char *nm = names ? tp_name_map_lookup(names, reg->name_hash) : NULL;
    if (!nm) {
        return tp_error_set(err, TP_STATUS_UNKNOWN_REGION,
                            "atlas '%s': region name_hash 0x%016llx not in name map (register the sprite name)",
                            atlas_dbg, (unsigned long long)reg->name_hash);
    }
    char *nm_a = tp_arena_strdup(arena, nm);
    if (!nm_a) {
        return tp_error_set(err, TP_STATUS_OOM, "atlas '%s': arena OOM copying region name", atlas_dbg);
    }
    if (reg->page_index >= page_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s' sprite '%s': page_index %u >= page_count %u",
                            atlas_dbg, nm_a, (unsigned)reg->page_index, (unsigned)page_count);
    }
    const tp_page *page = &pages[reg->page_index];
    int page_w = page->w;
    int page_h = page->h;

    uint32_t vs = reg->vertex_start;
    uint32_t vc = reg->vertex_count;
    uint32_t is = reg->index_start;
    uint32_t ic = reg->index_count;
    if ((uint64_t)vs + (uint64_t)vc > (uint64_t)ah->total_vertex_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s' sprite '%s': vertex range %u+%u > total %u",
                            atlas_dbg, nm_a, (unsigned)vs, (unsigned)vc, (unsigned)ah->total_vertex_count);
    }
    if ((uint64_t)is + (uint64_t)ic > (uint64_t)ah->total_index_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s' sprite '%s': index range %u+%u > total %u",
                            atlas_dbg, nm_a, (unsigned)is, (unsigned)ic, (unsigned)ah->total_index_count);
    }

    tp_point *verts = NULL;
    if (vc > 0) {
        verts = (tp_point *)tp_arena_alloc(arena, (size_t)vc * sizeof(tp_point));
        if (!verts) {
            return tp_error_set(err, TP_STATUS_OOM, "atlas '%s' sprite '%s': arena OOM (verts)", atlas_dbg, nm_a);
        }
    }

    int32_t min_lx = 0;
    int32_t max_lx = 0;
    int32_t min_ly = 0;
    int32_t max_ly = 0;
    int32_t min_px = 0;
    int32_t max_px = 0;
    int32_t min_py = 0;
    int32_t max_py = 0;
    uint16_t first_u = 0;
    uint16_t first_v = 0;

    const uint8_t *vbase = blob + ah->vertex_offset;
    for (uint32_t v = 0; v < vc; v++) {
        NtAtlasVertex vtx;
        memcpy(&vtx, vbase + (size_t)(vs + v) * sizeof(NtAtlasVertex), sizeof vtx);
        int32_t lx = vtx.local_x;
        int32_t ly = vtx.local_y;
        int32_t px = tp_uv_to_px(vtx.atlas_u, page_w);
        int32_t py = tp_uv_to_px(vtx.atlas_v, page_h);
        if (v == 0) {
            min_lx = max_lx = lx;
            min_ly = max_ly = ly;
            min_px = max_px = px;
            min_py = max_py = py;
            first_u = vtx.atlas_u;
            first_v = vtx.atlas_v;
        } else {
            if (lx < min_lx) { min_lx = lx; }
            if (lx > max_lx) { max_lx = lx; }
            if (ly < min_ly) { min_ly = ly; }
            if (ly > max_ly) { max_ly = ly; }
            if (px < min_px) { min_px = px; }
            if (px > max_px) { max_px = px; }
            if (py < min_py) { min_py = py; }
            if (py > max_py) { max_py = py; }
        }
        /* Store local coords now; y flips to y-down once trim_h is known. */
        verts[v].x = lx;
        verts[v].y = ly;
    }

    /* Recover the trim rect from the hull's LOCAL bounding box. The hull's min
     * corner is NOT necessarily (0,0): clipper2 inflates a convex/concave hull
     * OUTWARD, so min_lx (and min_ly) go negative past the un-inflated trim edge.
     * Normalize so the hull's bbox min corner IS the trim-local origin (0,0):
     *   - trim_w/h = the local SPAN (max - min) = the true as-drawn footprint;
     *   - verts.x  = lx - min_lx  (shift the hull's left edge to x = 0);
     *   - verts.y  = max_ly - ly  (y-up -> y-down; the hull TOP maps to y = 0);
     *   - ssx      = trim_offset_x + min_lx  (untrimmed-x of the hull left edge);
     *   - ssy      = src_h - trim_offset_y - max_ly  (untrimmed-y of hull top).
     * This preserves every vertex's ON-PAGE position (frame.xy + verts through the
     * D4 texel transform) and its UNTRIMMED-SOURCE position (spriteSourceSize.xy +
     * verts) EXACTLY, while making spriteSourceSize (== source_rect) equal the
     * vertices' bounding box -- the invariant every consumer relies on (the GUI
     * canvas hull overlay, json-neotolis, the Defold .tpinfo). Previously trim_w/h
     * assumed min == 0, leaving the trim origin |min_lx| px right of the real hull:
     * the drawn hull shifted left, and once a region's D4 mask rotated the verts,
     * in mask-dependent screen directions. Only RECT sprites recover the exact
     * un-inflated trim (min == 0); non-RECT keep the clipper2 inflation symmetric
     * about the sprite. */
    int32_t trim_w = max_lx - min_lx;
    int32_t trim_h = max_ly - min_ly;

    for (uint32_t v = 0; v < vc; v++) {
        verts[v].x = verts[v].x - min_lx; /* hull left edge -> x = 0        */
        verts[v].y = max_ly - verts[v].y; /* y-up local -> y-down, top -> 0 */
    }

    uint16_t *indices = NULL;
    if (ic > 0) {
        indices = (uint16_t *)tp_arena_alloc(arena, (size_t)ic * sizeof(uint16_t));
        if (!indices) {
            return tp_error_set(err, TP_STATUS_OOM, "atlas '%s' sprite '%s': arena OOM (indices)", atlas_dbg, nm_a);
        }
        memcpy(indices, blob + ah->index_offset + (size_t)is * sizeof(uint16_t), (size_t)ic * sizeof(uint16_t));
    }

    int32_t src_w = (int32_t)reg->source_w;
    int32_t src_h = (int32_t)reg->source_h;
    int32_t ssx = (int32_t)reg->trim_offset_x + min_lx;         /* left edge of the recovered hull bbox */
    int32_t ssy = src_h - (int32_t)reg->trim_offset_y - max_ly; /* top edge (verts.y == 0 anchor)       */

    bool trimmed = !(ssx == 0 && ssy == 0 && trim_w == src_w && trim_h == src_h);

    tp_sprite *s = &out->sprite;
    memset(s, 0, sizeof *s);
    s->name = nm_a;
    s->page = (int)reg->page_index;
    s->frame.x = min_px;
    s->frame.y = min_py;
    s->frame.w = trim_w;
    s->frame.h = trim_h;
    s->transform = reg->transform;
    s->trimmed = trimmed;
    s->spriteSourceSize.x = ssx;
    s->spriteSourceSize.y = ssy;
    s->spriteSourceSize.w = trim_w;
    s->spriteSourceSize.h = trim_h;
    s->sourceSize.w = src_w;
    s->sourceSize.h = src_h;
    s->pivot.x = reg->origin_x;
    s->pivot.y = 1.0f - reg->origin_y;
    for (int k = 0; k < 4; k++) {
        s->slice9_lrtb[k] = reg->slice9_lrtb[k];
    }
    s->verts = verts;
    s->vert_count = (int)vc;
    s->indices = indices;
    s->index_count = (int)ic;
    s->alias_of = -1;

    out->vertex_start = vs;
    out->page_index = reg->page_index;
    out->first_u = first_u;
    out->first_v = first_v;

    /* Invariants (plan §2.6): a violation means a corrupt pack or reader bug.
     * NOTE: trim_w/h are the hull's local SPAN (max - min), which for CONVEX /
     * CONCAVE shapes is the clipper2-INFLATED envelope (~1-2px/side past the true
     * trim rect), so trim_w may legitimately exceed source_w. Do NOT bounds-check
     * the trim rect against the source -- only RECT-shaped sprites recover the
     * exact trim rect (nt_builder_atlas.c:1161-1168 vs the inflate path). */
    if (!trimmed && !(ssx == 0 && ssy == 0 && trim_w == src_w && trim_h == src_h)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "atlas '%s' sprite '%s': untrimmed but spriteSourceSize != full source", atlas_dbg, nm_a);
    }
    if (vc > 0) {
        int32_t span_x = max_lx - min_lx;
        int32_t span_y = max_ly - min_ly;
        int32_t exp_w = 0;
        int32_t exp_h = 0;
        tp_transform_out_dims(reg->transform, span_x, span_y, &exp_w, &exp_h);
        int32_t aabb_w = max_px - min_px;
        int32_t aabb_h = max_py - min_py;
        if (aabb_w != exp_w || aabb_h != exp_h) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "atlas '%s' sprite '%s': UV AABB %dx%d != transform-swapped hull span %dx%d "
                                "(transform=%u)",
                                atlas_dbg, nm_a, aabb_w, aabb_h, exp_w, exp_h, (unsigned)reg->transform);
        }
        if (min_px < 0 || min_py < 0 || max_px > page_w || max_py > page_h) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "atlas '%s' sprite '%s': on-page AABB [%d,%d..%d,%d] outside page %dx%d", atlas_dbg,
                                nm_a, min_px, min_py, max_px, max_py, page_w, page_h);
        }
    }
    return TP_STATUS_OK;
}

/* Walk the atlas entry's meta run (plan §2.1). Records for one resource_id are
 * contiguous (writer sorts by resource_id) and the entry's meta_offset points
 * at the first; there is NO per-entry count or terminator, so we stop when the
 * record's resource_id changes. Absent meta (meta_offset==0) is not an error. */
static tp_status read_ppu_meta(const uint8_t *data, size_t total, uint32_t meta_offset, uint64_t atlas_id,
                               const char *atlas_dbg, float *out_ppu, tp_error *err) {
    *out_ppu = 1.0f;
    if (meta_offset == 0) {
        return TP_STATUS_OK;
    }
    if ((size_t)meta_offset + sizeof(NtMetaEntryHeader) > total) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s': meta_offset %u out of bounds (pack %zu)",
                            atlas_dbg, (unsigned)meta_offset, total);
    }
    const uint64_t ppu_kind = nt_hash64_str("pixels_per_unit").value;
    size_t off = meta_offset;
    for (;;) {
        if (off + sizeof(NtMetaEntryHeader) > total) {
            break; /* clean end of meta section */
        }
        NtMetaEntryHeader mh;
        memcpy(&mh, data + off, sizeof mh);
        if (mh.resource_id != atlas_id) {
            break; /* sorted by resource_id: past this atlas's run */
        }
        if ((uint64_t)off + (uint64_t)sizeof(NtMetaEntryHeader) + (uint64_t)mh.size > (uint64_t)total) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s': meta payload (%u bytes) overruns pack",
                                atlas_dbg, (unsigned)mh.size);
        }
        if (mh.kind == ppu_kind) {
            if (mh.size < sizeof(float)) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "atlas '%s': pixels_per_unit payload %u bytes < 4", atlas_dbg, (unsigned)mh.size);
            }
            float v = 0.0f;
            memcpy(&v, data + off + sizeof(NtMetaEntryHeader), sizeof v);
            *out_ppu = v;
            return TP_STATUS_OK;
        }
        off += sizeof(NtMetaEntryHeader) + align4(mh.size);
    }
    return TP_STATUS_OK; /* not found -> default 1.0 */
}

/* Parse one NT_ASSET_ATLAS entry into a fully-populated tp_result. */
static tp_status parse_atlas(const uint8_t *data, size_t total, const NtAssetEntry *entries, uint16_t asset_count,
                            const NtAssetEntry *atlas_entry, const struct tp_name_map *names, tp_arena *arena,
                            tp_result *out, tp_error *err) {
    /* Atlas display name: same reverse map, hex fallback on miss (plan §2.8). */
    const char *disp = names ? tp_name_map_lookup(names, atlas_entry->resource_id) : NULL;
    char hexname[32];
    if (!disp) {
        (void)snprintf(hexname, sizeof hexname, "atlas_%016llx", (unsigned long long)atlas_entry->resource_id);
        disp = hexname;
    }
    char *atlas_name = tp_arena_strdup(arena, disp);
    if (!atlas_name) {
        return tp_error_set(err, TP_STATUS_OOM, "arena OOM copying atlas name");
    }
    out->atlas_name = atlas_name;

    if (!in_bounds(atlas_entry->offset, atlas_entry->size, total)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s': entry [%u..+%u] overruns pack %zu", atlas_name,
                            (unsigned)atlas_entry->offset, (unsigned)atlas_entry->size, total);
    }
    const uint8_t *blob = data + atlas_entry->offset;
    size_t blob_size = atlas_entry->size;
    if (blob_size < sizeof(NtAtlasHeader)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s': blob %zu bytes < header %zu", atlas_name,
                            blob_size, sizeof(NtAtlasHeader));
    }
    NtAtlasHeader ah;
    memcpy(&ah, blob, sizeof ah);
    if (ah.magic != NT_ATLAS_MAGIC) {
        return tp_error_set(err, TP_STATUS_BAD_MAGIC, "atlas '%s': magic 0x%08x != ATLS 0x%08x", atlas_name,
                            (unsigned)ah.magic, (unsigned)NT_ATLAS_MAGIC);
    }
    if (ah.version != NT_ATLAS_VERSION) {
        return tp_error_set(err, TP_STATUS_BAD_VERSION, "atlas '%s': version %u != %u", atlas_name,
                            (unsigned)ah.version, (unsigned)NT_ATLAS_VERSION);
    }

    uint16_t page_count = ah.page_count;
    uint16_t region_count = ah.region_count;
    size_t tex_ids_off = sizeof(NtAtlasHeader);
    if (!in_bounds(tex_ids_off, (size_t)page_count * sizeof(uint64_t), blob_size)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s': %u texture ids overrun blob", atlas_name,
                            (unsigned)page_count);
    }
    size_t regions_off = tex_ids_off + (size_t)page_count * sizeof(uint64_t);
    if (!in_bounds(regions_off, (size_t)region_count * sizeof(NtAtlasRegion), blob_size)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s': %u regions overrun blob", atlas_name,
                            (unsigned)region_count);
    }
    if (!in_bounds(ah.vertex_offset, (size_t)ah.total_vertex_count * sizeof(NtAtlasVertex), blob_size)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s': vertex array overruns blob", atlas_name);
    }
    if (!in_bounds(ah.index_offset, (size_t)ah.total_index_count * sizeof(uint16_t), blob_size)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "atlas '%s': index array overruns blob", atlas_name);
    }

    /* Pages */
    out->page_count = (int)page_count;
    out->pages = NULL;
    tp_page *pages = NULL;
    if (page_count > 0) {
        pages = (tp_page *)tp_arena_alloc(arena, (size_t)page_count * sizeof(tp_page));
        if (!pages) {
            return tp_error_set(err, TP_STATUS_OOM, "atlas '%s': arena OOM (pages)", atlas_name);
        }
        memset(pages, 0, (size_t)page_count * sizeof(tp_page));
        for (uint16_t p = 0; p < page_count; p++) {
            uint64_t tex_id = 0;
            memcpy(&tex_id, blob + tex_ids_off + (size_t)p * sizeof(uint64_t), sizeof tex_id);
            tp_status st = parse_page(data, total, entries, asset_count, tex_id, (int)p, atlas_name, arena, &pages[p],
                                      err);
            if (st != TP_STATUS_OK) {
                return st;
            }
            char pbuf[288];
            if (page_count == 1) {
                (void)snprintf(pbuf, sizeof pbuf, "%s", atlas_name);
            } else {
                (void)snprintf(pbuf, sizeof pbuf, "%s.%u", atlas_name, (unsigned)p);
            }
            char *pn = tp_arena_strdup(arena, pbuf);
            if (!pn) {
                return tp_error_set(err, TP_STATUS_OOM, "atlas '%s': arena OOM (page name)", atlas_name);
            }
            pages[p].image_name = pn;
        }
        out->pages = pages;
    }

    /* Regions -> sprites */
    out->sprite_count = 0;
    out->sprites = NULL;
    if (region_count > 0) {
        region_parse_t *rp = (region_parse_t *)malloc((size_t)region_count * sizeof(region_parse_t));
        if (!rp) {
            return tp_error_set(err, TP_STATUS_OOM, "atlas '%s': OOM (region scratch)", atlas_name);
        }
        for (uint16_t r = 0; r < region_count; r++) {
            NtAtlasRegion reg;
            memcpy(&reg, blob + regions_off + (size_t)r * sizeof(NtAtlasRegion), sizeof reg);
            tp_status st = parse_region(&reg, blob, &ah, pages, page_count, names, arena, &rp[r], atlas_name, err);
            if (st != TP_STATUS_OK) {
                free(rp);
                return st;
            }
        }

        /* Deterministic order (plan §2.7); names are unique so strcmp is total. */
        qsort(rp, region_count, sizeof(region_parse_t), cmp_region_by_name);

        for (uint16_t i = 0; i < region_count; i++) {
            rp[i].sprite.alias_of = -1;
            for (uint16_t j = 0; j < i; j++) {
                if (rp[j].vertex_start == rp[i].vertex_start && rp[j].page_index == rp[i].page_index &&
                    rp[j].first_u == rp[i].first_u && rp[j].first_v == rp[i].first_v) {
                    rp[i].sprite.alias_of = (rp[j].sprite.alias_of >= 0) ? rp[j].sprite.alias_of : (int)j;
                    break;
                }
            }
        }

        tp_sprite *sprites = (tp_sprite *)tp_arena_alloc(arena, (size_t)region_count * sizeof(tp_sprite));
        if (!sprites) {
            free(rp);
            return tp_error_set(err, TP_STATUS_OOM, "atlas '%s': arena OOM (sprites)", atlas_name);
        }
        for (uint16_t i = 0; i < region_count; i++) {
            sprites[i] = rp[i].sprite;
        }
        free(rp);
        out->sprites = sprites;
        out->sprite_count = (int)region_count;
    }

    /* pixels_per_unit meta (plan §2.1) */
    float ppu = 1.0f;
    tp_status mst = read_ppu_meta(data, total, atlas_entry->meta_offset, atlas_entry->resource_id, atlas_name, &ppu,
                                  err);
    if (mst != TP_STATUS_OK) {
        return mst;
    }
    out->pixels_per_unit = ppu;
    return TP_STATUS_OK;
}

tp_status tp_pack_read_memory(const void *data, size_t size, const struct tp_name_map *names, struct tp_arena *arena,
                              struct tp_result ***out_results, int *out_count, tp_error *err) {
    if (out_results) {
        *out_results = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!data || !arena || !out_results || !out_count) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack_read_memory: NULL argument");
    }
    const uint8_t *buf = (const uint8_t *)data;
    if (size < sizeof(NtPackHeader)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "buffer %zu bytes < pack header %zu", size,
                            sizeof(NtPackHeader));
    }
    NtPackHeader ph;
    memcpy(&ph, buf, sizeof ph);
    if (ph.magic != NT_PACK_MAGIC) {
        return tp_error_set(err, TP_STATUS_BAD_MAGIC, "pack magic 0x%08x != NPAK 0x%08x", (unsigned)ph.magic,
                            (unsigned)NT_PACK_MAGIC);
    }
    if (ph.version != NT_PACK_VERSION) {
        return tp_error_set(err, TP_STATUS_BAD_VERSION, "pack version %u != %u", (unsigned)ph.version,
                            (unsigned)NT_PACK_VERSION);
    }
    if ((size_t)ph.total_size > size) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "pack total_size %u exceeds buffer %zu (truncated?)",
                            (unsigned)ph.total_size, size);
    }
    if (ph.total_size < sizeof(NtPackHeader)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "pack total_size %u < header", (unsigned)ph.total_size);
    }
    size_t total = ph.total_size;

    uint16_t asset_count = ph.asset_count;
    size_t entries_off = sizeof(NtPackHeader);
    if (!in_bounds(entries_off, (size_t)asset_count * sizeof(NtAssetEntry), total)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "entry table (%u entries) overruns pack", (unsigned)asset_count);
    }

    /* Copy the entry table into an aligned array (packed on-disk struct). */
    NtAssetEntry *entries = NULL;
    if (asset_count > 0) {
        entries = (NtAssetEntry *)malloc((size_t)asset_count * sizeof(NtAssetEntry));
        if (!entries) {
            return tp_error_set(err, TP_STATUS_OOM, "OOM copying entry table");
        }
        memcpy(entries, buf + entries_off, (size_t)asset_count * sizeof(NtAssetEntry));
    }

    int atlas_count = 0;
    for (uint16_t i = 0; i < asset_count; i++) {
        if (entries[i].asset_type == NT_ASSET_ATLAS) {
            atlas_count++;
        }
    }

    tp_result **results = NULL;
    if (atlas_count > 0) {
        results = (tp_result **)tp_arena_alloc(arena, (size_t)atlas_count * sizeof(tp_result *));
        if (!results) {
            free(entries);
            return tp_error_set(err, TP_STATUS_OOM, "arena OOM (results array)");
        }
    }

    int idx = 0;
    for (uint16_t i = 0; i < asset_count; i++) {
        if (entries[i].asset_type != NT_ASSET_ATLAS) {
            continue;
        }
        tp_result *res = (tp_result *)tp_arena_alloc(arena, sizeof(tp_result));
        if (!res) {
            free(entries);
            return tp_error_set(err, TP_STATUS_OOM, "arena OOM (tp_result)");
        }
        memset(res, 0, sizeof *res);
        res->pixels_per_unit = 1.0f;
        tp_status st = parse_atlas(buf, total, entries, asset_count, &entries[i], names, arena, res, err);
        if (st != TP_STATUS_OK) {
            free(entries);
            return st;
        }
        results[idx++] = res;
    }

    free(entries);
    *out_results = results;
    *out_count = atlas_count;
    return TP_STATUS_OK;
}

tp_status tp_pack_read_file(const char *path, const struct tp_name_map *names, struct tp_arena *arena,
                            struct tp_result ***out_results, int *out_count, tp_error *err) {
    if (out_results) {
        *out_results = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!path || !arena || !out_results || !out_count) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_pack_read_file: NULL argument");
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "cannot open '%s'", path);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "seek failed on '%s'", path);
    }
    long sz = ftell(f);
    if (sz < 0) {
        (void)fclose(f);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "ftell failed on '%s'", path);
    }
    rewind(f);
    size_t nbytes = (size_t)sz;
    void *bufmem = malloc(nbytes > 0 ? nbytes : 1);
    if (!bufmem) {
        (void)fclose(f);
        return tp_error_set(err, TP_STATUS_OOM, "OOM reading '%s' (%zu bytes)", path, nbytes);
    }
    size_t rd = fread(bufmem, 1, nbytes, f);
    (void)fclose(f);
    if (rd != nbytes) {
        free(bufmem);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "short read on '%s' (%zu of %zu)", path, rd, nbytes);
    }
    tp_status st = tp_pack_read_memory(bufmem, nbytes, names, arena, out_results, out_count, err);
    free(bufmem);
    return st;
}
