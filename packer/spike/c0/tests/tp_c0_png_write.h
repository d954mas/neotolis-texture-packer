#ifndef TP_C0_PNG_WRITE_H
#define TP_C0_PNG_WRITE_H

/*
 * Minimal, fully deterministic PNG encoder for C0-04 golden fixtures.
 *
 * stb_image_write cannot emit palette (color type 3) or 16-bit PNGs, and those
 * are exactly the decode paths this spike must pin (palette expansion, the
 * 16->8 reduction rule). This builds them from readable pixel arrays using
 * stored (uncompressed) zlib blocks -- no compression heuristics, no external
 * tool -- so the fixture bytes are reproducible by anyone and stb decodes them
 * through its real path. Test-support only; compiled with the spike's full
 * -Werror, so it is written conversion-clean.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t tp_png_crc(const uint8_t *buf, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= buf[i];
        for (int k = 0; k < 8; k++) {
            if (c & 1u) {
                c = (c >> 1) ^ 0xEDB88320u;
            } else {
                c >>= 1;
            }
        }
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t tp_png_adler(const uint8_t *d, size_t n) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + d[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

typedef struct {
    uint8_t *p;
    size_t len;
} tp_png_buf;

static void tp_png_u8(tp_png_buf *b, uint8_t v) { b->p[b->len++] = v; }

static void tp_png_be32(tp_png_buf *b, uint32_t v) {
    tp_png_u8(b, (uint8_t)(v >> 24));
    tp_png_u8(b, (uint8_t)(v >> 16));
    tp_png_u8(b, (uint8_t)(v >> 8));
    tp_png_u8(b, (uint8_t)v);
}

static void tp_png_bytes(tp_png_buf *b, const uint8_t *s, size_t n) {
    if (n) {
        memcpy(b->p + b->len, s, n);
        b->len += n;
    }
}

static void tp_png_chunk(tp_png_buf *b, const char type[4], const uint8_t *data, size_t dlen) {
    tp_png_be32(b, (uint32_t)dlen);
    size_t crc_start = b->len;
    tp_png_bytes(b, (const uint8_t *)type, 4);
    tp_png_bytes(b, data, dlen);
    tp_png_be32(b, tp_png_crc(b->p + crc_start, 4 + dlen));
}

static uint32_t tp_png_channels(int color_type) {
    switch (color_type) {
        case 0: return 1; /* grayscale */
        case 2: return 3; /* RGB */
        case 3: return 1; /* palette index */
        case 6: return 4; /* RGBA */
        default: return 0;
    }
}

/* Build a PNG. depth is 8 or 16. samples8 (depth 8) is width*height*channels
 * bytes; samples16 (depth 16) is width*height*channels uint16 (written
 * big-endian per the PNG spec). palette/trns used only for color type 3.
 * Caller frees the returned buffer. */
static uint8_t *tp_png_build(uint32_t w, uint32_t h, int color_type, int depth, const uint8_t *samples8, const uint16_t *samples16, const uint8_t *palette, uint32_t pal_count, const uint8_t *trns,
                             uint32_t trns_count, size_t *out_len) {
    uint32_t ch = tp_png_channels(color_type);
    uint32_t bytes_per_sample = (depth == 16) ? 2u : 1u;
    size_t row_bytes = (size_t)w * ch * bytes_per_sample;
    size_t raw_len = (size_t)h * (1 + row_bytes);

    uint8_t *raw = (uint8_t *)malloc(raw_len ? raw_len : 1);
    size_t rp = 0;
    for (uint32_t y = 0; y < h; y++) {
        raw[rp++] = 0; /* filter type 0 (none) */
        for (uint32_t x = 0; x < w; x++) {
            for (uint32_t c = 0; c < ch; c++) {
                size_t idx = ((size_t)y * w + x) * ch + c;
                if (depth == 16) {
                    uint16_t s = samples16[idx];
                    raw[rp++] = (uint8_t)(s >> 8);
                    raw[rp++] = (uint8_t)(s & 0xFFu);
                } else {
                    raw[rp++] = samples8[idx];
                }
            }
        }
    }

    /* zlib stream: header + stored deflate blocks + adler32. */
    size_t idat_cap = raw_len + (raw_len / 65535 + 1) * 5 + 16;
    uint8_t *idat = (uint8_t *)malloc(idat_cap);
    tp_png_buf zb = {idat, 0};
    tp_png_u8(&zb, 0x78);
    tp_png_u8(&zb, 0x01);
    size_t pos = 0;
    do {
        size_t blk = raw_len - pos;
        if (blk > 65535) {
            blk = 65535;
        }
        uint8_t final = (pos + blk >= raw_len) ? 1u : 0u;
        tp_png_u8(&zb, final);
        tp_png_u8(&zb, (uint8_t)(blk & 0xFFu));
        tp_png_u8(&zb, (uint8_t)((blk >> 8) & 0xFFu));
        uint16_t nlen = (uint16_t)~(uint16_t)blk;
        tp_png_u8(&zb, (uint8_t)(nlen & 0xFFu));
        tp_png_u8(&zb, (uint8_t)((nlen >> 8) & 0xFFu));
        tp_png_bytes(&zb, raw + pos, blk);
        pos += blk;
    } while (pos < raw_len);
    tp_png_be32(&zb, tp_png_adler(raw, raw_len));

    /* Assemble the PNG. */
    size_t cap = zb.len + (size_t)pal_count * 3 + trns_count + 256;
    uint8_t *png = (uint8_t *)malloc(cap);
    tp_png_buf b = {png, 0};
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    tp_png_bytes(&b, sig, 8);

    uint8_t ihdr[13];
    tp_png_buf ib = {ihdr, 0};
    tp_png_be32(&ib, w);
    tp_png_be32(&ib, h);
    tp_png_u8(&ib, (uint8_t)depth);
    tp_png_u8(&ib, (uint8_t)color_type);
    tp_png_u8(&ib, 0); /* compression */
    tp_png_u8(&ib, 0); /* filter */
    tp_png_u8(&ib, 0); /* interlace */
    tp_png_chunk(&b, "IHDR", ihdr, 13);

    if (color_type == 3 && palette) {
        tp_png_chunk(&b, "PLTE", palette, (size_t)pal_count * 3);
        if (trns && trns_count) {
            tp_png_chunk(&b, "tRNS", trns, trns_count);
        }
    }
    tp_png_chunk(&b, "IDAT", idat, zb.len);
    tp_png_chunk(&b, "IEND", NULL, 0);

    free(raw);
    free(idat);
    *out_len = b.len;
    return png;
}

/* Insert an ancillary chunk right after IHDR (offset 8 sig + 25 IHDR = 33).
 * Used to add an iCCP profile to a base PNG for the ICC-ignored test. Caller
 * frees the returned buffer. */
static uint8_t *tp_png_insert_after_ihdr(const uint8_t *png, size_t png_len, const char type[4], const uint8_t *data, size_t dlen, size_t *out_len) {
    const size_t ins_at = 33;
    size_t chunk_len = 12 + dlen; /* len + type + data + crc */
    uint8_t *out = (uint8_t *)malloc(png_len + chunk_len);
    memcpy(out, png, ins_at);
    tp_png_buf b = {out, ins_at};
    tp_png_chunk(&b, type, data, dlen);
    memcpy(out + b.len, png + ins_at, png_len - ins_at);
    *out_len = b.len + (png_len - ins_at);
    return out;
}

#endif /* TP_C0_PNG_WRITE_H */
