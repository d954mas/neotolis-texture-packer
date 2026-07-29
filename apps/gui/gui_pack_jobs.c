#include "gui_pack_internal.h"

#include <stdio.h>
#include <string.h>

#include "log/nt_log.h"
#include "time/nt_time.h"

#include "core/nt_assert.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_scan.h"

#include "gui_paths.h"
#include "gui_project.h"

#define GUI_PACK_MAX_ATLASES 64

typedef struct {
    char work_dir[TP_IDENTITY_PATH_MAX];
    bool work_dir_ready;
    gui_pack_async_kind debug_busy;
    double started_at;
} gui_pack_job_state;

static gui_pack_job_state s_adapter;

bool gui_pack_format_export_cancelled(const gui_pack_result_info *info,
                                      char *out, size_t cap) {
    const bool uncertain = info && info->publication_uncertain;
    const bool partial = info && info->partial_publication;
    if (out && cap > 0U) {
        if (uncertain) {
            (void)snprintf(
                out, cap,
                "Export cancelled; output may be partially updated.");
        } else if (partial) {
            (void)snprintf(
                out, cap,
                "Export cancelled after publishing %d target(s) / %d file(s).",
                info->targets, info->files);
        } else {
            (void)snprintf(out, cap, "Export cancelled.");
        }
    }
    return uncertain || partial;
}

bool gui_pack_format_export_failed(const gui_pack_result_info *info,
                                   char *out, size_t cap) {
    const bool uncertain = info && info->publication_uncertain;
    if (out && cap > 0U) {
        if (uncertain) {
            (void)snprintf(
                out, cap,
                "Export failed; output may be partially updated. Exported %d "
                "target(s); %d atlas(es) failed%s%s",
                info->targets, info->atlases_fail,
                info->err[0] ? " -- " : "", info->err);
        } else {
            out[0] = '\0';
        }
    }
    return uncertain;
}

