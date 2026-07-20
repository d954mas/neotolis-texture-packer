#include "tp_build_worker_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define tp_getpid _getpid
#else
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#define tp_getpid getpid
#endif

#include "tp_core/tp_build_worker.h" /* TP_BUILD_WORKER_ARGV1 */
#include "tp_build_proto_internal.h"
#include "tp_fs_internal.h"
#include "tp_proc_internal.h"

/* A valid reply is header(12) + response_head(16) + artifact(<=4096) +
 * message(<=4096) = ~8.2 KiB; the encoder structurally bounds EVERY reply to that,
 * so a real reply always fits the 64 KiB cap AND the 64 KiB pipe buffer: the child
 * writes its whole reply and exits without the parent reading concurrently -- the
 * parent waits (with cancel/timeout) THEN reads. The over-cap branch (read fills
 * the cap without EOF) is a fail-closed defense for a hypothetical worker that
 * overshoots; it is exercised via the reply_cap test seam (a lowered cap the worker
 * overshoots while staying under the pipe buffer). A reply larger than the actual
 * pipe buffer would instead block the child on a full pipe and surface via the
 * safety timeout as builder_crashed. */
#define TP_BUILD_WORKER_REPLY_CAP ((size_t)1u << 16)

/* The builder opens a narrow char path, so the child always writes this bare
 * relative ASCII name inside its ASCII staging CWD; the parent owns the real
 * UTF-8 destination and publishes to it. */
#define TP_BUILD_WORKER_OUT_NAME "out.ntpack"

/* Internal safety timeout (decision 0018 kickoff): cancellation is the primary
 * control; this only bounds a wedged builder. Overridable by the cancel/timeout
 * test via tp_build_worker_opts.timeout_ms; never user-facing. */
#define TP_BUILD_WORKER_TIMEOUT_MS (5 * 60 * 1000)

/* Cancel/timeout poll cadence while the child runs. */
#define TP_BUILD_WORKER_POLL_MS 20

#define TP_STAGING_PATH_MAX 4096

/* Windows caps a process current directory at MAX_PATH regardless of long-path
 * awareness, so the staging dir (and dir + "/out.ntpack") must stay well under
 * it; when work_dir is longer the staging dir is relocated up its same-volume
 * ancestor chain. POSIX has no such cap, so work_dir is always used directly. */
#if defined(_WIN32)
#define TP_STAGING_PARENT_BUDGET ((size_t)200)
#else
#define TP_STAGING_PARENT_BUDGET ((size_t)-1)
#endif

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
#endif
}

static void free_loaded_images(tp_image_rgba8 *images, int count) {
    if (!images) {
        return;
    }
    for (int i = 0; i < count; i++) {
        tp_image_free(&images[i]);
    }
    free(images);
}

/* The artifact the worker wrote into the staging dir must be a readable, non-empty
 * regular file before we accept and publish the pack. Reject a reparse point: on
 * Windows a *file* symlink/reparse classifies as REGULAR (POSIX lstat already
 * rejects it as OTHER), and publishing it would republish an attacker-chosen link
 * target. Matches the tp_export_defold "real regular file" probe. */
static bool staged_artifact_ok(const char *staged_path) {
    tp_fs_info info;
    return tp_fs_stat(staged_path, &info) && info.kind == TP_FS_KIND_REGULAR &&
           !info.reparse && info.size > 0U;
}

/* Strip the last path component (drops trailing separators, the component, then
 * its separator). Returns false at a root / single-component path (no progress). */
static bool strip_last_component(char *path) {
    size_t n = strlen(path);
    const size_t orig = n;
    while (n > 0U && (path[n - 1U] == '/' || path[n - 1U] == '\\')) {
        n--;
    }
    while (n > 0U && path[n - 1U] != '/' && path[n - 1U] != '\\') {
        n--;
    }
    while (n > 0U && (path[n - 1U] == '/' || path[n - 1U] == '\\')) {
        n--;
    }
    if (n == 0U || n == orig) {
        return false;
    }
    path[n] = '\0';
    return true;
}

static void remove_staging_dir(const char *staging);

/* True iff the process that owned a staging dir is gone, so the dir is a safe
 * cross-run leftover to reap. An access-denied / still-running process is treated
 * as ALIVE (kept) -- only a definitively absent pid is reaped. */
static bool staging_owner_is_dead(unsigned long pid) {
#if defined(_WIN32)
    if (pid == 0UL || pid > 0xFFFFFFFFUL) {
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        return GetLastError() == ERROR_INVALID_PARAMETER; /* no such pid; access-denied => exists */
    }
    DWORD code = 0;
    bool dead = GetExitCodeProcess(h, &code) && code != STILL_ACTIVE;
    (void)CloseHandle(h);
    return dead;
#else
    if (pid == 0UL || pid > (unsigned long)INT_MAX) {
        return false; /* 0/out-of-range have special or unsafe kill() semantics */
    }
    return kill((pid_t)pid, 0) == -1 && errno == ESRCH;
#endif
}

/* Best-effort cross-run cleanup: a host killed between make_staging_dir and
 * remove_staging_dir leaves a pkw-<pid>-<serial> dir behind forever. Before each
 * pack, sweep sibling pkw-* dirs whose owning pid (the leading hex field) is no
 * longer live and remove them. Silent and never fails the pack; our own dirs and
 * those of other live hosts are kept because their pid is still alive. */
static void reap_stale_staging_dirs(const char *parent) {
    tp_fs_dir *dir = tp_fs_dir_open(parent);
    if (!dir) {
        return;
    }
    tp_fs_dir_entry entry;
    for (;;) {
        if (tp_fs_dir_next(dir, &entry) != TP_FS_DIR_ENTRY) {
            break;
        }
        if (entry.info.kind != TP_FS_KIND_DIRECTORY || entry.info.reparse) {
            continue;
        }
        if (strncmp(entry.name, "pkw-", 4) != 0) {
            continue;
        }
        char *end = NULL;
        unsigned long pid = strtoul(entry.name + 4U, &end, 16);
        if (end == entry.name + 4U || *end != '-') {
            continue; /* not our "pkw-<hexpid>-..." shape: leave it alone */
        }
        if (!staging_owner_is_dead(pid)) {
            continue;
        }
        char child[TP_STAGING_PATH_MAX];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", parent, entry.name) >= sizeof child) {
            continue;
        }
        remove_staging_dir(child);
    }
    tp_fs_dir_close(dir);
}

/* Create a private ASCII staging dir on the destination's volume and return its
 * path. The dir is a sibling under work_dir when short enough; on Windows a long
 * work_dir forces relocation up the ancestor chain so the child's current
 * directory stays under MAX_PATH. The ancestor is normally the same volume as the
 * destination; if a junction/mount point splits them, the publish (tp_fs_replace)
 * falls back to a cross-volume copy rather than failing. Fails closed on an
 * unwritable parent. */
static bool make_staging_dir(const char *work_dir, char *out, size_t out_cap) {
    if (!work_dir || work_dir[0] == '\0') {
        return false;
    }
    char parent[TP_STAGING_PATH_MAX];
    if ((size_t)snprintf(parent, sizeof parent, "%s", work_dir) >= sizeof parent) {
        return false;
    }
    while (strlen(parent) > TP_STAGING_PARENT_BUDGET) {
        if (!strip_last_component(parent)) {
            break;
        }
    }
    reap_stale_staging_dirs(parent); /* best-effort sweep of dead-owner leftovers */
    static _Atomic uint64_t counter;
    const unsigned long pid = (unsigned long)tp_getpid();
    for (unsigned int attempt = 0U; attempt < 256U; attempt++) {
        const uint64_t serial =
            atomic_fetch_add_explicit(&counter, UINT64_C(1), memory_order_relaxed) + UINT64_C(1);
        int n = snprintf(out, out_cap, "%s/pkw-%08lx-%08llx", parent,
                         pid & 0xffffffffUL, (unsigned long long)(serial & 0xffffffffULL));
        if (n <= 0 || (size_t)n >= out_cap) {
            return false;
        }
        if (tp_fs_exists(out)) {
            continue; /* skip a leftover/foreign name rather than adopt it */
        }
        errno = 0;
        if (tp_fs_create_dir(out)) {
            return true;
        }
        if (errno == EEXIST) {
            continue; /* lost a create race: try the next serial */
        }
        return false; /* unwritable parent / real error: fail closed */
    }
    return false;
}

/* Remove the staging dir and everything under it, best effort, on EVERY exit
 * path. The builder writes a single flat file, but recurse to stay correct if a
 * future builder ever drops a sidecar. */
static void remove_staging_dir(const char *staging) {
    tp_fs_dir *dir = tp_fs_dir_open(staging);
    if (dir) {
        tp_fs_dir_entry entry;
        for (;;) {
            const tp_fs_dir_result r = tp_fs_dir_next(dir, &entry);
            if (r != TP_FS_DIR_ENTRY) {
                break;
            }
            char child[TP_STAGING_PATH_MAX];
            if ((size_t)snprintf(child, sizeof child, "%s/%s", staging, entry.name) >= sizeof child) {
                continue;
            }
            if (entry.info.kind == TP_FS_KIND_DIRECTORY && !entry.info.reparse) {
                remove_staging_dir(child);
            } else if (entry.info.kind == TP_FS_KIND_DIRECTORY) {
                /* Directory junction / dir-symlink (Windows classifies it as
                 * DIRECTORY): remove the link itself, never descend into and delete
                 * its target's contents. POSIX lstat already yields OTHER here. */
                (void)tp_fs_remove_dir(child);
            } else {
                (void)tp_fs_remove_file(child);
            }
        }
        tp_fs_dir_close(dir);
    }
    (void)tp_fs_remove_dir(staging);
}

/* Serialize settings + decoded pixels into a request frame. Path sprites take
 * their pixels from `loaded_images` (index-aligned); raw sprites from the desc.
 * The encoder copies every pixel block, so the caller may free loaded_images
 * immediately afterward. */
static tp_status encode_request(const tp_pack_settings *s,
                                const tp_image_rgba8 *loaded_images,
                                const char *out_name, uint8_t **out_bytes,
                                size_t *out_len, tp_error *err) {
    tp_build_proto_sprite *sprites =
        (tp_build_proto_sprite *)calloc((size_t)s->sprite_count, sizeof *sprites);
    if (!sprites) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_pack: worker request sprite table alloc failed");
    }
    for (int i = 0; i < s->sprite_count; i++) {
        const tp_pack_sprite_desc *sp = &s->sprites[i];
        tp_build_proto_sprite *ps = &sprites[i];
        ps->name = sp->name;
        ps->origin_x = sp->origin_x;
        ps->origin_y = sp->origin_y;
        for (int k = 0; k < 4; k++) {
            ps->slice9_lrtb[k] = sp->slice9_lrtb[k];
        }
        ps->ov_mask = sp->ov_mask;
        ps->ov_shape = sp->ov_shape;
        ps->ov_allow_rotate = sp->ov_allow_rotate;
        ps->ov_max_vertices = sp->ov_max_vertices;
        ps->ov_margin = sp->ov_margin;
        ps->ov_extrude = sp->ov_extrude;
        if (sp->path) {
            ps->width = (uint32_t)loaded_images[i].width;
            ps->height = (uint32_t)loaded_images[i].height;
            ps->rgba = loaded_images[i].pixels;
        } else {
            ps->width = (uint32_t)sp->w;
            ps->height = (uint32_t)sp->h;
            ps->rgba = sp->rgba;
        }
    }

    tp_build_proto_request req;
    memset(&req, 0, sizeof req);
    req.max_size = s->max_size;
    req.padding = s->padding;
    req.margin = s->margin;
    req.extrude = s->extrude;
    req.alpha_threshold = s->alpha_threshold;
    req.max_vertices = s->max_vertices;
    req.shape = s->shape;
    req.allow_transform = s->allow_transform ? 1u : 0u;
    req.power_of_two = s->power_of_two ? 1u : 0u;
    req.pixels_per_unit = s->pixels_per_unit;
    req.atlas_name = s->atlas_name;
    req.out_name = out_name;
    req.sprites = sprites;
    req.sprite_count = (uint32_t)s->sprite_count;

    tp_status st = tp_build_proto_encode_request(&req, out_bytes, out_len, err);
    free(sprites); /* sprite table is borrowed metadata; strings/pixels not owned here */
    return st;
}

/* Map the child's reply + how it terminated onto a tp_status (see the header for
 * the full table). `staged_path` is the child's staging-relative artifact the
 * parent verifies before publishing. Fills `err` on every non-OK outcome. */
static tp_status map_outcome(const uint8_t *reply, size_t reply_len, bool read_ok,
                             bool reply_eof, bool waited, tp_proc_result w,
                             const char *staged_path, tp_error *err) {
    tp_build_proto_response resp;
    bool reply_valid = read_ok && reply_eof &&
                       tp_build_proto_decode_response(reply, reply_len, &resp, NULL) == TP_STATUS_OK;

    const bool exited_zero = waited && w.how == TP_PROC_END_EXITED && w.code == 0;
    const bool exited_nonzero = waited && w.how == TP_PROC_END_EXITED && w.code != 0;
    const bool abnormal = !waited || w.how == TP_PROC_END_ABNORMAL;

    if (reply_valid) {
        tp_status st;
        if (resp.status == TP_STATUS_OK) {
            if (exited_zero && staged_artifact_ok(staged_path)) {
                st = TP_STATUS_OK;
            } else {
                st = tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                                  "tp_pack: build worker reported success but %s",
                                  exited_zero ? "produced no readable artifact"
                                              : "terminated abnormally");
            }
        } else {
            /* The builder ran and resolved a structured failure -- carry it,
             * regardless of the exit code (a fault worker may pair it with a
             * non-zero exit). */
            st = tp_error_set(err, TP_STATUS_BUILDER_FAILED, "tp_pack: build worker failed: %s",
                              (resp.message && resp.message[0]) ? resp.message : "(no detail)");
        }
        tp_build_proto_response_free(&resp);
        return st;
    }

    if (abnormal) {
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                            "tp_pack: build worker crashed (%s)",
                            waited ? "abnormal termination" : "could not be waited on");
    }
    if (exited_nonzero) {
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                            "tp_pack: build worker exited with code %d and no valid reply", w.code);
    }
    /* Clean exit but no usable reply -> fail closed. */
    return tp_error_set(err, TP_STATUS_BUILDER_FAILED, "tp_pack: build worker returned a %s reply",
                        (read_ok && !reply_eof) ? "oversized" : "malformed/truncated");
}

tp_status tp_build_worker_run(const tp_pack_settings *settings, tp_image_rgba8 *loaded_images,
                              const char *out_path, tp_error *err) {
    return tp_build_worker_run_opts(settings, loaded_images, out_path, NULL, err);
}

tp_status tp_build_worker_run_exe(const tp_pack_settings *settings, tp_image_rgba8 *loaded_images,
                                  const char *out_path, const char *worker_exe, tp_error *err) {
    tp_build_worker_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.worker_exe = worker_exe;
    return tp_build_worker_run_opts(settings, loaded_images, out_path, &opts, err);
}

