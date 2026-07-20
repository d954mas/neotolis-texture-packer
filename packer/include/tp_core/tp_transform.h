#ifndef TP_CORE_TP_TRANSFORM_H
#define TP_CORE_TP_TRANSFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Engine-type-free D4 geometry used by the canonical packed result and every
 * client that projects it. Apply order is diagonal -> flip H -> flip V. */
typedef enum tp_transform {
    TP_TRANSFORM_FLIP_H = 1,
    TP_TRANSFORM_FLIP_V = 2,
    TP_TRANSFORM_DIAGONAL = 4
} tp_transform;

/* Corner-space affine decode. Reflection is width - x / height - y, not the
 * texel-index form width - 1 - x. Inputs may lie outside the trim bounds. */
void tp_transform_decode(int32_t x, int32_t y, uint8_t flags, int32_t width,
                         int32_t height, int32_t *out_x, int32_t *out_y);
void tp_transform_decode_f(float x, float y, uint8_t flags, float width,
                           float height, float *out_x, float *out_y);

void tp_transform_out_dims(uint8_t flags, int32_t width, int32_t height,
                           int32_t *out_width, int32_t *out_height);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_TRANSFORM_H */
