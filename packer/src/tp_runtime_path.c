#include "tp_core/tp_format.h"

#include <stdio.h>
#include <string.h>

tp_status tp_format_root_from_executable(char *out, size_t out_cap,
                                         tp_error *error) {
    if (!out || out_cap == 0U) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format root requires a non-empty output buffer");
    }
    out[0] = '\0';
    char directory[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
    tp_status status =
        tp_executable_directory(directory, sizeof directory, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const size_t length = strlen(directory);
    const char *separator =
        length > 0U && directory[length - 1U] == '/' ? "" : "/";
    const int written =
        snprintf(out, out_cap, "%s%sformats", directory, separator);
    if (written < 0 || (size_t)written >= out_cap) {
        out[0] = '\0';
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "executable-relative format root exceeds output capacity");
    }
    return TP_STATUS_OK;
}
