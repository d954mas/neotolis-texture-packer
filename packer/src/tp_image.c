#include "tp_core/tp_image.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"
#include "tp_fs_internal.h"
#include "tp_image_priv.h"
#include "tp_utf8_internal.h"

/* Successful-decode counter for the tp_image__test_* seam (see header). Relaxed
 * atomics: a Pack worker thread increments while the test reads after joining it. */
static _Atomic uint64_t g_test_decode_count;

void tp_image__test_reset_decode_count(void) {
    atomic_store_explicit(&g_test_decode_count, UINT64_C(0), memory_order_relaxed);
}

uint64_t tp_image__test_decode_count(void) {
    return atomic_load_explicit(&g_test_decode_count, memory_order_relaxed);
}

static tp_status validate_utf8_path(const char *path, tp_error *err) {
    if (!path || path[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "image load: path is required");
    }
    return tp_utf8_validate_c_string(path, TP_STATUS_INVALID_UTF8,
                                     "image load path", err);
}

static tp_status validate_dimensions(int width, int height, size_t *bytes_out,
                                     tp_error *err) {
    if (width <= 0 || height <= 0) {
        return tp_error_set(err, TP_STATUS_UNSUPPORTED_TEXTURE,
                            "image load: decoded image has invalid dimensions %dx%d",
                            width, height);
    }
    if ((unsigned int)width > TP_IMAGE_MAX_DIMENSION ||
        (unsigned int)height > TP_IMAGE_MAX_DIMENSION) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "image load: dimension %dx%d exceeds %u",
                            width, height, TP_IMAGE_MAX_DIMENSION);
    }
    size_t w = (size_t)width;
    size_t h = (size_t)height;
    if (w > SIZE_MAX / h || w * h > SIZE_MAX / 4U) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "image load: decoded size overflows size_t");
    }
    size_t bytes = w * h * 4U;
    if (bytes > TP_IMAGE_MAX_RGBA_BYTES) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "image load: decoded size %zu exceeds %zu bytes",
                            bytes, TP_IMAGE_MAX_RGBA_BYTES);
    }
    *bytes_out = bytes;
    return TP_STATUS_OK;
}

void tp_image_free(tp_image_rgba8 *image) {
    if (!image) {
        return;
    }
    stbi_image_free(image->pixels);
    image->pixels = NULL;
    image->width = 0;
    image->height = 0;
}

tp_status tp_image_load_file(const char *path_utf8, tp_image_rgba8 *out,
                             tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "image load: out is required");
    }
    memset(out, 0, sizeof *out);

    tp_status status = validate_utf8_path(path_utf8, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    tp_fs_info info;
    if (!tp_fs_stat(path_utf8, &info)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "image load: cannot stat '%s': %s", path_utf8,
                            strerror(errno));
    }
    if (info.kind != TP_FS_KIND_REGULAR) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "image load: '%s' is not a regular file", path_utf8);
    }
    if (info.size == 0U) {
        return tp_error_set(err, TP_STATUS_UNSUPPORTED_TEXTURE,
                            "image load: '%s' is empty", path_utf8);
    }
    if (info.size > (uint64_t)TP_IMAGE_MAX_FILE_BYTES ||
        info.size > (uint64_t)INT_MAX || info.size > (uint64_t)SIZE_MAX) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "image load: file size %llu exceeds %zu bytes",
                            (unsigned long long)info.size,
                            TP_IMAGE_MAX_FILE_BYTES);
    }

    const size_t encoded_size = (size_t)info.size;
    unsigned char *encoded = (unsigned char *)malloc(encoded_size);
    if (!encoded) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "image load: cannot allocate %zu encoded bytes",
                            encoded_size);
    }

    FILE *file = tp_fs_fopen(path_utf8, "rb");
    if (!file) {
        free(encoded);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "image load: cannot open '%s': %s", path_utf8,
                            strerror(errno));
    }
    bool read_ok = tp_fs_read_all(file, encoded, encoded_size);
    int extra = read_ok ? fgetc(file) : EOF;
    bool stream_ok = read_ok && extra == EOF && !ferror(file);
    bool close_ok = tp_fs_close(file);
    if (!stream_ok || !close_ok) {
        free(encoded);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "image load: file changed or failed while reading '%s'",
                            path_utf8);
    }

    int expected_width = 0;
    int expected_height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(encoded, (int)encoded_size, &expected_width,
                               &expected_height, &channels)) {
        const char *reason = stbi_failure_reason();
        status = tp_error_set(err, TP_STATUS_UNSUPPORTED_TEXTURE,
                              "image load: cannot decode '%s': %s", path_utf8,
                              reason ? reason : "unsupported or corrupt image");
        free(encoded);
        return status;
    }

    size_t expected_bytes = 0U;
    status = validate_dimensions(expected_width, expected_height,
                                 &expected_bytes, err);
    if (status != TP_STATUS_OK) {
        free(encoded);
        return status;
    }

    int width = 0;
    int height = 0;
    unsigned char *pixels = stbi_load_from_memory(
        encoded, (int)encoded_size, &width, &height, &channels, 4);
    free(encoded);
    if (!pixels) {
        const char *reason = stbi_failure_reason();
        return tp_error_set(err, TP_STATUS_UNSUPPORTED_TEXTURE,
                            "image load: cannot decode '%s': %s", path_utf8,
                            reason ? reason : "unsupported or corrupt image");
    }

    size_t decoded_bytes = 0U;
    status = validate_dimensions(width, height, &decoded_bytes, err);
    if (status != TP_STATUS_OK || width != expected_width ||
        height != expected_height || decoded_bytes != expected_bytes) {
        stbi_image_free(pixels);
        if (status != TP_STATUS_OK) {
            return status;
        }
        return tp_error_set(err, TP_STATUS_UNSUPPORTED_TEXTURE,
                            "image load: dimensions changed during decode");
    }

    out->pixels = pixels;
    out->width = width;
    out->height = height;
    atomic_fetch_add_explicit(&g_test_decode_count, UINT64_C(1),
                              memory_order_relaxed);
    return TP_STATUS_OK;
}
