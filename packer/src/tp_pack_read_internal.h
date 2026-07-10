#ifndef TP_PACK_READ_INTERNAL_H
#define TP_PACK_READ_INTERNAL_H

/* Private helpers shared with the packer tests (test target adds src/ to its
 * include path and includes this directly). NOT part of the public API -- the
 * recovery math is exposed only so the synthetic transform test (§3.6) and the
 * UV property test (§3.3) can pin it without a full pack round-trip. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reference D4 decode -- byte-for-byte mirror of the engine serializer's
 * transform_point (nt_builder_atlas_geometry.c:983-1001), corner space.
 * Maps a trim-local corner (x,y) in [0..tw]x[0..th] to its on-page-relative
 * corner given the region's transform mask. Apply order: diagonal -> flipH ->
 * flipV. Uses corner reflection (w - x), NOT texel reflection (w - 1 - x). */
void tp_transform_decode(int32_t x, int32_t y, uint8_t flags, int32_t tw, int32_t th, int32_t *ox, int32_t *oy);

/* On-page footprint dims after the D4 transform: (th,tw) when the diagonal bit
 * (4) is set, else (tw,th). */
void tp_transform_out_dims(uint8_t flags, int32_t tw, int32_t th, int32_t *ow, int32_t *oh);

/* Exact UV<->pixel conversions for page dims <= 4096 (plan §2.5). Encode is the
 * builder's idealized round-half-up; decode inverts it exactly. */
uint16_t tp_px_to_uv(int32_t px, int32_t page_dim);
int32_t tp_uv_to_px(uint16_t u, int32_t page_dim);

#ifdef __cplusplus
}
#endif

#endif /* TP_PACK_READ_INTERNAL_H */
