#ifndef NTPACKER_GUI_PROJECT_DRIVER_H
#define NTPACKER_GUI_PROJECT_DRIVER_H

#include "gui_project.h"

/* Narrow controller-facing host driver. Views include gui_project_view.h and
 * submit requests; only the actions controller advances this boundary. */
typedef enum gui_format_reload_outcome {
    GUI_FORMAT_RELOAD_NONE = 0,
    GUI_FORMAT_RELOAD_SUCCEEDED,
    GUI_FORMAT_RELOAD_FAILED
} gui_format_reload_outcome;

typedef struct gui_format_reload_result {
    gui_format_reload_outcome outcome;
    tp_status status;
    int ready_count;
    int unavailable_count;
    char detail[256];
} gui_format_reload_result;

/* GUI-runtime catalog replacement. Repeated begin calls coalesce. The project
 * host cancels Pack/Export once and waits for their terminal receipt; Refresh
 * remains independent. Completion is returned by gui_project_step. */
tp_status gui_project_format_reload_begin(tp_error *err);
bool gui_project_format_reload_active(void);

typedef struct gui_project_step_result {
    gui_project_lifecycle_kind lifecycle_completed;
    gui_format_reload_result format_reload;
    tp_session_job_result completion;
} gui_project_step_result;

tp_status gui_project_step(
    gui_project_step_result *out, tp_error *err);
/* True only between a successful project step and the next non-const session
 * call. The actions FSM uses this linear boundary to stop a drain before any
 * remaining intent can observe a stale borrowed cut. */
bool gui_project_observation_is_valid(void);

#endif /* NTPACKER_GUI_PROJECT_DRIVER_H */
