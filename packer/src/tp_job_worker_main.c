#include "tp_job_worker_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#elif defined(TP_ENABLE_TEST_SEAMS)
#include <errno.h>
#endif

#include "hash/nt_hash.h"
#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_pack_hash.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"
#include "tp_fs_internal.h"
#include "tp_pack_priv.h"
#include "tp_proc_internal.h"

typedef struct worker_cancel {
    bool requested;
    bool transport_failed;
} worker_cancel;

typedef struct pack_hash_collect {
    tp_id128 *hashes;
    int count;
} pack_hash_collect;

static char *worker_strdup(const char *text) {
    if (!text) {
        return NULL;
    }
    const size_t length = strlen(text);
    char *copy = malloc(length + 1U);
    if (copy) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

static void free_pack_names(tp_job_worker_proto_name *names,
                            uint32_t count) {
    if (!names) {
        return;
    }
    for (uint32_t i = 0U; i < count; ++i) {
        free((void *)names[i].name);
    }
    free(names);
}

static double worker_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart == 0) {
        return 0.0;
    }
    return (double)counter.QuadPart * 1000.0 /
           (double)frequency.QuadPart;
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0.0;
    }
    return (double)value.tv_sec * 1000.0 +
           (double)value.tv_nsec / 1000000.0;
#endif
}

#ifdef TP_ENABLE_TEST_SEAMS
static void test_block_before_job_work(void) {
    const char *text =
        getenv("TP_TEST_JOB_WORKER_BLOCK_MS");
    if (!text || text[0] == '\0') {
        return;
    }
    char *end = NULL;
    const unsigned long milliseconds =
        strtoul(text, &end, 10);
    if (!end || *end != '\0' ||
        milliseconds == 0UL ||
        milliseconds > 60000UL) {
        return;
    }
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec delay = {
        .tv_sec =
            (time_t)(milliseconds / 1000UL),
        .tv_nsec =
            (long)((milliseconds % 1000UL) *
                   1000000UL),
    };
    while (nanosleep(&delay, &delay) != 0 &&
           errno == EINTR) {
    }
#endif
}
#endif

static bool worker_cancel_requested(void *context) {
    worker_cancel *cancel = context;
    if (cancel->requested || cancel->transport_failed) {
        return true;
    }
    const tp_proc_stdin_event event = tp_proc_child_poll_stdin();
    if (event == TP_PROC_STDIN_EVENT_CANCEL) {
        cancel->requested = true;
    } else if (event == TP_PROC_STDIN_EVENT_ERROR) {
        cancel->transport_failed = true;
    }
    return cancel->requested || cancel->transport_failed;
}

static bool write_frame(const uint8_t *bytes, size_t length) {
    return bytes && length > 0U &&
           fwrite(bytes, 1U, length, stdout) == length &&
           fflush(stdout) == 0;
}

static bool publish_progress(uint64_t request_id, int current, int total,
                             tp_job_worker_progress_phase phase) {
    const tp_job_worker_proto_progress progress = {
        .request_id = request_id,
        .current = current,
        .total = total,
        .phase = phase,
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    const tp_status status = tp_job_worker_proto_encode_progress(
        &progress, &bytes, &length, NULL);
    const bool wrote =
        status == TP_STATUS_OK && write_frame(bytes, length);
    free(bytes);
    return wrote;
}

static void collect_image_hash(void *context, int sprite_index,
                               int width, int height, const uint8_t *rgba,
                               const tp_pack_image_fingerprint *fingerprint) {
    (void)fingerprint;
    pack_hash_collect *collect = context;
    if (!collect || sprite_index < 0 ||
        sprite_index >= collect->count) {
        return;
    }
    collect->hashes[sprite_index] =
        tp_pack_semantic_image_hash(width, height, rgba);
}

static tp_status read_artifact(const char *path, uint8_t **out,
                               size_t *out_size, tp_error *err) {
    *out = NULL;
    *out_size = 0U;
    FILE *file = tp_fs_fopen(path, "rb");
    if (!file) {
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "job worker Pack artifact is missing");
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "job worker Pack artifact cannot be sized");
    }
    const long end = ftell(file);
    if (end <= 0 ||
        (uint64_t)end >
            (uint64_t)TP_JOB_WORKER_PROTO_MAX_ARTIFACT_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "job worker Pack artifact exceeds protocol cap");
    }
    uint8_t *bytes = malloc((size_t)end);
    if (!bytes) {
        (void)fclose(file);
        return tp_error_set(err, TP_STATUS_OOM,
                            "job worker Pack artifact allocation failed");
    }
    const bool complete =
        fread(bytes, 1U, (size_t)end, file) == (size_t)end &&
        fclose(file) == 0;
    if (!complete) {
        free(bytes);
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "job worker Pack artifact read failed");
    }
    *out = bytes;
    *out_size = (size_t)end;
    return TP_STATUS_OK;
}

