#include "tp_c0/tp_c0_error.h"

#include <stdarg.h>
#include <stdio.h>

/* No `default` inside the switch on purpose: a new enum value then trips
 * -Wswitch here, keeping the token table in lockstep with the enum. The trailing
 * return handles out-of-range casts. */
const char *tp_c0_detail_id(tp_c0_detail d) {
    switch (d) {
        case TP_C0_OK: return "ok";
        case TP_C0_ERR_NULL_ARG: return "null_arg";
        case TP_C0_ERR_EMPTY: return "empty";
        case TP_C0_ERR_BUFFER_TOO_SMALL: return "buffer_too_small";
        case TP_C0_ERR_OOM: return "oom";
        case TP_C0_ERR_INVALID_UTF8: return "invalid_utf8";
        case TP_C0_ERR_ID_BAD_PREFIX: return "id_bad_prefix";
        case TP_C0_ERR_ID_BAD_HEX: return "id_bad_hex";
        case TP_C0_ERR_ID_BAD_LENGTH: return "id_bad_length";
        case TP_C0_ERR_ID_TRAILING: return "id_trailing";
        case TP_C0_ERR_ID_NIL: return "id_nil";
        case TP_C0_ERR_RNG_SHORT: return "rng_short";
        case TP_C0_ERR_RNG_FAIL: return "rng_fail";
        case TP_C0_ERR_PATH_NOT_ABSOLUTE: return "path_not_absolute";
        case TP_C0_ERR_PATH_DRIVE_REL: return "path_drive_relative";
        case TP_C0_ERR_PATH_BAD_UNC: return "path_bad_unc";
        case TP_C0_ERR_PATH_DEVICE: return "path_device";
        case TP_C0_ERR_KEY_ABSOLUTE: return "key_absolute";
        case TP_C0_ERR_KEY_TRAVERSAL: return "key_traversal";
        case TP_C0_ERR_COLLISION_EXHAUSTED: return "collision_exhausted";
    }
    return "unknown";
}

tp_c0_detail tp_c0_fail(tp_error *err, tp_c0_detail d, const char *fmt, ...) {
    if (err) {
        if (fmt) {
            va_list args;
            va_start(args, fmt);
            (void)vsnprintf(err->msg, sizeof(err->msg), fmt, args);
            va_end(args);
        } else {
            err->msg[0] = '\0';
        }
    }
    return d;
}
