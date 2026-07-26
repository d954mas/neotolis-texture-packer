#ifndef NTPACKER_GUI_BENCH_H
#define NTPACKER_GUI_BENCH_H

/* Dev seam: the `--bench-perf[=<out.txt>]` headless perf-probe mode (compiled into EVERY build, same
 * spirit as --shot -- available in every build, NOT documented in --help). When active it opens the
 * owner-scale bench fixture, lets the project/rows settle for a few frames, then times the EXISTING
 * GUI interactions (row rebuild, filter, every sort key, selection, edit + undo/redo, refresh) and prints
 * machine-readable `bench_perf ...` lines, asserts the non-blocking hard-gates (async pack request +
 * refresh-does-not-mutate-revision), measures per-frame render time when a real GL context is present
 * (skipped under NTPACKER_GUI_HEADLESS), optionally mirrors the lines to a file, and quits.
 *
 * Timing is advisory (a slow number never fails the run -- D decides budgets); only an invariant
 * violation or a hard fixture-load failure makes the process exit non-zero (gui_bench_exit_code).
 *
 * The prototypes below are the only entry points main()/frame() call. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main() arg loop: consume `--bench-perf` or `--bench-perf=<out.txt>`. Returns true if `arg` was the
 * bench flag (so main() skips its project-arg fallback for it). */
bool gui_bench_parse_arg(const char *arg);

/* True while a --bench-perf run is active. main() gates the interactive startup side-effects (crash
 * handler, file log, crash-report + recovery modals) on it -- exactly like --shot -- so the headless
 * probe never blocks on a native modal, and supplies the default fixture path when no [project] arg
 * was given. */
bool gui_bench_active(void);

/* The default bench fixture opened when --bench-perf is active and no [project] arg was passed. */
const char *gui_bench_default_project(void);

/* frame(), UNCONDITIONAL (next to auto_pack_tick, before the can_render render block): captures the
 * frame-start timestamp and drives the bench state machine (warmup -> action/invariant probes ->
 * async-pack drain -> frame timing). No-op unless --bench-perf is active. */
void gui_bench_tick(void);

/* frame(), at the pre-swap point (next to gui_shot_post_draw): accrues per-frame render-time samples,
 * writes the optional output file, then quits at a clean frame boundary. No-op unless active. */
void gui_bench_post_draw(void);

/* main()'s return value: 0 for a clean run (or no bench), non-zero if a --bench-perf invariant assert
 * or a hard fixture-load failure fired. Persists after the run ends. */
int gui_bench_exit_code(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_BENCH_H */
