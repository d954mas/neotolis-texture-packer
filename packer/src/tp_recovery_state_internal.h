#ifndef TP_RECOVERY_STATE_INTERNAL_H
#define TP_RECOVERY_STATE_INTERNAL_H

#include "tp_core/tp_journal.h"
#include "tp_core/tp_project_lease.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_recovery_backend_types_internal.h"
#include "tp_recovery_internal.h"

#define TP_RECOVERY_LOCK_SUFFIX ".lock"
#define TP_RECOVERY_LOCK_PATH_MAX \
    (TP_IDENTITY_PATH_MAX + sizeof(TP_RECOVERY_LOCK_SUFFIX))

struct tp_recovery_store {
    char root[TP_IDENTITY_PATH_MAX];
    tp_id128 journal_key;
};

struct tp_recovery_claim {
    tp_recovery_lock_pin lock;
    char lock_path[TP_RECOVERY_LOCK_PATH_MAX];
    char journal_path[TP_IDENTITY_PATH_MAX];
    tp_id128 journal_key;
    tp_recovery_owned_candidate *candidate;
    tp_recovery_resolution *resolution;
};

struct tp_recovery_owned_candidate {
    tp_recovery_claim *owner;
    tp_project *project;
    tp_journal_meta metadata;
    tp_journal_recovery_status recovery_status;
    tp_id128 recovery_token;
    bool has_metadata;
    bool has_recovery_token;
    tp_recovery_file_pin journal_pin;
};

struct tp_recovery_resolution {
    tp_recovery_owned_candidate *candidate;
    tp_session *session;
    tp_project_lease *project_lease;
    tp_session_save_result last_receipt;
    bool has_receipt;
};

struct tp_recovery_live {
    char journal_path[TP_IDENTITY_PATH_MAX];
    tp_id128 journal_key;
    tp_recovery_lock_pin lock;
    char lock_path[TP_RECOVERY_LOCK_PATH_MAX];
    tp_recovery_file_pin journal_pin;
    tp_model *attached_model;
    int64_t metadata_timestamp;
    char metadata_name[256];
    bool healthy;
    bool finished;
    tp_status terminal_status;
};

bool tp_recovery__has_journal_suffix(const char *name);
const char *tp_recovery__path_basename(const char *path);
tp_status tp_recovery__store_journal_path(
    const tp_recovery_store *store, const char *input,
    char *journal, size_t journal_cap, tp_error *err);
tp_status tp_recovery__lock_path_for(
    const tp_recovery_store *store, const char *journal_input,
    char *journal, size_t journal_cap,
    char *lock, size_t lock_cap, tp_error *err);
tp_status tp_recovery__live_slot_generate(
    const tp_recovery_store *store, const tp_rng *rng,
    char *out, size_t out_cap, tp_error *err);

#endif /* TP_RECOVERY_STATE_INTERNAL_H */
