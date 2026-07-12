#ifndef TP_C0_ERROR_H
#define TP_C0_ERROR_H

/*
 * C0-01 contract-spike structured-error vocabulary.
 *
 * The spike primitives (id128, shape-ID, project-path, source-key, legacy-ID)
 * return a tp_c0_detail machine reason (0 == TP_C0_OK) and optionally fill a
 * tp_core tp_error buffer with human context -- structured error = id + context,
 * never an abort/crash on caller input (AGENTS.md invariant, master spec §59).
 *
 * tp_c0_detail_id() maps each reason to a stable lowercase machine token that a
 * CLI/MCP client matches on; the tokens are test-pinned so a rename is a visible
 * contract change, not a silent edit (mirrors tp_core tp_status_id()).
 *
 * These are spike codes: F1 promotes the surviving ones into the production
 * status vocabulary. Nothing here touches the on-disk project schema.
 */

#include "tp_core/tp_error.h" /* tp_error, header-only */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tp_c0_detail {
    TP_C0_OK = 0,

    /* generic */
    TP_C0_ERR_NULL_ARG,          /* required out/in pointer was NULL */
    TP_C0_ERR_EMPTY,             /* empty input where a value is required */
    TP_C0_ERR_BUFFER_TOO_SMALL,  /* caller buffer cannot hold the result */
    TP_C0_ERR_OOM,               /* allocation failed */
    TP_C0_ERR_INVALID_UTF8,      /* input is not well-formed UTF-8 */

    /* shape-ID parse */
    TP_C0_ERR_ID_BAD_PREFIX,     /* missing/unknown kind prefix */
    TP_C0_ERR_ID_BAD_HEX,        /* non-hex digit in the 128-bit body */
    TP_C0_ERR_ID_BAD_LENGTH,     /* body is not exactly 32 hex digits */
    TP_C0_ERR_ID_TRAILING,       /* trailing bytes after a complete ID */
    TP_C0_ERR_ID_NIL,            /* all-zero ID where a real ID is required */

    /* RNG seam */
    TP_C0_ERR_RNG_SHORT,         /* RNG returned fewer bytes than requested */
    TP_C0_ERR_RNG_FAIL,          /* RNG reported failure (negative return) */

    /* project-path canonicalization */
    TP_C0_ERR_PATH_NOT_ABSOLUTE, /* identity path must be absolute */
    TP_C0_ERR_PATH_DRIVE_REL,    /* Windows "C:foo" drive-relative form */
    TP_C0_ERR_PATH_BAD_UNC,      /* malformed UNC (//server without share) */
    TP_C0_ERR_PATH_DEVICE,       /* Windows \\?\ or \\.\ device/verbatim path */

    /* source-key normalization */
    TP_C0_ERR_KEY_ABSOLUTE,      /* source key must be source-root-relative */
    TP_C0_ERR_KEY_TRAVERSAL,     /* ".." component -- would escape source root */

    /* legacy-ID synthesis */
    TP_C0_ERR_COLLISION_EXHAUSTED, /* salt sweep could not disambiguate */

    /* ---- C0-02 operation / transaction contract (append-only) ---- */

    /* operation catalog */
    TP_C0_ERR_OP_UNKNOWN,        /* op wire name is not in the append-only catalog */

    /* transaction envelope / JSON schema */
    TP_C0_ERR_BAD_JSON,          /* input is not well-formed JSON (cJSON parse failed) */
    TP_C0_ERR_TXN_BAD_VERSION,   /* "schema" is absent/not-1 -- unknown protocol version */
    TP_C0_ERR_TXN_BAD_ID,        /* transaction id is not 32 lowercase hex (128-bit token) */
    TP_C0_ERR_TXN_DUPLICATE_ID,  /* txn id already seen in the idempotency retention set */
    TP_C0_ERR_TXN_MISSING_FIELD, /* a required envelope/operation field is absent */
    TP_C0_ERR_TXN_BAD_TYPE,      /* a field is present but has the wrong JSON type */
    TP_C0_ERR_UNKNOWN_FIELD,     /* unknown key in an op/envelope -- policy is REJECT (note §2) */

    /* selector resolution (request edge -> canonical ID) */
    TP_C0_ERR_SELECTOR_AMBIGUOUS,  /* selector matched more than one entity */
    TP_C0_ERR_SELECTOR_UNRESOLVED, /* selector matched no entity / left unresolved in canonical form */

    /* revision precondition (validate whole batch before any apply) */
    TP_C0_ERR_REVISION_CONFLICT, /* expected_revision < current: stale base, rebuild+retry */
    TP_C0_ERR_INVALID_REVISION,  /* expected_revision > current: impossible future base */

    /* Count sentinel: MUST stay last (append new codes before it). Lets decoders
     * iterate the FULL token space [0, TP_C0_DETAIL_COUNT) so a newly appended
     * token still round-trips on version skew instead of hardcoding the current
     * last enumerator. Shifts no existing value -- append-only-safe. */
    TP_C0_DETAIL_COUNT
} tp_c0_detail;

/* Stable lowercase machine token per reason (test-pinned contract). */
const char *tp_c0_detail_id(tp_c0_detail d);

/* Set prose into err (if non-NULL) and return `d`, so call sites can write
 * `return tp_c0_fail(err, TP_C0_ERR_X, "...", ...);` in one line. */
tp_c0_detail tp_c0_fail(tp_error *err, tp_c0_detail d, const char *fmt, ...) TP_PRINTF_ATTR(3, 4);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_ERROR_H */
