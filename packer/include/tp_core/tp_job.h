#ifndef TP_CORE_TP_JOB_H
#define TP_CORE_TP_JOB_H

#include <stdbool.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_pack_result.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_source_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_session_job_result_handle
    tp_session_job_result_handle;

typedef struct tp_pack_job_request {
    tp_id128 atlas_id;
    const char *work_dir;
    /* Host-owned admission identity. Zero asks the session to assign a
     * monotonic request id; generation zero remains data. */
    uint64_t session_instance_generation;
    uint64_t request_id;
    /* NULL runs the atlas' native settings. A non-NULL exporter id produces
     * the capability-clamped preview pack for that target format. */
    const char *preview_exporter_id;
} tp_pack_job_request;

/* Export is an external side-effect command, not a derived job or model
 * transaction. It uses the same session-owned runtime handle only for ordering,
 * progress, and cancellation; already-published files are not rolled back. */
typedef struct tp_export_command_request {
    const char *work_dir;
    uint64_t session_instance_generation;
    uint64_t request_id;
    /* Nil exports every eligible atlas. A stable non-nil ID restricts the
     * command to that atlas; frontends never pass a mutable collection index. */
    tp_id128 atlas_id;
    const char *target_exporter_id;
    const char *out_dir;
    bool dry_run;
} tp_export_command_request;

typedef struct tp_refresh_job_request {
    uint64_t session_instance_generation;
    uint64_t request_id;
} tp_refresh_job_request;

struct tp_pack_image_hash_cache;

typedef enum tp_pack_freshness {
    TP_PACK_FRESHNESS_STALE = 0,
    TP_PACK_FRESHNESS_CURRENT
} tp_pack_freshness;

typedef enum tp_pack_freshness_reason {
    TP_PACK_FRESHNESS_RESULT_HASH_NIL = 0,
    TP_PACK_FRESHNESS_MATCH,
    TP_PACK_FRESHNESS_HASH_MISMATCH,
    TP_PACK_FRESHNESS_PROBE_ERROR,
    TP_PACK_FRESHNESS_TOKEN_CHANGED
} tp_pack_freshness_reason;

typedef struct tp_session_pack_job_result {
    tp_id128 atlas_id;
    tp_arena *arena;
    tp_result *result;
    int missing_sources;
    tp_session_input_token input_token_at_start;
    /* Canonical semantic pack_input_hash of the job's immutable start inputs
     * (docs/architecture/jobs-pack-and-cache.md). Freshness is `result_hash ==
     * current_pack_input_hash`; the memory cache is keyed by it. Nil only if the
     * hash could not be computed (e.g. an unreadable source) -- a nil hash reads
     * as "always stale" and never matches a current hash. */
    tp_id128 pack_input_hash;
    /* Core-owned freshness verdict computed on the Pack worker before terminal
     * publication. `freshness_token` identifies the immutable session snapshot
     * used by that probe; clients must pass their current token through
     * tp_session_pack_result_freshness so a later edit fails closed as stale. */
    tp_id128 current_pack_input_hash;
    tp_session_input_token freshness_token;
    tp_pack_freshness freshness;
    tp_pack_freshness_reason freshness_reason;
    tp_status freshness_status;
    tp_error freshness_error;
    char preview_exporter_id[TP_EXPORTER_ID_MAX];
} tp_session_pack_job_result;

typedef struct tp_session_export_job_result {
    /* The one owned Export result. Every aggregate and publication fact lives
     * in this immutable host-assembled value; there is no parallel summary. */
    struct tp_export_command_report *report;
} tp_session_export_job_result;

typedef struct tp_export_command_atlas_report {
    tp_id128 id;
    const char *name;
    int sprite_count;
    int missing_sources;
    const char *skip_notice_id;
    const char *note;
    tp_status status;
    tp_error error;
    bool outcome_received;
    bool report_present;
    tp_export_report report;
    tp_export_notices notices;
} tp_export_command_atlas_report;