static const char *job_rejection_message(
    tp_session_job_rejection rejection) {
    switch (rejection) {
    case TP_SESSION_JOB_REJECTION_NONE:
        return "";
    case TP_SESSION_JOB_REJECTION_SUPERSEDED:
        return "job result was superseded";
    case TP_SESSION_JOB_REJECTION_CANCELLED:
        return "job was cancelled";
    case TP_SESSION_JOB_REJECTION_TARGET_DELETED:
        return "job target was deleted";
    case TP_SESSION_JOB_REJECTION_OLD_INSTANCE:
        return "job belongs to an old session instance";
    case TP_SESSION_JOB_REJECTION_DUPLICATE:
        return "duplicate job result";
    case TP_SESSION_JOB_REJECTION_SESSION_CLOSED:
        return "session closed before result admission";
    default:
        NT_ASSERT(false);
        return "unknown job rejection";
    }
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
        s_adapter.started_at = nt_time_now();
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

static bool current_observed_input_token(
    tp_session_input_token *out) {
    NT_ASSERT(out != NULL);
    return gui_project_observed_input_token(out);
}

static bool input_changed_since(tp_session_input_token token) {
    tp_session_input_token current = {0};
    return !current_observed_input_token(&current) ||
           !tp_session_input_token_equal(current, token);
}

static bool native_pack_input_changed_since(
    const tp_session_pack_job_result *pack) {
    tp_session_input_token live_token = {0};
    if (!pack ||
        !current_observed_input_token(&live_token)) {
        return true;
    }
    return tp_session_pack_result_freshness(pack, live_token, NULL) !=
           TP_PACK_FRESHNESS_CURRENT;
}

#ifdef TP_ENABLE_TEST_SEAMS
bool gui_pack__test_native_pack_input_changed_since(
    const tp_session_pack_job_result *pack) {
    return native_pack_input_changed_since(pack);
}
#endif

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
    if (gui_project_job_busy()) {
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
    return report_job_start(
        gui_project_job_enqueue_pack(
            atlas->id, s_adapter.work_dir,
            NULL, &error),
        &error,
        err, err_cap);
}

static bool export_start(tp_id128 atlas_id, char *err, size_t err_cap) {
    if (!require_work_dir(err, err_cap)) {
        return false;
    }
    if (gui_project_job_busy()) {
        if (err) {
            (void)snprintf(err, err_cap,
                           "busy -- a pack or export is already running");
        }
        return false;
    }
    gui_project_invalidate_sources();
    tp_error error = {{0}};
    return report_job_start(
        gui_project_job_enqueue_export(
            atlas_id, s_adapter.work_dir,
            &error),
        &error, err, err_cap);
}

bool gui_pack_export_async_start(char *err, size_t err_cap) {
    return export_start(tp_id128_nil(), err, err_cap);
}

gui_pack_done gui_pack_poll(gui_pack_result_info *out) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    /* Frame tick for the result store's background page compression (packet
     * S28): applies whatever the encoder finished since the last frame, which is
     * a mutex lock and a flag test per in-flight job. It rides this poll because
     * this is already the once-per-frame call into the pack adapter -- including
     * the drain loop the blocking adapters spin -- and the store must be pumped
     * from the one thread that owns it. */
    gui_pack__cold_pump();
    gui_host_completion completion = {0};
    if (!gui_project_host_take_completion(
            &completion)) {
        return GUI_PACK_DONE_NONE;
    }
    tp_session_job_result result =
        completion.result;
    completion.result =
        (tp_session_job_result){0};
    if (result.kind == TP_SESSION_JOB_NONE) {
        result.kind = completion.envelope.kind;
        result.state = completion.state;
        result.status = completion.status;
        result.error = completion.error;
    }
    const bool publish_result =
        completion.publish_result;
    const tp_session_job_rejection rejection =
        completion.rejection;
    const tp_status completion_status =
        completion.status;
    gui_host_completion_destroy(&completion);
    const bool cancelled =
        result.state == TP_SESSION_JOB_CANCELLED ||
        rejection ==
            TP_SESSION_JOB_REJECTION_CANCELLED;
    const bool rejected_failure =
        rejection != TP_SESSION_JOB_REJECTION_NONE &&
        rejection !=
            TP_SESSION_JOB_REJECTION_CANCELLED;
    if (out) {
        out->rejection = rejection;
        out->status = completion_status;
    }
    const bool preview = result.kind == TP_SESSION_JOB_PACK &&
                         result.pack.preview_exporter_id[0] != '\0';
    gui_pack_done done = GUI_PACK_DONE_NONE;

    if (result.kind == TP_SESSION_JOB_PACK) {
        if (rejected_failure) {
            if (out) {
                (void)snprintf(
                    out->err, sizeof out->err,
                    "%s",
                    job_rejection_message(
                        rejection));
            }
            done = preview
                       ? GUI_PACK_DONE_PREVIEW_FAIL
                       : GUI_PACK_DONE_PACK_FAIL;
        } else if (publish_result &&
            result.state == TP_SESSION_JOB_SUCCEEDED &&
            result.status == TP_STATUS_OK && result.pack.result) {
            const int atlas_index = current_atlas_index(result.pack.atlas_id);
            NT_ASSERT(atlas_index >= 0);
            if (out) {
                out->atlas_id = result.pack.atlas_id;
            }
            if (cancelled) {
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
                    if (!gui_pack_preview_publish(
                            &result, atlas_index, result.elapsed_ms, out)) {
                        done = GUI_PACK_DONE_PREVIEW_FAIL;
                    } else {
                        done = GUI_PACK_DONE_PREVIEW_OK;
                    }
                } else {
                    done = GUI_PACK_DONE_PREVIEW_OK;
                }
            } else if (!gui_pack_publish_native(
                           &result, atlas_index, result.elapsed_ms, out)) {
                done = GUI_PACK_DONE_PACK_FAIL;
            } else {
                if (out) {
                    out->atlas_index = atlas_index;
                    out->ms = result.elapsed_ms;
                    out->missing = result.pack.missing_sources;
                    out->input_changed =
                        native_pack_input_changed_since(&result.pack);
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
        NT_ASSERT(
            result.kind ==
            TP_SESSION_JOB_EXPORT);
        if (out) {
            out->targets = result.export_result.targets;
            out->files = result.export_result.files;
            out->notices = result.export_result.notices;
            out->atlases_ok = result.export_result.atlases_ok;
            out->atlases_fail = result.export_result.atlases_failed;
            out->atlases_skipped = result.export_result.atlases_skipped;
            out->partial_publication =
                result.export_result.partial_publication;
            out->publication_uncertain =
                result.export_result.publication_uncertain;
            (void)snprintf(out->err, sizeof out->err, "%s",
                           result.export_result.first_error);
        }
        if (rejected_failure) {
            if (out) {
                (void)snprintf(
                    out->err, sizeof out->err,
                    "%s",
                    job_rejection_message(
                        rejection));
            }
            done = GUI_PACK_DONE_EXPORT_FAIL;
        } else if (cancelled) {
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
    if (out) {
        out->kind = done;
    }
    return done;
}

static gui_pack_done drive_host_to_completion(
    gui_pack_result_info *out) {
    if (gui_project_frame_is_pinned()) {
        if (out) {
            (void)snprintf(
                out->err, sizeof out->err,
                "blocking Pack/Export is forbidden during a pinned frame");
        }
        return GUI_PACK_DONE_PACK_FAIL;
    }
    for (;;) {
        tp_error error = {{0}};
        const tp_status drain_status =
            gui_project_lifecycle_pump(
                NULL, &error);
        if (drain_status != TP_STATUS_OK) {
            if (out) {
                (void)snprintf(
                    out->err, sizeof out->err,
                    "%s",
                    error.msg[0]
                        ? error.msg
                        : tp_status_str(
                              drain_status));
            }
            return GUI_PACK_DONE_PACK_FAIL;
        }
        const tp_status observe_status =
            gui_project_frame_begin(&error);
        const gui_pack_done done =
            observe_status == TP_STATUS_OK
                ? gui_pack_poll(out)
                : GUI_PACK_DONE_NONE;
        gui_project_frame_end();
        if (observe_status != TP_STATUS_OK) {
            if (out) {
                (void)snprintf(
                    out->err, sizeof out->err,
                    "%s",
                    error.msg[0]
                        ? error.msg
                        : tp_status_str(
                              observe_status));
            }
            return GUI_PACK_DONE_PACK_FAIL;
        }
        if (done != GUI_PACK_DONE_NONE) {
            return done;
        }
        if (!gui_project_job_busy()) {
            if (out) {
                (void)snprintf(out->err, sizeof out->err,
                               "host job ended without a classified result");
            }
            return GUI_PACK_DONE_PACK_FAIL;
        }
        nt_time_sleep(0.001);
    }
}

bool gui_pack_async_busy(void) {
    return gui_project_job_busy() ||
           s_adapter.debug_busy !=
               GUI_PACK_ASYNC_NONE;
}

bool gui_pack_worker_active(void) {
    return gui_project_job_busy();
}

gui_pack_async_kind gui_pack_async_active_kind(void) {
    if (s_adapter.debug_busy != GUI_PACK_ASYNC_NONE) {
        return s_adapter.debug_busy;
    }
    const tp_session_job_observed_state state =
        gui_project_job_observed_state();
    const tp_session_job_kind kind =
        state.present && !state.terminal
            ? state.kind
            : gui_project_job_active_kind();
    return kind == TP_SESSION_JOB_EXPORT
               ? GUI_PACK_ASYNC_EXPORT
               : kind == TP_SESSION_JOB_PACK
                     ? GUI_PACK_ASYNC_PACK
                     : GUI_PACK_ASYNC_NONE;
}

double gui_pack_async_elapsed_sec(void) {
    if (s_adapter.debug_busy != GUI_PACK_ASYNC_NONE) {
        return 3.2;
    }
    return gui_project_job_busy() &&
                   s_adapter.started_at > 0.0
               ? nt_time_now() -
                     s_adapter.started_at
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
    const tp_session_job_observed_state state =
        gui_project_job_observed_state();
    const bool have =
        state.present && !state.terminal &&
        state.kind == TP_SESSION_JOB_EXPORT;
    if (cur) {
        *cur = have ? state.current : 0;
    }
    if (total) {
        *total = have ? state.total : 0;
    }
}

tp_status gui_pack_async_cancel(tp_error *err) {
    return gui_project_job_enqueue_cancel(err);
}

bool gui_pack_async_cancelling(void) {
    const tp_session_job_observed_state state =
        gui_project_job_observed_state();
    return state.present &&
           !state.terminal &&
           state.cancellation_requested;
}

#ifdef NTPACKER_GUI_DEV_SEAMS
/* Dev seam (--shot-packing / the self-test's busy-strip oracle): the ONLY writer of
 * s_adapter.debug_busy. Compiled out of the shipped binary, so the field stays zero
 * there and the synthetic busy presentation can never be reached at runtime. */
void gui_pack_debug_force_busy(gui_pack_async_kind kind) {
    s_adapter.debug_busy = kind;
}
#endif

bool gui_pack_preview_blocking(int atlas_index, const char *exporter_id,
                               char *err, size_t err_cap) {
    if (!gui_pack_preview_async_start(atlas_index, exporter_id, err, err_cap)) {
        return false;
    }
    gui_pack_result_info info;
    const gui_pack_done done =
        drive_host_to_completion(&info);
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
    if (gui_project_job_busy()) {
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
    return report_job_start(
        gui_project_job_enqueue_pack(
            atlas->id, s_adapter.work_dir,
            exporter_id, &error),
        &error,
        err, err_cap);
}

void gui_pack_shutdown(void) {
    gui_pack_clear(-1);
    gui_pack_preview_clear();
#ifdef TP_ENABLE_TEST_SEAMS
    /* The store's byte budget is a compile-time policy constant everywhere else;
     * a test that shrinks it owns it only for its own run, so shutdown -- which
     * every suite already calls between cases -- restores the shipped value. */
    gui_pack__test_set_result_budget(0U);
#endif
}

bool gui_pack_atlas(int atlas_index, double *out_ms, char *err,
                    size_t err_cap, char *notice, size_t notice_cap) {
    if (!gui_pack_async_start(atlas_index, err, err_cap)) {
        return false;
    }
    gui_pack_result_info info;
    const gui_pack_done done =
        drive_host_to_completion(&info);
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
    const gui_pack_done done =
        drive_host_to_completion(&info);
    if (out_targets) {
        *out_targets = info.targets;
    }
    if (out_notices) {
        *out_notices = info.notices;
    }
    if (notice && notice_cap > 0U) {
        if (info.atlases_skipped > 0) {
            (void)snprintf(notice, notice_cap,
                           "%d atlas(es) skipped (no usable images)",
                           info.atlases_skipped);
        } else if (info.notices > 0) {
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
