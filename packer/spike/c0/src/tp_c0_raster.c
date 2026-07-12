#include "tp_c0/tp_c0_raster.h"

#include <stdlib.h>
#include <string.h>

/* All math here is integer and fixed-width: the canonical bytes must be
 * byte-identical on Linux/macOS/Windows CI (no libm, no float, no host-endian,
 * no long-width). */

void tp_c0_image_free(tp_c0_image *img) {
    if (!img) {
        return;
    }
    free(img->rgba);
    img->rgba = NULL;
    img->width = 0;
    img->height = 0;
}

void tp_c0_raster_notices_reset(tp_c0_raster_notices *n) {
    if (n) {
        n->count = 0;
    }
}

void tp_c0_raster_notices_add(tp_c0_raster_notices *n, tp_c0_detail token) {
    if (!n || n->count >= TP_C0_RASTER_NOTICES_CAP) {
        return; /* cap is a spike bound, not corruption: silently saturate */
    }
    n->tokens[n->count++] = token;
}

bool tp_c0_raster_notices_has(const tp_c0_raster_notices *n, tp_c0_detail token) {
    if (!n) {
        return false;
    }
    for (unsigned i = 0; i < n->count; i++) {
        if (n->tokens[i] == token) {
            return true;
        }
    }
    return false;
}

tp_c0_raster_container tp_c0_raster_probe_container(const uint8_t *data, size_t len) {
    if (!data) {
        return TP_C0_CONTAINER_UNKNOWN;
    }
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (len >= 8 && memcmp(data, png_sig, 8) == 0) {
        return TP_C0_CONTAINER_PNG;
    }
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return TP_C0_CONTAINER_JPEG;
    }
    /* RIFF....WEBP */
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        return TP_C0_CONTAINER_WEBP;
    }
    return TP_C0_CONTAINER_UNKNOWN;
}

uint8_t tp_c0_raster_reduce16(uint16_t v) {
    /* Round to nearest over the 0..65535 domain; ties never occur (proven in
     * the header note), so no rounding-direction ambiguity. Unsigned 32-bit
     * math: 65535*255 + 32767 = 16744192 < 2^32, no overflow. */
    uint32_t num = (uint32_t)v * 255u + 32767u;
    return (uint8_t)(num / 65535u);
}

/* Expand one already-8-bit pixel of `layout` channels at src -> RGBA at dst. */
static void expand8_pixel(const uint8_t *src, tp_c0_raster_layout layout, uint8_t *dst) {
    switch (layout) {
        case TP_C0_RASTER_GRAY8:
            dst[0] = src[0];
            dst[1] = src[0];
            dst[2] = src[0];
            dst[3] = 255;
            break;
        case TP_C0_RASTER_GRAYA8:
            dst[0] = src[0];
            dst[1] = src[0];
            dst[2] = src[0];
            dst[3] = src[1];
            break;
        case TP_C0_RASTER_RGB8:
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
            break;
        case TP_C0_RASTER_RGBA8:
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
            break;
    }
}

static bool layout_valid(tp_c0_raster_layout layout) {
    return layout == TP_C0_RASTER_GRAY8 || layout == TP_C0_RASTER_GRAYA8 || layout == TP_C0_RASTER_RGB8 || layout == TP_C0_RASTER_RGBA8;
}

tp_c0_detail tp_c0_raster_expand8(const uint8_t *samples, uint32_t width, uint32_t height, tp_c0_raster_layout layout, uint8_t *out_rgba, tp_error *err) {
    if (!samples || !out_rgba) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "expand8: null buffer");
    }
    if (!layout_valid(layout)) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "expand8: bad layout %d", (int)layout);
    }
    if (width == 0 || height == 0) {
        return tp_c0_fail(err, TP_C0_ERR_EMPTY, "expand8: zero dimension");
    }
    size_t nch = (size_t)layout;
    size_t count = (size_t)width * (size_t)height;
    for (size_t i = 0; i < count; i++) {
        expand8_pixel(samples + i * nch, layout, out_rgba + i * 4);
    }
    return TP_C0_OK;
}

tp_c0_detail tp_c0_raster_expand16(const uint16_t *samples, uint32_t width, uint32_t height, tp_c0_raster_layout layout, uint8_t *out_rgba, tp_error *err) {
    if (!samples || !out_rgba) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "expand16: null buffer");
    }
    if (!layout_valid(layout)) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "expand16: bad layout %d", (int)layout);
    }
    if (width == 0 || height == 0) {
        return tp_c0_fail(err, TP_C0_ERR_EMPTY, "expand16: zero dimension");
    }
    size_t nch = (size_t)layout;
    size_t count = (size_t)width * (size_t)height;
    for (size_t i = 0; i < count; i++) {
        uint8_t reduced[4];
        for (size_t c = 0; c < nch; c++) {
            reduced[c] = tp_c0_raster_reduce16(samples[i * nch + c]);
        }
        expand8_pixel(reduced, layout, out_rgba + i * 4);
    }
    return TP_C0_OK;
}

/* Map output pixel (x,y) back to source column a / row b for EXIF orientation o
 * (1..8). w,h are SOURCE dims. For o in 5..8 the output dims are (h,w). */
