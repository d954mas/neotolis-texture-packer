#include "tp_core/tp_job.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "core/nt_assert.h"
#include "nt_pack_format.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_input.h"
#include "tp_name_map.h"
#include "tp_core/tp_pack_hash.h"
#include "tp_pack_read.h"
#include "tp_core/tp_project.h"
#include "tp_fs_internal.h"
#include "tp_job_owner_internal.h"
#include "tp_job_worker_process_internal.h"
#include "tp_pack_hash_priv.h"
#include "tp_project_internal.h"
#include "tp_session_snapshot_internal.h"

/* Bytes the host lifts out of the worker's artifact file per pump call. The pump
 * runs on the UI thread, so a 256 MiB multi-page atlas is spread over ~16 frames
 * instead of stalling one. */
#define TP_JOB_ARTIFACT_READ_BUDGET ((size_t)16U << 20)

/* Hard cap on the artifact the host will allocate for in one piece. The size is
 * whatever a separate process announced and the file system confirmed, so
 * without a cap a runaway or corrupt Pack turns into a multi-GB malloc and an
 * OOM kill instead of a structured terminal error. Same bound the transport
 * admits, so a reply that passes decode can never exceed it either. */
#define TP_JOB_ARTIFACT_MAX_BYTES TP_JOB_WORKER_PROTO_MAX_ARTIFACT_BYTES

typedef enum tp_job_terminal_claim {
    TP_JOB_TERMINAL_OPEN = 0,
    TP_JOB_TERMINAL_CANCEL_REQUESTED,
    TP_JOB_TERMINAL_CLAIMED
} tp_job_terminal_claim;

typedef struct tp_live_job {
    tp_session_owned_job owner;
    tp_session_job_kind kind;
    tp_job_worker_process *process;
    atomic_flag pump_gate;
    _Atomic int state;
    _Atomic int terminal_claim;
    _Atomic int current;
    _Atomic int total;
    _Atomic bool cancellation_accepted;
    double started_ms;
    double elapsed_ms;

    char *project_json;
    size_t project_json_len;
    char *project_dir;
    char *work_dir;
    char *preview_exporter_id;
    tp_id128 atlas_id;
    tp_session_input_token input_token_at_start;

    tp_session_job_target pack_observation_target;
    tp_session_job_target *observation_targets;
    size_t observation_target_count;

    tp_arena *arena;
    tp_result *pack_result;
    tp_session_job_result terminal_result;

    /* Chunked adoption of the worker's Pack artifact (job_adopt_artifact). The
     * file is a transient private handoff: it is read in budgeted slices, parsed
     * once, and then deleted along with its per-request directory. */
    char *artifact_path;
    FILE *artifact_file;
    uint8_t *artifact_bytes;
    size_t artifact_expected;
    size_t artifact_read;
    tp_status artifact_status;
    tp_error artifact_error;
    bool artifact_settled;
} tp_live_job;

#ifdef TP_ENABLE_TEST_SEAMS
static bool s_fail_next_observation_target_allocation;
static bool s_fail_next_artifact_path_allocation;

void tp_job__test_fail_next_observation_target_allocation(void) {
    s_fail_next_observation_target_allocation = true;
}

void tp_job__test_fail_next_artifact_path_allocation(void) {
    s_fail_next_artifact_path_allocation = true;
}

void tp_job__test_set_worker_timeout_ms(int timeout_ms) {
    tp_job_worker__test_set_timeout_ms(timeout_ms);
}

void tp_job__test_set_worker_cancel_grace_ms(int grace_ms) {
    tp_job_worker__test_set_cancel_grace_ms(grace_ms);
}

void tp_job__test_reset_all(void) {
    s_fail_next_observation_target_allocation = false;
    s_fail_next_artifact_path_allocation = false;
    tp_job_worker__test_reset();
}
#endif

static void job_discard_artifact(tp_live_job *job);
static void job_discard_unadopted_artifact(
    tp_live_job *job, const tp_job_worker_proto_response *response);

static void *job_observation_target_calloc(size_t count, size_t size) {
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_fail_next_observation_target_allocation) {
        s_fail_next_observation_target_allocation = false;
        return NULL;
    }
#endif
    return calloc(count, size);
}

