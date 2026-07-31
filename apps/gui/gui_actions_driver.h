#ifndef NTPACKER_GUI_ACTIONS_DRIVER_H
#define NTPACKER_GUI_ACTIONS_DRIVER_H

/* Host-facing driver for the actions FSM. Views include gui_actions.h and can
 * only submit typed inputs/read passive state; main() and explicit dev/test
 * adapters own this between-frame boundary. */

#include <stdbool.h>

#include "gui_pack.h"
#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum gui_job_request_kind {
    GUI_JOB_REQUEST_NONE = 0,
    GUI_JOB_REQUEST_REFRESH,
    GUI_JOB_REQUEST_CANCEL,
    GUI_JOB_REQUEST_PACK,
    GUI_JOB_REQUEST_EXPORT,
    GUI_JOB_REQUEST_PREVIEW,
    GUI_JOB_REQUEST_COUNT
} gui_job_request_kind;

typedef struct gui_job_request_receipt {
    gui_job_request_kind kind;
    bool admitted;
    char detail[256];
} gui_job_request_receipt;

enum {
    GUI_ACTIONS_STEP_MAX_JOB_RECEIPTS =
        GUI_JOB_REQUEST_COUNT - 1
};
typedef struct gui_actions_step_result {
    gui_job_request_receipt
        job_receipts[GUI_ACTIONS_STEP_MAX_JOB_RECEIPTS];
    int job_receipt_count;
    bool job_completion_present;
    gui_pack_result_info job_completion;
} gui_actions_step_result;

/* The one between-frame boundary: drain typed UI inputs until a mutable
 * session call closes the current observation cut, advance the explicit
 * project FSM once, consume task/lifecycle terminals, publish a fresh borrowed
 * view, and reconcile presentation. Remaining inputs stay controller-owned and
 * resume on a later call; callers never drive that sequence themselves. */
tp_status gui_actions_step(
    gui_actions_step_result *out, tp_error *err);

/* Host bootstrap/teardown adapters. They hide the lifecycle begin-to-step
 * protocol so main() does not become a second FSM driver. */
tp_status gui_actions_host_open(
    const char *path, tp_error *err);
tp_status gui_actions_host_shutdown_step(
    bool *out_closed, tp_error *err);

/* Releases action-owned heap after the host has stopped driving the FSM. */
void gui_actions_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_ACTIONS_DRIVER_H */