static void orient_src(uint32_t o, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t *a, uint32_t *b) {
    switch (o) {
        case 2: /* mirror horizontal */
            *a = w - 1 - x;
            *b = y;
            break;
        case 3: /* rotate 180 */
            *a = w - 1 - x;
            *b = h - 1 - y;
            break;
        case 4: /* mirror vertical */
            *a = x;
            *b = h - 1 - y;
            break;
        case 5: /* transpose (main diagonal) */
            *a = y;
            *b = x;
            break;
        case 6: /* rotate 90 CW */
            *a = y;
            *b = h - 1 - x;
            break;
        case 7: /* transverse (anti-diagonal) */
            *a = w - 1 - y;
            *b = h - 1 - x;
            break;
        case 8: /* rotate 90 CCW */
            *a = w - 1 - y;
            *b = x;
            break;
        default: /* 1 and unknown -> identity */
            *a = x;
            *b = y;
            break;
    }
}

tp_c0_detail tp_c0_raster_apply_orientation(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t orientation, tp_c0_image *out_img, tp_c0_raster_notices *notices, tp_error *err) {
    if (!rgba || !out_img) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "orientation: null arg");
    }
    if (width == 0 || height == 0) {
        return tp_c0_fail(err, TP_C0_ERR_EMPTY, "orientation: zero dimension");
    }

    uint32_t o = orientation;
    if (o < 1 || o > 8) {
        tp_c0_raster_notices_add(notices, TP_C0_NOTE_EXIF_ORIENTATION_UNKNOWN);
        o = 1; /* pinned policy: unknown tag -> identity, decode still succeeds */
    }

    bool swap = (o >= 5 && o <= 8);
    uint32_t ow = swap ? height : width;
    uint32_t oh = swap ? width : height;

    size_t bytes = (size_t)ow * (size_t)oh * 4u;
    uint8_t *out = (uint8_t *)malloc(bytes);
    if (!out) {
        return tp_c0_fail(err, TP_C0_ERR_OOM, "orientation: alloc %zu", bytes);
    }

    for (uint32_t y = 0; y < oh; y++) {
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t a;
            uint32_t b;
            orient_src(o, x, y, width, height, &a, &b);
            const uint8_t *sp = rgba + ((size_t)b * width + a) * 4;
            uint8_t *dp = out + ((size_t)y * ow + x) * 4;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }

    out_img->width = ow;
    out_img->height = oh;
    out_img->rgba = out;
    return TP_C0_OK;
}

void tp_c0_raster_note_icc(bool present, bool valid, tp_c0_raster_notices *notices) {
    if (!present) {
        return;
    }
    tp_c0_raster_notices_add(notices, valid ? TP_C0_NOTE_ICC_IGNORED : TP_C0_NOTE_ICC_PROFILE_BAD);
}

/* Read a 16-bit value from p with the given TIFF endianness (true == little). */
static uint16_t rd16(const uint8_t *p, bool le) {
    return le ? (uint16_t)(p[0] | ((uint16_t)p[1] << 8)) : (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p, bool le) {
    return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
              : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

/* Parse the TIFF block inside an Exif APP1 payload for tag 0x0112. tiff points
 * at the TIFF header ("II"/"MM"), avail is the bytes remaining from there. */
static bool exif_tiff_orientation(const uint8_t *tiff, size_t avail, uint32_t *out) {
    if (avail < 8) {
        return false;
    }
    bool le;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
        le = true;
    } else if (tiff[0] == 'M' && tiff[1] == 'M') {
        le = false;
    } else {
        return false;
    }
    if (rd16(tiff + 2, le) != 42) {
        return false;
    }
    uint32_t ifd = rd32(tiff + 4, le);
    if (ifd > avail || avail - ifd < 2) {
        return false;
    }
    uint16_t n = rd16(tiff + ifd, le);
    size_t entry = (size_t)ifd + 2;
    for (uint16_t i = 0; i < n; i++) {
        if (entry + 12 > avail) {
            return false;
        }
        const uint8_t *e = tiff + entry;
        uint16_t tag = rd16(e, le);
        if (tag == 0x0112) {
            uint16_t type = rd16(e + 2, le);
            if (type == 3) { /* SHORT: value in the first 2 bytes of the value field */
                *out = rd16(e + 8, le);
                return true;
            }
            if (type == 4) { /* LONG (rare for orientation, accept defensively) */
                *out = rd32(e + 8, le);
                return true;
            }
            return false;
        }
        entry += 12;
    }
    return false;
}

bool tp_c0_raster_exif_orientation(const uint8_t *jpeg, size_t len, uint32_t *out_orientation) {
    if (!jpeg || !out_orientation || len < 4) {
        return false;
    }
    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false; /* not a JPEG SOI */
    }
    size_t p = 2;
    while (p + 4 <= len) {
        if (jpeg[p] != 0xFF) {
            return false; /* desync: not at a marker */
        }
        uint8_t marker = jpeg[p + 1];
        if (marker == 0xD9 || marker == 0xDA) {
            return false; /* EOI or start of scan: no more headers */
        }
        uint16_t seg_len = (uint16_t)(((uint16_t)jpeg[p + 2] << 8) | jpeg[p + 3]);
        if (seg_len < 2 || p + 2 + seg_len > len) {
            return false;
        }
        const uint8_t *payload = jpeg + p + 4;
        size_t payload_len = (size_t)seg_len - 2;
        if (marker == 0xE1 && payload_len >= 6 && memcmp(payload, "Exif\0\0", 6) == 0) {
            return exif_tiff_orientation(payload + 6, payload_len - 6, out_orientation);
        }
        p += (size_t)2 + seg_len;
    }
    return false;
}