static double job_now_ms(void) {
#ifdef _WIN32
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

static uint32_t job_host_pid(void) {
#ifdef _WIN32
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

static char *job_strdup(const char *text) {
    if (!text) {
        return NULL;
    }
    const size_t length = strlen(text) + 1U;
    char *copy = malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

static char *job_artifact_path_strdup(const char *text) {
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_fail_next_artifact_path_allocation) {
        s_fail_next_artifact_path_allocation = false;
        return NULL;
    }
#endif
    return job_strdup(text);
}

static bool job_request_cancel(tp_live_job *job) {
    int expected = TP_JOB_TERMINAL_OPEN;
    if (!atomic_compare_exchange_strong_explicit(
            &job->terminal_claim, &expected,
            TP_JOB_TERMINAL_CANCEL_REQUESTED, memory_order_acq_rel,
            memory_order_acquire)) {
        return false;
    }
    atomic_store_explicit(&job->cancellation_accepted, true,
                          memory_order_release);
    return true;
}

/* Returns true when an accepted host cancellation owns the terminal
 * linearization. Child completion never races mutable session state. */
static bool job_claim_terminal(tp_live_job *job) {
    int expected = TP_JOB_TERMINAL_OPEN;
    if (atomic_compare_exchange_strong_explicit(
            &job->terminal_claim, &expected, TP_JOB_TERMINAL_CLAIMED,
            memory_order_acq_rel, memory_order_acquire)) {
        return false;
    }
    if (expected == TP_JOB_TERMINAL_CANCEL_REQUESTED) {
        atomic_store_explicit(&job->terminal_claim, TP_JOB_TERMINAL_CLAIMED,
                              memory_order_release);
        return true;
    }
    return false;
}

static void job_cancel_owned(tp_session_owned_job *owned) {
    (void)job_request_cancel((tp_live_job *)owned);
}

/* The session rejected this job's terminal result (cancelled, or its targets were
 * deleted), so nothing will ever adopt the packed pages. Free the arena now rather
 * than at destroy: for a big Pack that is every page's pixels, held for as long as
 * the host takes to consume the terminal state. The terminal result keeps its
 * status/error/metadata -- only the released pointers are cleared, so a later
 * tp_session_update reads NULL pack pointers, never freed memory. The
 * descriptor and observation_targets are untouched (the session borrows them).
 *
 * A released Pack payload also stops claiming SUCCEEDED. Leaving the terminal
 * state alone let tp_session_update hand back a SUCCEEDED Pack with
 * NULL arena/result -- a caller that trusts the state (they all do) then
 * dereferences NULL. The rejection is reported as what it is: CANCELLED when
 * this job's own cancellation was accepted, otherwise a structured failure for
 * the only other rejection the session has (its targets were deleted). */
static void job_release_payload_owned(tp_session_owned_job *owned) {
    tp_live_job *job = (tp_live_job *)owned;
    if (!job) {
        return;
    }
    tp_arena_destroy(job->arena);
    job->arena = NULL;
    job->pack_result = NULL;
    job->terminal_result.pack.arena = NULL;
    job->terminal_result.pack.result = NULL;
    if (job->kind != TP_SESSION_JOB_PACK ||
        job->terminal_result.state != TP_SESSION_JOB_SUCCEEDED) {
        return;
    }
    const bool cancelled = atomic_load_explicit(
        &job->cancellation_accepted, memory_order_acquire);
    job->terminal_result.state = cancelled ? TP_SESSION_JOB_CANCELLED
                                           : TP_SESSION_JOB_FAILED;
    job->terminal_result.status = tp_error_set(
        &job->terminal_result.error,
        cancelled ? TP_STATUS_CANCELLED : TP_STATUS_NOT_FOUND,
        cancelled ? "Pack cancelled"
                  : "Pack result was discarded: its targets no longer exist");
    atomic_store_explicit(&job->state, job->terminal_result.state,
                          memory_order_release);
}

static void job_destroy_owned(tp_session_owned_job *owned) {
    tp_live_job *job = (tp_live_job *)owned;
    if (!job) {
        return;
    }
    (void)job_request_cancel(job);
    if (job->process) {
        tp_job_worker_process_request_cancel(job->process);
    }
    /* Destroyed before its terminal was published (session teardown, or a
     * cancelled job that never re-entered the pump): an artifact announced but
     * never adopted is still on disk and only the wire path names it. */
    job_discard_unadopted_artifact(
        job, tp_job_worker_process_response(job->process));
    tp_job_worker_process_destroy(job->process);
    job_discard_artifact(job);
    tp_arena_destroy(job->arena);
    free(job->project_json);
    free(job->project_dir);
    free(job->work_dir);
    free(job->preview_exporter_id);
    free(job->observation_targets);
    free(job);
}

static tp_live_job *job_create(tp_session_job_kind kind) {
    tp_live_job *job = calloc(1U, sizeof *job);
    if (!job) {
        return NULL;
    }
    tp_session_owned_job_init(&job->owner, job_cancel_owned,
                              job_destroy_owned);
    job->owner.pump = NULL;
    job->owner.release_payload = job_release_payload_owned;
    job->kind = kind;
    atomic_flag_clear(&job->pump_gate);
    atomic_init(&job->state, TP_SESSION_JOB_RUNNING);
    atomic_init(&job->terminal_claim, TP_JOB_TERMINAL_OPEN);
    atomic_init(&job->current, 0);
    atomic_init(&job->total, 0);
    atomic_init(&job->cancellation_accepted, false);
    return job;
}

static bool job_observe(tp_session_owned_job *owned,
                        tp_session_job_sample *out) {
    tp_live_job *job = (tp_live_job *)owned;
    memset(out, 0, sizeof *out);
    out->state = (tp_session_job_state)atomic_load_explicit(
        &job->state, memory_order_acquire);
    out->current =
        atomic_load_explicit(&job->current, memory_order_relaxed);
    out->total =
        atomic_load_explicit(&job->total, memory_order_relaxed);
    out->cancellation_requested = atomic_load_explicit(
        &job->cancellation_accepted, memory_order_acquire);
    if (out->state != TP_SESSION_JOB_RUNNING) {
        out->terminal_status = job->terminal_result.status;
        out->terminal_error = job->terminal_result.error;
        out->terminal_result = &job->terminal_result;
    }
    return true;
}

static tp_status job_build_name_map(
    const tp_job_worker_proto_pack_result *pack, tp_name_map **out,
    tp_error *err) {
    *out = tp_name_map_create();
    if (!*out) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "Pack result name map allocation failed");
    }
    for (uint32_t i = 0U; i < pack->name_count; ++i) {
        const tp_status status = tp_name_map_insert_hashed(
            *out, pack->names[i].hash, pack->names[i].name);
        if (status != TP_STATUS_OK) {
            tp_name_map_destroy(*out);
            *out = NULL;
            return tp_error_set(err, status,
                                "Pack result name map is invalid");
        }
    }
    return TP_STATUS_OK;
}

