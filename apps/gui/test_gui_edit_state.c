#include "unity.h"

#include <string.h>

#include "gui_edit_state.h"

static const char *const TX_ONE =
    "11111111111111111111111111111111";
static const char *const TX_TWO =
    "22222222222222222222222222222222";

static tp_id128 test_id(unsigned char first) {
    tp_id128 id = {{0}};
    id.bytes[0] = first;
    return id;
}

static gui_edit_state idle_state(void) {
    gui_edit_state state;
    gui_edit_state_init(&state, test_id(0x21U));
    return state;
}

static gui_edit_state editing_state(void) {
    gui_edit_state state = idle_state();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_begin(
            &state, test_id(0x11U), 7,
            test_id(0x31U), &error));
    return state;
}

static gui_edit_state submitting_state(void) {
    gui_edit_state state = editing_state();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_commit(
            &state, false, TX_ONE, &error));
    return state;
}

static gui_edit_state conflicted_state(bool target_present) {
    gui_edit_state state = editing_state();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_model_revision(
            &state, 8, target_present, false, &error));
    return state;
}

static void assert_invalid_unchanged(
    tp_status status, const tp_error *error,
    const gui_edit_state *before,
    const gui_edit_state *after) {
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT, status);
    TEST_ASSERT_NOT_EQUAL(0, (int)strlen(error->msg));
    TEST_ASSERT_EQUAL_MEMORY(before, after, sizeof *after);
}

void setUp(void) {}
void tearDown(void) {}

void test_begin_records_only_lifecycle_and_stable_identity(void) {
    gui_edit_state state = editing_state();

    TEST_ASSERT_EQUAL_INT(GUI_EDIT_EDITING, state.phase);
    TEST_ASSERT_TRUE(
        tp_id128_eq(test_id(0x21U), state.view_id));
    TEST_ASSERT_TRUE(
        tp_id128_eq(test_id(0x11U), state.target_id));
    TEST_ASSERT_EQUAL_INT64(7, state.base_revision);
    TEST_ASSERT_EQUAL_INT64(0, state.submitted_revision);
    TEST_ASSERT_TRUE(
        tp_id128_eq(test_id(0x31U), state.draft_instance_id));
    TEST_ASSERT_EQUAL_STRING(
        "", state.submitted_transaction_id);
    TEST_ASSERT_TRUE(state.target_present);
}

void test_begin_rejects_invalid_identity_revision_and_second_draft(void) {
    gui_edit_state state = idle_state();
    gui_edit_state before = state;
    tp_error error = {{0}};

    assert_invalid_unchanged(
        gui_edit_begin(
            &state, tp_id128_nil(), 7,
            test_id(0x31U), &error),
        &error, &before, &state);

    memset(&error, 0, sizeof error);
    assert_invalid_unchanged(
        gui_edit_begin(
            &state, test_id(0x11U), -1,
            test_id(0x31U), &error),
        &error, &before, &state);

    memset(&error, 0, sizeof error);
    assert_invalid_unchanged(
        gui_edit_begin(
            &state, test_id(0x11U), 7,
            tp_id128_nil(), &error),
        &error, &before, &state);

    state = editing_state();
    before = state;
    memset(&error, 0, sizeof error);
    assert_invalid_unchanged(
        gui_edit_begin(
            &state, test_id(0x12U), 8,
            test_id(0x32U), &error),
        &error, &before, &state);
}

void test_commit_net_zero_finishes_and_valid_submit_records_receipt_identity(void) {
    tp_error error = {{0}};
    gui_edit_state state = editing_state();

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_commit(
            &state, true, NULL, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, state.phase);
    TEST_ASSERT_TRUE(
        tp_id128_eq(test_id(0x21U), state.view_id));

    state = editing_state();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_commit(
            &state, false, TX_ONE, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_SUBMITTING, state.phase);
    /* The draft never predicts where the commit lands. */
    TEST_ASSERT_EQUAL_INT64(0, state.submitted_revision);
    TEST_ASSERT_EQUAL_STRING(
        TX_ONE, state.submitted_transaction_id);
}

/* USA-15 partial: owns the invalid-action-per-phase table; the valid
 * transitions are spread over the other cases in this file. */
