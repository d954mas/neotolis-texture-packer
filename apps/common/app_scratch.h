#ifndef NTPACKER_APP_SCRATCH_H
#define NTPACKER_APP_SCRATCH_H

/* The one scratch root every ntpacker client uses for transient job output --
 * `<user cache dir>/ntpacker/work` -- plus the private per-request directory a
 * single Pack/Export job writes into.
 *
 * Neither client may pick its own location:
 *   - `<exe_dir>` is read-only for an install under Program Files or /usr/bin,
 *     and it is shared by every running instance, so two instances collided on
 *     one transient `<atlas>.ntpack` name.
 *   - the system temp dir is a RAM-backed tmpfs on most Linux distributions, so
 *     a multi-hundred-megabyte artifact would be charged to RAM, and Windows
 *     disk cleaners may delete files there while a job is still writing them.
 *
 * Request directories follow the `req-<hexpid>-<hexid>` naming contract shared
 * with the job worker (packer/src/tp_build_worker_internal.h): the pid in the
 * name is the pid whose death makes the directory garbage, so ONE reaper heals
 * every owner that follows the contract. This module creates and releases; it
 * does not reap. The reaper is the job worker's pid-liveness sweep over the same
 * root, which therefore also collects a directory a killed client left behind.
 *
 * Failure is structured data: a scratch root that cannot be resolved or created
 * fails the job request. There is deliberately no fallback location -- a silent
 * fallback is how the exe-dir and temp-dir conventions became load-bearing. */

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Platform user cache directory: %LOCALAPPDATA% on Windows, $XDG_CACHE_HOME
 * (else $HOME/.cache) elsewhere. Cache semantics, not state semantics: the
 * content here is reproducible and safe for the OS to reclaim between runs. */
tp_status app_scratch_cache_dir(char *out, size_t out_cap, tp_error *err);

/* "<cache_dir>/ntpacker/work", created with mkdir -p and verified to be a
 * directory. `cache_dir` is the explicit injection point: production passes
 * app_scratch_cache_dir(), a test passes its own sandbox. */
tp_status app_scratch_root_in(const char *cache_dir, char *out, size_t out_cap,
                              tp_error *err);

/* app_scratch_cache_dir() + app_scratch_root_in(): the production resolver both
 * shipped clients call. */
tp_status app_scratch_root(char *out, size_t out_cap, tp_error *err);

/* "<root>/req-<hexpid>-<hexid>", created empty. Two live requests of this
 * process never share a directory and no other process can name ours, which is
 * what keeps two concurrent jobs from clobbering one `<atlas>.ntpack`. */
tp_status app_scratch_request_dir(const char *root, uint64_t request_id,
                                  char *out, size_t out_cap, tp_error *err);

/* Releases a request directory this process created: removes the files it holds
 * and then the directory itself. Best-effort and silent. A directory a crash
 * left holding subdirectories survives for the shared `req-` reaper, which
 * removes it once this pid is definitively gone. Any path whose last component
 * is not a `req-` name is refused, so a malformed argument can never take a
 * directory this module did not create. */
void app_scratch_request_dir_release(const char *request_dir);

#ifdef __cplusplus
}
#endif

#endif
