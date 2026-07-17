#ifndef TP_CORE_SRC_TP_SESSION_INTERNAL_H
#define TP_CORE_SRC_TP_SESSION_INTERNAL_H

#include "tp_core/tp_session.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_transaction.h"

/* struct tp_session embeds the platform lock by value, so the session family's
 * private header carries the lock type. Only tp_session.c and tp_session_snapshot.c
 * read the layout; the other includers keep using tp_session as an opaque handle. */
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct tp_recovery_live tp_recovery_live;
typedef struct tp_project_lease tp_project_lease;
typedef struct tp_session_owned_job tp_session_owned_job;

#define TP_SESSION_EVENT_CAPACITY 64U

/* Serialized single-writer session layout. Promoted out of tp_session.c so the
 * snapshot/query TU (tp_session_snapshot.c) can sample the committed immutable
 * fields under the shared gate. It stays private to the session family: no
 * frontend or protocol adapter includes this header. */
struct tp_session {
#if defined(_WIN32)
    SRWLOCK gate;
#else
    pthread_mutex_t gate;
#endif
    tp_model *model;
    tp_recovery_live *recovery_live;
    tp_project_lease *project_lease;
    tp_session_owned_job *active_job;
    tp_session_identity identity;
    tp_id128 saved_file_fingerprint;
    tp_id128 recovery_token;
    bool has_saved_file_fingerprint;
    bool has_recovery_token;
    bool recovery_healthy;
    bool recovery_required;
    bool discarded;
    uint64_t admission_sequence;
    uint64_t model_generation;
    uint64_t source_generation;
    uint64_t event_sequence;
    tp_session_event events[TP_SESSION_EVENT_CAPACITY];
    size_t event_count;
    size_t event_start;
};

/* The single-writer gate, defined in tp_session.c. Both the writer TU and the
 * snapshot TU acquire it to sample a consistent admission point. */
void gate_lock(const tp_session *session);
void gate_unlock(const tp_session *session);

/* Recovery-health predicate defined in tp_session.c beside the live seam. The
 * writer gates mutation on it and snapshot_create records it, so both TUs need it. */
bool recovery_is_healthy(const tp_session *session);

/* Internal Open/recovery construction seam. Always consumes `project`, including
 * every failure path, so ownership cannot become ambiguous under allocation/RNG
 * faults. Not available to frontend adapters. */
tp_status tp_session_adopt_owned(tp_project *project, const tp_rng *rng,
                                 tp_session **out, tp_error *err);

/* Recovery-store construction seam. Consumes `project` on every path and
 * binds successful save receipts to `recovery_token`; it does not attach a
 * live slot or publish startup events. */
tp_status tp_session_create_detached_recovery(tp_project *project,
                                              const tp_rng *rng,
                                              tp_id128 recovery_token,
                                              tp_session **out,
                                              tp_error *err);

/* Persistence-only command for a detached recovery session. Caller owns the
 * project lease and supplies the conditional original fingerprint when needed;
 * this command never installs a normal long-lived session identity/lease. */
tp_status tp_session_save_detached_recovery(
    tp_session *session, const char *path,
    const tp_id128 *expected_fingerprint,
    tp_session_save_result *result, tp_error *err);

/* Recovery-live integration seam: transfers `journal` to the session model on
 * success; on failure the caller still owns it. */
tp_status tp_session_attach_journal(tp_session *session, tp_journal *journal,
                                    tp_error *err);

tp_status tp_session_attach_recovery_live(
    tp_session *session, tp_recovery_live *live,
    const tp_recovery_metadata *metadata, tp_error *err);

/* Component-local benchmark seam. Counts successful snapshot DTO/storage
 * allocations and their live-byte high-water on the calling thread. The
 * project-clone component has its own existing counter. */
void tp_session__test_reset_snapshot_allocations(void);
size_t tp_session__test_snapshot_allocation_count(void);
size_t tp_session__test_snapshot_allocation_bytes(void);
void tp_session__test_fail_snapshot_allocation_after(size_t successful);

/* Recovery orchestration uses this only to complete the ownership transfer on
 * an attach error: accepted live handles remain session-owned even degraded. */
bool tp_session__owns_recovery_live(const tp_session *session,
                                    const tp_recovery_live *live);
/* Recovery orchestration preflight: reject duplicate attach before acquiring a
 * second filesystem lock. No owner pointer crosses the component boundary. */
bool tp_session__has_recovery_owner(const tp_session *session);
/* Recovery-core-only borrowed identity used synchronously to exclude/refuse
 * this process's live journal. The pointer never crosses a public boundary. */
const char *tp_session__recovery_journal_path(const tp_session *session);

/* Component-private bridge for packer-owned algorithms. The project pointer
 * never crosses into a frontend or public header. */
const tp_project *tp_session_snapshot_project_internal(
    const tp_session_snapshot *snapshot);

#endif /* TP_CORE_SRC_TP_SESSION_INTERNAL_H */
