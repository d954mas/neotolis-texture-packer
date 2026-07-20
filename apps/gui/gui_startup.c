/* ntpacker-gui: pure startup open/defer decision -- see gui_startup.h for the truth table + rationale.
 * No I/O, no status writes: it exists so main()'s crash-recovery data-loss guard has a single
 * source of truth that the headless selftest can drive over its full truth table (J14). */
#include "gui_startup.h"

gui_startup_action gui_startup_decide(bool arg_present, bool arg_exists, bool recovered) {
    if (recovered && arg_present) {
        return GUI_STARTUP_DEFER; /* recovered unsaved work + a file arg -> defer (never silently discard) */
    }
    if (!arg_present) {
        return GUI_STARTUP_IDLE; /* no file arg -> nothing to open */
    }
    if (!arg_exists) {
        return GUI_STARTUP_MISSING; /* stale/missing arg (and not recovered) */
    }
    return GUI_STARTUP_OPEN; /* arg present + exists + not recovered -> open it */
}
