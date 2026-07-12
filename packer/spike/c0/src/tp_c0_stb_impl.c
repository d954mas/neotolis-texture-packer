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

#include <stddef.h>

/* Shared null-arg check: every entry in ptrs must be non-NULL (caller passes
 * one entry per required out/in pointer). One canonical null-arg error,
 * tagged with `who` so the two wrappers stay distinguishable. */
static tp_c0_detail stb_check_args(const void *const *ptrs, size_t n, const char *who, tp_error *err) {
    for (size_t i = 0; i < n; i++) {
        if (!ptrs[i]) {
            return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "%s: null arg", who);
        }
    }
    return TP_C0_OK;
}

/* Shared length guard: stb takes a plain int length, so len must be non-zero
 * and fit in a positive int. */
static tp_c0_detail stb_check_len(size_t len, const char *who, tp_error *err) {
    if (len == 0 || len > (size_t)0x7fffffff) {
        return tp_c0_fail(err, TP_C0_ERR_DECODE_FAILED, "%s: bad length", who);
    }
    return TP_C0_OK;
}

/* Shared stb-failure -> tp_c0_detail mapping. */
static tp_c0_detail stb_decode_failed(const char *who, tp_error *err) {
    return tp_c0_fail(err, TP_C0_ERR_DECODE_FAILED, "%s: %s", who, stbi_failure_reason() ? stbi_failure_reason() : "decode failed");
}

tp_c0_detail tp_c0_stb_decode_rgba8(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h, uint8_t **out_rgba, tp_error *err) {
    const void *args[] = {data, out_w, out_h, out_rgba};
    tp_c0_detail rc = stb_check_args(args, sizeof args / sizeof args[0], "stb_decode_rgba8", err);
    if (rc != TP_C0_OK) {
        return rc;
    }
    *out_rgba = NULL;
    rc = stb_check_len(len, "stb_decode_rgba8", err);
    if (rc != TP_C0_OK) {
        return rc;
    }
    int w = 0;
    int h = 0;
    int ch = 0;
    stbi_uc *px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) {
        stbi_image_free(px);
        return stb_decode_failed("stb_decode_rgba8", err);
    }
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    *out_rgba = (uint8_t *)px;
    return TP_C0_OK;
}

tp_c0_detail tp_c0_stb_decode_u16(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h, int *out_channels, uint16_t **out_samples, tp_error *err) {
    const void *args[] = {data, out_w, out_h, out_channels, out_samples};
    tp_c0_detail rc = stb_check_args(args, sizeof args / sizeof args[0], "stb_decode_u16", err);
    if (rc != TP_C0_OK) {
        return rc;
    }
    *out_samples = NULL;
    rc = stb_check_len(len, "stb_decode_u16", err);
    if (rc != TP_C0_OK) {
        return rc;
    }
    int w = 0;
    int h = 0;
    int ch = 0;
    stbi_us *px = stbi_load_16_from_memory(data, (int)len, &w, &h, &ch, 0);
    if (!px || w <= 0 || h <= 0 || ch < 1 || ch > 4) {
        stbi_image_free(px);
        return stb_decode_failed("stb_decode_u16", err);
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
