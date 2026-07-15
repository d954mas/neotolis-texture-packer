#ifndef TP_CORE_TP_IDENTITY_H
#define TP_CORE_TP_IDENTITY_H

/*
 * Project identity boundary (F1-00; master spec §5.1, §15-16, §59 items 1-3).
 *
 * Saved-project identity is the canonical normalized path of the
 * `.ntpacker_project` file; an unsaved session's identity is a random 128-bit
 * runtime ID (NOT a path). `Save As`, and the first Save of an unsaved session,
 * are ATOMIC identity transitions. Recovery/journal/live-session consumers (F2+)
 * key off this boundary. Identity is NEVER written into `.ntpacker_project` --
 * it is exposed only through the runtime DTO below (F1-00 task 4, §59 item 2).
 *
 * The canonical path has two layers, matching the accepted C0-01 contract:
 *   (a) LEXICAL canonicalization -- separators, drive-letter case, UNC, `\\?\`
 *       device-alias policy, `.`/`..`/repeated-sep normalization. Pure, so a
 *       not-yet-created Save-As destination canonicalizes fine.
 *   (b) FILESYSTEM realpath/symlink resolution layered on top: an existing path
 *       is resolved via realpath / GetFinalPathNameByHandleW (final-component
 *       symlinks/junctions are followed to their target); a not-yet-created
 *       destination resolves its existing PARENT directory and appends the final
 *       component lexically. `..` is resolved LEXICALLY *before* realpath (the
 *       C0-01-pinned policy: identity is textual for `..`, not symlink-physical).
 *
 * Case equality policy: POSIX byte-exact (case-sensitive); Windows folds ASCII
 * case. All entry points return a structured tp_status + tp_error on bad input
 * -- never an abort/crash (AGENTS.md invariant).
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest canonical identity path (matches tp_project's internal TP_PATH_MAX). */
#define TP_IDENTITY_PATH_MAX 4096

/* A project fingerprint is deliberately bounded: external-change detection must
 * never stream forever from a growing file or allocate/read an unbounded input. */
#define TP_IDENTITY_FILE_MAX_BYTES (64U * 1024U * 1024U)

/* ----- canonical saved-project path (task 1) ----------------------------- */

/* Lexically canonicalize an ABSOLUTE `.ntpacker_project` path under the NATIVE
 * host rules (POSIX vs Windows chosen by #if defined(_WIN32)). Touches no
 * filesystem, so it also canonicalizes a not-yet-created Save-As destination.
 * Structured errors: PATH_NOT_ABSOLUTE / PATH_DRIVE_RELATIVE / PATH_BAD_UNC /
 * PATH_DEVICE / INVALID_ARGUMENT (empty|NULL) / OUT_OF_BOUNDS (too long). */
tp_status tp_identity_path_lexical(const char *input, char *out, size_t cap, tp_error *err);

/* Full canonical identity of a `.ntpacker_project` path: lexical canonicalization
 * (a) THEN the filesystem realpath/symlink resolution (b). For an existing file
 * this follows symlinks/junctions to the real target; for a not-yet-created
 * destination it resolves the existing parent directory and appends the final
 * component. In addition to the lexical errors, returns PATH_RESOLVE_FAILED when
 * the destination's parent directory does not exist (or cannot be resolved:
 * permission, symlink loop -- prose carries the specifics). */
tp_status tp_identity_path_canonical(const char *input, char *out, size_t cap, tp_error *err);

/* Identity equality of two ALREADY-canonical paths under the native host case
 * policy (POSIX byte-exact; Windows ASCII case-fold). */
bool tp_identity_path_equal(const char *canon_a, const char *canon_b);

/* Content fingerprint of an exact byte buffer. `bytes` may be NULL only when
 * `len == 0`. This is the primitive file load/save paths use before releasing the
 * exact buffer they consumed/produced, avoiding a racy reopen-and-rehash step. */
tp_status tp_identity_bytes_fingerprint(const void *bytes, size_t len, tp_id128 *out, tp_error *err);

