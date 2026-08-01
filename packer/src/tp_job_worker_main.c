#include "tp_job_worker_internal.h"
#include "tp_export_job_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif
#if !defined(_WIN32) && defined(TP_ENABLE_TEST_SEAMS)
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
#include "tp_build_worker_internal.h"
#include "tp_cancel_source.h"
#include "tp_fs_internal.h"
#include "tp_pack_priv.h"
#include "tp_proc_internal.h"

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
static void worker_sleep_ms(unsigned long milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000UL),
        .tv_nsec = (long)((milliseconds % 1000UL) * 1000000UL),
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
#endif
}

/* Bounded millisecond seam value, or 0 when the variable is unset/invalid. */
static unsigned long test_block_ms(const char *name) {
    const char *text = getenv(name);
    if (!text || text[0] == '\0') {
        return 0UL;
    }
    char *end = NULL;
    const unsigned long milliseconds = strtoul(text, &end, 10);
    if (!end || *end != '\0' || milliseconds == 0UL ||
        milliseconds > 60000UL) {
        return 0UL;
    }
    return milliseconds;
}

static void test_block_before_job_work(void) {
    const unsigned long milliseconds =
        test_block_ms("TP_TEST_JOB_WORKER_BLOCK_MS");
    if (milliseconds > 0UL) {
        worker_sleep_ms(milliseconds);
    }
}

/* Park INSIDE run_pack, after the private request directory exists, until the
 * host's cancellation arrives (or the cap expires). This is the only way to
 * exercise a cancel against a Pack that has already taken resources on disk:
 * TP_TEST_JOB_WORKER_BLOCK_MS parks BEFORE any work, where "no directory was
 * left behind" is vacuously true. */
static void test_block_in_pack_until_cancel(tp_cancel_source *cancel) {
    const unsigned long milliseconds =
        test_block_ms("TP_TEST_JOB_WORKER_BLOCK_IN_PACK_MS");
    for (unsigned long waited = 0UL; waited < milliseconds; ++waited) {
        if (tp_cancel_source_poll(cancel)) {
            return;
        }
        worker_sleep_ms(1UL);
    }
}

/* Damage the published artifact AFTER its size was announced, so the host's
 * digestless validation (regular file + exact size + pack magic) can be driven
 * end to end without a real disk fault. "truncate" moves the size away from the
 * announced one; "magic" keeps the size and destroys the header. */
static void test_fault_artifact(const char *path) {
    const char *mode = getenv("TP_TEST_JOB_WORKER_ARTIFACT_FAULT");
    if (!mode || mode[0] == '\0') {
        return;
    }
    FILE *file = tp_fs_fopen(
        path, strcmp(mode, "magic") == 0 ? "r+b" : "wb");
    if (!file) {
        return;
    }
    (void)fwrite("XXXX", 1U, 4U, file);
    (void)fclose(file);
}
#endif

#ifdef TP_ENABLE_TEST_SEAMS
/* Make this worker deaf to the cancel byte, so a Pack the host cancelled still
 * runs to a SUCCEEDED terminal with a real artifact on disk. That is the exact
 * race the host's "an accepted cancellation outranks the terminal that raced it"
 * rule exists for, and the only way to prove the host deletes the artifact
 * nobody will adopt. */
static bool test_ignores_cancel(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *text = getenv("TP_TEST_JOB_WORKER_IGNORE_CANCEL");
        cached = text && text[0] == '1' ? 1 : 0;
    }
    return cached == 1;
}
#endif

/* The worker's ONE destructive read of its control channel. tp_cancel_source
 * owns the latch, so this stays a pure event -> reason mapping. */