/* Remove the worker's private per-request directory once its artifact is gone.
 * Only a "req-" basename is ever removed, so a malformed or hostile path can
 * never make the host delete a directory it did not hand out. */
static void job_remove_request_dir(const char *artifact_path) {
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
    if (length == 0U || length >= TP_IDENTITY_PATH_MAX) {
        return;
    }
    char directory[TP_IDENTITY_PATH_MAX];
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
    (void)tp_fs_remove_dir(directory);
}

/* The artifact path is worker-reported, so it is UNTRUSTED input: a corrupted or
 * hostile terminal frame naming an arbitrary existing file must never make the
 * host open it -- much less delete it. The host cannot predict the name (only the
 * worker knows the request directory it created), so it validates the shape it
 * does own: the path lies under this job's work_dir, its parent is a direct
 * "req-" child of that work_dir, both components are non-empty and neither is
 * "..". No canonicalisation is needed because a ".." segment is rejected outright
 * rather than resolved, and the work_dir prefix is compared byte for byte: the
 * worker builds the path by concatenating the very bytes the host sent it, so an
 * honest reply always matches exactly and any spelling that does not is refused
 * (the check can only get stricter, never looser). A path that fails is never
 * stored, opened, or removed. */
static bool job_artifact_path_is_contained(const tp_live_job *job,
                                           const char *path) {
    if (!job || !job->work_dir || job->work_dir[0] == '\0' || !path ||
        path[0] == '\0' || strlen(path) >= TP_IDENTITY_PATH_MAX) {
        return false;
    }
    size_t work_length = strlen(job->work_dir);
    while (work_length > 0U && (job->work_dir[work_length - 1U] == '/' ||
                                job->work_dir[work_length - 1U] == '\\')) {
        work_length--;
    }
    if (work_length == 0U ||
        strncmp(path, job->work_dir, work_length) != 0) {
        return false;
    }
    const char *cursor = path + work_length;
    if (*cursor != '/' && *cursor != '\\') {
        return false;
    }
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
    const char *directory = cursor;
    while (*cursor && *cursor != '/' && *cursor != '\\') {
        cursor++;
    }
    const size_t directory_length = (size_t)(cursor - directory);
    if (directory_length < 4U || strncmp(directory, "req-", 4U) != 0 ||
        (*cursor != '/' && *cursor != '\\')) {
        return false;
    }
    const char *file = cursor + 1;
    if (file[0] == '\0' || strcmp(file, "..") == 0) {
        return false;
    }
    for (const char *scan = file; *scan; ++scan) {
        if (*scan == '/' || *scan == '\\') {
            return false; /* exactly one file inside exactly one req- directory */
        }
    }
    return true;
}

/* The artifact is a transient private handoff, never a cache: release it as soon
 * as its bytes are in this process or can never be. Whatever a killed host leaves
 * behind is swept on the next Pack by the reaper, which keys the private
 * directory's name on the HOST pid -- the directory outlives its worker. */
static void job_discard_artifact(tp_live_job *job) {
    if (job->artifact_file) {
        (void)fclose(job->artifact_file);
        job->artifact_file = NULL;
    }
    free(job->artifact_bytes);
    job->artifact_bytes = NULL;
    if (job->artifact_path) {
        (void)tp_fs_remove_file(job->artifact_path);
        job_remove_request_dir(job->artifact_path);
        free(job->artifact_path);
        job->artifact_path = NULL;
    }
}

/* Delete an artifact the worker published but this host never adopted. A
 * cancelled Pack skips the read entirely, so job->artifact_path was never filled
 * and job_discard_artifact has nothing to remove -- yet a real `.ntpack` and its
 * private directory are sitting on disk. The only handle on them is the wire
 * path, which gets the SAME containment check as the adoption path; a path that
 * fails it is left strictly alone. */
static void job_discard_unadopted_artifact(
    tp_live_job *job, const tp_job_worker_proto_response *response) {
    if (!job || job->artifact_path || job->artifact_settled ||
        job->kind != TP_SESSION_JOB_PACK || !response ||
        response->kind != TP_SESSION_JOB_PACK ||
        !response->pack.artifact_path ||
        response->pack.artifact_path[0] == '\0' ||
        !job_artifact_path_is_contained(job, response->pack.artifact_path)) {
        return;
    }
    (void)tp_fs_remove_file(response->pack.artifact_path);
    job_remove_request_dir(response->pack.artifact_path);
}

