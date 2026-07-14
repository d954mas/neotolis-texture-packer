/* ntpacker-gui: the startup CLI-arg open/defer decision, extracted as a PURE function.
 *
 * H/P1-8 fix: main()'s crash-recovery data-loss guard used to be inline branches with ZERO automated
 * coverage (a refactor that restored an unconditional gui_project_open on a CLI arg would ship green).
 * This is the SINGLE SOURCE OF TRUTH for that decision: it does NO I/O and writes NO status -- the
 * caller maps the returned action to a status line and/or a gui_project_open. Being pure (bools in,
 * enum out) it is directly headless-testable (main() is not); the selftest drives the full truth table
 * (J14). Truth table:
 *
 *   recovered && arg_present               -> GUI_STARTUP_DEFER    recovered unsaved work is live; opening
 *                                                                  the arg would silently discard it
 *   !arg_present                           -> GUI_STARTUP_IDLE     no file arg -> nothing to open
 *   arg_present && !arg_exists (& !rec.)   -> GUI_STARTUP_MISSING  stale/missing file arg
 *   arg_present && arg_exists  (& !rec.)   -> GUI_STARTUP_OPEN     open it
 *
 * DEFER takes precedence over MISSING: a recovered launch with a stale arg still DEFERS (it must never
 * become a "project not found" that clobbers the recovery warning -- finding 2). Recovered unsaved work
 * is never destroyed and never opened over.
 */
#ifndef NTPACKER_GUI_STARTUP_H
#define NTPACKER_GUI_STARTUP_H

#include <stdbool.h>

typedef enum { GUI_STARTUP_OPEN, GUI_STARTUP_DEFER, GUI_STARTUP_MISSING, GUI_STARTUP_IDLE } gui_startup_action;

/* Pure: no set_status / no gui_project_open inside. `recovered` is the DURABLE model predicate
 * (gui_project_has_recovered_unsaved), not the drained one-shot notice -- one source of truth for the
 * condition. See the header comment for the full truth table. */
gui_startup_action gui_startup_decide(bool arg_present, bool arg_exists, bool recovered);

#endif /* NTPACKER_GUI_STARTUP_H */
