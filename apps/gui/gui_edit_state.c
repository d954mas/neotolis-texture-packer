#include "gui_edit_state.h"

#include <ctype.h>
#include <string.h>

#include "core/nt_assert.h"

static tp_status invalid_transition(
    tp_error *err, const char *message) {
    return tp_error_set(
        err, TP_STATUS_INVALID_ARGUMENT, "%s", message);
}

static bool transaction_id_valid(
    const char transaction_id[33]) {
    if (!transaction_id ||
        strlen(transaction_id) != 32U) {
        return false;
    }
    for (size_t index = 0U; index < 32U; ++index) {
        const unsigned char byte =
            (unsigned char)transaction_id[index];
        if (!isdigit(byte) &&
            !(byte >= (unsigned char)'a' &&
              byte <= (unsigned char)'f')) {
            return false;
        }
    }
    return true;
}

static void become_idle(gui_edit_state *state) {
    NT_ASSERT(state != NULL);
    const tp_id128 view_id = state->view_id;
    memset(state, 0, sizeof *state);
    state->view_id = view_id;
}

static void become_conflicted(
    gui_edit_state *state, bool target_present) {
    NT_ASSERT(state != NULL);
    state->phase = GUI_EDIT_CONFLICTED;
    state->submitted_revision = 0;
    state->submitted_transaction_id[0] = '\0';
    state->target_present = target_present;
}

static tp_status validate_submit(
    int64_t base_revision,
    const char transaction_id[33],
    int64_t submitted_revision, tp_error *err) {
    if (!transaction_id_valid(transaction_id) ||
        submitted_revision <= base_revision) {
        return invalid_transition(
            err,
            "GUI draft submit requires a canonical transaction and advancing revision");
    }
    return TP_STATUS_OK;
}

static void start_submit(
    gui_edit_state *state,
    const char transaction_id[33],
    int64_t submitted_revision) {
    NT_ASSERT(state != NULL);
    NT_ASSERT(transaction_id != NULL);
    state->phase = GUI_EDIT_SUBMITTING;
    state->submitted_revision = submitted_revision;
    (void)memcpy(
        state->submitted_transaction_id,
        transaction_id,
        sizeof state->submitted_transaction_id);
}

void gui_edit_state_init(
    gui_edit_state *state, tp_id128 view_id) {
    NT_ASSERT(state != NULL);
    memset(state, 0, sizeof *state);
    state->view_id = view_id;
}

tp_status gui_edit_begin(
    gui_edit_state *state, tp_id128 target_id,
    int64_t base_revision, tp_id128 draft_instance_id,
    tp_error *err) {
    if (!state) {
        return invalid_transition(
            err, "GUI edit begin requires state");
    }
    if (state->phase != GUI_EDIT_IDLE) {
        return invalid_transition(
            err, "cannot begin a second active GUI draft");
    }
    if (tp_id128_is_nil(state->view_id) ||
        tp_id128_is_nil(target_id) ||
        tp_id128_is_nil(draft_instance_id) ||
        base_revision < 0) {
        return invalid_transition(
            err,
            "GUI draft begin requires stable identities and revision");
    }

    state->phase = GUI_EDIT_EDITING;
    state->target_id = target_id;
    state->base_revision = base_revision;
    state->draft_instance_id = draft_instance_id;
    state->target_present = true;
    return TP_STATUS_OK;
}

tp_status gui_edit_commit(
    gui_edit_state *state, bool net_zero,
    const char transaction_id[33],
    int64_t submitted_revision, tp_error *err) {
    if (!state) {
        return invalid_transition(
            err, "GUI edit commit requires state");
    }
    if (state->phase != GUI_EDIT_EDITING) {
        return invalid_transition(
            err, "only an editing GUI draft can commit");
    }
    if (net_zero) {
        become_idle(state);
        return TP_STATUS_OK;
    }
    const tp_status status =
        validate_submit(
            state->base_revision, transaction_id,
            submitted_revision, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    start_submit(
        state, transaction_id, submitted_revision);
    return TP_STATUS_OK;
}

tp_status gui_edit_submit_result(
    gui_edit_state *state, bool exact_owner,
    tp_status status, bool committed, bool no_change,
    bool echo_observed, int64_t receipt_revision,
    int64_t current_revision, tp_error *err) {
    if (!state) {
        return invalid_transition(
            err, "GUI edit submit result requires state");
    }
    if (state->phase != GUI_EDIT_SUBMITTING ||
        !exact_owner) {
        return invalid_transition(
            err,
            "GUI submit result does not own the active draft");
    }
    if (status == TP_STATUS_OK &&
        (no_change || (committed && echo_observed))) {
        become_idle(state);
        return TP_STATUS_OK;
    }
    if (status == TP_STATUS_OK && committed) {
        state->submitted_revision = receipt_revision;
        return TP_STATUS_OK;
    }
    if (current_revision == state->base_revision) {
        state->phase = GUI_EDIT_EDITING;
        state->submitted_revision = 0;
        state->submitted_transaction_id[0] = '\0';
    } else {
        become_conflicted(state, state->target_present);
    }
    return TP_STATUS_OK;
}

tp_status gui_edit_model_revision(
    gui_edit_state *state, int64_t revision,
    bool target_present, bool exact_echo, tp_error *err) {
    if (!state) {
        return invalid_transition(
            err, "GUI model revision requires edit state");
    }
    if (state->phase == GUI_EDIT_IDLE ||
        revision <= state->base_revision) {
        return TP_STATUS_OK;
    }
    if (state->phase == GUI_EDIT_SUBMITTING &&
        exact_echo) {
        become_idle(state);
        return TP_STATUS_OK;
    }
    become_conflicted(state, target_present);
    return TP_STATUS_OK;
}

tp_status gui_edit_resync(
    gui_edit_state *state, int64_t revision,
    bool target_present, bool exact_terminal,
    tp_error *err) {
    if (!state) {
        return invalid_transition(
            err, "GUI resync requires edit state");
    }
    if (state->phase == GUI_EDIT_IDLE) {
        return TP_STATUS_OK;
    }
    if (state->phase == GUI_EDIT_SUBMITTING &&
        exact_terminal) {
        become_idle(state);
        return TP_STATUS_OK;
    }
    if (revision != state->base_revision) {
        become_conflicted(state, target_present);
    }
    return TP_STATUS_OK;
}

tp_status gui_edit_apply_mine(
    gui_edit_state *state, int64_t revision,
    int64_t submitted_revision, bool target_present,
    const char transaction_id[33], tp_error *err) {
    if (!state) {
        return invalid_transition(
            err, "GUI Apply Mine requires edit state");
    }
    if (state->phase != GUI_EDIT_CONFLICTED) {
        return invalid_transition(
            err,
            "Apply Mine requires a conflicted GUI draft");
    }
    if (!target_present) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "the edited target no longer exists");
    }
    const tp_status status =
        validate_submit(
            revision, transaction_id,
            submitted_revision, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    state->base_revision = revision;
    state->target_present = true;
    start_submit(
        state, transaction_id, submitted_revision);
    return TP_STATUS_OK;
}

tp_status gui_edit_discard(
    gui_edit_state *state, tp_error *err) {
    if (!state) {
        return invalid_transition(
            err, "GUI edit discard requires state");
    }
    if (state->phase == GUI_EDIT_SUBMITTING) {
        return invalid_transition(
            err,
            "an uncertain submitted draft cannot be discarded");
    }
    become_idle(state);
    return TP_STATUS_OK;
}
