#ifndef TP_CORE_TP_ERROR_H
#define TP_CORE_TP_ERROR_H

/* tp_core error model: no asserts on bad input anywhere in tp_core -- every
 * fallible entry point returns a tp_status and (optionally) fills a tp_error
 * message buffer instead of crashing on caller-supplied data. */

#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tp_status {
    TP_STATUS_OK = 0,
    TP_STATUS_UNIMPLEMENTED,
    TP_STATUS_INVALID_ARGUMENT,
    TP_STATUS_BAD_MAGIC,
    TP_STATUS_BAD_VERSION,
    TP_STATUS_OUT_OF_BOUNDS,
    TP_STATUS_HASH_COLLISION,
    TP_STATUS_UNKNOWN_REGION,
    TP_STATUS_PAGE_NOT_FOUND,
    TP_STATUS_UNSUPPORTED_TEXTURE,
    TP_STATUS_OOM,
    TP_STATUS_BUILDER_FAILED, /* nt_builder start/finish returned an error (tp_pack) */
    TP_STATUS_BAD_PROJECT,    /* malformed/invalid .ntpacker_project JSON (tp_project) */

    /* --- project identity faults (F1-00, promoted from the C0-01 spike) ---
     * Append-only: new values go at the END so existing tokens never shift.
     * Distinct identity faults that a client acts on differently; generic
     * empty/NULL/too-small inputs still reuse INVALID_ARGUMENT / OUT_OF_BOUNDS. */
    TP_STATUS_PATH_NOT_ABSOLUTE,  /* identity path is not absolute (tp_identity) */
    TP_STATUS_PATH_DRIVE_RELATIVE, /* Windows "C:foo" drive-relative form (tp_identity) */
    TP_STATUS_PATH_BAD_UNC,       /* malformed UNC (//server without share) (tp_identity) */
    TP_STATUS_PATH_DEVICE,        /* Windows \\?\ / \\.\ device path is not an identity (tp_identity) */
    TP_STATUS_PATH_RESOLVE_FAILED, /* realpath/GetFinalPathNameByHandle failed: missing parent dir,
                                    * permission, symlink loop, ... prose carries the specifics (tp_identity) */
    TP_STATUS_RNG_FAILED,         /* the injected RNG did not deliver the requested bytes (tp_id) */
    TP_STATUS_IDENTITY_COLLISION, /* Save-As destination canonicalizes to an already-claimed key (tp_identity) */

    /* --- structural-ID faults (F1-01, promoted from the C0-01 id/legacy spike) ---
     * Append-only: new values go at the END. Distinct id faults a client acts on
     * differently; generic NULL/too-small inputs still reuse INVALID_ARGUMENT /
     * OUT_OF_BOUNDS (a format buffer too small is OUT_OF_BOUNDS). */
    TP_STATUS_ID_MALFORMED,        /* shape-ID text is malformed (bad prefix/hex/length/trailing/empty),
                                    * or a nil ID appears where a real structural ID is required (tp_id) */
    TP_STATUS_DUPLICATE_ID,        /* two structural entities share one persistent ID on load/validate (tp_project) */
    TP_STATUS_ID_COLLISION_EXHAUSTED, /* deterministic legacy salt sweep could not disambiguate synthetic IDs
                                       * (unreachable with the default hash; only a degenerate injected hash hits it) */

    /* --- source-key normalization faults (F1-02, promoted from the C0-01 srckey spike) ---
     * Append-only: new values go at the END. A source-local key that is invalid
     * UTF-8, absolute, or contains a '..' traversal component; generic empty/NULL
     * inputs still reuse INVALID_ARGUMENT and an overflowing buffer OUT_OF_BOUNDS. */
    TP_STATUS_INVALID_UTF8,        /* text is not well-formed UTF-8 (tp_srckey) */
    TP_STATUS_KEY_ABSOLUTE,        /* source key/path must be source-root-relative, not absolute (tp_srckey) */
    TP_STATUS_KEY_TRAVERSAL,       /* a '..' component would escape the source root (tp_srckey) */

    /* --- selector resolution faults (F1-03, master spec §5.4) ---
     * Append-only: new values go at the END. A human selector must resolve to
     * EXACTLY ONE id before it is used; zero and >1 matches are distinct faults a
     * client acts on differently (NOT_FOUND surfaces "no such entity", AMBIGUOUS
     * hands back a candidate list the caller disambiguates). Generic NULL/empty
     * selector text still reuses INVALID_ARGUMENT. */
    TP_STATUS_NOT_FOUND,           /* a selector matched no entity (tp_selector) */
    TP_STATUS_AMBIGUOUS_SELECTOR,  /* a selector matched more than one entity; a candidate list is returned */

    /* --- typed operation-engine faults (F2-01, master spec §6-6.2, §7) ---
     * Append-only: new values go at the END. Distinct faults a client acts on
     * differently from the generic INVALID_ARGUMENT: UNKNOWN_OP -> the op kind is
     * not in the catalog (fix the verb/wire); OUT_OF_RANGE -> a payload VALUE is
     * outside its allowed numeric range (adjust the value, the field name + bound is
     * in the message). A malformed/missing/wrong-form field still reuses
     * INVALID_ARGUMENT; a malformed shape ID reuses ID_MALFORMED; an addressed
     * entity that does not exist reuses NOT_FOUND (dangling id or unresolved
     * selector -- the message distinguishes). */
    TP_STATUS_UNKNOWN_OP,          /* the operation kind/wire is not in the append-only catalog (tp_operation) */
    TP_STATUS_OUT_OF_RANGE,        /* a payload value is outside its allowed range (tp_op_validate) */

    /* --- transaction concurrency faults (F2-02, master spec §8) ---
     * Append-only: new values go at the END. A transaction carries an
     * expected_revision optimistic-concurrency precondition; the two mismatch
     * directions are distinct faults a client acts on differently. CONFLICT (stale:
     * expected < current) -> the caller rebuilds against the new state and retries;
     * INVALID (expected > current, a revision that never existed) -> a client bug,
     * not a retry. No CRDT / auto-merge in v1 (§8, plan §420). Idempotency reuses
     * the existing DUPLICATE_ID token ("duplicate_id"): a re-submitted transaction
     * id is a duplicate the message distinguishes from a structural-id collision. */
    TP_STATUS_REVISION_CONFLICT,   /* expected_revision < current: stale; rebuild and retry (tp_transaction) */
    TP_STATUS_INVALID_REVISION,    /* expected_revision > current: a revision that never existed (tp_transaction) */

    /* --- recovery-journal durability fault (F2-04, master spec §7.1, §22.3) ---
     * Append-only: new value at the END. A committed transaction is not acknowledged
     * until its recovery record is durably appended (§7.1); when that durable append
     * fails the transaction is rolled back (live model byte-unchanged, no committed
     * event) and this distinct status tells the caller to retry the SAME transaction
     * id -- it is a durability failure, not OOM and not a validation reject. An
     * OOM-class journal fault (index/buffer allocation) still reuses OOM. */
    TP_STATUS_JOURNAL_FAILED,      /* recovery-journal durable append failed (tp_journal) */
    TP_STATUS_FILE_CHANGED_EXTERNALLY, /* saved project bytes differ from the persisted session fingerprint */
    TP_STATUS_RECOVERY_CLEANUP_FAILED, /* recovered output is safe, but its orphan journal could not be removed */
    TP_STATUS_RECOVERY_BUSY,           /* another process claimed this recovery journal */
    TP_STATUS_RECOVERY_CLAIM_FAILED,   /* recovery journal lock could not be created/acquired for a storage reason */
    TP_STATUS_PROJECT_LIVE,            /* canonical project identity is leased by another writer */
    TP_STATUS_UNSUPPORTED_CAPABILITY,  /* the selected client has no authority/surface for this capability */
    TP_STATUS_FILE_EXISTS              /* create-only publication found an existing destination */
} tp_status;

