#include "tp_core/tp_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vendored builder-only writer, linked directly into tp_core (NOT via
 * nt_builder -- no basisu, no static-CRT pin leaks; #282). */
#include "stb_image_write.h"

#define TP_PNG_PATH_MAX 1024

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
    for (int p = 0; p < result->page_count; p++) {
        const tp_page *pg = &result->pages[p];
        if (!pg->rgba || pg->w <= 0 || pg->h <= 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_write_pages: page %d has no pixels", p);
        }
        char path[TP_PNG_PATH_MAX];
        int nn = snprintf(path, sizeof path, "%s-%d.png", out_path_base, p);
        if (nn < 0 || (size_t)nn >= sizeof path) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_export_write_pages: page path too long");
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
        int ok = stbi_write_png(path, pg->w, pg->h, 4, pixels, pg->w * 4);
        free(tmp);
        if (!ok) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_export_write_pages: failed writing '%s'", path);
        }
    }
    return TP_STATUS_OK;
}