static tp_status run_pack(const tp_job_worker_proto_request *request,
                          tp_project *project, worker_cancel *cancel,
                          tp_job_worker_proto_response *response,
                          tp_error *err) {
    const int atlas_index =
        tp_project_find_atlas_by_id(project, request->atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "job worker Pack atlas was not found");
    }
    if (!publish_progress(request->request_id, 0, 1,
                          TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL)) {
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "job worker progress channel failed");
    }
    const tp_cancel_token cancel_token = {
        worker_cancel_requested, cancel};
    tp_pack_input input = {0};
    tp_status status = tp_pack_input_build_cancellable(
        project, atlas_index, &input, &cancel_token, err);
    if (status == TP_STATUS_OK && input.count == 0) {
        status = tp_error_set(err, TP_STATUS_NOT_FOUND,
                              "atlas has no usable images");
    }
    if (status != TP_STATUS_OK) {
        tp_pack_input_free(&input);
        return status;
    }

    tp_pack_settings settings = {0};
    status = tp_project_atlas_to_settings(
        project, atlas_index, &settings, err);
    if (status != TP_STATUS_OK) {
        tp_pack_input_free(&input);
        return status;
    }
    if (request->preview_exporter_id &&
        request->preview_exporter_id[0]) {
        const tp_exporter *exporter =
            tp_exporter_find(request->preview_exporter_id);
        if (!exporter) {
            tp_pack_input_free(&input);
            return tp_error_set(
                err, TP_STATUS_NOT_FOUND,
                "unknown preview exporter '%s'",
                request->preview_exporter_id);
        }
        tp_pack_settings effective = {0};
        status = tp_export_effective_settings(
            &settings, &exporter->caps, &effective);
        if (status != TP_STATUS_OK) {
            tp_pack_input_free(&input);
            return status;
        }
        settings = effective;
    }
    settings.work_dir = request->work_dir;
    settings.sprites = input.descs;
    settings.sprite_count = input.count;

    tp_job_worker_proto_name *names = calloc(
        (size_t)input.count + 1U, sizeof *names);
    tp_id128 *image_hashes = calloc(
        (size_t)input.count, sizeof *image_hashes);
    if (!names || !image_hashes) {
        free(names);
        free(image_hashes);
        tp_pack_input_free(&input);
        return tp_error_set(err, TP_STATUS_OOM,
                            "job worker Pack metadata allocation failed");
    }
    const size_t atlas_name_length =
        strlen(settings.atlas_name);
    char *atlas_name =
        malloc(atlas_name_length + 1U);
    if (!atlas_name) {
        free(names);
        free(image_hashes);
        tp_pack_input_free(&input);
        return tp_error_set(
            err, TP_STATUS_OOM,
            "job worker Pack atlas-name allocation failed");
    }
    memcpy(
        atlas_name, settings.atlas_name,
        atlas_name_length + 1U);
    names[0].name = atlas_name;
    names[0].hash =
        nt_hash64_str(settings.atlas_name).value;
    uint32_t owned_name_count = 1U;
    for (int i = 0; i < input.count; ++i) {
        const size_t name_length = strlen(input.descs[i].name);
        char *name = malloc(name_length + 1U);
        if (!name) {
            free_pack_names(names, owned_name_count);
            free(image_hashes);
            tp_pack_input_free(&input);
            return tp_error_set(
                err, TP_STATUS_OOM,
                "job worker Pack name allocation failed");
        }
        memcpy(name, input.descs[i].name, name_length + 1U);
        names[i + 1].name = name;
        names[i + 1].hash =
            nt_hash64_str(input.descs[i].name).value;
        owned_name_count++;
    }
    pack_hash_collect collect = {image_hashes, input.count};
    tp_arena *arena = tp_arena_create(0);
    tp_result *result = NULL;
    if (!arena) {
        status = TP_STATUS_OOM;
    } else {
        (void)publish_progress(request->request_id, 0, 1,
                               TP_JOB_WORKER_PHASE_BUILD);
        status = tp_pack_cancellable_observed(
            &settings, arena, &result, worker_cancel_requested, cancel,
            collect_image_hash, &collect, err);
    }

    tp_id128 input_hash = tp_id128_nil();
    if (status == TP_STATUS_OK && result) {
        tp_error hash_error = {{0}};
        if (tp_pack_input_hash_from_images(
                &settings,
                request->preview_exporter_id &&
                        request->preview_exporter_id[0]
                    ? request->preview_exporter_id
                    : NULL,
                image_hashes, input.count, &input_hash,
                &hash_error) != TP_STATUS_OK) {
            input_hash = tp_id128_nil();
        }
    }
    free(image_hashes);

    uint8_t *artifact = NULL;
    size_t artifact_size = 0U;
    if (status == TP_STATUS_OK && result) {
        char path[512];
        const int length = snprintf(
            path, sizeof path, "%s/%s.ntpack",
            settings.work_dir, settings.atlas_name);
        if (length < 0 || (size_t)length >= sizeof path) {
            status = tp_error_set(
                err, TP_STATUS_OUT_OF_BOUNDS,
                "job worker Pack artifact path is too long");
        } else {
            status = read_artifact(
                path, &artifact, &artifact_size, err);
        }
    }

    if (status == TP_STATUS_OK && result) {
        tp_id128 current_hash = tp_id128_nil();
        tp_error freshness_error = {{0}};
        const tp_status freshness_status =
            tp_pack_input_hash_compute(
                &settings,
                request->preview_exporter_id &&
                        request->preview_exporter_id[0]
                    ? request->preview_exporter_id
                    : NULL,
                NULL, &current_hash, &freshness_error);
        response->pack.atlas_id = request->atlas_id;
        response->pack.missing_sources = input.missing_sources;
        response->pack.input_token_at_start = request->input_token;
        response->pack.pack_input_hash = input_hash;
        response->pack.current_pack_input_hash = current_hash;
        response->pack.freshness_token = request->input_token;
        response->pack.freshness_status = freshness_status;
        response->pack.freshness_error = freshness_error;
        response->pack.freshness_error.file_io.path =
            worker_strdup(freshness_error.file_io.path);
        if (freshness_error.file_io.path &&
            !response->pack.freshness_error.file_io.path) {
            response->pack.freshness_status = TP_STATUS_OOM;
            (void)tp_error_set(
                &response->pack.freshness_error, TP_STATUS_OOM,
                "job worker freshness error path allocation failed");
        }
        if (tp_id128_is_nil(input_hash)) {
            response->pack.freshness = TP_PACK_FRESHNESS_STALE;
            response->pack.freshness_reason =
                TP_PACK_FRESHNESS_RESULT_HASH_NIL;
        } else if (response->pack.freshness_status != TP_STATUS_OK) {
            response->pack.freshness = TP_PACK_FRESHNESS_STALE;
            response->pack.freshness_reason =
                TP_PACK_FRESHNESS_PROBE_ERROR;
        } else if (tp_id128_eq(input_hash, current_hash)) {
            response->pack.freshness = TP_PACK_FRESHNESS_CURRENT;
            response->pack.freshness_reason =
                TP_PACK_FRESHNESS_MATCH;
        } else {
            response->pack.freshness = TP_PACK_FRESHNESS_STALE;
            response->pack.freshness_reason =
                TP_PACK_FRESHNESS_HASH_MISMATCH;
        }
        (void)snprintf(
            response->pack.preview_exporter_id,
            sizeof response->pack.preview_exporter_id, "%s",
            request->preview_exporter_id
                ? request->preview_exporter_id
                : "");
        response->pack.names = names;
        response->pack.name_count = owned_name_count;
        response->pack.artifact = artifact;
        response->pack.artifact_size = artifact_size;
    } else {
        free_pack_names(names, owned_name_count);
        free(artifact);
    }
    tp_arena_destroy(arena);
    tp_pack_input_free(&input);
    return status;
}

