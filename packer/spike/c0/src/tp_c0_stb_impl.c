/* The single stb_image translation unit for the C0-04 spike. Compiled -w and
 * without sanitizers (see tp_c0_stb.h); STBI_NO_SIMD forces the deterministic
 * scalar integer decode path on every ISA. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_NO_STDIO       /* memory decode only; no fopen surface */
#define STBI_NO_LINEAR      /* no float HDR->linear path */
#define STBI_NO_HDR         /* no float output paths at all */
#include "stb_image.h"

#include "tp_c0/tp_c0_stb.h"

tp_c0_detail tp_c0_stb_decode_rgba8(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h, uint8_t **out_rgba, tp_error *err) {
    if (!data || !out_w || !out_h || !out_rgba) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "stb_decode_rgba8: null arg");
    }
    *out_rgba = NULL;
    if (len == 0 || len > (size_t)0x7fffffff) {
        return tp_c0_fail(err, TP_C0_ERR_DECODE_FAILED, "stb_decode_rgba8: bad length");
    }
    int w = 0;
    int h = 0;
    int ch = 0;
    stbi_uc *px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) {
        stbi_image_free(px);
        return tp_c0_fail(err, TP_C0_ERR_DECODE_FAILED, "stb_decode_rgba8: %s", stbi_failure_reason() ? stbi_failure_reason() : "decode failed");
    }
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    *out_rgba = (uint8_t *)px;
    return TP_C0_OK;
}

tp_c0_detail tp_c0_stb_decode_u16(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h, int *out_channels, uint16_t **out_samples, tp_error *err) {
    if (!data || !out_w || !out_h || !out_channels || !out_samples) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "stb_decode_u16: null arg");
    }
    *out_samples = NULL;
    if (len == 0 || len > (size_t)0x7fffffff) {
        return tp_c0_fail(err, TP_C0_ERR_DECODE_FAILED, "stb_decode_u16: bad length");
    }
    int w = 0;
    int h = 0;
    int ch = 0;
    stbi_us *px = stbi_load_16_from_memory(data, (int)len, &w, &h, &ch, 0);
    if (!px || w <= 0 || h <= 0 || ch < 1 || ch > 4) {
        stbi_image_free(px);
        return tp_c0_fail(err, TP_C0_ERR_DECODE_FAILED, "stb_decode_u16: %s", stbi_failure_reason() ? stbi_failure_reason() : "decode failed");
    }
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    *out_channels = ch;
    *out_samples = (uint16_t *)px;
    return TP_C0_OK;
}

void tp_c0_stb_free(void *pixels) {
    stbi_image_free(pixels);
}
