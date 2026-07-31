#ifndef NTPACKER_GUI_ACTIONS_DEV_H
#define NTPACKER_GUI_ACTIONS_DEV_H

/* Explicit non-view adapters for benchmarks, screenshots, self-tests, and
 * focused action tests. Production views must use gui_actions.h only. */

#include "gui_actions_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

void do_pack_blocking(void);
void do_undo(void);
void do_redo(void);

/* Keep driving until a step both enters and leaves with the task slot idle.
 * That confirmed idle-entry tick admits any automatic Refresh accumulated
 * behind a task that became terminal on the preceding tick. Dev adapters use
 * this instead of reconstructing startup/Refresh sequencing. */
bool gui_actions_dev_settle_task(tp_error *err);

#if defined(NTPACKER_GUI_DEV_SEAMS) || defined(TP_ENABLE_TEST_SEAMS)
bool gui_actions_refresh_diff_headless(int *out_added, int *out_removed,
                                       int *out_changed,
                                       int *out_unavailable);
#endif

bool gui_actions_refresh_should_mark_stale(tp_status status,
                                           bool sources_invalidated);

#ifdef TP_ENABLE_TEST_SEAMS
/* Test-only half-step for proving that requests remain deferred before the
 * host boundary. Shipping callers must use gui_actions_step. */
void gui_actions__test_drain_intents(void);
#endif

#ifdef NTPACKER_GUI_SELFTEST
void gui_actions__selftest_drain_intents(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_ACTIONS_DEV_H */
