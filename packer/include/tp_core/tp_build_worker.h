#ifndef TP_CORE_TP_BUILD_WORKER_H
#define TP_CORE_TP_BUILD_WORKER_H

/* Private build-worker entry + dispatch
 * (docs/architecture/jobs-pack-and-cache.md).
 *
 * The production Pack path runs nt_builder in a child process so a builder
 * abort/allocation/codec/write failure can never terminate the host. There is
 * NO separately shipped worker executable: each host re-execs itself with the
 * hidden argv[1] mode below and services the request. The mode never touches
 * any UI or ordinary CLI surface -- keep it out of help text and verb tables. */

#include <stdbool.h>

#include "tp_core/tp_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hidden argv[1] token that marks a process as the re-exec'd build worker. */
#define TP_BUILD_WORKER_ARGV1 "__build-worker"

/* True when this process was launched as the build worker child, i.e.
 * argc >= 2 && argv[1] == TP_BUILD_WORKER_ARGV1. EVERY first-party executable
 * whose main() can reach tp_pack MUST call this FIRST -- before any engine init,
 * UI, or argument parsing -- and, when it returns true,
 * `return tp_build_worker_main();`. A pack re-execs the current executable, so a
 * main that skips this dispatch would re-run its own program as the "worker" and
 * (for a program that itself packs) fork-bomb. */
bool tp_build_is_worker_invocation(int argc, char **argv);

/* Service exactly one pack: read one bounded, length-checked request frame from
 * stdin, run it through the same in-process builder driver the parent would have
 * used, write one bounded response frame to stdout, and return 0. Returns
 * non-zero only if it cannot emit a reply at all. It takes over the process:
 * call it and return its result. Never asserts on wire bytes (fail closed). */
int tp_build_worker_main(void);

/* Complete one startup scan through the isolated compiler and atomically
 * finalize its immutable catalog.  This is the only host-visible compile
 * facade: callers exchange value-owned scan/catalog objects and never touch
 * Lua or the private compiler protocol.  On success `*owned_scan` is consumed.
 */
tp_status tp_build_format_catalog_compile(
    tp_format_catalog_scan **owned_scan, tp_format_catalog **out_catalog,
    tp_error *error);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_BUILD_WORKER_H */
