#ifndef TP_C0_STB_H
#define TP_C0_STB_H

/*
 * C0-04 decoder glue -- the ONLY place stb_image is compiled for the spike.
 *
 * Isolated in its own TU/target for two reasons the contract note pins:
 *   1. Determinism: built with STBI_NO_SIMD so the scalar integer JPEG path is
 *      used on every ISA. stb's SSE2 (x86) and NEON (arm) IDCT kernels are not
 *      guaranteed bit-identical, and NEON is not even auto-enabled -- so without
 *      this a JPEG golden could differ between x86 and arm64 CI.
 *   2. Sanitizers/warnings: vendored third-party code is compiled -w and without
 *      ASan/UBSan (matching the engine's stb_image target), so stb internals
 *      never trip the spike's -Werror or CI's -fno-sanitize-recover=all. The
 *      pure tp_c0_raster pipeline keeps full warnings + sanitizers.
 *
 * No stb types leak across this header. Returned pixels are freed with
 * tp_c0_stb_free.
 */

#include "tp_c0/tp_c0_error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode any stb-supported format to RGBA8 (stb applies channel expansion and
 * A=255 for no-alpha inputs). On success returns TP_C0_OK and sets *out_rgba
 * (width*height*4). On a truncated/garbage/unsupported stream returns
 * TP_C0_ERR_DECODE_FAILED and leaves *out_rgba NULL. */
tp_c0_detail tp_c0_stb_decode_rgba8(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h, uint8_t **out_rgba, tp_error *err);

/* Decode to native-channel 16-bit samples (stbi_load_16), preserving 16-bit
 * precision so the caller applies the pinned tp_c0_raster_reduce16 rule instead
 * of stb's `>> 8` truncation. *out_channels is 1/2/3/4. */
tp_c0_detail tp_c0_stb_decode_u16(const uint8_t *data, size_t len, uint32_t *out_w, uint32_t *out_h, int *out_channels, uint16_t **out_samples, tp_error *err);

void tp_c0_stb_free(void *pixels);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_STB_H */
