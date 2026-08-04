#ifndef NTPACKER_GUI_PROJECT_INTERNAL_H
#define NTPACKER_GUI_PROJECT_INTERNAL_H

#include "app_format_catalog.h"
#include "gui_project_driver.h"

/* Small active/candidate host plus presentation-only state. */
typedef struct gui_project_state {
    /* One process-host owner for the active generation and all startup failure
     * context. Keeping the shared result whole makes its close operation the
     * only lifecycle authority when that result grows. */
    app_format_catalog formats;
    /* Installed atomically with `formats` for this exact retained generation. */
    tp_format_catalog *format_projection_catalog;
    const tp_format_descriptor **available_formats;
    int available_format_count;
    tp_session *session;
    tp_session *candidate;
    const struct tp_session_view *view;
    gui_project_lifecycle_state lifecycle_state;
    uint64_t instance_generation;
    uint64_t reduced_instance_generation;
    uint64_t snapshot_lifetime_generation;
    uint64_t published_instance_generation;
    uint64_t published_snapshot_generation;
    uint64_t observed_source_generation;
    int64_t observed_revision;
    bool snapshot_published;
    bool observation_valid;
    bool refresh_pending;
    bool discard_retired_session;
    double drain_started_at;
    bool drain_deadline_expired;
    gui_project_controller_status_port
        controller_status;
    bool preview_stale;
    char name[256];
    bool op_error;
    tp_status op_error_status;
    char op_error_msg[256];
    bool recovery_notice_active;
    gui_recovery_notice recovery_notice;
    char recovery_root[TP_IDENTITY_PATH_MAX];
    bool recovery_required;
    bool recovery_setup_notice_pending;
    char recovery_setup_notice[256];
    bool save_notice_pending;
    char save_notice[256];
} gui_project_state;

extern gui_project_state s_project;

tp_session *gui_project__borrow_active_session(void);
void gui_project__note_session_reject(tp_status status, const tp_error *err);
void gui_project__note_recovery_degraded(tp_status status);
void gui_project__sync_recovery_notice(void);
void gui_project__attach_recovery_live(void);
tp_status gui_project__prepare_candidate_recovery(
    tp_session *session, tp_error *err);
bool gui_project__ingress_is_open(void);
tp_session *gui_project__mutation_session(void);
void gui_project__invalidate_observation(void);
void gui_project__assert_lifecycle_invariants(void);
tp_status gui_project__advance_lifecycle(
    gui_project_lifecycle_kind *completed,
    tp_error *err);
void gui_project__publish_view(
    const struct tp_session_view *view);
void gui_project__reduce_view(void);
void gui_project__clear_format_projection(void);
/* Consumes `candidate` only on success. Projection materialization completes
 * before the active catalog/projection pair is replaced. */
tp_status gui_project__install_format_catalog(
    app_format_catalog *candidate, tp_error *err);
#ifdef TP_ENABLE_TEST_SEAMS
void gui_project__test_set_format_projection_alloc_fail(bool fail);
#endif

#endif /* NTPACKER_GUI_PROJECT_INTERNAL_H */
