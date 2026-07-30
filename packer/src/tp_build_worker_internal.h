#ifndef TP_BUILD_WORKER_INTERNAL_H
#define TP_BUILD_WORKER_INTERNAL_H

/* Parent side of the private build worker
 * (docs/architecture/jobs-pack-and-cache.md).
 * This is the production Pack cutover: tp_pack hands validated settings + decoded
 * pixels here, and the builder runs in a re-exec'd child process so an
 * NT_BUILD_ASSERT, allocation, codec, or write failure in nt_builder can never
 * terminate the host. The in-process driver (tp_build_driver.c) stays as the body
 * the worker child runs and the byte-identical oracle -- it is NOT a fallback:
 * every destination (including non-ASCII / long paths) now packs through the
 * worker via an ASCII staging dir + UTF-8 publication (H0.4). */

#include "tp_core/tp_cancel.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_image.h"
#include "tp_core/tp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 *   cancellation observed                                -> TP_STATUS_CANCELLED, nothing published */
tp_status tp_build_worker_run(const tp_pack_settings *settings,
                              tp_image_rgba8 *loaded_images,
                              const char *out_path, tp_error *err);

/* Same run, carrying cooperative cancellation from tp_pack_cancellable. `cancel`
 * may be NULL. This is the whole production configuration surface of the worker:
 * the executable, the safety timeout, and the reply cap are not caller-settable
 * in a shipping build. */
tp_status tp_build_worker_run_cancellable(const tp_pack_settings *settings,
                                          tp_image_rgba8 *loaded_images,
                                          const char *out_path,
                                          const tp_cancel_token *cancel,
                                          tp_error *err);

#ifdef TP_ENABLE_TEST_SEAMS
/* Fault-injection controls, held apart from the production configuration above so
 * a shipping consumer cannot see -- let alone set -- a knob whose only purpose is
 * to break the run. All-zero means production defaults. Compiled out of every
 * build that does not define TP_ENABLE_TEST_SEAMS, so a consumer must recompile
 * tp_build_worker.c with the define (see the tp_test_build_worker target). */
typedef struct tp_build_worker_test_controls {
    const char *worker_exe;  /* NULL = this process's own module path */
    int timeout_ms;          /* <= 0 = default 5-min safety timeout */
    size_t reply_cap;        /* 0 = default 64 KiB; a lowered cap (< pipe buffer)
                              * drives the over-cap fail-closed branch */
} tp_build_worker_test_controls;

/* Runs with fault-injection controls (`controls` may be NULL) plus the same
 * cancellation the production entry takes. Lets the fault-injecting worker
 * binaries exercise the crash / malformed-reply / non-zero-exit / hang outcome
 * mapping. */
tp_status tp_build_worker__test_run(
    const tp_pack_settings *settings, tp_image_rgba8 *loaded_images,
    const char *out_path, const tp_build_worker_test_controls *controls,
    const tp_cancel_token *cancel, tp_error *err);

/* Convenience over tp_build_worker__test_run for the common case: an explicit
 * worker executable and nothing else. */
tp_status tp_build_worker__test_run_exe(const tp_pack_settings *settings,
                                        tp_image_rgba8 *loaded_images,
                                        const char *out_path,
                                        const char *worker_exe, tp_error *err);
#endif

/* Private-directory hygiene, shared with the outer job worker (which stages one
 * `.ntpack` per Pack request in its own `req-<hexpid>-<id>` directory under
 * work_dir). Both owners follow one naming contract so one reaper heals both.
 * The pid in the name is always the pid whose death makes the directory garbage:
 * the worker's own for `pkw-` staging, but the HOST's for `req-`, because a
 * request directory outlives its worker until the host has read the artifact.
 *
 * tp_worker_remove_dir_tree: best-effort recursive removal that never descends
 * into a junction / directory symlink (it unlinks the link itself).
 * tp_worker_reap_stale_dirs: best-effort sweep of `<parent>/<prefix><hexpid>-*`
 * directories whose owning pid is definitively gone. An access-denied or live
 * pid is treated as alive and kept. */
void tp_worker_remove_dir_tree(const char *path);
void tp_worker_reap_stale_dirs(const char *parent, const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* TP_BUILD_WORKER_INTERNAL_H */