static tp_status run_export(const tp_job_worker_proto_request *request,
                            tp_project *project, worker_cancel *cancel,
                            tp_job_worker_proto_response *response,
                            tp_error *err) {
    const tp_cancel_token cancel_token = {
        worker_cancel_requested, cancel};
    int eligible = 0;
    for (int i = 0; i < project->atlas_count; ++i) {
        const tp_project_atlas *atlas = &project->atlases[i];
        if (!tp_id128_is_nil(request->atlas_id) &&
            !tp_id128_eq(request->atlas_id, atlas->id)) {
            continue;
        }
        for (int target = 0; target < atlas->target_count; ++target) {
            if (atlas->targets[target].enabled) {
                eligible++;
                break;
            }
        }
    }
    if (eligible == 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "nothing to export");
    }

    tp_status first_status = TP_STATUS_OK;
    int current = 0;
    for (int i = 0; i < project->atlas_count; ++i) {
        const tp_project_atlas *atlas = &project->atlases[i];
        if (!tp_id128_is_nil(request->atlas_id) &&
            !tp_id128_eq(request->atlas_id, atlas->id)) {
            continue;
        }
        bool enabled = false;
        for (int target = 0; target < atlas->target_count; ++target) {
            enabled = enabled || atlas->targets[target].enabled;
        }
        if (!enabled) {
            continue;
        }
        if (worker_cancel_requested(cancel)) {
            break;
        }
        current++;
        (void)publish_progress(
            request->request_id, current - 1, eligible,
            TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL);
        tp_pack_input input = {0};
        tp_error atlas_error = {{0}};
        tp_status status = tp_pack_input_build_cancellable(
            project, i, &input, &cancel_token, &atlas_error);
        if (status == TP_STATUS_OK && input.count == 0) {
            response->export_result.atlases_skipped++;
            tp_pack_input_free(&input);
            continue;
        }
        for (int target = 0;
             status == TP_STATUS_OK &&
             target < atlas->target_count;
             ++target) {
            if (!atlas->targets[target].enabled) {
                continue;
            }
            char output_path[TP_IDENTITY_PATH_MAX];
            status = tp_project_resolve_path(
                project, atlas->targets[target].out_path,
                output_path, sizeof output_path);
            if (status == TP_STATUS_OK) {
                tp_mkdirs_parent(output_path);
            } else {
                (void)tp_error_set(
                    &atlas_error, status,
                    "job worker cannot resolve Export output path");
            }
        }
        tp_arena *arena = tp_arena_create(0);
        tp_export_notices notices;
        tp_export_notices_init(&notices);
        tp_export_report report = {0};
        int runs = 0;
        if (status == TP_STATUS_OK && arena) {
            (void)publish_progress(
                request->request_id, current, eligible,
                TP_JOB_WORKER_PHASE_EXPORT_WRITE);
            tp_export_run_opts opts = {
                .report = &report,
                .cancel = &cancel_token,
            };
            status = tp_export_run_ex(
                project, i, input.descs, input.count,
                request->work_dir, arena, &notices, &runs, &opts,
                &atlas_error);
        } else if (status == TP_STATUS_OK) {
            status = TP_STATUS_OOM;
        }
        bool writer_failed = false;
        const char *writer_error = NULL;
        for (int target = 0; target < report.target_count; ++target) {
            if (report.targets[target].ok) {
                response->export_result.targets++;
                response->export_result.files +=
                    report.targets[target].written_file_count;
            } else if (report.targets[target].writer_outcome ==
                       TP_EXPORT_WRITER_FAILED) {
                response->export_result.publication_uncertain = true;
                writer_failed = true;
                if (!writer_error) {
                    writer_error = report.targets[target].error;
                }
            }
        }
        response->export_result.notices += notices.count;
        if (writer_failed) {
            response->export_result.atlases_failed++;
            if (response->export_result.first_error[0] == '\0') {
                (void)snprintf(
                    response->export_result.first_error,
                    sizeof response->export_result.first_error,
                    "%s: %s", atlas->name,
                    writer_error && writer_error[0]
                        ? writer_error
                        : "Export writer failed");
            }
            if (status != TP_STATUS_CANCELLED &&
                first_status == TP_STATUS_OK) {
                first_status =
                    status == TP_STATUS_OK
                        ? TP_STATUS_BUILDER_FAILED
                        : status;
                if (atlas_error.msg[0]) {
                    *err = atlas_error;
                } else {
                    (void)tp_error_set(
                        err, first_status, "%s",
                        writer_error && writer_error[0]
                            ? writer_error
                            : "Export writer failed");
                }
            }
        } else if (status == TP_STATUS_OK) {
            response->export_result.atlases_ok++;
        } else if (status != TP_STATUS_CANCELLED) {
            response->export_result.atlases_failed++;
            if (first_status == TP_STATUS_OK) {
                first_status = status;
                *err = atlas_error;
                (void)snprintf(
                    response->export_result.first_error,
                    sizeof response->export_result.first_error,
                    "%s: %s", atlas->name,
                    atlas_error.msg[0]
                        ? atlas_error.msg
                        : tp_status_str(status));
            }
        }
        tp_export_notices_free(&notices);
        tp_arena_destroy(arena);
        tp_pack_input_free(&input);
        if (status == TP_STATUS_CANCELLED) {
            break;
        }
    }
    response->export_result.partial_publication =
        response->export_result.targets > 0 &&
        (cancel->requested || first_status != TP_STATUS_OK);
    return cancel->requested ? TP_STATUS_CANCELLED : first_status;
}

