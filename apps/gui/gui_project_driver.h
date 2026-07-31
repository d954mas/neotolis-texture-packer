#ifndef NTPACKER_GUI_PROJECT_DRIVER_H
#define NTPACKER_GUI_PROJECT_DRIVER_H

#include "gui_project.h"

/* Narrow controller-facing host driver. Views include gui_project_view.h and
 * submit requests; only the actions controller advances this boundary. */
typedef struct gui_project_step_result {
    gui_project_lifecycle_kind lifecycle_completed;
    tp_session_job_result completion;
} gui_project_step_result;

tp_status gui_project_step(
    gui_project_step_result *out, tp_error *err);
/* True only between a successful project step and the next non-const session
 * call. The actions FSM uses this linear boundary to stop a drain before any
 * remaining intent can observe a stale borrowed cut. */
bool gui_project_observation_is_valid(void);

#endif /* NTPACKER_GUI_PROJECT_DRIVER_H */