void test_commit_rejects_wrong_phase_and_bad_transaction_without_mutation(void) {
    tp_error error = {{0}};
    gui_edit_state state = idle_state();
    gui_edit_state before = state;

    assert_invalid_unchanged(
        gui_edit_commit(
            &state, false, TX_ONE, &error),
        &error, &before, &state);

    state = editing_state();
    before = state;
    memset(&error, 0, sizeof error);
    assert_invalid_unchanged(
        gui_edit_commit(
            &state, false, "ABC", &error),
        &error, &before, &state);

    memset(&error, 0, sizeof error);
    assert_invalid_unchanged(
        gui_edit_commit(
            &state, false, NULL, &error),
        &error, &before, &state);
}

void test_submit_result_requires_normalized_exact_owner(void) {
    gui_edit_state state = submitting_state();
    const gui_edit_state before = state;
    tp_error error = {{0}};

    assert_invalid_unchanged(
        gui_edit_submit_result(
            &state, false, TP_STATUS_OK, true,
            false, true, 8, 8, &error),
        &error, &before, &state);
}

/* USA-11 partial: OK/no_change terminates its draft without waiting for an
 * event. Limit: the committed-WITH-change OK half still waits for the echo
 * (gui_edit_submit_result), so "no event needed" holds only for no_change. */
void test_submit_result_resolves_success_no_change_and_observed_echo(void) {
    tp_error error = {{0}};
    gui_edit_state state = submitting_state();

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_submit_result(
            &state, true, TP_STATUS_OK, false,
            true, false, 7, 7, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, state.phase);

    state = submitting_state();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_submit_result(
            &state, true, TP_STATUS_OK, true,
            false, true, 8, 8, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, state.phase);
}

void test_submit_result_keeps_pending_commit_or_restores_rejected_draft(void) {
    tp_error error = {{0}};
    gui_edit_state state = submitting_state();

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_submit_result(
            &state, true, TP_STATUS_OK, true,
            false, false, 9, 8, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_SUBMITTING, state.phase);
    TEST_ASSERT_EQUAL_INT64(9, state.submitted_revision);

    state = submitting_state();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_submit_result(
            &state, true, TP_STATUS_INVALID_ARGUMENT,
            false, false, false, 0, 7, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_EDITING, state.phase);
    TEST_ASSERT_EQUAL_INT64(0, state.submitted_revision);
    TEST_ASSERT_EQUAL_STRING(
        "", state.submitted_transaction_id);

    state = submitting_state();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_submit_result(
            &state, true, TP_STATUS_OOM,
            false, false, false, 0, 7, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, state.phase);
    TEST_ASSERT_EQUAL_INT64(
        0, state.submitted_revision);
    TEST_ASSERT_EQUAL_STRING(
        "", state.submitted_transaction_id);

    state = submitting_state();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_submit_result(
            &state, true, TP_STATUS_INVALID_ARGUMENT,
            false, false, false, 0, 8, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_CONFLICTED, state.phase);
}

/* USA-17 partial: the v1 rule is revision plus exact receipt and never
 * inspects the field, so same-field and different-field commits share it;
 * there is no explicit paired same/different-field case. */
void test_model_revision_ignores_stale_resolves_exact_and_conflicts_false_exact(void) {
    tp_error error = {{0}};
    gui_edit_state state = submitting_state();
    const gui_edit_state before = state;

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_model_revision(
            &state, 7, false, false, &error));
    TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof state);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_model_revision(
            &state, 8, true, true, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, state.phase);

    state = submitting_state();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_model_revision(
            &state, 8, false, false, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_CONFLICTED, state.phase);
    TEST_ASSERT_FALSE(state.target_present);
    TEST_ASSERT_TRUE(
        tp_id128_eq(test_id(0x11U), state.target_id));
}

/* USA-18: two view ids -- the same commit resolves the owner and conflicts the
 * other view. Spec §16 pre-authorizes this reducer-level proof while production
 * has a single draft owner (§13), so it is full, not partial, coverage. */
