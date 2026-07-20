#ifndef TP_BUILD_WORKER_INTERNAL_H
#define TP_BUILD_WORKER_INTERNAL_H

/* Parent side of the private build worker (decision 0018, ROADMAP H0.3-b). This
 * is the production Pack cutover: tp_pack hands validated settings + decoded
 * pixels here, and the builder runs in a re-exec'd child process so an
 * NT_BUILD_ASSERT, allocation, codec, or write failure in nt_builder can never
 * terminate the host. The in-process driver (tp_build_driver.c) stays: it is the
 * body the worker child runs, the non-ASCII fallback below, and the oracle. */

#include "tp_core/tp_error.h"
#include "tp_core/tp_image.h"
#include "tp_core/tp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs one atlas through the private build worker. Signature mirrors
 * tp_build_driver_run and CONSUMES `loaded_images` (frees it on every path).
 * Outcome mapping:
 *   worker exit 0 + valid OK reply + readable artifact  -> TP_STATUS_OK
 *   valid reply carrying a builder/sink failure          -> TP_STATUS_BUILDER_FAILED (carried msg)
 *   crash / signal / abnormal / non-zero-without-a-reply -> TP_STATUS_BUILDER_CRASHED
 *   malformed / truncated / oversized reply, clean exit  -> TP_STATUS_BUILDER_FAILED (fail closed)
 *
 * TEMPORARY H0.3-b fallback: a non-ASCII `out_path` cannot yet be handed to the
 * builder through the ASCII worker out_name (the ASCII staging dir + tp_fs move
 * is H0.4), so such a pack runs the in-process driver instead -- identical to
 * the pre-worker behavior, so no regression. Documented loudly at the seam. */
tp_status tp_build_worker_run(const tp_pack_settings *settings,
                              tp_image_rgba8 *loaded_images,
                              const char *out_path, tp_error *err);

/* Test seam: run against an explicit worker executable (`worker_exe` NULL = own
 * module path). Lets the fault-injecting worker binaries exercise the crash /
 * malformed-reply / non-zero-exit outcome mapping. Production uses
 * tp_build_worker_run, which never takes an executable from the environment. */
tp_status tp_build_worker_run_exe(const tp_pack_settings *settings,
                                  tp_image_rgba8 *loaded_images,
                                  const char *out_path, const char *worker_exe,
                                  tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_BUILD_WORKER_INTERNAL_H */
