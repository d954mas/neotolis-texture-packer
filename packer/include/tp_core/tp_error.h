#ifndef TP_CORE_TP_ERROR_H
#define TP_CORE_TP_ERROR_H

/* tp_core error model: no asserts on bad input anywhere in tp_core -- every
 * fallible entry point returns a tp_status and (optionally) fills a tp_error
 * message buffer instead of crashing on caller-supplied data. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable Save publication phases. A FILE_IO_FAILED error identifies the exact
 * pre-publication boundary that failed; NONE is used by every other status. */
typedef enum tp_file_io_phase {
    TP_FILE_IO_PHASE_NONE = 0,
    TP_FILE_IO_PHASE_TEMP_OPEN,
    TP_FILE_IO_PHASE_TEMP_WRITE,
    TP_FILE_IO_PHASE_FILE_SYNC,
    TP_FILE_IO_PHASE_TEMP_CLOSE,
    TP_FILE_IO_PHASE_ATOMIC_REPLACE,
    TP_FILE_IO_PHASE_ATOMIC_CREATE
} tp_file_io_phase;

/* Diagnostic path capacity, including the NUL. Deliberately the same bound as
 * `msg`: the path is operator/agent context, every producer already interpolates
 * it into that equally bounded message, and a longer path is truncated here for
 * exactly the reason it is truncated there. */
#define TP_FILE_IO_PATH_MAX 256

