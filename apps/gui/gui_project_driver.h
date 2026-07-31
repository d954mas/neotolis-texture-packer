#ifndef NTPACKER_GUI_PROJECT_DRIVER_H
#define NTPACKER_GUI_PROJECT_DRIVER_H

#include "gui_project.h"

/* Narrow controller-facing host driver. Views include gui_project_view.h and
 * submit requests; only the actions controller and private blocking adapter
 * advance this boundary. */
typedef struct gui_project_step_result {
    gui_project_lifecycle_kind lifecycle_completed;
    tp_session_job_result completion;
} gui_project_step_result;

tp_status gui_project_step(
    gui_project_step_result *out, tp_error *err);

#endif /* NTPACKER_GUI_PROJECT_DRIVER_H */
