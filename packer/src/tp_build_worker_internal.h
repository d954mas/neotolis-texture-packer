#ifndef TP_BUILD_WORKER_INTERNAL_H
#define TP_BUILD_WORKER_INTERNAL_H

/* Parent side of the private build worker (decision 0018, ROADMAP H0.3-b/H0.4).
 * This is the production Pack cutover: tp_pack hands validated settings + decoded
 * pixels here, and the builder runs in a re-exec'd child process so an
 * NT_BUILD_ASSERT, allocation, codec, or write failure in nt_builder can never
 * terminate the host. The in-process driver (tp_build_driver.c) stays as the body
 * the worker child runs and the byte-identical oracle -- it is NOT a fallback:
 * every destination (including non-ASCII / long paths) now packs through the
 * worker via an ASCII staging dir + UTF-8 publication (H0.4). */

#include "tp_core/tp_error.h"
#include "tp_core/tp_image.h"
#include "tp_core/tp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional knobs for a worker run. All-zero = production defaults (own module
 * path, 5-minute safety timeout, full 64 KiB reply cap, no cancellation).
 * `worker_exe`, `timeout_ms` and `reply_cap` are test seams;
 * `cancel_poll`/`cancel_ctx` carry cooperative cancellation from
 * tp_pack_cancellable. */
typedef struct tp_build_worker_opts {
    const char *worker_exe;           /* NULL = this process's own module path */
    int timeout_ms;                   /* <= 0 = default 5-min safety timeout */
    size_t reply_cap;                 /* 0 = default 64 KiB; a lowered cap (< pipe buffer)
                                       * lets a test drive the over-cap fail-closed branch */
    tp_pack_cancel_poll cancel_poll;  /* NULL = no cancellation */
    void *cancel_ctx;
    bool *out_cancelled;              /* set true iff cancellation was observed; may be NULL */
} tp_build_worker_opts;

/* Runs one atlas through the private build worker. Mirrors tp_build_driver_run and
 * CONSUMES `loaded_images` (frees it on every path). The child is spawned with its
 * CWD set to a private ASCII staging dir on the destination's volume; it writes a
 * bare relative "out.ntpack" there; the parent atomically publishes that artifact
 * to the real UTF-8 `out_path` and removes the staging dir on EVERY exit path.
 * Outcome mapping:
 *   worker exit 0 + valid OK reply + published artifact  -> TP_STATUS_OK
 *   valid reply carrying a builder/sink failure          -> TP_STATUS_BUILDER_FAILED (carried msg)
 *   crash / signal / abnormal / non-zero / timeout       -> TP_STATUS_BUILDER_CRASHED
 *   OK reply but no readable artifact, or a valid artifact the host could not
 *     publish (dest locked / parent dir absent / cross-device)  -> TP_STATUS_BUILDER_CRASHED
 *   malformed / truncated / oversized reply, clean exit  -> TP_STATUS_BUILDER_FAILED (fail closed)
 *   cancellation observed                                -> TP_STATUS_OK, *opts->out_cancelled = true, nothing published */
tp_status tp_build_worker_run(const tp_pack_settings *settings,
                              tp_image_rgba8 *loaded_images,
                              const char *out_path, tp_error *err);

/* Test seam: run against an explicit worker executable (`worker_exe` NULL = own
 * module path). Lets the fault-injecting worker binaries exercise the crash /
 * malformed-reply / non-zero-exit outcome mapping. */
tp_status tp_build_worker_run_exe(const tp_pack_settings *settings,
                                  tp_image_rgba8 *loaded_images,
                                  const char *out_path, const char *worker_exe,
                                  tp_error *err);

/* Full entry: cancellation, an overridable timeout, and an explicit worker exe.
 * `opts` may be NULL (production defaults). tp_pack routes here with a cancel
 * poll; the cancel/timeout tests route here with a hang worker + tiny timeout. */
tp_status tp_build_worker_run_opts(const tp_pack_settings *settings,
                                   tp_image_rgba8 *loaded_images,
                                   const char *out_path,
                                   const tp_build_worker_opts *opts, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_BUILD_WORKER_INTERNAL_H */