typedef struct tp_file_io_context {
    tp_file_io_phase phase;
    /* Owned BY VALUE. tp_error crosses process, job and session boundaries as a
     * plain struct assignment, so this must never be a borrow: a pointer here
     * outlived by its owner is a use-after-free the copy cannot see. Empty means
     * "no path". Session boundaries overwrite private canonical buffers with the
     * public path before returning the error to their caller. */
    char path[TP_FILE_IO_PATH_MAX];
    /* errno-compatible cause captured before cleanup can overwrite it. */
    int native_code;
} tp_file_io_context;

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

    /* --- project identity faults ---
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

    /* --- structural-ID faults ---
     * Append-only: new values go at the END. Distinct id faults a client acts on
     * differently; generic NULL/too-small inputs still reuse INVALID_ARGUMENT /
     * OUT_OF_BOUNDS (a format buffer too small is OUT_OF_BOUNDS). */
    TP_STATUS_ID_MALFORMED,        /* shape-ID text is malformed (bad prefix/hex/length/trailing/empty),
                                    * or a nil ID appears where a real structural ID is required (tp_id) */
    TP_STATUS_DUPLICATE_ID,        /* two structural entities share one persistent ID on load/validate (tp_project) */

    /* --- source-key normalization faults ---
     * Append-only: new values go at the END. A source-local key that is invalid
     * UTF-8, absolute, or contains a '..' traversal component; generic empty/NULL
     * inputs still reuse INVALID_ARGUMENT and an overflowing buffer OUT_OF_BOUNDS. */
    TP_STATUS_INVALID_UTF8,        /* text is not well-formed UTF-8 (tp_srckey) */
    TP_STATUS_KEY_ABSOLUTE,        /* source key/path must be source-root-relative, not absolute (tp_srckey) */
    TP_STATUS_KEY_TRAVERSAL,       /* a '..' component would escape the source root (tp_srckey) */

    /* --- selector resolution faults (model/operation contract) ---
     * Append-only: new values go at the END. A human selector must resolve to
     * EXACTLY ONE id before it is used; zero and >1 matches are distinct faults a
     * client acts on differently (NOT_FOUND surfaces "no such entity", AMBIGUOUS
     * hands back a candidate list the caller disambiguates). Generic NULL/empty
     * selector text still reuses INVALID_ARGUMENT. */
    TP_STATUS_NOT_FOUND,           /* a selector matched no entity (tp_selector) */
    TP_STATUS_AMBIGUOUS_SELECTOR,  /* a selector matched more than one entity; a candidate list is returned */

    /* --- typed operation-engine faults ---
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

    /* --- transaction concurrency faults ---
     * Append-only: new values go at the END. A transaction carries an
     * expected_revision optimistic-concurrency precondition; the two mismatch
     * directions are distinct faults a client acts on differently. CONFLICT (stale:
     * expected < current) -> the caller rebuilds against the new state and retries;
     * INVALID (expected > current, a revision that never existed) -> a client bug,
     * not a retry. No CRDT / auto-merge in v1. Idempotency reuses
     * the existing DUPLICATE_ID token ("duplicate_id"): a re-submitted transaction
     * id is a duplicate the message distinguishes from a structural-id collision. */
    TP_STATUS_REVISION_CONFLICT,   /* expected_revision < current: stale; rebuild and retry (tp_transaction) */
    TP_STATUS_INVALID_REVISION,    /* expected_revision > current: a revision that never existed (tp_transaction) */

    /* --- recovery-journal durability fault ---
     * Append-only: new value at the END. This status reports that a recovery
     * record was not durably established. At the model/session boundary that
     * failure degrades recovery but does not roll back an already-published live
     * transaction. An OOM-class journal fault still reuses OOM. */
    TP_STATUS_JOURNAL_FAILED,      /* recovery-journal durable append failed (tp_journal) */
    TP_STATUS_FILE_CHANGED_EXTERNALLY, /* saved project bytes differ from the persisted session fingerprint */
    TP_STATUS_RECOVERY_CLEANUP_FAILED, /* recovered output is safe, but its orphan journal could not be removed */
    TP_STATUS_RECOVERY_BUSY,           /* another process claimed this recovery journal */
    TP_STATUS_RECOVERY_CLAIM_FAILED,   /* recovery journal lock could not be created/acquired for a storage reason */
    TP_STATUS_PROJECT_LIVE,            /* canonical project identity is leased by another writer */
    TP_STATUS_UNSUPPORTED_CAPABILITY,  /* the selected client has no authority/surface for this capability */
    TP_STATUS_FILE_EXISTS,             /* create-only publication found an existing destination */
    /* The project was atomically published and is the authoritative saved
     * state, but the containing-directory durability barrier failed. Clients
     * must report this as a success notice, never retry as if no write occurred. */
    TP_STATUS_FILE_DURABILITY_UNCERTAIN,

    /* A Save failed before atomic publication. The destination and saved
     * session baseline are unchanged; file_io carries phase/path/cause. */
    TP_STATUS_FILE_IO_FAILED,

    /* --- fallible builder containment fault ---
     * Append-only: new value at the END. A PROCESS-LEVEL abnormal outcome of the
     * private build worker: it aborted, crashed on a signal, timed out, exited
     * non-zero with no valid reply, reported success but staged no readable
     * artifact, or produced a valid artifact the host could not publish -- so no
     * trustworthy PUBLISHED artifact exists. Distinct from BUILDER_FAILED, which
     * covers a controlled builder/sink failure the worker reported in a VALID reply
     * OR a clean-exit malformed/truncated/oversized reply (fail closed). The host
     * survives and the last successful preview stays authoritative (tp_build worker
     * boundary). */
    TP_STATUS_BUILDER_CRASHED,

    /* --- cooperative cancellation (U-02) ---
     * Append-only: new value at the END. A caller-requested cooperative cancel
     * stopped an interruptible core walk (the recursive folder scan / pack-input
     * build) before it completed. NOT a failure: the partial result is freed and the
     * output left empty, and the async pack job surfaces this as
     * TP_SESSION_JOB_CANCELLED. Distinct from the generic faults so a caller can tell
     * "you asked me to stop" apart from "the input was bad". */
    TP_STATUS_CANCELLED,

    /* A session already owns one transient Refresh, Pack, or Export task. */
    TP_STATUS_BUSY
} tp_status;

/* No heap and no borrowed pointers, so a plain struct copy of a tp_error stays
 * valid for as long as the destination does. The single anonymous aggregate
 * preserves the long-standing `tp_error error = {{0}}` initializer under the
 * project's -Wmissing-field-initializers -Werror policy. */
typedef union tp_error {
    struct {
        char msg[256];
        tp_file_io_context file_io;
    };
} tp_error;

#if defined(__GNUC__) || defined(__clang__)
#define TP_PRINTF_ATTR(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define TP_PRINTF_ATTR(fmt_idx, args_idx)
#endif

/* snprintf/vsnprintf truncate on a BYTE boundary, so a message or path that
 * overflows its fixed buffer can end mid-codepoint. Downstream that one dangling
 * byte is not a cosmetic defect: the worker wire validator rejects the whole
 * payload as invalid UTF-8 and the host loses the entire diagnostic. Truncation
 * is fixed HERE, at the only producer, by dropping a trailing incomplete
 * sequence: walk back over the continuation bytes (0b10xxxxxx, at most three) to
 * their lead byte and cut at the lead unless the sequence is complete. A no-op
 * for ASCII and for any text that fit. */
