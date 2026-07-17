#ifndef TP_CORE_TP_RECOVERY_H
#define TP_CORE_TP_RECOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_journal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TP_RECOVERY_MAX_CANDIDATES 16
#define TP_RECOVERY_MAX_SCAN_DIAGNOSTICS 16

typedef struct tp_session tp_session;

typedef struct tp_recovery_metadata {
    int64_t timestamp;
    const char *project_path;
    const char *project_name;
    const tp_id128 *file_fingerprint;
} tp_recovery_metadata;

typedef struct tp_recovery_candidate {
    char journal_path[TP_IDENTITY_PATH_MAX];
    char original_path[TP_IDENTITY_PATH_MAX];
    char name[256];
    int64_t timestamp;
    tp_journal_recovery_status status;
    bool adoptable;
    tp_id128 file_fingerprint;
    bool has_file_fingerprint;
} tp_recovery_candidate;

/* A per-entry scan failure. Scan remains best-effort and returns TP_STATUS_OK
 * when the root itself was enumerated: readable candidates stay available and
 * callers receive a stable status id plus the path of each skipped journal.
 * BAD_PROJECT means readable but malformed/corrupt; PATH_RESOLVE_FAILED means
 * the entry could not be opened as a regular no-follow journal. */
typedef struct tp_recovery_scan_diagnostic {
    char journal_path[TP_IDENTITY_PATH_MAX];
    tp_status status;
} tp_recovery_scan_diagnostic;

typedef struct tp_recovery_candidates {
    tp_recovery_candidate items[TP_RECOVERY_MAX_CANDIDATES];
    size_t count;
    tp_recovery_scan_diagnostic diagnostics[TP_RECOVERY_MAX_SCAN_DIAGNOSTICS];
    size_t diagnostic_count;
    bool has_more; /* candidates or diagnostics were omitted by a cap/budget */
} tp_recovery_candidates;

/* Frontend-facing recovery orchestration. Frontends inject host policy (root,
 * domain key, paths, action) and receive owned-value DTOs; store, liveness,
 * claim, journal, detached-session, and project-lease lifetimes stay here. */
typedef enum tp_recovery_action {
    TP_RECOVERY_ACTION_DISCARD = 0,
    TP_RECOVERY_ACTION_SAVE_ORIGINAL,
    TP_RECOVERY_ACTION_SAVE_AS,
} tp_recovery_action;

typedef struct tp_recovery_resolve_result {
    bool journal_deleted;
    bool project_saved;
    char target_path[TP_IDENTITY_PATH_MAX];
    tp_id128 file_fingerprint;
    bool has_file_fingerprint;
} tp_recovery_resolve_result;

/* Thin frontend flow. The caller supplies only host policy and value DTOs;
 * recovery core creates and releases every store/claim owner internally.
 * A successful attach transfers the live owner to `session`. */
tp_status tp_recovery_root_validate(const char *root, tp_id128 journal_key,
                                    tp_error *err);
tp_status tp_recovery_session_attach(
    const char *root, tp_id128 journal_key, const tp_rng *rng,
    tp_session *session, const tp_recovery_metadata *metadata, tp_error *err);
tp_status tp_recovery_scan_root(const char *root, tp_id128 journal_key,
                                const tp_session *live_session,
                                tp_recovery_candidates *out, tp_error *err);
tp_status tp_recovery_resolve_journal(
    const char *root, tp_id128 journal_key, const char *journal_path,
    const tp_session *live_session, tp_recovery_action action,
    const char *target_path, const tp_rng *rng,
    tp_recovery_resolve_result *out, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif
