#include "tp_core/tp_job.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "tinycthread.h"

#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_input.h"
#include "tp_job_owner_internal.h"

typedef struct tp_job_export_atlas {
    int index;
    int enabled_targets;
} tp_job_export_atlas;

typedef struct tp_live_job {
    tp_session_owned_job owner;
    tp_session_job_kind kind;
    tp_session *session;
    _Atomic int state;
    _Atomic bool cancelled;
    _Atomic int current;
    int total;
    bool thread_started;
    thrd_t thread;
    double started_ms;
    double elapsed_ms;
    tp_status status;
    tp_error error;

    tp_id128 atlas_id;
    uint64_t model_generation_at_start;
    tp_pack_input input;
    tp_pack_settings settings;
    char *atlas_name;
    char *work_dir;
    char preview_exporter_id[64];
    tp_arena *arena;
    tp_result *pack_result;

    tp_export_snapshot_job *export_job;
    tp_job_export_atlas *export_atlases;
    int export_atlas_count;
    tp_session_export_job_result export_result;
} tp_live_job;

static double job_now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
    struct timespec time;
    (void)clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec * 1000.0 + (double)time.tv_nsec / 1000000.0;
#endif
}

static char *job_strdup(const char *text) {
    if (!text) {
        return NULL;
    }
    const size_t size = strlen(text) + 1U;
    char *copy = malloc(size);
    if (copy) {
        memcpy(copy, text, size);
    }
    return copy;
}

static void job_cancel_owned(tp_session_owned_job *owned) {
    tp_live_job *job = (tp_live_job *)owned;
    atomic_store_explicit(&job->cancelled, true, memory_order_relaxed);
}

static void job_join(tp_live_job *job) {
    if (job->thread_started) {
        (void)thrd_join(job->thread, NULL);
        job->thread_started = false;
    }
}

static void job_destroy_owned(tp_session_owned_job *owned) {
    tp_live_job *job = (tp_live_job *)owned;
    if (!job) {
        return;
    }
    job_cancel_owned(owned);
    job_join(job);
    tp_pack_input_free(&job->input);
    free(job->atlas_name);
    free(job->work_dir);
    tp_arena_destroy(job->arena);
    tp_export_snapshot_job_destroy(job->export_job);
    free(job->export_atlases);
    free(job);
}

static tp_live_job *job_create(tp_session *session,
                               tp_session_job_kind kind) {
    tp_live_job *job = calloc(1U, sizeof *job);
    if (!job) {
        return NULL;
    }
    tp_session_owned_job_init(&job->owner, job_cancel_owned,
                              job_destroy_owned);
    job->kind = kind;
    job->session = session;
    job->status = TP_STATUS_OK;
    atomic_init(&job->state, TP_SESSION_JOB_RUNNING);
    atomic_init(&job->cancelled, false);
    atomic_init(&job->current, 0);
    return job;
}

static int pack_worker(void *context) {
    tp_live_job *job = context;
    tp_error error = {{0}};
    tp_result *result = NULL;
    const double start = job_now_ms();
    const tp_status status = tp_pack(&job->settings, job->arena, &result,
                                     &error);
    job->elapsed_ms = job_now_ms() - start;
    job->status = status;
    job->error = error;
    job->pack_result = result;
    atomic_store_explicit(&job->current, 1, memory_order_relaxed);
    const bool cancelled = atomic_load_explicit(&job->cancelled,
                                                 memory_order_relaxed);
    atomic_store_explicit(
        &job->state,
        cancelled ? TP_SESSION_JOB_CANCELLED
                  : (status == TP_STATUS_OK && result
                         ? TP_SESSION_JOB_SUCCEEDED
                         : TP_SESSION_JOB_FAILED),
        memory_order_release);
    return 0;
}

static int export_worker(void *context) {
    tp_live_job *job = context;
    const double start = job_now_ms();
    tp_status first_status = TP_STATUS_OK;
    tp_error first_error = {{0}};
    for (int i = 0; i < job->export_atlas_count; ++i) {
        if (atomic_load_explicit(&job->cancelled, memory_order_relaxed)) {
            break;
        }
        atomic_store_explicit(&job->current, i + 1, memory_order_relaxed);
        const tp_job_export_atlas *atlas = &job->export_atlases[i];
        tp_arena *arena = tp_arena_create(0);
        tp_export_notices notices;
        tp_export_notices_init(&notices);
        tp_error error = {{0}};
        int runs = 0;
        int missing = 0;
        tp_status status = arena
            ? tp_export_snapshot_job_run_atlas(
                  job->export_job, atlas->index, arena, &notices, &runs,
                  &missing, &error)
            : TP_STATUS_OOM;
        if (status == TP_STATUS_OK) {
            job->export_result.targets += atlas->enabled_targets;
            job->export_result.notices += notices.count;
            job->export_result.atlases_ok++;
        } else {
            job->export_result.atlases_failed++;
            if (first_status == TP_STATUS_OK) {
                first_status = status;
                first_error = error;
                tp_export_snapshot_atlas_info info;
                memset(&info, 0, sizeof info);
                (void)tp_export_snapshot_job_atlas_info(
                    job->export_job, atlas->index, &info, NULL);
                (void)snprintf(job->export_result.first_error,
                               sizeof job->export_result.first_error,
                               "%s: %s", info.name ? info.name : "?",
                               error.msg[0] ? error.msg : tp_status_str(status));
            }
        }
        tp_export_notices_free(&notices);
        tp_arena_destroy(arena);
    }
    job->elapsed_ms = job_now_ms() - start;
    const bool cancelled = atomic_load_explicit(&job->cancelled,
                                                 memory_order_relaxed);
    job->status = first_status;
    job->error = first_error;
    atomic_store_explicit(
        &job->state,
        cancelled ? TP_SESSION_JOB_CANCELLED
                  : (job->export_result.atlases_failed > 0
                         ? TP_SESSION_JOB_FAILED
                         : TP_SESSION_JOB_SUCCEEDED),
        memory_order_release);
    return 0;
}

static tp_status job_start_thread(tp_live_job *job, thrd_start_t worker,
                                  tp_error *err) {
    tp_status status = tp_session_job_attach_internal(job->session, &job->owner,
                                                       err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    job->started_ms = job_now_ms();
    if (thrd_create(&job->thread, worker, job) != thrd_success) {
        (void)tp_session_job_detach_internal(job->session, &job->owner, NULL);
        return tp_error_set(err, TP_STATUS_OOM,
                            "could not create Pack/Export worker thread");
    }
    job->thread_started = true;
    return TP_STATUS_OK;
}

tp_status tp_session_pack_job_start(tp_session *session,
                                    const tp_pack_job_request *request,
                                    tp_error *err) {
    if (!session || !request || tp_id128_is_nil(request->atlas_id) ||
        !request->work_dir) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Pack job requires session, atlas id, and work dir");
    }
    tp_session_snapshot *snapshot = NULL;
    tp_status status = tp_session_snapshot_create(session, &snapshot, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_by_id(
        snapshot, request->atlas_id);
    if (!atlas) {
        tp_session_snapshot_destroy(snapshot);
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "Pack job atlas was not found");
    }
    tp_live_job *job = job_create(session, TP_SESSION_JOB_PACK);
    if (!job) {
        tp_session_snapshot_destroy(snapshot);
        return tp_error_set(err, TP_STATUS_OOM, "Pack job allocation failed");
    }
    job->atlas_id = request->atlas_id;
    job->model_generation_at_start =
        tp_session_snapshot_model_generation(snapshot);
    job->atlas_name = job_strdup(atlas->name);
    job->work_dir = job_strdup(request->work_dir);
    status = tp_pack_input_build_snapshot(snapshot, request->atlas_id,
                                          &job->input, err);
    if (status == TP_STATUS_OK) {
        status = tp_pack_settings_build_snapshot(snapshot, request->atlas_id,
                                                 &job->settings, err);
    }
    if (status == TP_STATUS_OK && request->preview_exporter_id) {
        const tp_exporter *exporter = tp_exporter_find(
            request->preview_exporter_id);
        if (!exporter) {
            status = tp_error_set(err, TP_STATUS_NOT_FOUND,
                                  "unknown preview exporter '%s'",
                                  request->preview_exporter_id);
        } else {
            tp_pack_settings effective;
            status = tp_export_effective_settings(&job->settings,
                                                  &exporter->caps, &effective);
            if (status == TP_STATUS_OK) {
                job->settings = effective;
                (void)snprintf(job->preview_exporter_id,
                               sizeof job->preview_exporter_id, "%s",
                               request->preview_exporter_id);
            }
        }
    }
    tp_session_snapshot_destroy(snapshot);
    if (status != TP_STATUS_OK || !job->atlas_name || !job->work_dir) {
        if (status == TP_STATUS_OK) {
            status = tp_error_set(err, TP_STATUS_OOM,
                                  "Pack job input allocation failed");
        }
        tp_session_job_release_internal(&job->owner);
        return status;
    }
    if (job->input.count == 0) {
        tp_session_job_release_internal(&job->owner);
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "atlas has no usable images");
    }
    job->arena = tp_arena_create(0);
    if (!job->arena) {
        tp_session_job_release_internal(&job->owner);
        return tp_error_set(err, TP_STATUS_OOM, "Pack job arena allocation failed");
    }
    job->settings.work_dir = job->work_dir;
    job->settings.atlas_name = job->atlas_name;
    job->settings.sprites = job->input.descs;
    job->settings.sprite_count = job->input.count;
    job->total = 1;
    status = job_start_thread(job, pack_worker, err);
    if (status != TP_STATUS_OK) {
        tp_session_job_release_internal(&job->owner);
    }
    return status;
}

tp_status tp_session_export_start(tp_session *session,
                                  const tp_export_command_request *request,
                                  tp_error *err) {
    if (!session || !request || !request->work_dir) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export job requires session and work dir");
    }
    tp_session_snapshot *snapshot = NULL;
    tp_status status = tp_session_snapshot_create(session, &snapshot, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_live_job *job = job_create(session, TP_SESSION_JOB_EXPORT);
    if (!job) {
        tp_session_snapshot_destroy(snapshot);
        return tp_error_set(err, TP_STATUS_OOM, "Export job allocation failed");
    }
    status = tp_export_snapshot_job_create(snapshot, request->work_dir,
                                           &job->export_job, err);
    tp_session_snapshot_destroy(snapshot);
    if (status != TP_STATUS_OK) {
        tp_session_job_release_internal(&job->owner);
        return status;
    }
    const int atlas_count = tp_export_snapshot_job_atlas_count(job->export_job);
    job->export_atlases = calloc((size_t)atlas_count,
                                 sizeof *job->export_atlases);
    if (atlas_count > 0 && !job->export_atlases) {
        tp_session_job_release_internal(&job->owner);
        return tp_error_set(err, TP_STATUS_OOM,
                            "Export job atlas list allocation failed");
    }
    for (int i = 0; i < atlas_count; ++i) {
        tp_export_snapshot_atlas_info info;
        status = tp_export_snapshot_job_atlas_info(job->export_job, i, &info,
                                                   err);
        if (status != TP_STATUS_OK) {
            tp_session_job_release_internal(&job->owner);
            return status;
        }
        const bool selected = tp_id128_is_nil(request->atlas_id) ||
                              tp_id128_eq(request->atlas_id, info.atlas_id);
        if (selected && info.enabled_target_count > 0 && info.source_count > 0) {
            tp_job_export_atlas *target =
                &job->export_atlases[job->export_atlas_count++];
            target->index = i;
            target->enabled_targets = info.enabled_target_count;
        }
    }
    if (job->export_atlas_count == 0) {
        tp_session_job_release_internal(&job->owner);
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "nothing to export");
    }
    job->total = job->export_atlas_count;
    status = job_start_thread(job, export_worker, err);
    if (status != TP_STATUS_OK) {
        tp_session_job_release_internal(&job->owner);
    }
    return status;
}

