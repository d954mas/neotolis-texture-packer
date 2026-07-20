#include "gui_pack_internal.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "log/nt_log.h"

#include "tp_core/tp_error.h"
#include "tp_core/tp_scan.h"

#include "gui_paths.h"
#include "gui_project.h"

#define GUI_PACK_MAX_ATLASES 64

typedef struct {
    char work_dir[TP_IDENTITY_PATH_MAX];
    bool work_dir_ready;
    gui_pack_async_kind debug_busy;
    bool cancel_requested;
} gui_pack_job_state;

static gui_pack_job_state s_adapter;

static tp_session *job_session(void) {
    return gui_project_session_for_jobs();
}

static bool job_active(void) {
    tp_session *session = job_session();
    return session && tp_session_job_active(session);
}

static int current_atlas_index(tp_id128 atlas_id) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int i = 0; i < count; i++) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, i);
        if (atlas && tp_id128_eq(atlas->id, atlas_id)) {
            return i;
        }
    }
    return -1;
}

static bool report_job_start(tp_status status, const tp_error *error,
                             char *err, size_t err_cap) {
    if (status == TP_STATUS_OK) {
        s_adapter.cancel_requested = false;
        return true;
    }
    if (err && err_cap > 0U) {
        (void)snprintf(err, err_cap, "%s",
                       error && error->msg[0] ? error->msg
                                              : tp_status_str(status));
    }
    return false;
}

static bool require_work_dir(char *err, size_t err_cap) {
    if (s_adapter.work_dir_ready) {
        return true;
    }
    if (err && err_cap > 0U) {
        (void)snprintf(err, err_cap,
                       "pack work directory is unavailable or exceeds the supported path limit");
    }
    return false;
}

static bool input_changed_since(tp_session_input_token token) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return !snapshot || !tp_session_input_token_equal(
                            tp_session_snapshot_input_token(snapshot), token);
}

bool gui_pack_init(const char *work_dir) {
    s_adapter.work_dir_ready = false;
    if (!gui_paths_copy_normalized(work_dir ? work_dir : ".",
                                   s_adapter.work_dir,
                                   sizeof s_adapter.work_dir)) {
        return false;
    }
    tp_mkdirs(s_adapter.work_dir);
    s_adapter.work_dir_ready = tp_scan_is_dir(s_adapter.work_dir);
    if (!s_adapter.work_dir_ready) {
        s_adapter.work_dir[0] = '\0';
    }
    return s_adapter.work_dir_ready;
}

bool gui_pack_async_start(int atlas_index, char *err, size_t err_cap) {
    if (!require_work_dir(err, err_cap)) {
        return false;
    }
    if (job_active()) {
        if (err) {
            (void)snprintf(err, err_cap,
                           "busy -- a pack or export is already running");
        }
        return false;
    }
    if (atlas_index < 0 || atlas_index >= GUI_PACK_MAX_ATLASES) {
        if (err) {
            (void)snprintf(err, err_cap, "atlas index out of range");
        }
        return false;
    }
    gui_project_invalidate_sources();
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!atlas) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }

    tp_error error = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas->id,
        .work_dir = s_adapter.work_dir,
        .preview_exporter_id = NULL,
    };
    return report_job_start(
        tp_session_pack_job_start(job_session(), &request, &error), &error,
        err, err_cap);
}

static bool export_start(tp_id128 atlas_id, char *err, size_t err_cap) {
    if (!require_work_dir(err, err_cap)) {
        return false;
    }
    if (job_active()) {
        if (err) {
            (void)snprintf(err, err_cap,
                           "busy -- a pack or export is already running");
        }
        return false;
    }
    gui_project_invalidate_sources();
    tp_session *session = job_session();
    if (!session) {
        if (err) {
            (void)snprintf(err, err_cap, "no project");
        }
        return false;
    }

    tp_error error = {{0}};
    const tp_export_command_request request = {
        .work_dir = s_adapter.work_dir,
        .atlas_id = atlas_id,
    };
    return report_job_start(tp_session_export_start(session, &request, &error),
                            &error, err, err_cap);
}

