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
        case TP_C0_ERR_OP_UNKNOWN: return "op_unknown";
        case TP_C0_ERR_BAD_JSON: return "bad_json";
        case TP_C0_ERR_TXN_BAD_VERSION: return "txn_bad_version";
        case TP_C0_ERR_TXN_BAD_ID: return "txn_bad_id";
        case TP_C0_ERR_TXN_DUPLICATE_ID: return "txn_duplicate_id";
        case TP_C0_ERR_TXN_MISSING_FIELD: return "txn_missing_field";
        case TP_C0_ERR_TXN_BAD_TYPE: return "txn_bad_type";
        case TP_C0_ERR_UNKNOWN_FIELD: return "unknown_field";
        case TP_C0_ERR_SELECTOR_AMBIGUOUS: return "selector_ambiguous";
        case TP_C0_ERR_SELECTOR_UNRESOLVED: return "selector_unresolved";
        case TP_C0_ERR_REVISION_CONFLICT: return "revision_conflict";
        case TP_C0_ERR_INVALID_REVISION: return "invalid_revision";
        case TP_C0_ERR_JOURNAL_SHORT: return "journal_short";
        case TP_C0_ERR_JOURNAL_BAD_MAGIC: return "journal_bad_magic";
        case TP_C0_ERR_JOURNAL_BAD_VERSION: return "journal_bad_version";
        case TP_C0_ERR_JOURNAL_BAD_KIND: return "journal_bad_kind";
        case TP_C0_ERR_JOURNAL_BAD_CHECKSUM: return "journal_bad_checksum";
        case TP_C0_ERR_JOURNAL_TOO_LARGE: return "journal_too_large";
        case TP_C0_ERR_JOURNAL_RETENTION_FULL: return "journal_retention_full";
        case TP_C0_ERR_DECODE_FAILED: return "decode_failed";
        case TP_C0_ERR_FORMAT_UNSUPPORTED: return "format_unsupported";
        case TP_C0_NOTE_ICC_IGNORED: return "icc_ignored";
        case TP_C0_NOTE_ICC_PROFILE_BAD: return "icc_profile_bad";
        case TP_C0_NOTE_EXIF_ORIENTATION_UNKNOWN: return "exif_orientation_unknown";
        case TP_C0_DETAIL_COUNT: return ""; /* sentinel, not a real code; keeps -Wswitch happy */
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