static tp_status job_artifact_fail(tp_live_job *job, tp_status status,
                                   const char *message) {
    job->artifact_status =
        tp_error_set(&job->artifact_error, status, "%s", message);
    job->artifact_settled = true;
    job_discard_artifact(job);
    return job->artifact_status;
}

/* Open + validate the artifact the worker published. Digestless by design: the
 * file is ours, produced moments ago in a directory only this request's worker
 * wrote to, so a real regular file of exactly the announced size whose first
 * bytes are the pack magic is proof enough -- a full-file hash would cost
 * seconds on a large atlas and prove nothing extra. */
static bool job_artifact_open(tp_live_job *job,
                              const tp_job_worker_proto_pack_result *pack) {
    if (!pack->artifact_path || pack->artifact_path[0] == '\0' ||
        pack->artifact_size == 0U) {
        (void)job_artifact_fail(job, TP_STATUS_BUILDER_FAILED,
                                "Pack worker returned no artifact path");
        return false;
    }
    /* Containment first: a path that fails it is UNTRUSTED and is never
     * stored, opened, or removed. */
    if (!job_artifact_path_is_contained(job, pack->artifact_path)) {
        (void)job_artifact_fail(
            job, TP_STATUS_BUILDER_FAILED,
            "Pack artifact path is outside this job's private request directory");
        return false;
    }
    /* The path is stored the moment it is trusted: job_artifact_fail deletes
     * the file and its request directory THROUGH the stored path, so every
     * later failure in this adoption cleans up a real contained artifact
     * instead of orphaning it -- job_artifact_fail settles the artifact, which
     * disarms the destroy-time unadopted backstop for this job. */
    job->artifact_path = job_artifact_path_strdup(pack->artifact_path);
    if (!job->artifact_path) {
        /* The one failure that trusts the path but has nowhere to store it:
         * delete through the wire path directly, exactly what the backstop
         * this fail() disarms would have done. */
        (void)tp_fs_remove_file(pack->artifact_path);
        job_remove_request_dir(pack->artifact_path);
        (void)job_artifact_fail(job, TP_STATUS_OOM,
                                "Pack artifact path allocation failed");
        return false;
    }
    /* Below four bytes the magic check would be skipped, and above the cap the
     * host would allocate an artifact no real atlas produces. */
    if (pack->artifact_size < 4U ||
        pack->artifact_size > TP_JOB_ARTIFACT_MAX_BYTES) {
        (void)job_artifact_fail(
            job, TP_STATUS_BUILDER_FAILED,
            "Pack artifact size is too small to be a pack file or exceeds the host cap");
        return false;
    }
    tp_fs_info info;
    if (!tp_fs_stat(job->artifact_path, &info) ||
        info.kind != TP_FS_KIND_REGULAR || info.reparse ||
        info.size != pack->artifact_size) {
        (void)job_artifact_fail(
            job, TP_STATUS_BUILDER_FAILED,
            "Pack artifact is missing, is not a regular file, or changed size");
        return false;
    }
    job->artifact_expected = (size_t)pack->artifact_size;
    job->artifact_bytes = malloc(job->artifact_expected);
    if (!job->artifact_bytes) {
        (void)job_artifact_fail(job, TP_STATUS_OOM,
                                "Pack artifact allocation failed");
        return false;
    }
    job->artifact_file = tp_fs_fopen(job->artifact_path, "rb");
    if (!job->artifact_file) {
        (void)job_artifact_fail(job, TP_STATUS_BUILDER_FAILED,
                                "Pack artifact could not be opened");
        return false;
    }
    return true;
}

/* Lift at most TP_JOB_ARTIFACT_READ_BUDGET bytes per call. Returns true once the
 * artifact is settled -- fully read, or failed with a structured error the
 * terminal will carry (the previous Pack result is left untouched either way). */
static bool job_adopt_artifact(tp_live_job *job,
                               const tp_job_worker_proto_pack_result *pack) {
    if (job->artifact_settled) {
        return true;
    }
    if (!job->artifact_file && !job_artifact_open(job, pack)) {
        return true;
    }
    const size_t remaining = job->artifact_expected - job->artifact_read;
    const size_t budget = remaining < TP_JOB_ARTIFACT_READ_BUDGET
                              ? remaining
                              : TP_JOB_ARTIFACT_READ_BUDGET;
    const size_t got = fread(job->artifact_bytes + job->artifact_read, 1U,
                             budget, job->artifact_file);
    job->artifact_read += got;
    if (got != budget) {
        (void)job_artifact_fail(job, TP_STATUS_BUILDER_FAILED,
                                "Pack artifact ended before its declared size");
        return true;
    }
    if (job->artifact_read >= 4U) {
        uint32_t magic = 0U;
        memcpy(&magic, job->artifact_bytes, sizeof magic);
        if (magic != NT_PACK_MAGIC) {
            (void)job_artifact_fail(job, TP_STATUS_BAD_MAGIC,
                                    "Pack artifact is not a pack file");
            return true;
        }
    }
    if (job->artifact_read < job->artifact_expected) {
        return false; /* more budget next pump; the job stays RUNNING */
    }
    (void)fclose(job->artifact_file);
    job->artifact_file = NULL;
    job->artifact_settled = true;
    return true;
}

static tp_status job_inflate_pack(
    tp_live_job *job, const tp_job_worker_proto_pack_result *pack,
    tp_error *err) {
    if (job->artifact_status != TP_STATUS_OK) {
        *err = job->artifact_error;
        return job->artifact_status;
    }
    tp_name_map *names = NULL;
    tp_status status = job_build_name_map(pack, &names, err);
    if (status != TP_STATUS_OK) {
        /* The artifact is dead the moment the inflate fails: nothing else will
         * ever read it, so release the staged bytes, the file and the private
         * directory here rather than leaving them to job destruction. */
        job_discard_artifact(job);
        return status;
    }
    job->arena = tp_arena_create(0);
    if (!job->arena) {
        tp_name_map_destroy(names);
        return tp_error_set(err, TP_STATUS_OOM,
                            "Pack result arena allocation failed");
    }
    tp_result **results = NULL;
    int result_count = 0;
    status = tp_pack_read_memory(
        job->artifact_bytes, job->artifact_read, names, job->arena,
        &results, &result_count, err);
    tp_name_map_destroy(names);
    /* The reader copies every name and page into the arena, so the staged bytes
     * and the file itself are dead the moment it returns. */
    job_discard_artifact(job);
    if (status == TP_STATUS_OK && result_count != 1) {
        status = tp_error_set(
            err, TP_STATUS_BUILDER_FAILED,
            "Pack worker returned %d atlas results; expected one",
            result_count);
    }
    if (status != TP_STATUS_OK) {
        tp_arena_destroy(job->arena);
        job->arena = NULL;
        return status;
    }
    job->pack_result = results[0];
    return TP_STATUS_OK;
}

static void job_copy_pack_metadata(
    tp_session_pack_job_result *out,
    const tp_job_worker_proto_pack_result *pack) {
    out->atlas_id = pack->atlas_id;
    out->missing_sources = pack->missing_sources;
    out->input_token_at_start = pack->input_token_at_start;
    out->pack_input_hash = pack->pack_input_hash;
    out->current_pack_input_hash = pack->current_pack_input_hash;
    out->freshness_token = pack->freshness_token;
    out->freshness = pack->freshness;
    out->freshness_reason = pack->freshness_reason;
    out->freshness_status = pack->freshness_status;
    out->freshness_error = pack->freshness_error;
    memcpy(out->preview_exporter_id, pack->preview_exporter_id,
           sizeof out->preview_exporter_id);
}

static void job_publish_response(
    tp_live_job *job, const tp_job_worker_proto_response *response) {
    tp_session_job_result *result = &job->terminal_result;
    memset(result, 0, sizeof *result);
    result->kind = job->kind;
    result->elapsed_ms = response ? response->elapsed_ms
                                  : job_now_ms() - job->started_ms;

    const bool cancelled = job_claim_terminal(job);
    if (response && job->kind == TP_SESSION_JOB_EXPORT) {
        result->export_result = response->export_result;
    }
    if (cancelled) {
        /* The single cancel decision point: an accepted host cancellation
         * outranks whatever terminal raced it, and nothing will adopt the
         * worker's artifact, so drop it here rather than at job destruction.
         * A cancel that raced a SUCCEEDED terminal never opened the artifact at
         * all, so the wire path is the only handle on the file the worker did
         * publish -- take it too, or the `.ntpack` outlives the run. */
        job_discard_artifact(job);
        job_discard_unadopted_artifact(job, response);
        result->state = TP_SESSION_JOB_CANCELLED;
        result->status = tp_error_set(
            &result->error, TP_STATUS_CANCELLED, "%s cancelled",
            job->kind == TP_SESSION_JOB_PACK ? "Pack" : "Export");
        if (job->kind == TP_SESSION_JOB_EXPORT) {
            result->export_result.partial_publication =
                result->export_result.targets > 0;
        }
        job->elapsed_ms = result->elapsed_ms;
        atomic_store_explicit(&job->state, result->state,
                              memory_order_release);
        return;
    }

    if (!response ||
        response->kind != job->kind ||
        response->session_instance_generation !=
            job->owner.observation_descriptor
                .session_instance_generation ||
        response->request_id !=
            job->owner.observation_descriptor.request_id) {
        result->state = TP_SESSION_JOB_FAILED;
        result->status = tp_error_set(
            &result->error, TP_STATUS_BUILDER_FAILED,
            "job worker returned mismatched terminal identity");
    } else {
        result->state = response->state;
        result->status = response->status;
        result->error = response->error;
        if (job->kind == TP_SESSION_JOB_PACK) {
            job_copy_pack_metadata(&result->pack, &response->pack);
            if (response->state == TP_SESSION_JOB_SUCCEEDED) {
                tp_error inflate_error = {{0}};
                const tp_status inflate_status =
                    job_inflate_pack(job, &response->pack,
                                     &inflate_error);
                if (inflate_status != TP_STATUS_OK) {
                    result->state = TP_SESSION_JOB_FAILED;
                    result->status = inflate_status;
                    result->error = inflate_error;
                } else {
                    result->pack.arena = job->arena;
                    result->pack.result = job->pack_result;
                }
            }
        }
    }
    job->elapsed_ms = result->elapsed_ms;
    if (result->state == TP_SESSION_JOB_SUCCEEDED) {
        const int total =
            atomic_load_explicit(&job->total, memory_order_relaxed);
        atomic_store_explicit(&job->current, total,
                              memory_order_relaxed);
    }
    atomic_store_explicit(&job->state, result->state,
                          memory_order_release);
}

