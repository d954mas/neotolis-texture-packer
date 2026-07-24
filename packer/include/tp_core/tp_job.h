#ifndef TP_CORE_TP_JOB_H
#define TP_CORE_TP_JOB_H

#include <stdbool.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tp_session_job_kind {
    TP_SESSION_JOB_NONE = 0,
    TP_SESSION_JOB_PACK,
    TP_SESSION_JOB_EXPORT
} tp_session_job_kind;

typedef enum tp_session_job_state {
    TP_SESSION_JOB_RUNNING = 1,
    TP_SESSION_JOB_SUCCEEDED,
    TP_SESSION_JOB_FAILED,
    TP_SESSION_JOB_CANCELLED
} tp_session_job_state;

typedef struct tp_pack_job_request {
    tp_id128 atlas_id;
    const char *work_dir;
    /* NULL runs the atlas' native settings. A non-NULL exporter id produces
     * the capability-clamped preview pack for that target format. */
    const char *preview_exporter_id;
} tp_pack_job_request;

/* Export is an external side-effect command, not a derived job or model
 * transaction. It uses the same session-owned runtime handle only for ordering,
 * progress, and cancellation; already-published files are not rolled back. */
typedef struct tp_export_command_request {
    const char *work_dir;
    /* Nil exports every eligible atlas. A stable non-nil ID restricts the
     * command to that atlas; frontends never pass a mutable collection index. */
    tp_id128 atlas_id;
} tp_export_command_request;

typedef struct tp_session_job_progress {
    tp_session_job_kind kind;
    tp_session_job_state state;
    int current;
    int total;
    double elapsed_ms;
} tp_session_job_progress;

typedef struct tp_session_pack_job_result {
    tp_id128 atlas_id;
    tp_arena *arena;
    tp_result *result;
    int missing_sources;
    tp_session_input_token input_token_at_start;
    /* Canonical semantic pack_input_hash of the job's immutable start inputs
     * (master spec §10.2, decision 0004). Freshness is `result_hash ==
     * current_pack_input_hash`; the memory cache is keyed by it. Nil only if the
     * hash could not be computed (e.g. an unreadable source) -- a nil hash reads
     * as "always stale" and never matches a current hash. */
    tp_id128 pack_input_hash;
    char preview_exporter_id[TP_EXPORTER_ID_MAX];
} tp_session_pack_job_result;

typedef struct tp_session_export_job_result {
    int targets;
    int notices;
    int atlases_ok;
    int atlases_failed;
    char first_error[256];
    /* Selected atlases with enabled targets but no usable input images. */
    int atlases_skipped;
} tp_session_export_job_result;

typedef struct tp_session_job_result {
    tp_session_job_kind kind;
    tp_session_job_state state;
    tp_status status;
    tp_error error;
    double elapsed_ms;
    union {
        tp_session_pack_job_result pack;
        tp_session_export_job_result export_result;
    };
} tp_session_job_result;

/* One concrete derived job may be active per session. The session owns its
 * handle/lifetime; algorithms and worker implementation stay in tp_build. */
tp_status tp_session_pack_job_start(tp_session *session,
                                    const tp_pack_job_request *request,
                                    tp_error *err);
tp_status tp_session_export_start(tp_session *session,
                                  const tp_export_command_request *request,
                                  tp_error *err);
bool tp_session_job_active(const tp_session *session);
tp_status tp_session_job_poll(const tp_session *session,
                              tp_session_job_progress *out, tp_error *err);
/* Accepts cancellation only before terminal claim. Repeated or late requests
 * return TP_STATUS_INVALID_ARGUMENT. A completed Export remains successful when
 * its final outputs committed before the accepted request was observed. */
tp_status tp_session_job_cancel(tp_session *session, tp_error *err);
/* Succeeds only after poll reports a terminal state. Transfers a successful
 * Pack arena/result to `out` and releases the session-owned job handle. */
tp_status tp_session_job_take_result(tp_session *session,
                                     tp_session_job_result *out,
                                     tp_error *err);
void tp_session_job_result_destroy(tp_session_job_result *result);

/* Recomputes the CURRENT pack_input_hash for `atlas_id` from the live session's
 * immutable snapshot, WITHOUT starting a job (master spec §10.2-10.3, decision
 * 0004). This is the freshness/selection primitive: compare it against a
 * completed result's hash to decide current-vs-stale, and probe the memory cache
 * with it after Undo/Redo. `cache` may be NULL (decode every call) or a
 * session-lifetime tp_pack_image_hash_cache for cheap repeats; caching never
 * changes the hash value. On a source that cannot be read the underlying status
 * propagates and *out_hash is left nil. Never auto-packs. */
struct tp_pack_image_hash_cache;
tp_status tp_session_pack_input_hash(tp_session *session, tp_id128 atlas_id,
                                     struct tp_pack_image_hash_cache *cache,
                                     tp_id128 *out_hash, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif
