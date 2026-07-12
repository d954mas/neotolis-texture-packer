#ifndef TP_C0_RASTER_H
#define TP_C0_RASTER_H

/*
 * C0-04 reference raster normalization pipeline (master spec §10.2, §11.1,
 * §59 item 28, §60 item 7; decision 0005).
 *
 * These are the FORMAT-INDEPENDENT canonical steps that turn raw decoded
 * samples into the one canonical image the semantic-image-hash is taken over:
 *
 *     RGBA8, rows top-to-bottom, channels R,G,B,A, straight alpha,
 *     no row padding, orientation already applied.
 *
 * This TU is the executable policy B0-03 / B1-01 must implement; it never calls
 * a decoder itself (decoder-independent). The stb glue used by the golden tests
 * lives in the separate, un-sanitized tp_c0_stb TU. Everything here is pure
 * integer math -- no libm, no float, no host-endianness -- so the canonical
 * bytes are byte-identical on every CI OS.
 */

#include "tp_c0/tp_c0_error.h" /* tp_c0_detail, tp_error, header-only */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical decoded image. rgba is width*height*4 bytes, owned by the struct. */
typedef struct tp_c0_image {
    uint32_t width;
    uint32_t height;
    uint8_t *rgba;
} tp_c0_image;

void tp_c0_image_free(tp_c0_image *img); /* frees rgba, zeroes the struct */

/* Non-fatal notices gathered while normalizing (ICC ignored, unknown EXIF...).
 * Kept as tp_c0_detail tokens so a client matches one stable id table; a notice
 * is NEVER a function's error return on a successful decode. Fixed small cap:
 * the policy emits at most a handful per image. */
enum { TP_C0_RASTER_NOTICES_CAP = 8 };
typedef struct tp_c0_raster_notices {
    unsigned count;
    tp_c0_detail tokens[TP_C0_RASTER_NOTICES_CAP];
} tp_c0_raster_notices;

void tp_c0_raster_notices_reset(tp_c0_raster_notices *n);
void tp_c0_raster_notices_add(tp_c0_raster_notices *n, tp_c0_detail token);
bool tp_c0_raster_notices_has(const tp_c0_raster_notices *n, tp_c0_detail token);

/* Channel layout of already-decoded samples. Value == channel count. Palette is
 * expanded to RGB8/RGBA8 by the decoder before it reaches this pipeline (stb
 * always returns direct color), so there is no palette layout here. */
typedef enum tp_c0_raster_layout {
    TP_C0_RASTER_GRAY8 = 1,
    TP_C0_RASTER_GRAYA8 = 2,
    TP_C0_RASTER_RGB8 = 3,
    TP_C0_RASTER_RGBA8 = 4
} tp_c0_raster_layout;

/* Recognized container, by leading magic bytes. */
typedef enum tp_c0_raster_container {
    TP_C0_CONTAINER_UNKNOWN = 0,
    TP_C0_CONTAINER_PNG,
    TP_C0_CONTAINER_JPEG,
    TP_C0_CONTAINER_WEBP
} tp_c0_raster_container;

tp_c0_raster_container tp_c0_raster_probe_container(const uint8_t *data, size_t len);

/* PINNED 16-bit -> 8-bit reduction (decision 0005 fixed rounding rule).
 *
 *     reduced = (v * 255 + 32767) / 65535     (round to nearest)
 *
 * Ties are impossible over 0..65535 (v*255/65535 is never an exact half), so
 * this is exact round-to-nearest. Differs from stb's stbi_load `v >> 8`
 * truncation (e.g. v=0x01FF -> 2 here, 1 under truncation): the production
 * decoder MUST reduce through this rule (decode 16-bit via stbi_load_16, not
 * stbi_load) or the semantic image hash changes. */
uint8_t tp_c0_raster_reduce16(uint16_t v);

/* Channel expansion + straight-alpha A=255 for no-alpha inputs. samples is
 * width*height*layout bytes; out_rgba is width*height*4 bytes (caller-owned). */
tp_c0_detail tp_c0_raster_expand8(const uint8_t *samples, uint32_t width, uint32_t height, tp_c0_raster_layout layout, uint8_t *out_rgba, tp_error *err);

/* 16-bit expansion: samples is width*height*layout uint16 values (host order,
 * as the decoder returns them), reduced per tp_c0_raster_reduce16 then expanded
 * to RGBA8. out_rgba is width*height*4 bytes (caller-owned). */
tp_c0_detail tp_c0_raster_expand16(const uint16_t *samples, uint32_t width, uint32_t height, tp_c0_raster_layout layout, uint8_t *out_rgba, tp_error *err);

/* EXIF orientation as a pure pixel transform so the result is
 * "orientation already applied". orientation in 1..8 is the 8 EXIF values;
 * 5..8 swap width/height. Any other value is treated as identity (1) and adds
 * the exif_orientation_unknown notice (pinned policy: never fail decode on a
 * stray tag). Allocates out_img->rgba (caller frees via tp_c0_image_free). */
tp_c0_detail tp_c0_raster_apply_orientation(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t orientation, tp_c0_image *out_img, tp_c0_raster_notices *notices, tp_error *err);

/* ICC policy (decision 0005): profiles are never applied; pixels never change.
 * present+valid -> icc_ignored notice; present+invalid -> icc_profile_bad
 * notice; absent -> no notice. */
void tp_c0_raster_note_icc(bool present, bool valid, tp_c0_raster_notices *notices);

/* Parse the EXIF orientation tag (0x0112) out of a JPEG APP1/Exif segment --
 * stb never does this. Returns true and writes the raw tag value to
 * *out_orientation when found (caller range-checks 1..8); false when the file
 * has no EXIF orientation. Bounds-checked against len, both TIFF byte orders. */
bool tp_c0_raster_exif_orientation(const uint8_t *jpeg, size_t len, uint32_t *out_orientation);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_RASTER_H */
