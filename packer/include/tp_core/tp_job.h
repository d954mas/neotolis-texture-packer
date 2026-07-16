#ifndef TP_CORE_TP_JOB_H
#define TP_CORE_TP_JOB_H

#include <stdbool.h>

#include "tp_core/tp_arena.h"
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
    char preview_exporter_id[64];
} tp_session_pack_job_result;

typedef struct tp_session_export_job_result {
    int targets;
    int notices;
    int atlases_ok;
    int atlases_failed;
    char first_error[256];
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
tp_status tp_session_job_cancel(tp_session *session, tp_error *err);
/* Succeeds only after poll reports a terminal state. Transfers a successful
 * Pack arena/result to `out` and releases the session-owned job handle. */
tp_status tp_session_job_take_result(tp_session *session,
                                     tp_session_job_result *out,
                                     tp_error *err);
void tp_session_job_result_destroy(tp_session_job_result *result);

#ifdef __cplusplus
}
#endif

#endif