static inline void tp_error_trim_partial_utf8(char *text) {
    const size_t length = strlen(text);
    size_t trailing = 0U;
    while (trailing < 3U && trailing < length &&
           ((unsigned char)text[length - 1U - trailing] & 0xC0U) == 0x80U) {
        trailing++;
    }
    if (trailing == length) {
        /* Continuation bytes all the way down: there is no lead byte to keep. */
        if (length > 0U) {
            text[0] = '\0';
        }
        return;
    }
    const size_t lead_index = length - 1U - trailing;
    const unsigned char lead = (unsigned char)text[lead_index];
    if (lead < 0x80U) {
        /* An ASCII byte is complete on its own; anything after it is stray. */
        text[length - trailing] = '\0';
        return;
    }
    size_t width = 0U;
    if ((lead & 0xE0U) == 0xC0U) {
        width = 2U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        width = 3U;
    } else if ((lead & 0xF8U) == 0xF0U) {
        width = 4U;
    }
    if (width != trailing + 1U) {
        text[lead_index] = '\0';
    }
}

/* Formats into err->msg (if err != NULL) and returns status, so call sites can
 * write `return tp_error_set(err, TP_STATUS_X, "...", ...);` in one line. */
static inline tp_status tp_error_set(tp_error *err, tp_status status, const char *fmt, ...) TP_PRINTF_ATTR(3, 4);

static inline tp_status tp_error_set(tp_error *err, tp_status status, const char *fmt, ...) {
    if (err) {
        err->file_io.phase = TP_FILE_IO_PHASE_NONE;
        err->file_io.path[0] = '\0';
        err->file_io.native_code = 0;
        if (fmt) {
            va_list args;
            va_start(args, fmt);
            (void)vsnprintf(err->msg, sizeof(err->msg), fmt, args);
            va_end(args);
            tp_error_trim_partial_utf8(err->msg);
        } else {
            err->msg[0] = '\0';
        }
    }
    return status;
}

static inline tp_status tp_error_set_file_io(
    tp_error *err, tp_file_io_phase phase, const char *path, int native_code,
    const char *fmt, ...) TP_PRINTF_ATTR(5, 6);

static inline tp_status tp_error_set_file_io(
    tp_error *err, tp_file_io_phase phase, const char *path, int native_code,
    const char *fmt, ...) {
    if (err) {
        err->file_io.phase = phase;
        (void)snprintf(err->file_io.path, sizeof err->file_io.path, "%s",
                       path ? path : "");
        tp_error_trim_partial_utf8(err->file_io.path);
        err->file_io.native_code = native_code;
        if (fmt) {
            va_list args;
            va_start(args, fmt);
            (void)vsnprintf(err->msg, sizeof(err->msg), fmt, args);
            va_end(args);
            tp_error_trim_partial_utf8(err->msg);
        } else {
            err->msg[0] = '\0';
        }
    }
    return TP_STATUS_FILE_IO_FAILED;
}

static inline const char *tp_file_io_phase_id(tp_file_io_phase phase) {
    switch (phase) {
        case TP_FILE_IO_PHASE_NONE: return "none";
        case TP_FILE_IO_PHASE_TEMP_OPEN: return "temp_open";
        case TP_FILE_IO_PHASE_TEMP_WRITE: return "temp_write";
        case TP_FILE_IO_PHASE_FILE_SYNC: return "file_sync";
        case TP_FILE_IO_PHASE_TEMP_CLOSE: return "temp_close";
        case TP_FILE_IO_PHASE_ATOMIC_REPLACE: return "atomic_replace";
        case TP_FILE_IO_PHASE_ATOMIC_CREATE: return "atomic_create";
    }
    return "unknown";
}

/* One row per status: enum suffix, human prose, stable machine token. Both
 * accessors below are generated from this list, so prose and token can never
 * drift apart or cover different sets. The machine token (enum-name style, minus
 * the TP_STATUS_ prefix) is a contract an agent matches on in --json payloads and
 * is test-pinned; APPEND-ONLY, never rename one. Neither generated switch has a
 * `default`, so a new enum value missing from this list trips -Wswitch. */