/* Content fingerprint of a project file for external-change detection. Hashes at
 * most TP_IDENTITY_FILE_MAX_BYTES exact file bytes (formatting included) through
 * the shared tp_hasher. The path must name a regular file directly: symlinks,
 * directories, devices, pipes, and files larger than the bound are rejected.
 * Reading is limited to the size observed after open, so a concurrently growing
 * input cannot keep the call alive forever. Before returning, the current pathname
 * is re-opened and must still identify the same file/inode whose bytes were read;
 * an atomic concurrent replacement is therefore rejected. On failure `out` is nil. */
tp_status tp_identity_file_fingerprint(const char *path, tp_id128 *out, tp_error *err);

/* ----- session identity DTO + Save-As transition (tasks 3, 4) ------------- */

typedef enum tp_identity_kind {
    TP_IDENTITY_UNSAVED = 0, /* temp runtime session ID (id128), no path yet */
    TP_IDENTITY_SAVED = 1    /* canonical `.ntpacker_project` path */
} tp_identity_kind;

/* The current identity of one session. A plain value type (embed by value / copy
 * to snapshot). NOT stored in the project file. F2 journal/recovery keys off
 * tp_session_identity_key(); the live-session registry keys off _equal(). */
typedef struct tp_session_identity {
    tp_identity_kind kind;
    tp_id128 session_id;                      /* UNSAVED: the temp runtime ID; SAVED: nil */
    char canonical_path[TP_IDENTITY_PATH_MAX]; /* SAVED: canonical key; UNSAVED: "" */
} tp_session_identity;

/* Initialize an UNSAVED session identity: a random temp runtime ID via `rng`
 * (tests inject a deterministic fill; production passes tp_rng_os()). On RNG
 * failure returns TP_STATUS_RNG_FAILED and leaves *id zeroed -- NOT a usable
 * identity (tp_session_identity_is_valid() == false). */
tp_status tp_session_identity_init_unsaved(tp_session_identity *id, const tp_rng *rng, tp_error *err);

/* ATOMIC transition to a saved canonical path -- the first Save of an unsaved
 * session, or a Save-As from an already-saved one. `dest_path` is canonicalized
 * via tp_identity_path_canonical (realpath layer); ONLY on full success is *id
 * overwritten (kind=SAVED, canonical_path=key). ANY failure (bad/relative/device
 * path, missing destination parent, buffer overflow) leaves *id byte-for-byte
 * UNCHANGED -- the old identity survives (rollback). */
tp_status tp_session_identity_transition_to_path(tp_session_identity *id, const char *dest_path, tp_error *err);

/* Like _transition_to_path, but first rejects the destination if its canonical
 * key equals one already held by ANOTHER session (`claimed_keys[0..count)`,
 * compared under the native host case policy) -> TP_STATUS_IDENTITY_COLLISION
 * with *id unchanged. This is the single-process "atomic destination claim" the
 * F2 journal/session layer needs; it is NOT cross-process ownership (Epic A).
 * `claimed_keys` may be NULL when `count` == 0. */
tp_status tp_session_identity_claim_path(tp_session_identity *id, const char *dest_path,
                                         const char *const *claimed_keys, size_t claimed_count, tp_error *err);

/* Same-session equality (journal/registry key equality): UNSAVED == UNSAVED iff
 * equal session_id; SAVED == SAVED iff canonical paths are equal under host case
 * policy; different kinds are never equal. NULL args compare not-equal. */
bool tp_session_identity_equal(const tp_session_identity *a, const tp_session_identity *b);

/* Stable string key for journal/recovery keying, written into `out` (cap must be
 * at least TP_IDENTITY_PATH_MAX for a SAVED key). SAVED -> the canonical path;
 * UNSAVED -> "session:" + 32 lowercase hex of the session ID. This gives F2 one
 * key form for BOTH saved and unsaved sessions. OUT_OF_BOUNDS if `cap` is too
 * small; INVALID_ARGUMENT for a not-yet-initialized identity. */
tp_status tp_session_identity_key(const tp_session_identity *id, char *out, size_t cap, tp_error *err);

/* True if *id is a usable identity (initialized, not a failed init): a SAVED id
 * with a non-empty path, or an UNSAVED id with a non-nil session_id. */
bool tp_session_identity_is_valid(const tp_session_identity *id);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_IDENTITY_H */
