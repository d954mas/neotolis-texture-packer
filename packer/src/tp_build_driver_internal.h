#ifndef TP_BUILD_DRIVER_INTERNAL_H
#define TP_BUILD_DRIVER_INTERNAL_H

/* Private tp_build seam: the in-process nt_builder driver extracted from
 * tp_pack (decision 0018, ROADMAP H0.3). This is the ONLY nt_builder call site
 * in production; tp_pack keeps validate/preflight/name-map/read-back and hands
 * decoded pixels to the driver. The signature mirrors the future private build
 * worker (settings + raw pixels + a resolved output path); the process cutover
 * (H0.3-b) wraps this same body behind the versioned worker protocol. */

#include "tp_core/tp_error.h"
#include "tp_core/tp_image.h"
#include "tp_core/tp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Drives nt_builder for one atlas into `out_path` using the §5 export-friendly
 * profile and the exact former run_builder call sequence, so the produced
 * .ntpack is byte-identical to a direct nt_builder invocation (pinned by the
 * byte-identical oracle test). `settings` is already validated by tp_pack.
 *
 * `loaded_images` is the caller-decoded RGBA8 for path sprites (index-aligned
 * with settings->sprites; NULL when no sprite has a path). The driver CONSUMES
 * it: after nt_builder deep-copies each sprite (and on every failure path) it
 * frees the images, matching the former in-place free that let decode buffers
 * release before encode/assembly.
 *
 * `out_path` is the caller-resolved output path -- the driver never composes
 * directories. Returns TP_STATUS_OK, or TP_STATUS_BUILDER_FAILED with `err`
 * filled when nt_builder start/finish reports an error. */
tp_status tp_build_driver_run(const tp_pack_settings *settings,
                              tp_image_rgba8 *loaded_images,
                              const char *out_path, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_BUILD_DRIVER_INTERNAL_H */