#define TP_STATUS_LIST(X)                                                          \
    X(OK, "ok", "ok")                                                              \
    X(UNIMPLEMENTED, "unimplemented", "unimplemented")                             \
    X(INVALID_ARGUMENT, "invalid argument", "invalid_argument")                    \
    X(BAD_MAGIC, "bad magic", "bad_magic")                                         \
    X(BAD_VERSION, "bad version", "bad_version")                                   \
    X(OUT_OF_BOUNDS, "out of bounds", "out_of_bounds")                             \
    X(HASH_COLLISION, "hash collision", "hash_collision")                          \
    X(UNKNOWN_REGION, "unknown region", "unknown_region")                          \
    X(PAGE_NOT_FOUND, "page not found", "page_not_found")                          \
    X(UNSUPPORTED_TEXTURE, "unsupported texture", "unsupported_texture")           \
    X(OOM, "out of memory", "oom")                                                 \
    X(BUILDER_FAILED, "builder failed", "builder_failed")                          \
    X(BAD_PROJECT, "bad project file", "bad_project")                              \
    X(PATH_NOT_ABSOLUTE, "path is not absolute", "path_not_absolute")              \
    X(PATH_DRIVE_RELATIVE, "path is drive-relative", "path_drive_relative")        \
    X(PATH_BAD_UNC, "malformed UNC path", "path_bad_unc")                          \
    X(PATH_DEVICE, "device path is not a project identity", "path_device")         \
    X(PATH_RESOLVE_FAILED, "could not resolve path", "path_resolve_failed")        \
    X(RNG_FAILED, "random generator failed", "rng_failed")                         \
    X(IDENTITY_COLLISION, "identity collision", "identity_collision")              \
    X(ID_MALFORMED, "malformed structural id", "id_malformed")                     \
    X(DUPLICATE_ID, "duplicate structural id", "duplicate_id")                     \
    X(INVALID_UTF8, "invalid UTF-8", "invalid_utf8")                               \
    X(KEY_ABSOLUTE, "source key is not relative", "key_absolute")                  \
    X(KEY_TRAVERSAL, "source key escapes its root", "key_traversal")               \
    X(NOT_FOUND, "selector matched no entity", "not_found")                        \
    X(AMBIGUOUS_SELECTOR, "selector is ambiguous", "ambiguous_selector")           \
    X(UNKNOWN_OP, "unknown operation", "unknown_op")                               \
    X(OUT_OF_RANGE, "value out of range", "out_of_range")                          \
    X(REVISION_CONFLICT, "revision conflict", "revision_conflict")                 \
    X(INVALID_REVISION, "invalid revision", "invalid_revision")                    \
    X(JOURNAL_FAILED, "recovery journal append failed", "journal_failed")          \
    X(FILE_CHANGED_EXTERNALLY, "project file changed externally",                  \
      "file_changed_externally")                                                   \
    X(RECOVERY_CLEANUP_FAILED, "recovery cleanup failed",                          \
      "recovery_cleanup_failed")                                                   \
    X(RECOVERY_BUSY, "recovery journal is busy", "recovery_busy")                  \
    X(RECOVERY_CLAIM_FAILED, "recovery journal claim failed",                      \
      "recovery_claim_failed")                                                     \
    X(PROJECT_LIVE, "project is live in another writer", "project_live")           \
    X(UNSUPPORTED_CAPABILITY, "unsupported client capability",                     \
      "unsupported_capability")                                                    \
    X(FILE_EXISTS, "file already exists", "file_exists")                           \
    X(FILE_DURABILITY_UNCERTAIN, "project file durability is uncertain",           \
      "file_durability_uncertain")                                                 \
    X(FILE_IO_FAILED, "project file I/O failed", "file_io_failed")                 \
    X(BUILDER_CRASHED, "builder crashed", "builder_crashed")                       \
    X(CANCELLED, "cancelled", "cancelled")                                          \
    X(BUSY, "session task is busy", "busy")

static inline const char *tp_status_str(tp_status status) {
    switch (status) {
#define TP_STATUS_X_STR(name, prose, id) case TP_STATUS_##name: return prose;
        TP_STATUS_LIST(TP_STATUS_X_STR)
#undef TP_STATUS_X_STR
    }
    return "unknown status";
}

static inline const char *tp_status_id(tp_status status) {
    switch (status) {
#define TP_STATUS_X_ID(name, prose, id) case TP_STATUS_##name: return id;
        TP_STATUS_LIST(TP_STATUS_X_ID)
#undef TP_STATUS_X_ID
    }
    return "unknown_status";
}

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_ERROR_H */
