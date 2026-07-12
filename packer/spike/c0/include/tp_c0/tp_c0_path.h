#ifndef TP_C0_PATH_H
#define TP_C0_PATH_H

/*
 * C0-01 task 1: canonical saved-project-path policy for project identity
 * (master spec §5.1, §16, §59 items 1-3).
 *
 * Project identity is the canonical normalized path of the .ntpacker_project
 * file. tp_c0_project_path_canonical() is the LEXICAL half of that contract:
 *
 *   - Pure: it touches no filesystem, so it also canonicalizes a not-yet-created
 *     Save As destination (the final component need not exist).
 *   - It does NOT resolve symlinks/junctions or realpath. Symlink/junction
 *     equivalence is a filesystem property resolved once the file exists (at/
 *     after first save); until then two lexically different symlink-equivalent
 *     destinations are distinct identities (documented in C0-01-contract.md).
 *   - The `host` rule set is an explicit parameter, not #ifdef'd, so the same
 *     golden vectors run deterministically on every OS (no platform-specific
 *     golden output).
 *
 * Case policy lives in tp_c0_project_path_equal(): POSIX is byte-exact
 * (case-sensitive); WINDOWS folds ASCII case (its filesystem is
 * case-insensitive). The canonical STRING preserves case except the Windows
 * drive letter, which is upper-cased.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_c0/tp_c0_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tp_c0_host {
    TP_C0_HOST_POSIX = 0,
    TP_C0_HOST_WINDOWS = 1
} tp_c0_host;

/* Longest canonical path this spike accepts (matches tp_project TP_PATH_MAX). */
#define TP_C0_PATH_MAX 4096

/* The host rule set of the current build. */
tp_c0_host tp_c0_host_native(void);

/* Lexically canonicalize an ABSOLUTE project path under `host`:
 *   - WINDOWS treats '\\' and '/' as separators; POSIX treats '\\' as a literal
 *     filename byte.
 *   - forms accepted: POSIX "/..."; Windows "X:/...", "X:" (drive root), and UNC
 *     "//server/share/...". Windows "X:foo" (drive-relative) and a bare/relative
 *     path are rejected with a structured error.
 *   - '.' components dropped; '//' collapsed (UNC leading "//" preserved); '..'
 *     resolved lexically and clamped at the root; trailing '/' stripped.
 *   - output separators are '/', the Windows drive letter is upper-cased, all
 *     other case is preserved.
 * `out` needs cap up to TP_C0_PATH_MAX. Structured error on non-absolute /
 * drive-relative / malformed-UNC / too-long input. */
tp_c0_detail tp_c0_project_path_canonical(const char *input, tp_c0_host host, char *out, size_t cap,
                                          tp_error *err);

/* Identity equality of two ALREADY-canonical paths under host case policy. */
bool tp_c0_project_path_equal(const char *canon_a, const char *canon_b, tp_c0_host host);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_PATH_H */
