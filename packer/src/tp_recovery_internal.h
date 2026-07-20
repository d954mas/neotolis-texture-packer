#ifndef TP_RECOVERY_INTERNAL_H
#define TP_RECOVERY_INTERNAL_H

#include "tp_core/tp_recovery.h"
#include "tp_recovery_live_seam.h"

typedef struct tp_recovery_store tp_recovery_store;
typedef struct tp_recovery_claim tp_recovery_claim;
typedef struct tp_recovery_owned_candidate tp_recovery_owned_candidate;
typedef struct tp_recovery_resolution tp_recovery_resolution;
typedef struct tp_session_save_result tp_session_save_result;

tp_status tp_recovery_store_create(const char *root, tp_id128 journal_key,
                                   tp_recovery_store **out, tp_error *err);
void tp_recovery_store_destroy(tp_recovery_store *store);
tp_status tp_recovery_store_create_live(tp_recovery_store *store,
                                        const char *journal_path,
                                        tp_recovery_live **out, tp_error *err);
tp_status tp_recovery_live_update_metadata(tp_recovery_live *live,
                                           const tp_recovery_metadata *metadata,
                                           tp_error *err);
tp_status tp_recovery_store_scan(tp_recovery_store *store, const char *live_slot,
                                 tp_recovery_candidates *out, tp_error *err);
tp_status tp_recovery_store_claim(tp_recovery_store *store,
                                  const char *journal_path,
                                  tp_recovery_claim **out, tp_error *err);
void tp_recovery_claim_release(tp_recovery_claim *claim);
tp_status tp_recovery_claim_recover(tp_recovery_claim *claim,
                                    tp_recovery_owned_candidate **out,
                                    tp_error *err);
tp_status tp_recovery_candidate_create_resolution(
    tp_recovery_owned_candidate *candidate, const tp_rng *rng,
    tp_recovery_resolution **out, tp_error *err);
tp_status tp_recovery_resolution_save_original(
    tp_recovery_resolution *resolution, tp_session_save_result *receipt,
    tp_error *err);
tp_status tp_recovery_resolution_save_as(
    tp_recovery_resolution *resolution, const char *target_path,
    tp_session_save_result *receipt, tp_error *err);
tp_status tp_recovery_resolution_finalize(
    tp_recovery_resolution *resolution,
    const tp_session_save_result *receipt, bool *journal_deleted,
    tp_error *err);
void tp_recovery_resolution_cancel(tp_recovery_resolution *resolution);
void tp_recovery_resolution_destroy(tp_recovery_resolution *resolution);
tp_status tp_recovery_claim_discard(tp_recovery_claim *claim, tp_error *err);

/* Test-only deterministic ranking seam used by the legacy GUI parity corpus. */
void tp_recovery__test_candidate_insert(tp_recovery_candidates *out,
                                        const tp_recovery_candidate *candidate);

/* Frontend selftest seams. They preserve the public no-owner boundary: the
 * held claim and verify fault remain entirely inside recovery core. */
bool tp_recovery__test_hold_foreign_lock(
    const char *root, tp_id128 journal_key, const char *journal_path);
void tp_recovery__test_release_foreign_lock(void);
void tp_recovery__test_fail_next_resolve_verify(void);
/* Forces the next live-retire cleanup to fail after Save As has published its
 * destination. This cross-platform seam verifies post-publication semantics. */
void tp_recovery__test_fail_next_live_retire_cleanup(void);
tp_status tp_recovery__test_session_attach_at(
    const char *root, tp_id128 journal_key, const char *journal_path,
    tp_session *session, const tp_recovery_metadata *metadata, tp_error *err);
#ifndef _WIN32
/* Forces the next pinned-journal quarantine unlink to fail. The production
 * cleanup path must restore, or leave a scan-visible quarantine candidate. */
void tp_recovery__test_fail_next_quarantine_unlink(void);
#endif
tp_status tp_recovery__test_craft_metadata_journal(
    const char *path, tp_id128 key, int64_t timestamp,
    const char *project_path, const char *project_name, tp_error *err);
tp_status tp_recovery__test_peek_candidate(
    const char *path, tp_recovery_candidate *out, tp_error *err);

#endif