static void job_pump_owned(tp_session_owned_job *owned) {
    tp_live_job *job = (tp_live_job *)owned;
    if (!job->process ||
        atomic_load_explicit(&job->state, memory_order_acquire) !=
            TP_SESSION_JOB_RUNNING ||
        atomic_flag_test_and_set_explicit(&job->pump_gate,
                                          memory_order_acquire)) {
        return;
    }

    if (atomic_load_explicit(&job->cancellation_accepted,
                             memory_order_acquire)) {
        tp_job_worker_process_request_cancel(job->process);
    }
    tp_job_worker_process_pump(job->process);
    const tp_job_worker_proto_progress progress =
        tp_job_worker_process_progress(job->process);
    if (progress.request_id ==
            job->owner.observation_descriptor.request_id &&
        progress.current >= 0 && progress.total >= 0 &&
        progress.current <= progress.total) {
        atomic_store_explicit(&job->current, progress.current,
                              memory_order_relaxed);
        atomic_store_explicit(&job->total, progress.total,
                              memory_order_relaxed);
    }
    if (tp_job_worker_process_terminal(job->process)) {
        const tp_job_worker_proto_response *response =
            tp_job_worker_process_response(job->process);
        /* A successful Pack terminal only announces its artifact; the bytes are
         * adopted over the following pumps, so the job stays RUNNING until the
         * file is in hand. A cancelled job skips the read entirely -- its result
         * is going to be discarded. */
        if (response && job->kind == TP_SESSION_JOB_PACK &&
            response->state == TP_SESSION_JOB_SUCCEEDED &&
            !atomic_load_explicit(&job->cancellation_accepted,
                                  memory_order_acquire) &&
            !job_adopt_artifact(job, &response->pack)) {
            atomic_flag_clear_explicit(&job->pump_gate,
                                       memory_order_release);
            return;
        }
        job_publish_response(job, response);
    }
    atomic_flag_clear_explicit(&job->pump_gate, memory_order_release);
}

static tp_status job_prepare_snapshot_request(
    tp_session_snapshot *snapshot, const char *work_dir,
    tp_live_job *job, tp_error *err) {
    const tp_project *project = snapshot->project;
    tp_status status = tp_project_checkpoint_save_buffer(
        project, &job->project_json, &job->project_json_len, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const char *project_dir =
        project->project_dir
            ? project->project_dir
            : project->source_base_dir
                  ? project->source_base_dir
                  : work_dir;
    job->project_dir = job_strdup(project_dir);
    job->work_dir = job_strdup(work_dir);
    if (!job->project_dir || !job->work_dir) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "job request path allocation failed");
    }
    return TP_STATUS_OK;
}

static tp_status job_start_process(void *context, tp_error *err) {
    tp_live_job *job = context;
    const tp_session_job_descriptor *descriptor =
        &job->owner.observation_descriptor;
    const tp_job_worker_proto_request request = {
        .kind = job->kind,
        .session_instance_generation =
            descriptor->session_instance_generation,
        .request_id = descriptor->request_id,
        /* The worker names its private request directory after THIS pid: the
         * directory outlives the worker (the host reads the artifact in chunks
         * after the child exits), so the cross-run reaper must ask whether the
         * host is still alive, not the worker. */
        .host_pid = job_host_pid(),
        .project_json = (const uint8_t *)job->project_json,
        .project_json_len = job->project_json_len,
        .atlas_id = job->atlas_id,
        .project_dir = job->project_dir,
        .work_dir = job->work_dir,
        .preview_exporter_id = job->preview_exporter_id,
        .input_token = job->input_token_at_start,
    };
    const tp_status status = tp_job_worker_process_start(
        &request, &job->process, err);
    if (status == TP_STATUS_OK) {
        job->started_ms = job_now_ms();
    }
    return status;
}

static tp_status job_start(tp_session *session, tp_live_job *job,
                           tp_error *err) {
    job->owner.pump = job_pump_owned;
    const tp_status status = tp_session_job_start_internal(
        session, &job->owner, job_start_process, job, err);
    if (status != TP_STATUS_OK) {
        tp_session_job_release_internal(&job->owner);
    }
    return status;
}

