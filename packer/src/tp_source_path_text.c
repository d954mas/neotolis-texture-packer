#include "tp_source_path_text_internal.h"

#include <string.h>

#include "tp_utf8_internal.h"

tp_status tp_source_path_text_admit(const char *path) {
    if (!path || path[0] == '\0') {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    size_t length = 0U;
    while (length < (size_t)TP_SOURCE_PATH_TEXT_CAP && path[length] != '\0') {
        length++;
    }
    if (length == (size_t)TP_SOURCE_PATH_TEXT_CAP) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    return tp_utf8_validate_c_string(path, TP_STATUS_INVALID_UTF8,
                                     "source path", NULL);
}

tp_status tp_source_path_text_normalize(const char *path, char *out,
                                        size_t capacity) {
    if (!out || capacity == 0U) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    const tp_status status = tp_source_path_text_admit(path);
    if (status != TP_STATUS_OK) {
        out[0] = '\0';
        return status;
    }
    const size_t length = strlen(path);
    if (length >= capacity) {
        out[0] = '\0';
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    for (size_t i = 0U; i <= length; i++) {
        out[i] = path[i] == '\\' ? '/' : path[i];
    }
    return TP_STATUS_OK;
}

uint64_t tp_source_path_text_hash(const char *admitted_path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    if (!admitted_path) {
        return hash;
    }
    for (const unsigned char *p = (const unsigned char *)admitted_path; *p;
         p++) {
        const unsigned char byte =
            *p == (unsigned char)'\\' ? (unsigned char)'/' : *p;
        hash ^= (uint64_t)byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool tp_source_path_text_equal(const char *left, const char *right) {
    if (tp_source_path_text_admit(left) != TP_STATUS_OK ||
        tp_source_path_text_admit(right) != TP_STATUS_OK) {
        return false;
    }
    for (;;) {
        const char a = *left == '\\' ? '/' : *left;
        const char b = *right == '\\' ? '/' : *right;
        if (a != b || a == '\0') {
            return a == b;
        }
        left++;
        right++;
    }
}