void test_same_commit_resolves_only_the_view_with_the_exact_receipt(void) {
    tp_error error = {{0}};
    gui_edit_state owner;
    gui_edit_state observer;

    gui_edit_state_init(&owner, test_id(0x21U));
    gui_edit_state_init(&observer, test_id(0x22U));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_begin(
            &owner, test_id(0x11U), 7,
            test_id(0x31U), &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_begin(
            &observer, test_id(0x11U), 7,
            test_id(0x32U), &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_commit(
            &owner, false, TX_ONE, &error));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_model_revision(
            &owner, 8, true, true, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, owner.phase);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_model_revision(
            &observer, 8, true, false, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, observer.phase);
    TEST_ASSERT_TRUE(
        tp_id128_eq(test_id(0x11U), observer.target_id));
    TEST_ASSERT_TRUE(
        tp_id128_eq(
            test_id(0x32U), observer.draft_instance_id));
}

void test_resync_conflicts_only_changed_revision_or_resolves_exact_terminal(void) {
    tp_error error = {{0}};
    gui_edit_state state = editing_state();
    const gui_edit_state before = state;

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_resync(
            &state, 7, true, false, &error));
    TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof state);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_resync(
            &state, 8, false, false, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_CONFLICTED, state.phase);
    TEST_ASSERT_FALSE(state.target_present);

    state = submitting_state();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_resync(
            &state, 7, true, true, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, state.phase);
}

void test_apply_mine_rejects_deleted_target_and_bad_submit_byte_identically(void) {
    tp_error error = {{0}};
    gui_edit_state state = conflicted_state(false);
    gui_edit_state before = state;

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_NOT_FOUND,
        gui_edit_apply_mine(
            &state, 8, false, TX_TWO, &error));
    TEST_ASSERT_NOT_EQUAL(0, (int)strlen(error.msg));
    TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof state);

    memset(&error, 0, sizeof error);
    assert_invalid_unchanged(
        gui_edit_apply_mine(
            &state, 8, true, "bad", &error),
        &error, &before, &state);
}

void test_apply_mine_submits_once_and_later_foreign_revision_conflicts(void) {
    tp_error error = {{0}};
    gui_edit_state state = conflicted_state(true);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_apply_mine(
            &state, 8, true, TX_TWO, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_SUBMITTING, state.phase);
    TEST_ASSERT_EQUAL_INT64(8, state.base_revision);
    TEST_ASSERT_EQUAL_INT64(0, state.submitted_revision);
    TEST_ASSERT_EQUAL_STRING(
        TX_TWO, state.submitted_transaction_id);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_edit_model_revision(
            &state, 9, true, false, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_CONFLICTED, state.phase);
}

void test_discard_clears_edit_and_conflict_but_rejects_uncertain_submit(void) {
    tp_error error = {{0}};
    gui_edit_state state = editing_state();

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, gui_edit_discard(&state, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, state.phase);
    TEST_ASSERT_TRUE(
        tp_id128_eq(test_id(0x21U), state.view_id));

    state = conflicted_state(false);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, gui_edit_discard(&state, &error));
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, state.phase);

    state = submitting_state();
    const gui_edit_state before = state;
    memset(&error, 0, sizeof error);
    assert_invalid_unchanged(
        gui_edit_discard(&state, &error),
        &error, &before, &state);
}

void test_null_state_is_a_structured_invalid_input(void) {
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_edit_discard(NULL, &error));
    TEST_ASSERT_NOT_EQUAL(0, (int)strlen(error.msg));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(
        test_begin_records_only_lifecycle_and_stable_identity);
    RUN_TEST(
        test_begin_rejects_invalid_identity_revision_and_second_draft);
    RUN_TEST(
        test_commit_net_zero_finishes_and_valid_submit_records_receipt_identity);
    RUN_TEST(
        test_commit_rejects_wrong_phase_and_bad_transaction_without_mutation);
    RUN_TEST(
        test_submit_result_requires_normalized_exact_owner);
    RUN_TEST(
        test_submit_result_resolves_success_no_change_and_observed_echo);
    RUN_TEST(
        test_submit_result_keeps_pending_commit_or_restores_rejected_draft);
    RUN_TEST(
        test_model_revision_ignores_stale_resolves_exact_and_conflicts_false_exact);
    RUN_TEST(
        test_same_commit_resolves_only_the_view_with_the_exact_receipt);
    RUN_TEST(
        test_resync_conflicts_only_changed_revision_or_resolves_exact_terminal);
    RUN_TEST(
        test_apply_mine_rejects_deleted_target_and_bad_submit_byte_identically);
    RUN_TEST(
        test_apply_mine_submits_once_and_later_foreign_revision_conflicts);
    RUN_TEST(
        test_discard_clears_edit_and_conflict_but_rejects_uncertain_submit);
    RUN_TEST(test_null_state_is_a_structured_invalid_input);
    return UNITY_END();
}