/* Fixed-size message buffer -- no heap, safe to embed by value on the stack. */
typedef struct tp_error {
    char msg[256];
} tp_error;

#if defined(__GNUC__) || defined(__clang__)
#define TP_PRINTF_ATTR(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define TP_PRINTF_ATTR(fmt_idx, args_idx)
#endif

/* Formats into err->msg (if err != NULL) and returns status, so call sites can
 * write `return tp_error_set(err, TP_STATUS_X, "...", ...);` in one line. */
static inline tp_status tp_error_set(tp_error *err, tp_status status, const char *fmt, ...) TP_PRINTF_ATTR(3, 4);

static inline tp_status tp_error_set(tp_error *err, tp_status status, const char *fmt, ...) {
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
    return status;
}

static inline const char *tp_status_str(tp_status status) {
    switch (status) {
        case TP_STATUS_OK: return "ok";
        case TP_STATUS_UNIMPLEMENTED: return "unimplemented";
        case TP_STATUS_INVALID_ARGUMENT: return "invalid argument";
        case TP_STATUS_BAD_MAGIC: return "bad magic";
        case TP_STATUS_BAD_VERSION: return "bad version";
        case TP_STATUS_OUT_OF_BOUNDS: return "out of bounds";
        case TP_STATUS_HASH_COLLISION: return "hash collision";
        case TP_STATUS_UNKNOWN_REGION: return "unknown region";
        case TP_STATUS_PAGE_NOT_FOUND: return "page not found";
        case TP_STATUS_UNSUPPORTED_TEXTURE: return "unsupported texture";
        case TP_STATUS_OOM: return "out of memory";
        case TP_STATUS_BUILDER_FAILED: return "builder failed";
        case TP_STATUS_BAD_PROJECT: return "bad project file";
        case TP_STATUS_PATH_NOT_ABSOLUTE: return "path is not absolute";
        case TP_STATUS_PATH_DRIVE_RELATIVE: return "path is drive-relative";
        case TP_STATUS_PATH_BAD_UNC: return "malformed UNC path";
        case TP_STATUS_PATH_DEVICE: return "device path is not a project identity";
        case TP_STATUS_PATH_RESOLVE_FAILED: return "could not resolve path";
        case TP_STATUS_RNG_FAILED: return "random generator failed";
        case TP_STATUS_IDENTITY_COLLISION: return "identity collision";
        case TP_STATUS_ID_MALFORMED: return "malformed structural id";
        case TP_STATUS_DUPLICATE_ID: return "duplicate structural id";
        case TP_STATUS_ID_COLLISION_EXHAUSTED: return "legacy id collision sweep exhausted";
        case TP_STATUS_INVALID_UTF8: return "invalid UTF-8";
        case TP_STATUS_KEY_ABSOLUTE: return "source key is not relative";
        case TP_STATUS_KEY_TRAVERSAL: return "source key escapes its root";
        case TP_STATUS_NOT_FOUND: return "selector matched no entity";
        case TP_STATUS_AMBIGUOUS_SELECTOR: return "selector is ambiguous";
        case TP_STATUS_UNKNOWN_OP: return "unknown operation";
        case TP_STATUS_OUT_OF_RANGE: return "value out of range";
        case TP_STATUS_REVISION_CONFLICT: return "revision conflict";
        case TP_STATUS_INVALID_REVISION: return "invalid revision";
        case TP_STATUS_JOURNAL_FAILED: return "recovery journal append failed";
        case TP_STATUS_FILE_CHANGED_EXTERNALLY: return "project file changed externally";
        case TP_STATUS_RECOVERY_CLEANUP_FAILED: return "recovery cleanup failed";
        case TP_STATUS_RECOVERY_BUSY: return "recovery journal is busy";
        case TP_STATUS_RECOVERY_CLAIM_FAILED: return "recovery journal claim failed";
        case TP_STATUS_PROJECT_LIVE: return "project is live in another writer";
        case TP_STATUS_UNSUPPORTED_CAPABILITY: return "unsupported client capability";
        case TP_STATUS_FILE_EXISTS: return "file already exists";
    }
    return "unknown status";
}