bool gui_pack_export_async_start(char *err, size_t err_cap) {
    return export_start(tp_id128_nil(), err, err_cap);
}

gui_pack_done gui_pack_poll(gui_pack_result_info *out) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    tp_session *session = job_session();
    if (!session || !tp_session_job_active(session)) {
        return GUI_PACK_DONE_NONE;
    }
    tp_error error = {{0}};
    tp_session_job_progress progress;
    if (tp_session_job_poll(session, &progress, &error) != TP_STATUS_OK ||
        progress.state == TP_SESSION_JOB_RUNNING) {
        return GUI_PACK_DONE_NONE;
    }
    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    if (tp_session_job_take_result(session, &result, &error) != TP_STATUS_OK) {
        if (out) {
            (void)snprintf(out->err, sizeof out->err, "%s",
                           error.msg[0] ? error.msg
                                        : "could not take job result");
        }
        return progress.kind == TP_SESSION_JOB_EXPORT
                   ? GUI_PACK_DONE_EXPORT_FAIL
                   : GUI_PACK_DONE_PACK_FAIL;
    }
    const bool cancelled = s_adapter.cancel_requested ||
                           result.state == TP_SESSION_JOB_CANCELLED;
    const bool preview = result.kind == TP_SESSION_JOB_PACK &&
                         result.pack.preview_exporter_id[0] != '\0';
    gui_pack_done done = GUI_PACK_DONE_NONE;

    if (result.kind == TP_SESSION_JOB_PACK) {
        if (result.state == TP_SESSION_JOB_SUCCEEDED &&
            result.status == TP_STATUS_OK && result.pack.result) {
            const int atlas_index = current_atlas_index(result.pack.atlas_id);
            if (cancelled || atlas_index < 0) {
                done = preview ? GUI_PACK_DONE_PREVIEW_CANCELLED
                               : GUI_PACK_DONE_PACK_CANCELLED;
            } else if (preview) {
                const bool input_changed =
                    input_changed_since(result.pack.input_token_at_start);
                if (out) {
                    out->atlas_index = atlas_index;
                    out->ms = result.elapsed_ms;
                    out->input_changed = input_changed;
                }
                if (!input_changed) {
                    gui_pack_preview_publish(&result.pack, atlas_index,
                                             result.elapsed_ms);
                }
                done = GUI_PACK_DONE_PREVIEW_OK;
            } else if (!gui_pack_publish_native(
                           &result.pack, atlas_index, result.elapsed_ms, out)) {
                done = GUI_PACK_DONE_PACK_FAIL;
            } else {
                if (out) {
                    out->atlas_index = atlas_index;
                    out->ms = result.elapsed_ms;
                    out->missing = result.pack.missing_sources;
                    out->input_changed = input_changed_since(
                        result.pack.input_token_at_start);
                    if (result.pack.missing_sources > 0) {
                        (void)snprintf(out->note, sizeof out->note,
                                       "%d missing file(s) skipped",
                                       result.pack.missing_sources);
                    }
                }
                done = GUI_PACK_DONE_PACK_OK;
            }
        } else if (cancelled) {
            done = preview ? GUI_PACK_DONE_PREVIEW_CANCELLED
                           : GUI_PACK_DONE_PACK_CANCELLED;
        } else {
            if (out) {
                (void)snprintf(out->err, sizeof out->err, "%s",
                               result.error.msg[0]
                                   ? result.error.msg
                                   : tp_status_str(result.status));
            }
            done = preview ? GUI_PACK_DONE_PREVIEW_FAIL
                           : GUI_PACK_DONE_PACK_FAIL;
        }
    } else {
        if (out) {
            out->targets = result.export_result.targets;
            out->notices = result.export_result.notices;
            out->atlases_ok = result.export_result.atlases_ok;
            out->atlases_fail = result.export_result.atlases_failed;
            (void)snprintf(out->err, sizeof out->err, "%s",
                           result.export_result.first_error);
        }
        if (cancelled) {
            done = GUI_PACK_DONE_EXPORT_CANCELLED;
        } else if (result.state == TP_SESSION_JOB_FAILED ||
                   result.export_result.atlases_failed > 0) {
            done = GUI_PACK_DONE_EXPORT_FAIL;
        } else {
            done = GUI_PACK_DONE_EXPORT_OK;
        }
        nt_log_info("gui_pack(async): export %d target(s), %d ok, %d fail, %d notice(s) in %.1f ms%s%s",
                    result.export_result.targets,
                    result.export_result.atlases_ok,
                    result.export_result.atlases_failed,
                    result.export_result.notices, result.elapsed_ms,
                    result.export_result.first_error[0] ? ": " : "",
                    result.export_result.first_error);
    }

    tp_session_job_result_destroy(&result);
    s_adapter.cancel_requested = false;
    if (out) {
        out->kind = done;
    }
    return done;
}