typedef struct tp_export_command_report {
    bool dry_run;
    tp_export_command_atlas_report *atlases;
    int atlas_count;
    int targets_ok;
    int targets_failed;
    int files_written;
    int notices;
    int atlases_ok;
    int atlases_failed;
    int atlases_skipped;
    char first_error[256];
    bool partial_publication;
    bool publication_uncertain;
    bool had_pack_failure;
    bool had_export_failure;
} tp_export_command_report;

typedef struct tp_session_refresh_job_result {
    int added;
    int removed;
    int changed;
    int unavailable;
    /* Session adopts this immutable replacement before publishing the
     * terminal completion to a client; client receipts observe NULL. */
    tp_source_runtime_projection *projection;
} tp_session_refresh_job_result;

struct tp_session_job_result {
    tp_session_job_kind kind;
    tp_session_job_state state;
    tp_session_job_rejection rejection;
    tp_status status;
    tp_error error;
    double elapsed_ms;
    union {
        tp_session_pack_job_result pack;
        tp_session_export_job_result export_result;
        tp_session_refresh_job_result refresh;
    };
    /* Private refcounted owner for this result receipt. Callers must treat it
     * as opaque and release through result_destroy. */
    tp_session_job_result_handle *_owner;
};

/* One concrete task may be active per session. The session owns its
 * handle/lifetime; algorithms and worker implementation stay in tp_build. */
tp_status tp_session_pack_job_start(tp_session *session,
                                    const tp_pack_job_request *request,
                                    tp_error *err);
tp_status tp_session_export_start(tp_session *session,
                                  const tp_export_command_request *request,
                                  tp_error *err);
/* Synchronous saved-file facade over the same immutable request builder,
 * worker controller, and report assembler used by live session jobs. It clones
 * the supplied read-only snapshot into a detached session and therefore never
 * acquires the project writer lease. */
tp_status tp_export_command_run_snapshot(
    const struct tp_session_snapshot *snapshot,
    const tp_export_command_request *request,
    tp_session_job_result *out, tp_error *err);
tp_status tp_session_refresh_start(tp_session *session,
                                   const tp_refresh_job_request *request,
                                   tp_error *err);
bool tp_session_job_active(const tp_session *session);
/* Accepts cancellation only before the terminal-boundary claim. Export's
 * worker emits that claim immediately after its final eligible writer returns;
 * the host linearizes it when the next tp_session_update admits the typed
 * boundary event. A cancellation already accepted by the host keeps ownership;
 * repeated requests and requests after admission are rejected. */
tp_status tp_session_job_cancel(tp_session *session, tp_error *err);
void tp_session_job_result_destroy(tp_session_job_result *result);
/* Releases request and process storage retained by a taken result while
 * preserving its result data. Only a result returned by tp_session_update can
 * be compacted; owner-less calls are no-ops. Long-lived receipts should be
 * compacted before storage. Idempotent. */
void tp_session_job_result_compact(tp_session_job_result *result);

/* Returns the typed core freshness verdict for a completed Pack result. A live
 * token newer than the worker's verified snapshot always yields STALE with
 * TOKEN_CHANGED; clients do not implement their own nil/error fallback rules. */
tp_pack_freshness tp_session_pack_result_freshness(
    const tp_session_pack_job_result *result,
    tp_session_input_token live_token,
    tp_pack_freshness_reason *out_reason);

/* Recomputes the CURRENT pack_input_hash for `atlas_id` from the live session's
 * immutable snapshot, WITHOUT starting a job
 * (docs/architecture/jobs-pack-and-cache.md). This is the freshness/selection
 * primitive: compare it against a
 * completed result's hash to decide current-vs-stale, and probe the memory cache
 * with it after Undo/Redo. `cache` may be NULL (decode every call) or a
 * session-lifetime tp_pack_image_hash_cache for cheap repeats; caching never
 * changes the hash value. On a source that cannot be read the underlying status
 * propagates and *out_hash is left nil. Never auto-packs. */
tp_status tp_session_pack_input_hash(tp_session *session, tp_id128 atlas_id,
                                     struct tp_pack_image_hash_cache *cache,
                                     tp_id128 *out_hash, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif
