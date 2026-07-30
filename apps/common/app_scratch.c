#include "app_scratch.h"

/* libc first (before the tp_core headers) -- matches gui_paths.c; on macOS clang
 * a header pulling <stdio.h> first otherwise leaves snprintf undeclared here
 * (implicit-decl = hard error). */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>

#include "nt_utf8_argv.h"
#define app_scratch_getpid _getpid
#else
#include <unistd.h>
#define app_scratch_getpid getpid
#endif

#include "nt_utf8_fs.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_scan.h" /* tp_mkdirs / tp_scan_is_dir / tp_scan_visit_dir */

/* One pass of app_scratch_request_dir_release() collects this many bytes of
 * NUL-separated file names before it starts deleting. Names are collected and
 * deleted in separate passes because a directory stream that is mutated while
 * it is being read may skip entries; the loop repeats until a pass finds
 * nothing left. */
#define APP_SCRATCH_RELEASE_BATCH 4096U

/* Fits any request directory this module hands out (bounded by the caller's
 * buffer, at most TP_IDENTITY_PATH_MAX-1) plus one filename component. */
#define APP_SCRATCH_CHILD_MAX (TP_IDENTITY_PATH_MAX + 260)

static void clear_output(char *out, size_t out_cap) {
    if (out && out_cap > 0U) {
        out[0] = '\0';
    }
}

#if !defined(_WIN32)
static tp_status copy_dir(const char *value, char *out, size_t out_cap,
                          tp_error *err) {
    const int copied = snprintf(out, out_cap, "%s", value);
    if (copied <= 0 || (size_t)copied >= out_cap) {
        clear_output(out, out_cap);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "the application cache-directory path is too long");
    }
    return TP_STATUS_OK;
}
#endif

tp_status app_scratch_cache_dir(char *out, size_t out_cap, tp_error *err) {
    if (!out || out_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid application cache-directory output");
    }
    clear_output(out, out_cap);
#if defined(_WIN32)
    char platform_error[160] = {0};
    bool found = false;
    if (!nt_win_environment_utf8(L"LOCALAPPDATA", out, out_cap, &found,
                                 platform_error, sizeof platform_error)) {
        clear_output(out, out_cap);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "%s",
                            platform_error);
    }
    if (!found || out[0] == '\0') {
        clear_output(out, out_cap);
        return tp_error_set(
            err, TP_STATUS_PATH_RESOLVE_FAILED,
            "LOCALAPPDATA is not set, so the application cache directory cannot be resolved");
    }
    return TP_STATUS_OK;
#else
    /* CACHE, not state: XDG_CACHE_HOME is the reclaimable tier. The recovery
     * journals under gui_paths_app_data_root() deliberately use XDG_STATE_HOME
     * instead -- they must survive a cache sweep, these artifacts must not. */
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] != '\0') {
        return copy_dir(xdg, out, out_cap, err);
    }
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        const int joined = snprintf(out, out_cap, "%s/.cache", home);
        if (joined <= 0 || (size_t)joined >= out_cap) {
            clear_output(out, out_cap);
            return tp_error_set(
                err, TP_STATUS_OUT_OF_BOUNDS,
                "the application cache-directory path is too long");
        }
        return TP_STATUS_OK;
    }
    return tp_error_set(
        err, TP_STATUS_PATH_RESOLVE_FAILED,
        "neither XDG_CACHE_HOME nor HOME is set, so the application cache directory cannot be resolved");
#endif
}

tp_status app_scratch_root_in(const char *cache_dir, char *out, size_t out_cap,
                              tp_error *err) {
    if (!cache_dir || cache_dir[0] == '\0' || !out || out_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid application scratch-root arguments");
    }
    const size_t length = strlen(cache_dir);
    const bool separated = cache_dir[length - 1U] == '/' ||
                           cache_dir[length - 1U] == '\\';
    const int joined = snprintf(
        out, out_cap, separated ? "%sntpacker/work" : "%s/ntpacker/work",
        cache_dir);
    if (joined <= 0 || (size_t)joined >= out_cap) {
        clear_output(out, out_cap);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "the application scratch-root path is too long");
    }
    tp_mkdirs(out); /* first run creates <cache>/ntpacker and <...>/work */
    if (tp_scan_is_dir(out)) {
        return TP_STATUS_OK;
    }
    const tp_status status = tp_error_set(
        err, TP_STATUS_PATH_RESOLVE_FAILED,
        "could not create the application scratch directory '%s'", out);
    clear_output(out, out_cap);
    return status;
}

tp_status app_scratch_root(char *out, size_t out_cap, tp_error *err) {
    char cache_dir[TP_IDENTITY_PATH_MAX];
    const tp_status status =
        app_scratch_cache_dir(cache_dir, sizeof cache_dir, err);
    if (status != TP_STATUS_OK) {
        clear_output(out, out_cap);
        return status;
    }
    return app_scratch_root_in(cache_dir, out, out_cap, err);
}

tp_status app_scratch_request_dir(const char *root, uint64_t request_id,
                                  char *out, size_t out_cap, tp_error *err) {
    if (!root || root[0] == '\0' || !out || out_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid scratch request-directory arguments");
    }
    const unsigned long pid = (unsigned long)app_scratch_getpid() & 0xffffffffUL;
    const int joined = snprintf(out, out_cap, "%s/req-%08lx-%016llx", root, pid,
                                (unsigned long long)request_id);
    if (joined <= 0 || (size_t)joined >= out_cap) {
        clear_output(out, out_cap);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "the scratch request-directory path is too long");
    }
    if (tp_scan_exists(out)) {
        /* Our own pid AND our own request id: a leftover of this very request
         * (a recycled pid whose owner is long gone), never a live peer's
         * directory. Start from an empty one so a stale `<atlas>.ntpack` can
         * never be mistaken for this run's output. */
        app_scratch_request_dir_release(out);
    }
    tp_mkdirs(out);
    if (tp_scan_is_dir(out)) {
        return TP_STATUS_OK;
    }
    const tp_status status = tp_error_set(
        err, TP_STATUS_FILE_IO_FAILED,
        "could not create the private scratch directory '%s'", out);
    clear_output(out, out_cap);
    return status;
}

typedef struct release_batch {
    char names[APP_SCRATCH_RELEASE_BATCH];
    size_t used;
    bool full;
} release_batch;

static bool collect_name(void *ctx, const char *name, uint64_t size) {
    (void)size;
    release_batch *batch = (release_batch *)ctx;
    const size_t length = strlen(name) + 1U;
    if (length > sizeof batch->names - batch->used) {
        batch->full = true;
        return false; /* stop this pass; the next one takes the rest */
    }
    memcpy(batch->names + batch->used, name, length);
    batch->used += length;
    return true;
}

/* True when the last component of `path` is a `req-` request-directory name. */
static bool is_request_dir_path(const char *path) {
    const char *base = path;
    for (const char *cursor = path; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    return strncmp(base, "req-", 4U) == 0;
}

void app_scratch_request_dir_release(const char *request_dir) {
    if (!request_dir || request_dir[0] == '\0' ||
        !is_request_dir_path(request_dir)) {
        return;
    }
    release_batch batch;
    char child[APP_SCRATCH_CHILD_MAX];
    /* Bounded: each pass that fills the batch removes at least one entry, and a
     * request directory holds one `<atlas>.ntpack` per atlas. */
    for (unsigned pass = 0U; pass < 64U; ++pass) {
        batch.used = 0U;
        batch.full = false;
        (void)tp_scan_visit_dir(request_dir, "", collect_name, &batch);
        if (batch.used == 0U) {
            break;
        }
        for (size_t offset = 0U; offset < batch.used;) {
            const char *name = batch.names + offset;
            const int joined =
                snprintf(child, sizeof child, "%s/%s", request_dir, name);
            if (joined > 0 && (size_t)joined < sizeof child) {
                (void)nt_utf8_remove(child);
            }
            offset += strlen(name) + 1U;
        }
        if (!batch.full) {
            break;
        }
    }
    (void)nt_utf8_rmdir(request_dir);
}