static gui_pack_done wait_for_job(gui_pack_result_info *out) {
    for (;;) {
        const gui_pack_done done = gui_pack_poll(out);
        if (done != GUI_PACK_DONE_NONE) {
            return done;
        }
        if (!job_active()) {
            if (out) {
                (void)snprintf(out->err, sizeof out->err,
                               "session job ended without a typed result");
            }
            return GUI_PACK_DONE_PACK_FAIL;
        }
#ifdef _WIN32
        Sleep(1);
#else
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&pause, NULL);
#endif
    }
}

bool gui_pack_async_busy(void) {
    return job_active() || s_adapter.debug_busy != GUI_PACK_ASYNC_NONE;
}

bool gui_pack_worker_active(void) {
    return job_active();
}

gui_pack_async_kind gui_pack_async_active_kind(void) {
    if (s_adapter.debug_busy != GUI_PACK_ASYNC_NONE) {
        return s_adapter.debug_busy;
    }
    tp_session *session = job_session();
    if (!session || !tp_session_job_active(session)) {
        return GUI_PACK_ASYNC_NONE;
    }
    tp_session_job_progress progress;
    return tp_session_job_poll(session, &progress, NULL) == TP_STATUS_OK &&
                   progress.kind == TP_SESSION_JOB_EXPORT
               ? GUI_PACK_ASYNC_EXPORT
               : GUI_PACK_ASYNC_PACK;
}

double gui_pack_async_elapsed_sec(void) {
    if (s_adapter.debug_busy != GUI_PACK_ASYNC_NONE) {
        return 3.2;
    }
    tp_session *session = job_session();
    tp_session_job_progress progress;
    return session &&
                   tp_session_job_poll(session, &progress, NULL) == TP_STATUS_OK
               ? progress.elapsed_ms / 1000.0
               : 0.0;
}

void gui_pack_export_progress(int *cur, int *total) {
    if (s_adapter.debug_busy == GUI_PACK_ASYNC_EXPORT) {
        if (cur) {
            *cur = 2;
        }
        if (total) {
            *total = 3;
        }
        return;
    }
    tp_session_job_progress progress;
    tp_session *session = job_session();
    const bool have = session &&
                      tp_session_job_poll(session, &progress, NULL) ==
                          TP_STATUS_OK;
    if (cur) {
        *cur = have ? progress.current : 0;
    }
    if (total) {
        *total = have ? progress.total : 0;
    }
}

void gui_pack_async_cancel(void) {
    tp_session *session = job_session();
    if (session && tp_session_job_cancel(session, NULL) == TP_STATUS_OK) {
        s_adapter.cancel_requested = true;
    }
}

bool gui_pack_async_cancelling(void) {
    return job_active() && s_adapter.cancel_requested;
}