tp_status tp_session_pack_job_start(tp_session *session,
                                    const tp_pack_job_request *request,
                                    tp_error *err) {
    if (!session || !request || tp_id128_is_nil(request->atlas_id) ||
        !request->work_dir) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Pack job requires session, atlas id, and work dir");
    }
    if (request->preview_exporter_id) {
        tp_status status =
            tp_exporter_id_validate(request->preview_exporter_id, err);
        if (status != TP_STATUS_OK) {
            return status;
        }
        if (!tp_exporter_find(request->preview_exporter_id)) {
            return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                "unknown preview exporter '%s'",
                                request->preview_exporter_id);
        }
    }

    tp_session_snapshot *snapshot = NULL;
    tp_status status =
        tp_session_snapshot_create(session, &snapshot, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (!tp_session_snapshot_atlas_by_id(snapshot,
                                         request->atlas_id)) {
        tp_session_snapshot_destroy(snapshot);
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "Pack job atlas was not found");
    }

    tp_live_job *job = job_create(TP_SESSION_JOB_PACK);
    if (!job) {
        tp_session_snapshot_destroy(snapshot);
        return tp_error_set(err, TP_STATUS_OOM,
                            "Pack job allocation failed");
    }
    job->atlas_id = request->atlas_id;
    job->input_token_at_start =
        tp_session_snapshot_input_token(snapshot);
    job->preview_exporter_id = job_strdup(
        request->preview_exporter_id
            ? request->preview_exporter_id
            : "");
    status = job_prepare_snapshot_request(
        snapshot, request->work_dir, job, err);
    tp_session_snapshot_destroy(snapshot);
    if (status != TP_STATUS_OK || !job->preview_exporter_id) {
        if (status == TP_STATUS_OK) {
            status = tp_error_set(
                err, TP_STATUS_OOM,
                "Pack preview exporter allocation failed");
        }
        tp_session_job_release_internal(&job->owner);
        return status;
    }

    job->pack_observation_target =
        (tp_session_job_target){
            .kind = TP_SESSION_JOB_TARGET_ATLAS,
            .atlas_id = request->atlas_id,
        };
    const tp_session_job_descriptor descriptor = {
        .session_instance_generation =
            request->session_instance_generation,
        .request_id = request->request_id,
        .kind = TP_SESSION_JOB_PACK,
        .base_input_token = job->input_token_at_start,
        .targets = &job->pack_observation_target,
        .target_count = 1U,
    };
    tp_session_owned_job_configure_observation(
        &job->owner, &descriptor, job_observe);
    atomic_store_explicit(&job->total, 1, memory_order_relaxed);
    return job_start(session, job, err);
}

tp_status tp_session_export_start(tp_session *session,
                                  const tp_export_command_request *request,
                                  tp_error *err) {
    if (!session || !request || !request->work_dir) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export job requires session and work dir");
    }
    tp_session_snapshot *snapshot = NULL;
    tp_status status =
        tp_session_snapshot_create(session, &snapshot, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_live_job *job = job_create(TP_SESSION_JOB_EXPORT);
    if (!job) {
        tp_session_snapshot_destroy(snapshot);
        return tp_error_set(err, TP_STATUS_OOM,
                            "Export job allocation failed");
    }
    job->atlas_id = request->atlas_id;
    job->input_token_at_start =
        tp_session_snapshot_input_token(snapshot);
    job->preview_exporter_id = job_strdup("");

    const int atlas_count =
        tp_session_snapshot_atlas_count(snapshot);
    size_t target_count = 0U;
    int eligible_atlases = 0;
    for (int i = 0; i < atlas_count; ++i) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, i);
        if (!atlas ||
            (!tp_id128_is_nil(request->atlas_id) &&
             !tp_id128_eq(request->atlas_id, atlas->id))) {
            continue;
        }
        bool eligible = false;
        for (int t = 0; t < atlas->target_count; ++t) {
            const tp_snapshot_target *target =
                tp_session_snapshot_target_at(
                    snapshot, atlas->id, t);
            if (target && target->enabled) {
                target_count++;
                eligible = true;
            }
        }
        eligible_atlases += eligible ? 1 : 0;
    }
    if (target_count > 0U) {
        job->observation_targets =
            job_observation_target_calloc(
                target_count, sizeof *job->observation_targets);
        if (!job->observation_targets) {
            tp_session_snapshot_destroy(snapshot);
            tp_session_job_release_internal(&job->owner);
            return tp_error_set(
                err, TP_STATUS_OOM,
                "Export job target identity allocation failed");
        }
    }
    for (int i = 0; i < atlas_count; ++i) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, i);
        if (!atlas ||
            (!tp_id128_is_nil(request->atlas_id) &&
             !tp_id128_eq(request->atlas_id, atlas->id))) {
            continue;
        }
        for (int t = 0; t < atlas->target_count; ++t) {
            const tp_snapshot_target *target =
                tp_session_snapshot_target_at(
                    snapshot, atlas->id, t);
            if (!target || !target->enabled) {
                continue;
            }
            job->observation_targets[
                job->observation_target_count++] =
                (tp_session_job_target){
                    .kind =
                        TP_SESSION_JOB_TARGET_EXPORT_TARGET,
                    .atlas_id = atlas->id,
                    .id = target->id,
                };
        }
    }
    if (eligible_atlases == 0) {
        tp_session_snapshot_destroy(snapshot);
        tp_session_job_release_internal(&job->owner);
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "nothing to export");
    }
    status = job_prepare_snapshot_request(
        snapshot, request->work_dir, job, err);
    tp_session_snapshot_destroy(snapshot);
    if (status != TP_STATUS_OK || !job->preview_exporter_id) {
        if (status == TP_STATUS_OK) {
            status = tp_error_set(
                err, TP_STATUS_OOM,
                "Export request allocation failed");
        }
        tp_session_job_release_internal(&job->owner);
        return status;
    }

    const tp_session_job_descriptor descriptor = {
        .session_instance_generation =
            request->session_instance_generation,
        .request_id = request->request_id,
        .kind = TP_SESSION_JOB_EXPORT,
        .base_input_token = job->input_token_at_start,
        .targets = job->observation_targets,
        .target_count = job->observation_target_count,
    };
    tp_session_owned_job_configure_observation(
        &job->owner, &descriptor, job_observe);
    atomic_store_explicit(&job->total, eligible_atlases,
                          memory_order_relaxed);
    return job_start(session, job, err);
}