/* Stable lowercase machine token per status (enum-name style, minus the
 * TP_STATUS_ prefix) for --json error payloads. Distinct from tp_status_str's
 * human prose: this token is a contract an agent matches on and is test-pinned.
 * No `default` inside the switch on purpose -- a new enum value then trips
 * -Wswitch here too, keeping token + prose in lockstep. */
static inline const char *tp_status_id(tp_status status) {
    switch (status) {
        case TP_STATUS_OK: return "ok";
        case TP_STATUS_UNIMPLEMENTED: return "unimplemented";
        case TP_STATUS_INVALID_ARGUMENT: return "invalid_argument";
        case TP_STATUS_BAD_MAGIC: return "bad_magic";
        case TP_STATUS_BAD_VERSION: return "bad_version";
        case TP_STATUS_OUT_OF_BOUNDS: return "out_of_bounds";
        case TP_STATUS_HASH_COLLISION: return "hash_collision";
        case TP_STATUS_UNKNOWN_REGION: return "unknown_region";
        case TP_STATUS_PAGE_NOT_FOUND: return "page_not_found";
        case TP_STATUS_UNSUPPORTED_TEXTURE: return "unsupported_texture";
        case TP_STATUS_OOM: return "oom";
        case TP_STATUS_BUILDER_FAILED: return "builder_failed";
        case TP_STATUS_BAD_PROJECT: return "bad_project";
        case TP_STATUS_PATH_NOT_ABSOLUTE: return "path_not_absolute";
        case TP_STATUS_PATH_DRIVE_RELATIVE: return "path_drive_relative";
        case TP_STATUS_PATH_BAD_UNC: return "path_bad_unc";
        case TP_STATUS_PATH_DEVICE: return "path_device";
        case TP_STATUS_PATH_RESOLVE_FAILED: return "path_resolve_failed";
        case TP_STATUS_RNG_FAILED: return "rng_failed";
        case TP_STATUS_IDENTITY_COLLISION: return "identity_collision";
        case TP_STATUS_ID_MALFORMED: return "id_malformed";
        case TP_STATUS_DUPLICATE_ID: return "duplicate_id";
        case TP_STATUS_ID_COLLISION_EXHAUSTED: return "id_collision_exhausted";
        case TP_STATUS_INVALID_UTF8: return "invalid_utf8";
        case TP_STATUS_KEY_ABSOLUTE: return "key_absolute";
        case TP_STATUS_KEY_TRAVERSAL: return "key_traversal";
        case TP_STATUS_NOT_FOUND: return "not_found";
        case TP_STATUS_AMBIGUOUS_SELECTOR: return "ambiguous_selector";
        case TP_STATUS_UNKNOWN_OP: return "unknown_op";
        case TP_STATUS_OUT_OF_RANGE: return "out_of_range";
        case TP_STATUS_REVISION_CONFLICT: return "revision_conflict";
        case TP_STATUS_INVALID_REVISION: return "invalid_revision";
        case TP_STATUS_JOURNAL_FAILED: return "journal_failed";
        case TP_STATUS_FILE_CHANGED_EXTERNALLY: return "file_changed_externally";
        case TP_STATUS_RECOVERY_CLEANUP_FAILED: return "recovery_cleanup_failed";
        case TP_STATUS_RECOVERY_BUSY: return "recovery_busy";
        case TP_STATUS_RECOVERY_CLAIM_FAILED: return "recovery_claim_failed";
        case TP_STATUS_PROJECT_LIVE: return "project_live";
        case TP_STATUS_UNSUPPORTED_CAPABILITY: return "unsupported_capability";
        case TP_STATUS_FILE_EXISTS: return "file_exists";
    }
    return "unknown_status";
}

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_ERROR_H */
