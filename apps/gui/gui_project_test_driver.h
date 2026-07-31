#ifndef NTPACKER_GUI_PROJECT_TEST_DRIVER_H
#define NTPACKER_GUI_PROJECT_TEST_DRIVER_H

#include <stdio.h>

#include "core/nt_assert.h"
#include "gui_project.h"
#include "time/nt_time.h"

static inline bool gui_project_test_state_is_draining(
    gui_project_lifecycle_state state) {
    return state == GUI_PROJECT_LIFECYCLE_NEW_DRAINING ||
           state == GUI_PROJECT_LIFECYCLE_OPEN_DRAINING ||
           state == GUI_PROJECT_LIFECYCLE_SHUTDOWN_DRAINING;
}

static inline bool gui_project_test_state_is_transitioning(
    gui_project_lifecycle_state state) {
    return state != GUI_PROJECT_LIFECYCLE_ACTIVE &&
           state != GUI_PROJECT_LIFECYCLE_CLOSED;
}

static inline tp_status gui_project_test_drain(
    gui_project_lifecycle_kind *completed,
    tp_error *err) {
    gui_project_lifecycle_kind terminal =
        GUI_PROJECT_LIFECYCLE_NONE;
    unsigned int attempts = 0U;
    while (attempts < 5000U &&
           gui_project_test_state_is_transitioning(
               gui_project_lifecycle_state_query())) {
        attempts++;
        tp_status status =
            gui_project_lifecycle_pump(
                &terminal, err);
        if (status != TP_STATUS_OK) {
            return status;
        }
        if (gui_project_test_state_is_draining(
                gui_project_lifecycle_state_query())) {
            status = gui_project_frame_begin(err);
            if (status != TP_STATUS_OK) {
                return status;
            }
            tp_session_job_result completion = {0};
            if (gui_project_take_completion(
                    &completion)) {
                tp_session_job_result_destroy(
                    &completion);
            }
            gui_project_frame_end();
            nt_time_sleep(0.001);
        }
    }
    if (gui_project_test_state_is_transitioning(
            gui_project_lifecycle_state_query())) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI lifecycle did not converge within the test budget");
    }
    if (terminal ==
        GUI_PROJECT_LIFECYCLE_NONE) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI lifecycle completed without a terminal signal");
    }
    if (completed) {
        *completed = terminal;
    }
    return TP_STATUS_OK;
}

static inline tp_status gui_project_test_finish(
    gui_project_lifecycle_kind expected,
    tp_error *err) {
    gui_project_lifecycle_kind completed =
        GUI_PROJECT_LIFECYCLE_NONE;
    const tp_status status =
        gui_project_test_drain(&completed, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (completed != expected) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI lifecycle completed with an unexpected receipt");
    }
    /* New/Open leave one coalesced Refresh pending after ACTIVE opens ingress.
     * Its only admission point is frame_begin, so publish that first boundary
     * before waiting for the asynchronous bootstrap. */
    unsigned int attempts = 0U;
    if (expected == GUI_PROJECT_LIFECYCLE_NEW ||
        expected == GUI_PROJECT_LIFECYCLE_OPEN) {
        tp_status update_status =
            gui_project_frame_begin(err);
        if (update_status != TP_STATUS_OK) {
            return update_status;
        }
        gui_project_frame_end();
    }
    while (gui_project_job_busy() &&
           attempts++ < 5000U) {
        tp_status update_status =
            gui_project_frame_begin(err);
        if (update_status != TP_STATUS_OK) {
            return update_status;
        }
        tp_session_job_result completion = {0};
        if (gui_project_take_completion(
                &completion)) {
            tp_session_job_result_destroy(
                &completion);
        }
        gui_project_frame_end();
        nt_time_sleep(0.001);
    }
    if (gui_project_job_busy()) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI initial Refresh did not converge within the test budget");
    }
    return TP_STATUS_OK;
}

static inline bool gui_project_test_new(void) {
    tp_error err = {{0}};
    return gui_project_lifecycle_begin_new(
               &err) == TP_STATUS_OK &&
           gui_project_test_finish(
               GUI_PROJECT_LIFECYCLE_NEW,
               &err) == TP_STATUS_OK;
}

static inline tp_status gui_project_test_open(
    const char *path, char *err_out,
    size_t err_cap) {
    tp_error err = {{0}};
    tp_status status =
        gui_project_lifecycle_begin_open(
            path, &err);
    if (status == TP_STATUS_OK) {
        status = gui_project_test_finish(
            GUI_PROJECT_LIFECYCLE_OPEN,
            &err);
    }
    if (status != TP_STATUS_OK &&
        err_out && err_cap > 0U) {
        (void)snprintf(
            err_out, err_cap, "%s",
            err.msg[0] ? err.msg
                       : tp_status_str(status));
    }
    return status;
}

static inline void gui_project_test_shutdown(
    bool discard_recovery) {
    if (gui_project_test_state_is_transitioning(
            gui_project_lifecycle_state_query())) {
        tp_error err = {{0}};
        gui_project_lifecycle_kind prior =
            GUI_PROJECT_LIFECYCLE_NONE;
        const tp_status drain_status =
            gui_project_test_drain(
                &prior, &err);
        NT_ASSERT(drain_status == TP_STATUS_OK);
    }
    if (gui_project_lifecycle_state_query() ==
        GUI_PROJECT_LIFECYCLE_ACTIVE) {
        tp_error err = {{0}};
        const tp_status begin_status =
            gui_project_lifecycle_begin_shutdown(
                discard_recovery, &err);
        NT_ASSERT(begin_status == TP_STATUS_OK);
        const tp_status finish_status =
            gui_project_test_finish(
                GUI_PROJECT_LIFECYCLE_SHUTDOWN,
                &err);
        NT_ASSERT(finish_status == TP_STATUS_OK);
    }
    gui_project_shutdown();
}

#endif /* NTPACKER_GUI_PROJECT_TEST_DRIVER_H */
