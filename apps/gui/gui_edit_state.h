#ifndef NTPACKER_GUI_EDIT_STATE_H
#define NTPACKER_GUI_EDIT_STATE_H

#include "tp_core/tp_id.h"

typedef enum gui_edit_phase {
    GUI_EDIT_IDLE = 0,
    GUI_EDIT_EDITING,
    GUI_EDIT_SUBMITTING,
    GUI_EDIT_CONFLICTED,
} gui_edit_phase;

typedef struct gui_edit_state {
    gui_edit_phase phase;
    tp_id128 view_id;
    tp_id128 target_id;
    int64_t base_revision;
    int64_t submitted_revision;
    tp_id128 draft_instance_id;
    char submitted_transaction_id[33];
    bool target_present;
} gui_edit_state;

void gui_edit_state_init(gui_edit_state *state, tp_id128 view_id);

tp_status gui_edit_begin(
    gui_edit_state *state, tp_id128 target_id,
    int64_t base_revision, tp_id128 draft_instance_id,
    tp_error *err);

tp_status gui_edit_commit(
    gui_edit_state *state, bool net_zero,
    const char transaction_id[33],
    int64_t submitted_revision, tp_error *err);

tp_status gui_edit_submit_result(
    gui_edit_state *state, bool exact_owner,
    tp_status status, bool committed, bool no_change,
    bool echo_observed, int64_t receipt_revision,
    int64_t current_revision, tp_error *err);

tp_status gui_edit_model_revision(
    gui_edit_state *state, int64_t revision,
    bool target_present, bool exact_echo, tp_error *err);

tp_status gui_edit_resync(
    gui_edit_state *state, int64_t revision,
    bool target_present, bool exact_terminal, tp_error *err);

tp_status gui_edit_apply_mine(
    gui_edit_state *state, int64_t revision,
    int64_t submitted_revision, bool target_present,
    const char transaction_id[33], tp_error *err);

tp_status gui_edit_discard(
    gui_edit_state *state, tp_error *err);

#endif /* NTPACKER_GUI_EDIT_STATE_H */
