#include "tp_core/tp_transform.h"

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
