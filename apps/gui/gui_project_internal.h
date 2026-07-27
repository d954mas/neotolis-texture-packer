#ifndef NTPACKER_GUI_PROJECT_INTERNAL_H
#define NTPACKER_GUI_PROJECT_INTERNAL_H

#include "gui_project.h"
#include "gui_host_binding.h"

/* Private storage shared only by the physical gui_project implementation files.
 * This is the existing adapter state gathered in one place, not a second model. */
typedef struct gui_project_state {
    gui_host_binding binding;
    gui_project_lifecycle_kind lifecycle_kind;
    uint64_t observed_instance_generation;
    int64_t observed_revision;
    bool binding_initialized;
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

void gui_project__snapshot_drop(void);
tp_status gui_project__client_init(tp_error *err);
tp_session *gui_project__borrow_active_session(void);
void gui_project__note_session_reject(tp_status status, const tp_error *err);
void gui_project__note_recovery_degraded(tp_status status);
void gui_project__sync_recovery_notice(void);
void gui_project__attach_recovery_live(tp_session *session);
tp_status gui_project__prepare_candidate_recovery(
    tp_session *session, tp_error *err);
bool gui_project__ingress_is_open(void);

#endif /* NTPACKER_GUI_PROJECT_INTERNAL_H */
