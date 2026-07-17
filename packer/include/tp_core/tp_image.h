#ifndef TP_CORE_TP_IMAGE_H
#define TP_CORE_TP_IMAGE_H

/* Shared raster-file ingress for packer clients.
 *
 * Paths are strict UTF-8. Encoded bytes are read through the packer's UTF-8
 * filesystem boundary and decoded from memory to tightly packed, top-to-bottom,
 * straight-alpha RGBA8. Resource limits are part of the public contract so a
 * malformed/compressed-bomb input cannot request unbounded allocations. */

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TP_IMAGE_MAX_FILE_BYTES ((size_t)257U * 1024U * 1024U)
#define TP_IMAGE_MAX_DIMENSION 16384U
#define TP_IMAGE_MAX_RGBA_BYTES ((size_t)256U * 1024U * 1024U)

typedef struct tp_image_rgba8 {
    uint8_t *pixels; /* width * height * 4 bytes; owned by this value */
    int width;
    int height;
} tp_image_rgba8;

/* Loads one regular file. `out` is reset before any work and stays empty on
 * failure. Empty/corrupt/unsupported images return UNSUPPORTED_TEXTURE;
 * encoded/dimension/decoded-byte limits return OUT_OF_BOUNDS; malformed path
 * text returns INVALID_UTF8. Other filesystem failures return
 * INVALID_ARGUMENT with path context. */
tp_status tp_image_load_file(const char *path_utf8, tp_image_rgba8 *out,
                             tp_error *err);

/* Releases pixels with the decoder's matching allocator and resets `image`.
 * Safe for NULL and already-empty values. */
void tp_image_free(tp_image_rgba8 *image);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_IMAGE_H */