bool tp_session_job_active(const tp_session *session) {
    tp_session_owned_job *job =
        tp_session_job_acquire_internal(session);
    const bool active = job != NULL;
    tp_session_job_release_internal(job);
    return active;
}

tp_status tp_session_job_cancel(tp_session *session, tp_error *err) {
    tp_live_job *job =
        (tp_live_job *)tp_session_job_acquire_internal(session);
    if (!job) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "session has no active job");
    }
    const bool accepted = job_request_cancel(job);
    tp_session_job_release_internal(&job->owner);
    return accepted
               ? TP_STATUS_OK
               : tp_error_set(
                     err, TP_STATUS_INVALID_ARGUMENT,
                     "job cancellation was already requested or the job is terminal");
}

void tp_session_job_result_destroy(tp_session_job_result *result) {
    if (!result) {
        return;
    }
    if (result->_owner) {
        tp_session_job_release_internal(
            (tp_session_owned_job *)result->_owner);
    }
    memset(result, 0, sizeof *result);
}

void tp_session_job_result_compact(tp_session_job_result *result) {
    tp_live_job *job =
        result ? (tp_live_job *)result->_owner : NULL;
    if (!job) {
        return;
    }
    /* An artifact the worker published but nothing adopted is named only by
     * the wire path inside the process's response frame; it must be taken
     * BEFORE the process -- and that frame with it -- goes away. Both discards
     * need job->work_dir/artifact_path still intact, so they run first. */
    job_discard_unadopted_artifact(
        job, tp_job_worker_process_response(job->process));
    job_discard_artifact(job);
    tp_job_worker_process_destroy(job->process);
    job->process = NULL;
    free(job->project_json);
    job->project_json = NULL;
    job->project_json_len = 0U;
    free(job->project_dir);
    job->project_dir = NULL;
    free(job->work_dir);
    job->work_dir = NULL;
    free(job->preview_exporter_id);
    job->preview_exporter_id = NULL;
    /* The arena, terminal_result, and the observation descriptor (which the
     * observed job state still borrows) stay untouched: job_destroy_owned
     * remains the exactly-once release for them, and every pointer freed above
     * is NULL so that destroy stays a no-op for it. */
}

tp_pack_freshness tp_session_pack_result_freshness(
    const tp_session_pack_job_result *result,
    tp_session_input_token live_token,
    tp_pack_freshness_reason *out_reason) {
    tp_pack_freshness_reason reason =
        TP_PACK_FRESHNESS_RESULT_HASH_NIL;
    tp_pack_freshness freshness = TP_PACK_FRESHNESS_STALE;
    if (result && !tp_id128_is_nil(result->pack_input_hash)) {
        if (!tp_session_input_token_equal(result->freshness_token,
                                          live_token)) {
            reason = TP_PACK_FRESHNESS_TOKEN_CHANGED;
        } else {
            reason = result->freshness_reason;
            freshness = result->freshness;
        }
    }
    if (out_reason) {
        *out_reason = reason;
    }
    return freshness;
}

static tp_status pack_input_hash_snapshot(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *preview_exporter_id,
    tp_pack_image_hash_cache *cache, tp_id128 *out_hash,
    tp_error *err) {
    tp_pack_input input = {0};
    tp_pack_settings settings = {0};
    tp_status status = tp_pack_input_build_snapshot(
        snapshot, atlas_id, &input, err);
    if (status == TP_STATUS_OK) {
        status = tp_pack_settings_build_snapshot(
            snapshot, atlas_id, &settings, err);
    }
    if (status == TP_STATUS_OK) {
        settings.sprites = input.descs;
        settings.sprite_count = input.count;
        status = tp_pack_input_hash_compute(
            &settings,
            preview_exporter_id && preview_exporter_id[0]
                ? preview_exporter_id
                : NULL,
            cache, out_hash, err);
    }
    tp_pack_input_free(&input);
    return status;
}

tp_status tp_session_pack_input_hash(
    tp_session *session, tp_id128 atlas_id,
    struct tp_pack_image_hash_cache *cache, tp_id128 *out_hash,
    tp_error *err) {
    if (!session || !out_hash || tp_id128_is_nil(atlas_id)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "pack input hash requires session, atlas id, and output");
    }
    *out_hash = tp_id128_nil();
    tp_session_snapshot *snapshot = NULL;
    tp_status status =
        tp_session_snapshot_create(session, &snapshot, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = pack_input_hash_snapshot(
        snapshot, atlas_id, NULL, cache, out_hash, err);
    tp_session_snapshot_destroy(snapshot);
    return status;
}