static tp_cancel_reason child_stdin_probe(void *context) {
    (void)context;
#ifdef TP_ENABLE_TEST_SEAMS
    if (test_ignores_cancel()) {
        return TP_CANCEL_REASON_NONE;
    }
#endif
    const tp_proc_stdin_event event = tp_proc_child_poll_stdin();
    if (event == TP_PROC_STDIN_EVENT_CANCEL) {
        return TP_CANCEL_REASON_REQUESTED;
    }
    if (event == TP_PROC_STDIN_EVENT_ERROR) {
        return TP_CANCEL_REASON_CONTROL_LOST;
    }
    return TP_CANCEL_REASON_NONE;
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

typedef struct export_terminal_context {
    uint64_t request_id;
    int current;
    int eligible;
    bool claimed;
} export_terminal_context;

static bool publish_export_terminal_boundary_now(
    export_terminal_context *context) {
    if (!context || context->claimed) {
        return false;
    }
    context->claimed = true;
    (void)publish_progress(
        context->request_id, context->current, context->eligible,
        TP_JOB_WORKER_PHASE_EXPORT_TERMINAL_BOUNDARY);
#ifdef TP_ENABLE_TEST_SEAMS
    const char *marker =
        getenv("TP_TEST_JOB_WORKER_EXPORT_BOUNDARY_MARKER");
    if (marker && marker[0]) {
        FILE *file = tp_fs_fopen(marker, "wb");
        if (file) {
            (void)fwrite("claimed", 1U, 7U, file);
            (void)fclose(file);
        }
    }
    const unsigned long milliseconds = test_block_ms(
        "TP_TEST_JOB_WORKER_BLOCK_AFTER_EXPORT_BOUNDARY_MS");
    if (milliseconds > 0UL) {
        worker_sleep_ms(milliseconds);
    }
#endif
    return true;
}

static bool publish_export_terminal_boundary(void *opaque) {
    export_terminal_context *context = opaque;
    return context &&
           context->current == context->eligible &&
           publish_export_terminal_boundary_now(context);
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

/* One private directory per Pack request under the host-chosen work_dir. It is
 * what keeps a native Pack, a preview Pack and a second app instance from
 * colliding on the single `<atlas_name>.ntpack` name, and it gives the host an
 * exact path to adopt.
 *
 * Named with the HOST pid, not this worker's. The directory deliberately
 * OUTLIVES the worker: the host adopts the artifact in budgeted slices after the
 * child has exited, and only then deletes file and directory. Under the worker
 * pid, tp_worker_reap_stale_dirs -- which reaps `<prefix><hexpid>-*` whose pid is
 * definitively dead -- would see an exited worker and let the NEXT Pack (of any
 * instance sharing this work_dir) delete an artifact still being read. Keying the
 * name on the host makes the liveness question the right one. The inner builder's
 * `pkw-` staging keeps the worker pid: it never outlives its owner. */
static bool make_request_dir(const char *work_dir, uint64_t request_id,
                             uint32_t host_pid, char *out, size_t out_cap,
                             bool *out_too_long) {
    *out_too_long = false;
    tp_worker_reap_stale_dirs(work_dir, "req-");
    const int length = snprintf(
        out, out_cap, "%s/req-%08lx-%016llx", work_dir,
        (unsigned long)host_pid, (unsigned long long)request_id);
    if (length <= 0 || (size_t)length >= out_cap) {
        out[0] = '\0';
        *out_too_long = true;
        return false;
    }
    if (tp_fs_exists(out)) {
        /* Our host's pid AND its request id: a leftover of this very request,
         * never a live peer's directory. */
        tp_worker_remove_dir_tree(out);
    }
    if (tp_fs_create_dir(out)) {
        return true;
    }
    out[0] = '\0';
    return false;
}

/* Remove the private request directory holding a published artifact, given the
 * artifact's path. Only a "req-" basename is ever removed, so a malformed path
 * can never take a directory this process did not create. */
static void remove_request_dir_of(const char *artifact_path) {
    const char *separator = NULL;
    for (const char *cursor = artifact_path; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            separator = cursor;
        }
    }
    if (!separator) {
        return;
    }
    const size_t length = (size_t)(separator - artifact_path);
    /* Holds the request directory of any admissible artifact path: work_dir up
     * to TP_IDENTITY_PATH_MAX-1 plus the 31-char "/req-..." component. */
    char directory[TP_IDENTITY_PATH_MAX + 32];
    if (length == 0U || length >= sizeof directory) {
        return;
    }
    memcpy(directory, artifact_path, length);
    directory[length] = '\0';
    const char *base = directory;
    for (const char *cursor = directory; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    if (strncmp(base, "req-", 4U) != 0) {
        return;
    }
    tp_worker_remove_dir_tree(directory);
}

static tp_status run_pack(const tp_job_worker_proto_request *request,
                          tp_project *project, tp_cancel_source *cancel,
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
    const tp_cancel_token cancel_token = tp_cancel_source_token(cancel);
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
            &settings, &exporter->format->caps, &effective);
        if (status != TP_STATUS_OK) {
            tp_pack_input_free(&input);
            return status;
        }
        settings = effective;
    }
    /* work_dir alone may be a full TP_IDENTITY_PATH_MAX-1 path (the host and the
     * proto admit it up to that bound); the "/req-" + hexpid + '-' + hex request
     * id component adds 31 more, so the buffer must exceed TP_IDENTITY_PATH_MAX
     * by that component. A 544 cap made a long but perfectly legal work_dir fail
     * every Pack as "path is too long" while Export on the same work_dir worked.
     * The END-TO-END admissible work_dir is still tighter than this buffer: the
     * artifact REPLY path (request_dir + '/' + atlas_name + ".ntpack") must stay
     * under TP_JOB_WORKER_PROTO_MAX_PATH_BYTES and the host's containment cap
     * (both TP_IDENTITY_PATH_MAX-1), so work_dir is effectively bounded by
     * TP_IDENTITY_PATH_MAX-1 minus 38 fixed chars minus strlen(atlas_name). */
    char request_dir[TP_IDENTITY_PATH_MAX + 32];
    bool request_dir_too_long = false;
    if (!make_request_dir(request->work_dir, request->request_id,
                          request->host_pid, request_dir, sizeof request_dir,
                          &request_dir_too_long)) {
        tp_pack_input_free(&input);
        return request_dir_too_long
                   ? tp_error_set(
                         err, TP_STATUS_INVALID_ARGUMENT,
                         "job worker Pack work directory path is too long")
                   : tp_error_set(
                         err, TP_STATUS_FILE_IO_FAILED,
                         "job worker could not create its private Pack directory");
    }
#ifdef TP_ENABLE_TEST_SEAMS
    test_block_in_pack_until_cancel(cancel);
#endif
    settings.work_dir = request_dir;
    settings.sprites = input.descs;
    settings.sprite_count = input.count;

    tp_job_worker_proto_name *names = calloc(
        (size_t)input.count + 1U, sizeof *names);
    tp_id128 *image_hashes = calloc(
        (size_t)input.count, sizeof *image_hashes);
    if (!names || !image_hashes) {
        free(names);
        free(image_hashes);
        tp_worker_remove_dir_tree(request_dir);
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
        tp_worker_remove_dir_tree(request_dir);
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
            tp_worker_remove_dir_tree(request_dir);
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

    /* Produce only: this process never reads the artifact back. The host owns
     * the single parse, so a tp_pack_read here would cost a second full decode
     * of every page for a tp_result nobody in this process looks at. */
    pack_hash_collect collect = {image_hashes, input.count};
    /* Sized for the reply-path bound (see the request_dir comment): the proto
     * refuses to encode an artifact path past TP_IDENTITY_PATH_MAX-1 bytes, so
     * a smaller buffer here only manufactured an earlier "path too long". */
    char artifact_path[TP_IDENTITY_PATH_MAX];
    (void)publish_progress(request->request_id, 0, 1,
                           TP_JOB_WORKER_PHASE_BUILD);
    status = tp_pack_produce_observed(
        &settings, artifact_path, sizeof artifact_path, &cancel_token,
        collect_image_hash, &collect, err);

    uint64_t artifact_size = 0U;
    if (status == TP_STATUS_OK) {
        tp_fs_info info;
        if (!tp_fs_stat(artifact_path, &info) ||
            info.kind != TP_FS_KIND_REGULAR || info.reparse ||
            info.size == 0U) {
            status = tp_error_set(
                err, TP_STATUS_BUILDER_FAILED,
                "job worker Pack artifact is missing or unreadable");
        } else if (info.size > TP_JOB_WORKER_PROTO_MAX_ARTIFACT_BYTES) {
            /* The host reads the artifact into one buffer, so an artifact past
             * the transport cap must fail as a structured terminal here rather
             * than as an unencodable frame or a multi-GB host allocation. */
            status = tp_error_set(
                err, TP_STATUS_OUT_OF_BOUNDS,
                "job worker Pack artifact exceeds the transport size cap");
        } else {
            artifact_size = info.size;
#ifdef TP_ENABLE_TEST_SEAMS
            test_fault_artifact(artifact_path);
#endif
        }
    }

    tp_id128 input_hash = tp_id128_nil();
    if (status == TP_STATUS_OK) {
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

    char *owned_artifact_path =
        status == TP_STATUS_OK ? worker_strdup(artifact_path) : NULL;
    if (status == TP_STATUS_OK && !owned_artifact_path) {
        status = tp_error_set(err, TP_STATUS_OOM,
                              "job worker Pack artifact path allocation failed");
    }
    tp_id128 current_hash = tp_id128_nil();
    tp_error freshness_error = {{0}};
    tp_status freshness_status = TP_STATUS_OK;
    if (status == TP_STATUS_OK) {
        freshness_status = tp_pack_input_hash_compute(
            &settings,
            request->preview_exporter_id &&
                    request->preview_exporter_id[0]
                ? request->preview_exporter_id
                : NULL,
            NULL, &current_hash, &freshness_error);
    }
    /* A cancellation admitted while the artifact was produced, hashed, or probed
     * for freshness outranks success. Fold it HERE -- after the LAST long step
     * and before a single pack field is published -- because this is the last
     * point where the private directory can still be removed. Folding it before
     * the freshness probe left a cancel arriving during that probe to the
     * terminal-frame boundary, which publishes an artifact path nobody adopts
     * and a directory nobody deletes. */
    if (status == TP_STATUS_OK && tp_cancel_source_fired(cancel)) {
        status = tp_error_set(
            err, TP_STATUS_CANCELLED,
            "Pack was cancelled before its result was published");
    }

    if (status == TP_STATUS_OK) {
        response->pack.atlas_id = request->atlas_id;
        response->pack.missing_sources = input.missing_sources;
        response->pack.input_token_at_start = request->input_token;
        response->pack.pack_input_hash = input_hash;
        response->pack.current_pack_input_hash = current_hash;
        response->pack.freshness_token = request->input_token;
        response->pack.freshness_status = freshness_status;
        response->pack.freshness_error = freshness_error;
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
        response->pack.artifact_path = owned_artifact_path;
        response->pack.artifact_size = artifact_size;
    } else {
        /* Nothing will be adopted: take the private directory (and whatever the
         * builder published into it) with us rather than leaving it for the
         * cross-run reaper. */
        free(owned_artifact_path);
        free_pack_names(names, owned_name_count);
        tp_worker_remove_dir_tree(request_dir);
    }
    tp_pack_input_free(&input);
    return status;
}

static tp_status run_export(const tp_job_worker_proto_request *request,
                            tp_project *project, tp_cancel_source *cancel,
                            tp_job_worker_proto_response *response,
                            bool *out_terminal_claimed,
                            tp_error *err) {
    if (out_terminal_claimed) {
        *out_terminal_claimed = false;
    }
    const tp_cancel_token cancel_token = tp_cancel_source_token(cancel);
    char request_dir[TP_IDENTITY_PATH_MAX + 32];
    bool request_dir_too_long = false;
    if (!make_request_dir(request->work_dir, request->request_id,
                          request->host_pid, request_dir, sizeof request_dir,
                          &request_dir_too_long)) {
        return request_dir_too_long
                   ? tp_error_set(
                         err, TP_STATUS_INVALID_ARGUMENT,
                         "job worker Export work directory path is too long")
                   : tp_error_set(
                         err, TP_STATUS_FILE_IO_FAILED,
                         "job worker could not create its private Export directory");
    }

    export_terminal_context terminal_context = {
        .request_id = request->request_id,
    };
    const tp_export_snapshot_job_opts opts = {
        .terminal_boundary = publish_export_terminal_boundary,
        .terminal_boundary_context = &terminal_context,
    };
    tp_export_snapshot_job *job = NULL;
    tp_status status = tp_export_project_job_create_internal(
        project, request_dir, &opts, &job, err);
    if (status != TP_STATUS_OK) {
        tp_worker_remove_dir_tree(request_dir);
        return status;
    }
    int eligible = 0;
    const int atlas_count =
        tp_export_snapshot_job_atlas_count(job);
    for (int i = 0; i < atlas_count; ++i) {
        tp_export_snapshot_atlas_info info = {0};
        if (tp_export_snapshot_job_atlas_info(
                job, i, &info, NULL) == TP_STATUS_OK &&
            (tp_id128_is_nil(request->atlas_id) ||
             tp_id128_eq(request->atlas_id,
                         info.atlas_id)) &&
            info.enabled_target_count > 0) {
            ++eligible;
        }
    }
    if (eligible == 0) {
        tp_export_snapshot_job_destroy(job);
        tp_worker_remove_dir_tree(request_dir);
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "nothing to export");
    }
    terminal_context.eligible = eligible;

    tp_status first_status = TP_STATUS_OK;
    int current = 0;
    bool cancellation_ended_work = false;
    for (int i = 0; i < atlas_count; ++i) {
        tp_export_snapshot_atlas_info info = {0};
        status = tp_export_snapshot_job_atlas_info(
            job, i, &info, err);
        if (status != TP_STATUS_OK) {
            first_status = status;
            break;
        }
        if (!tp_id128_is_nil(request->atlas_id) &&
            !tp_id128_eq(request->atlas_id,
                         info.atlas_id)) {
            continue;
        }
        if (info.enabled_target_count == 0) {
            continue;
        }
        if (tp_cancel_source_poll(cancel)) {
            cancellation_ended_work = true;
            break;
        }
        current++;
        terminal_context.current = current;
        (void)publish_progress(
            request->request_id, current - 1, eligible,
            TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL);
        tp_error atlas_error = {{0}};
        tp_arena *arena = tp_arena_create(0);
        tp_export_notices notices;
        tp_export_notices_init(&notices);
        tp_export_report report = {0};
        int runs = 0;
        if (arena) {
            (void)publish_progress(
                request->request_id, current, eligible,
                TP_JOB_WORKER_PHASE_EXPORT_WRITE);
#ifdef TP_ENABLE_TEST_SEAMS
            /* Lets client tests observe a real non-zero worker progress frame
             * before terminal publication. The process inherited this value;
             * changing the parent environment after spawn cannot affect it. */
            const unsigned long progress_block_ms =
                test_block_ms(
                    "TP_TEST_JOB_WORKER_BLOCK_AFTER_EXPORT_PROGRESS_MS");
            if (progress_block_ms > 0UL) {
                worker_sleep_ms(progress_block_ms);
            }
#endif
            status =
                tp_export_snapshot_job_run_atlas_ex_cancellable(
                    job, i, arena, &notices, &report, &runs,
                    NULL, NULL, &cancel_token, &atlas_error);
        } else {
            status = tp_error_set(
                &atlas_error, TP_STATUS_OOM,
                "job worker could not allocate Export arena");
        }
        if (report.input_outcome ==
            TP_EXPORT_INPUT_NO_USABLE_IMAGES) {
            response->export_result.atlases_skipped++;
            tp_export_notices_free(&notices);
            tp_arena_destroy(arena);
            continue;
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
                response->export_result.publication_uncertain |=
                    report.targets[target].publication_uncertain;
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
                    "%s: %s", info.name,
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
                    "%s: %s", info.name,
                    atlas_error.msg[0]
                        ? atlas_error.msg
                        : tp_status_str(status));
            }
        }
        tp_export_notices_free(&notices);
        tp_arena_destroy(arena);
        if (status == TP_STATUS_CANCELLED) {
            cancellation_ended_work = true;
            break;
        }
    }
    /* The per-writer callback is the narrow boundary when the final eligible
     * target actually invokes a writer. A command can still have published
     * earlier targets and end with only skipped/pre-writer-failure work. Once
     * that tail has returned there is no future writer, so publish the same
     * boundary here rather than letting a later Cancel rewrite real output. */
    if (!terminal_context.claimed &&
        !cancellation_ended_work &&
        (response->export_result.targets > 0 ||
         response->export_result.publication_uncertain)) {
        (void)publish_export_terminal_boundary_now(
            &terminal_context);
    }
    /* Nothing in here is published to the host: the exported files already went
     * to their target output paths, and the staged `.ntpack` was an internal
     * step. Take it with us on success, failure, and cancellation alike. */
    tp_export_snapshot_job_destroy(job);
    tp_worker_remove_dir_tree(request_dir);
    const bool cancelled =
        !terminal_context.claimed &&
        tp_cancel_source_fired(cancel);
    if (out_terminal_claimed) {
        *out_terminal_claimed = terminal_context.claimed;
    }
    response->export_result.partial_publication =
        response->export_result.targets > 0 &&
        (cancelled || first_status != TP_STATUS_OK);
    return cancelled ? TP_STATUS_CANCELLED : first_status;
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
    bool export_terminal_claimed = false;

    tp_project *project = NULL;
    tp_cancel_source cancel = {.probe = child_stdin_probe};
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
            &request, project, &cancel, &response,
            &export_terminal_claimed, &error);
    } else if (status == TP_STATUS_OK) {
        status = tp_error_set(
            &error, TP_STATUS_INVALID_ARGUMENT,
            "job worker request kind is invalid");
    }
    response.elapsed_ms = worker_now_ms() - start;
    /* One terminal decision for BOTH kinds. An observed cancellation -- the
     * cancel byte or a broken control channel -- outranks whatever the run
     * returned, and the STATUS is folded with the state so the encoder's
     * consistency rule admits the frame. Before this fold a cancelled Pack
     * encoded SUCCEEDED+CANCELLED, was rejected, the worker exited with no frame
     * at all, and the host reported "job worker process crashed"; a Pack or
     * Export whose control channel died reported a silently truncated success. */
    if (tp_cancel_source_fired(&cancel) &&
        !export_terminal_claimed &&
        status != TP_STATUS_CANCELLED) {
        status = tp_error_set(
            &error, TP_STATUS_CANCELLED, "%s",
            tp_cancel_source_reason(&cancel) == TP_CANCEL_REASON_CONTROL_LOST
                ? "job worker control channel failed"
                : "job was cancelled");
    }
    response.status = status;
    response.error = error;
    response.state =
        response.status == TP_STATUS_CANCELLED
            ? TP_SESSION_JOB_CANCELLED
            : response.status == TP_STATUS_OK
                  ? TP_SESSION_JOB_SUCCEEDED
                  : TP_SESSION_JOB_FAILED;

    uint8_t *encoded = NULL;
    size_t encoded_length = 0U;
    tp_error encode_error = {{0}};
    tp_status encode_status = tp_job_worker_proto_encode_response(
        &response, &encoded, &encoded_length, &encode_error);
    bool payload_dropped = false;
    if (encode_status != TP_STATUS_OK) {
        /* Fail closed for real: a payload the codec refuses (a name map past the
         * frame cap is the reachable case) used to end the worker with no frame
         * at all, which the host could only report as "process crashed". Send the
         * same terminal without the payload instead, so the operator gets the
         * structured encode error. */
        tp_job_worker_proto_response fallback = {0};
        fallback.kind = response.kind;
        fallback.session_instance_generation =
            response.session_instance_generation;
        fallback.request_id = response.request_id;
        fallback.state = TP_SESSION_JOB_FAILED;
        fallback.status = encode_status;
        fallback.elapsed_ms = response.elapsed_ms;
        (void)tp_error_set(&fallback.error, encode_status, "%s",
                           encode_error.msg[0]
                               ? encode_error.msg
                               : "job worker terminal frame could not be encoded");
        encode_status = tp_job_worker_proto_encode_response(
            &fallback, &encoded, &encoded_length, NULL);
        payload_dropped = true;
    }
    const bool wrote =
        encode_status == TP_STATUS_OK &&
        write_frame(encoded, encoded_length);
    free(encoded);
    if (response.kind == TP_SESSION_JOB_PACK && response.pack.artifact_path &&
        (!wrote || payload_dropped)) {
        /* The artifact was announced to nobody: the frame carrying its path never
         * reached the host (or carried no payload at all), so this process is the
         * last owner of the file and its private directory. */
        remove_request_dir_of(response.pack.artifact_path);
    }
    if (response.kind == TP_SESSION_JOB_PACK) {
        free_pack_names(
            (tp_job_worker_proto_name *)response.pack.names,
            response.pack.name_count);
        free((void *)response.pack.artifact_path);
    }
    tp_project_destroy(project);
    tp_job_worker_proto_request_free(&request);
    return wrote ? 0 : 1;
}