void gui_pack_debug_force_busy(gui_pack_async_kind kind) {
    s_adapter.debug_busy = kind;
}

bool gui_pack_preview_blocking(int atlas_index, const char *exporter_id,
                               char *err, size_t err_cap) {
    if (!gui_pack_preview_async_start(atlas_index, exporter_id, err, err_cap)) {
        return false;
    }
    gui_pack_result_info info;
    const gui_pack_done done = wait_for_job(&info);
    if (done == GUI_PACK_DONE_PREVIEW_OK && info.atlas_index == atlas_index &&
        !info.input_changed) {
        return true;
    }
    if (err && err_cap > 0U) {
        (void)snprintf(err, err_cap, "%s",
                       info.err[0] ? info.err
                                   : "preview pack did not complete");
    }
    return false;
}

bool gui_pack_preview_async_start(int atlas_index, const char *exporter_id,
                                  char *err, size_t err_cap) {
    if (!require_work_dir(err, err_cap)) {
        return false;
    }
    if (job_active()) {
        if (err) {
            (void)snprintf(err, err_cap,
                           "busy -- a pack or export is already running");
        }
        return false;
    }
    gui_project_invalidate_sources();
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!atlas) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }
    tp_error error = {{0}};
    const tp_pack_job_request request = {
        .atlas_id = atlas->id,
        .work_dir = s_adapter.work_dir,
        .preview_exporter_id = exporter_id,
    };
    return report_job_start(
        tp_session_pack_job_start(job_session(), &request, &error), &error,
        err, err_cap);
}

void gui_pack_shutdown(void) {
    if (job_active()) {
        gui_pack_async_cancel();
        while (job_active()) {
            if (gui_pack_poll(NULL) != GUI_PACK_DONE_NONE) {
                break;
            }
#ifdef _WIN32
            Sleep(1);
#else
            const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
            (void)nanosleep(&pause, NULL);
#endif
        }
    }
    gui_pack_clear(-1);
    gui_pack_preview_clear();
}

bool gui_pack_atlas(int atlas_index, double *out_ms, char *err,
                    size_t err_cap, char *notice, size_t notice_cap) {
    if (!gui_pack_async_start(atlas_index, err, err_cap)) {
        return false;
    }
    gui_pack_result_info info;
    const gui_pack_done done = wait_for_job(&info);
    if (done != GUI_PACK_DONE_PACK_OK || info.atlas_index != atlas_index) {
        if (err && err_cap > 0U) {
            (void)snprintf(err, err_cap, "%s",
                           info.err[0] ? info.err : "pack did not complete");
        }
        return false;
    }
    if (out_ms) {
        *out_ms = info.ms;
    }
    if (notice && notice_cap > 0U) {
        (void)snprintf(notice, notice_cap, "%s", info.note);
    }
    return true;
}

bool gui_pack_export(int atlas_index, int *out_targets, int *out_notices,
                     char *err, size_t err_cap, char *notice,
                     size_t notice_cap) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!atlas) {
        if (err) {
            (void)snprintf(err, err_cap, "no such atlas");
        }
        return false;
    }
    if (!export_start(atlas->id, err, err_cap)) {
        return false;
    }
    gui_pack_result_info info;
    const gui_pack_done done = wait_for_job(&info);
    if (out_targets) {
        *out_targets = info.targets;
    }
    if (out_notices) {
        *out_notices = info.notices;
    }
    if (notice && notice_cap > 0U) {
        if (info.notices > 0) {
            (void)snprintf(notice, notice_cap, "%d metadata notice(s)",
                           info.notices);
        } else {
            notice[0] = '\0';
        }
    }
    if (done != GUI_PACK_DONE_EXPORT_OK) {
        if (err && err_cap > 0U) {
            (void)snprintf(err, err_cap, "%s",
                           info.err[0] ? info.err
                                       : "export did not complete");
        }
        return false;
    }
    return true;
}