tp_status tp_build_worker_run_opts(const tp_pack_settings *settings, tp_image_rgba8 *loaded_images,
                                   const char *out_path, const tp_build_worker_opts *opts,
                                   tp_error *err) {
    if (opts && opts->out_cancelled) {
        *opts->out_cancelled = false;
    }

    /* Resolve the worker executable BEFORE consuming loaded_images. A self-path
     * failure is NOT silently downgraded to an in-process build: that would run
     * nt_builder in the host and lose containment, so it fails closed. */
    char self[4096];
    const char *exe = opts ? opts->worker_exe : NULL;
    if (!exe) {
        if (!tp_proc_self_path(self, sizeof self)) {
            free_loaded_images(loaded_images, settings->sprite_count);
            return tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                                "tp_pack: could not locate the build worker executable");
        }
        exe = self;
    }

    /* Private ASCII staging dir on the destination's volume: the child runs with
     * this as its CWD and writes a bare relative ASCII name, so a Unicode / long
     * destination never reaches the builder's narrow output path. */
    char staging[TP_STAGING_PATH_MAX];
    if (!make_staging_dir(settings->work_dir, staging, sizeof staging)) {
        free_loaded_images(loaded_images, settings->sprite_count);
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                            "tp_pack: could not create a build worker staging directory under '%s'",
                            settings->work_dir);
    }
    char staged_path[TP_STAGING_PATH_MAX];
    if ((size_t)snprintf(staged_path, sizeof staged_path, "%s/%s", staging, TP_BUILD_WORKER_OUT_NAME) >=
        sizeof staged_path) {
        remove_staging_dir(staging);
        free_loaded_images(loaded_images, settings->sprite_count);
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED, "tp_pack: build worker staging path too long");
    }

    uint8_t *req_bytes = NULL;
    size_t req_len = 0U;
    tp_status st = encode_request(settings, loaded_images, TP_BUILD_WORKER_OUT_NAME, &req_bytes, &req_len, err);
    /* The request now owns a copy of every pixel block; release the caller's
     * decode buffers (mirrors the driver freeing before encode/assembly). */
    free_loaded_images(loaded_images, settings->sprite_count);
    if (st != TP_STATUS_OK) {
        remove_staging_dir(staging);
        return st;
    }

    tp_proc *proc = tp_proc_spawn(exe, TP_BUILD_WORKER_ARGV1, staging);
    if (!proc) {
        free(req_bytes);
        remove_staging_dir(staging);
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED, "tp_pack: could not spawn build worker '%s'", exe);
    }

    /* The worker reads its whole request to EOF before producing anything, so this
     * blocking write always drains; a failed/short write means the child died and
     * the outcome map covers it. */
    bool wrote = tp_proc_write_stdin(proc, req_bytes, req_len);
    free(req_bytes);
    (void)wrote;

    /* Wait for the child while polling cancellation and the safety timeout. This
     * runs BEFORE reading stdout: a hung child that never closes its stdout can no
     * longer wedge the parent in a blocking read -- cancel/timeout kills it, which
     * breaks the pipes and lets the subsequent read reach EOF at once. */
    const int timeout_ms = (opts && opts->timeout_ms > 0) ? opts->timeout_ms : TP_BUILD_WORKER_TIMEOUT_MS;
    const double start = now_ms();
    tp_proc_result w = {TP_PROC_END_ABNORMAL, -1};
    bool finished = false;
    bool cancelled = false;
    bool timed_out = false;
    for (;;) {
        if (opts && opts->cancel_poll && opts->cancel_poll(opts->cancel_ctx)) {
            tp_proc_kill(proc);
            cancelled = true;
            break;
        }
        bool slice_finished = false;
        if (!tp_proc_wait_slice(proc, TP_BUILD_WORKER_POLL_MS, &w, &slice_finished)) {
            tp_proc_kill(proc); /* hard wait error: treat as an abnormal end */
            break;
        }
        if (slice_finished) {
            finished = true;
            break;
        }
        if (now_ms() - start >= (double)timeout_ms) {
            tp_proc_kill(proc);
            timed_out = true;
            break;
        }
    }

    /* Reply cap is the full 64 KiB in production; a test may lower it (below the
     * pipe buffer) so a worker that overshoots the cap still write-and-exits and the
     * post-wait read observes the over-cap (not-EOF) fail-closed branch. */
    const size_t reply_cap =
        (opts && opts->reply_cap > 0U) ? opts->reply_cap : TP_BUILD_WORKER_REPLY_CAP;
    uint8_t *reply = NULL;
    size_t reply_len = 0U;
    bool reply_eof = false;
    bool read_ok = false;
    if (finished) {
        reply = (uint8_t *)malloc(reply_cap);
        read_ok = reply && tp_proc_read_stdout(proc, reply, reply_cap, &reply_len, &reply_eof);
    }
    tp_proc_destroy(proc); /* reaps a killed child; a finished child is already reaped */

    if (cancelled) {
        remove_staging_dir(staging);
        if (opts && opts->out_cancelled) {
            *opts->out_cancelled = true;
        }
        free(reply);
        return TP_STATUS_OK; /* nothing published; caller maps this to a cancelled job */
    }
    if (timed_out) {
        remove_staging_dir(staging);
        free(reply);
        return tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                            "tp_pack: build worker timed out after %d ms (builder timeout)", timeout_ms);
    }

    st = map_outcome(reply, reply_len, read_ok, reply_eof, finished, w, staged_path, err);
    /* Publication failure of an otherwise-valid artifact maps to BUILDER_CRASHED:
     * under the worker's {OK, BUILDER_FAILED, BUILDER_CRASHED} return contract there
     * is no dedicated host-side-IO status, and the taxonomy invariant that matters
     * to callers holds -- no trustworthy PUBLISHED artifact exists, the host
     * survives, and nothing was written over the destination. */
    if (st == TP_STATUS_OK && !tp_fs_replace(staged_path, out_path)) {
        st = tp_error_set(err, TP_STATUS_BUILDER_CRASHED,
                          "tp_pack: build worker artifact could not be published to '%s'", out_path);
    }
    remove_staging_dir(staging);
    free(reply);
    return st;
}
