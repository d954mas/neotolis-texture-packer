#ifndef TP_C0_EXIF_FIXTURE_H
#define TP_C0_EXIF_FIXTURE_H

/* Shared little-endian APP1/Exif TIFF template used by the EXIF-orientation
 * test fixtures in test_c0_raster.c (pure parser vectors) and
 * test_c0_raster_decode.c (spliced onto a real JPEG). Layout: APP1 marker +
 * segment length (34) + "Exif\0\0" + TIFF header (byte order II, IFD0 @
 * offset 8) + one IFD entry (tag 0x0112 ORIENTATION, type SHORT, count 1) +
 * next-IFD = 0. TP_C0_EXIF_APP1_LE_ORIENT_OFFSET is the only byte callers
 * need to patch (the low byte of the LE SHORT orientation value). */

#include <stdint.h>
#include <string.h>

#define TP_C0_EXIF_APP1_LE_LEN 36
#define TP_C0_EXIF_APP1_LE_ORIENT_OFFSET 28

static const uint8_t tp_c0_exif_app1_le_template[TP_C0_EXIF_APP1_LE_LEN] = {
    0xFF, 0xE1, 0x00, 0x22,                         /* APP1, len 34 */
    'E',  'x',  'i',  'f',  0x00, 0x00,             /* Exif\0\0 */
    'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, /* TIFF LE, IFD0 @ 8 */
    0x01, 0x00,                                     /* count 1 */
    0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, /* tag 0x0112 SHORT count 1 */
    0x00, 0x00, 0x00, 0x00,                         /* value (patch byte 28) */
    0x00, 0x00, 0x00, 0x00};                        /* next IFD */

/* Copy the template into out[TP_C0_EXIF_APP1_LE_LEN], patching the
 * orientation value. */
static inline void tp_c0_exif_app1_le_build(uint8_t out[TP_C0_EXIF_APP1_LE_LEN], uint8_t orientation) {
    memcpy(out, tp_c0_exif_app1_le_template, TP_C0_EXIF_APP1_LE_LEN);
    out[TP_C0_EXIF_APP1_LE_ORIENT_OFFSET] = orientation;
}

#endif /* TP_C0_EXIF_FIXTURE_H */
