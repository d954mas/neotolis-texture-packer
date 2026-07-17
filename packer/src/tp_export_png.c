#include "tp_core/tp_export.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_fs_internal.h"

/* Vendored builder-only writer, linked directly into tp_core (NOT via
 * nt_builder -- no basisu, no static-CRT pin leaks; #282). */
#include "stb_image_write.h"

/* stb exposes this implementation symbol but omits it from the public
 * declaration block. The vendored implementation uses the default malloc/free
 * hooks (stb_image_write_impl.c). */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels, int stride_bytes, int x, int y, int n,
                                            int *out_len);

/* Premultiplies straight-alpha RGBA8 into `dst` (w*h*4). Rounds to nearest. */
static void premultiply_rgba(const uint8_t *src, uint8_t *dst, int w, int h) {
    size_t px = (size_t)w * (size_t)h;
    for (size_t i = 0; i < px; i++) {
        uint32_t a = src[i * 4 + 3];
        dst[i * 4 + 0] = (uint8_t)((src[i * 4 + 0] * a + 127U) / 255U);
        dst[i * 4 + 1] = (uint8_t)((src[i * 4 + 1] * a + 127U) / 255U);
        dst[i * 4 + 2] = (uint8_t)((src[i * 4 + 2] * a + 127U) / 255U);
        dst[i * 4 + 3] = (uint8_t)a;
    }
}

tp_status tp_export_write_pages(const tp_result *result, const char *out_path_base, bool premultiply, tp_error *err) {
    if (!result || !out_path_base) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_write_pages: NULL result/out_path_base");
    }
    /* Validate the complete output set before encoding or writing page zero. */
    for (int p = 0; p < result->page_count; p++) {
        char path[TP_IDENTITY_PATH_MAX];
        tp_status st = tp_export_page_path(out_path_base, p, path, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
    }
    for (int p = 0; p < result->page_count; p++) {
        const tp_page *pg = &result->pages[p];
        if (!pg->rgba || pg->w <= 0 || pg->h <= 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_write_pages: page %d has no pixels", p);
        }
        if (pg->w > INT_MAX / 4 || (size_t)pg->w > SIZE_MAX / (size_t)pg->h ||
            (size_t)pg->w * (size_t)pg->h > SIZE_MAX / 4U) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_export_write_pages: page %d dimensions overflow", p);
        }
        char path[TP_IDENTITY_PATH_MAX];
        tp_status st = tp_export_page_path(out_path_base, p, path, err);
        if (st != TP_STATUS_OK) {
            return st;
        }

        const uint8_t *pixels = pg->rgba;
        uint8_t *tmp = NULL;
        if (premultiply) {
            tmp = (uint8_t *)malloc((size_t)pg->w * (size_t)pg->h * 4U);
            if (!tmp) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_export_write_pages: OOM premultiplying page %d", p);
            }
            premultiply_rgba(pg->rgba, tmp, pg->w, pg->h);
            pixels = tmp;
        }
        int png_size = 0;
        unsigned char *png = stbi_write_png_to_mem(pixels, pg->w * 4, pg->w, pg->h, 4, &png_size);
        free(tmp);
        if (!png || png_size <= 0) {
            free(png);
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_export_write_pages: failed encoding page %d", p);
        }
        bool wrote = tp_fs_write_file(path, png, (size_t)png_size);
        free(png);
        if (!wrote) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_export_write_pages: failed writing '%s'", path);
        }
    }
    return TP_STATUS_OK;
}