bool tp_session_job_active(const tp_session *session) {
    tp_session_owned_job *job = tp_session_job_acquire_internal(session);
    const bool active = job != NULL;
    tp_session_job_release_internal(job);
    return active;
}

tp_status tp_session_job_poll(const tp_session *session,
                              tp_session_job_progress *out, tp_error *err) {
    if (!session || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "job poll requires session and output");
    }
    tp_live_job *job =
        (tp_live_job *)tp_session_job_acquire_internal(session);
    if (!job) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "session has no active job");
    }
    memset(out, 0, sizeof *out);
    out->kind = job->kind;
    out->state = (tp_session_job_state)atomic_load_explicit(
        &job->state, memory_order_acquire);
    out->current = atomic_load_explicit(&job->current,
                                        memory_order_relaxed);
    out->total = job->total;
    out->elapsed_ms = out->state == TP_SESSION_JOB_RUNNING
                          ? job_now_ms() - job->started_ms
                          : job->elapsed_ms;
    tp_session_job_release_internal(&job->owner);
    return TP_STATUS_OK;
}

tp_status tp_session_job_cancel(tp_session *session, tp_error *err) {
    tp_session_owned_job *job = tp_session_job_acquire_internal(session);
    if (!job) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "session has no active job");
    }
    job->cancel(job);
    tp_session_job_release_internal(job);
    return TP_STATUS_OK;
}

tp_status tp_session_job_take_result(tp_session *session,
                                     tp_session_job_result *out,
                                     tp_error *err) {
    if (!session || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "job result requires session and output");
    }
    tp_live_job *job =
        (tp_live_job *)tp_session_job_acquire_internal(session);
    if (!job) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "session has no active job");
    }
    const tp_session_job_state state =
        (tp_session_job_state)atomic_load_explicit(&job->state,
                                                   memory_order_acquire);
    if (state == TP_SESSION_JOB_RUNNING) {
        tp_session_job_release_internal(&job->owner);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "job is still running");
    }
    tp_status status = tp_session_job_detach_internal(
        session, &job->owner, err);
    if (status != TP_STATUS_OK) {
        tp_session_job_release_internal(&job->owner);
        return status;
    }
    /* Drop the session's ownership. This call keeps its acquired pin until the
     * result has been transferred, so concurrent poll/cancel readers cannot
     * observe freed storage. */
    tp_session_job_release_internal(&job->owner);
    job_join(job);
    memset(out, 0, sizeof *out);
    out->kind = job->kind;
    out->state = state;
    out->status = job->status;
    out->error = job->error;
    out->elapsed_ms = job->elapsed_ms;
    if (job->kind == TP_SESSION_JOB_PACK) {
        out->pack.atlas_id = job->atlas_id;
        out->pack.missing_sources = job->input.missing_sources;
        out->pack.model_generation_at_start = job->model_generation_at_start;
        (void)snprintf(out->pack.preview_exporter_id,
                       sizeof out->pack.preview_exporter_id, "%s",
                       job->preview_exporter_id);
        if (state == TP_SESSION_JOB_SUCCEEDED) {
            out->pack.arena = job->arena;
            out->pack.result = job->pack_result;
            job->arena = NULL;
            job->pack_result = NULL;
        }
    } else {
        out->export_result = job->export_result;
    }
    tp_session_job_release_internal(&job->owner);
    return TP_STATUS_OK;
}

void tp_session_job_result_destroy(tp_session_job_result *result) {
    if (!result) {
        return;
    }
    if (result->kind == TP_SESSION_JOB_PACK) {
        tp_arena_destroy(result->pack.arena);
    }
    memset(result, 0, sizeof *result);
}