int tp_job_worker_main_request(const uint8_t *bytes, size_t length) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
#ifdef TP_ENABLE_TEST_SEAMS
    test_block_before_job_work();
#endif
    tp_error error = {{0}};
    tp_job_worker_proto_request request = {0};
    tp_status status = tp_job_worker_proto_decode_request(
        bytes, length, &request, &error);
    tp_job_worker_proto_response response = {0};
    response.kind = request.kind;
    response.session_instance_generation =
        request.session_instance_generation;
    response.request_id = request.request_id;
    const double start = worker_now_ms();

    tp_project *project = NULL;
    worker_cancel cancel = {0};
    if (status == TP_STATUS_OK) {
        status = tp_project_load_buffer(
            (const char *)request.project_json,
            request.project_json_len, &project, &error);
    }
    if (status == TP_STATUS_OK) {
        const size_t project_dir_length = strlen(request.project_dir);
        char *project_dir = malloc(project_dir_length + 1U);
        if (!project_dir) {
            status = tp_error_set(
                &error, TP_STATUS_OOM,
                "job worker project directory allocation failed");
        } else {
            memcpy(project_dir, request.project_dir,
                   project_dir_length + 1U);
            free(project->project_dir);
            project->project_dir = project_dir;
        }
    }
    if (status == TP_STATUS_OK && request.kind == TP_SESSION_JOB_PACK) {
        status = run_pack(
            &request, project, &cancel, &response, &error);
    } else if (status == TP_STATUS_OK &&
               request.kind == TP_SESSION_JOB_EXPORT) {
        status = run_export(
            &request, project, &cancel, &response, &error);
    } else if (status == TP_STATUS_OK) {
        status = tp_error_set(
            &error, TP_STATUS_INVALID_ARGUMENT,
            "job worker request kind is invalid");
    }
    response.elapsed_ms = worker_now_ms() - start;
    response.status = status;
    response.error = error;
    response.error.file_io.path = worker_strdup(error.file_io.path);
    if (error.file_io.path && !response.error.file_io.path) {
        response.status = TP_STATUS_OOM;
        (void)tp_error_set(
            &response.error, TP_STATUS_OOM,
            "job worker error path allocation failed");
    }
    response.state =
        response.status == TP_STATUS_CANCELLED || cancel.requested
            ? TP_SESSION_JOB_CANCELLED
            : response.status == TP_STATUS_OK
                  ? TP_SESSION_JOB_SUCCEEDED
                  : TP_SESSION_JOB_FAILED;

    uint8_t *encoded = NULL;
    size_t encoded_length = 0U;
    const tp_status encode_status =
        tp_job_worker_proto_encode_response(
            &response, &encoded, &encoded_length, NULL);
    const bool wrote =
        encode_status == TP_STATUS_OK &&
        write_frame(encoded, encoded_length);
    free(encoded);
    free((void *)response.error.file_io.path);
    if (response.kind == TP_SESSION_JOB_PACK) {
        free((void *)response.pack.freshness_error.file_io.path);
        free_pack_names(
            (tp_job_worker_proto_name *)response.pack.names,
            response.pack.name_count);
        free((void *)response.pack.artifact);
    }
    tp_project_destroy(project);
    tp_job_worker_proto_request_free(&request);
    return wrote ? 0 : 1;
}
