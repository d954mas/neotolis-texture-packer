#include "tp_core/tp_transform.h"

void tp_transform_decode(int32_t x, int32_t y, uint8_t flags, int32_t tw,
                         int32_t th, int32_t *ox, int32_t *oy) {
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

void tp_transform_out_dims(uint8_t flags, int32_t tw, int32_t th,
                           int32_t *ow, int32_t *oh) {
    if (flags & 4u) {
        *ow = th;
        *oh = tw;
    } else {
        *ow = tw;
        *oh = th;
    }
}

void tp_transform_decode_f(float x, float y, uint8_t flags, float width,
                           float height, float *out_x, float *out_y) {
    float transformed_x = x;
    float transformed_y = y;
    if ((flags & (uint8_t)TP_TRANSFORM_DIAGONAL) != 0U) {
        const float swap = transformed_x;
        transformed_x = transformed_y;
        transformed_y = swap;
    }
    const float output_width =
        (flags & (uint8_t)TP_TRANSFORM_DIAGONAL) != 0U ? height : width;
    const float output_height =
        (flags & (uint8_t)TP_TRANSFORM_DIAGONAL) != 0U ? width : height;
    if ((flags & (uint8_t)TP_TRANSFORM_FLIP_H) != 0U) {
        transformed_x = output_width - transformed_x;
    }
    if ((flags & (uint8_t)TP_TRANSFORM_FLIP_V) != 0U) {
        transformed_y = output_height - transformed_y;
    }
    *out_x = transformed_x;
    *out_y = transformed_y;
}
