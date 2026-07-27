#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_operation.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_session_internal.h"
#include "unity.h"

static int deterministic_fill(void *context, uint8_t *out, size_t length) {
    uint8_t *seed = context;
    for (size_t i = 0U; i < length; ++i) {
        out[i] = (uint8_t)(*seed + (uint8_t)i);
    }
    *seed = (uint8_t)(*seed + 17U);
    return (int)length;
}

static tp_session *make_session(void) {
    uint8_t seed = 1U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_session_create(&rng, &session, &err), err.msg);
    TEST_ASSERT_NOT_NULL(session);
    return session;
}

static tp_id128 first_atlas_id(tp_session *session) {
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &err), err.msg);
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 id = atlas->id;
    tp_session_snapshot_destroy(snapshot);
    return id;
}

static void apply_rename(tp_session *session, tp_id128 atlas_id,
                         const char *id, const char *name,
                         const char *label, const char *author,
                         bool expect_commit) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = atlas_id;
    const size_t name_size = strlen(name) + 1U;
    op.u.atlas_rename.name = malloc(name_size);
    TEST_ASSERT_NOT_NULL(op.u.atlas_rename.name);
    memcpy(op.u.atlas_rename.name, name, name_size);

    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, id, 33U);
    request.expected_revision = tp_session_revision(session);
    request.label = (char *)label;
    request.author = (char *)author;
    request.ops = &op;
    request.op_count = 1;

    tp_txn_result result;
    memset(&result, 0, sizeof result);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_apply(session, &request, &result, &err), err.msg);
    TEST_ASSERT_EQUAL(expect_commit, result.committed);
    TEST_ASSERT_EQUAL(!expect_commit, result.no_change);
    tp_txn_result_free(&result);
    tp_operation_free(&op);
}

static tp_session_observation *initial_observation(
    tp_session *session, tp_session_observation_token *token) {
    tp_error err = {{0}};
    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, NULL, &observation, &err), err.msg);
    TEST_ASSERT_NOT_NULL(observation);
    TEST_ASSERT_TRUE(
        tp_session_observation_resync_required(observation));
    TEST_ASSERT_NOT_NULL(tp_session_observation_snapshot(observation));
    *token = tp_session_observation_token_query(observation);
    return observation;
}

typedef struct injected_commit {
    tp_session *session;
    tp_id128 atlas_id;
} injected_commit;

static void inject_commit_after_cut(void *context) {
    injected_commit *injected = context;
    apply_rename(injected->session, injected->atlas_id,
                 "41000000000000000000000000000001", "after-cut",
                 "Rename after cut", "human", true);
}

void setUp(void) {
    tp_session__test_set_observe_after_cut(NULL, NULL);
    tp_session__test_reset_snapshot_allocations();
}

void tearDown(void) {
    tp_session__test_set_observe_after_cut(NULL, NULL);
    tp_session__test_reset_snapshot_allocations();
}

void test_observation_cut_remains_consistent_when_commit_follows_unlock(void) {
    tp_session *session = make_session();
    injected_commit injected = {session, first_atlas_id(session)};
    tp_session__test_set_observe_after_cut(inject_commit_after_cut, &injected);

    tp_session_observation *before = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, NULL, &before, &err), err.msg);
    TEST_ASSERT_NOT_NULL(before);
    const tp_session_observation_token cut =
        tp_session_observation_token_query(before);
    TEST_ASSERT_EQUAL_UINT64(0U, cut.event_sequence);
    TEST_ASSERT_EQUAL_INT64(
        0, tp_session_snapshot_revision(
               tp_session_observation_snapshot(before)));
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));

    tp_session_observation *after = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &cut, &after, &err), err.msg);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_FALSE(tp_session_observation_resync_required(after));
    TEST_ASSERT_EQUAL_size_t(1U,
        tp_session_observation_event_count(after));
    const tp_session_event *event =
        tp_session_observation_event_at(after, 0U);
    TEST_ASSERT_NOT_NULL(event);
    TEST_ASSERT_EQUAL_UINT64(1U, event->sequence);
    TEST_ASSERT_EQUAL_STRING(
        "41000000000000000000000000000001",
        event->transaction_id);
    TEST_ASSERT_EQUAL_STRING("Rename after cut", event->label);
    TEST_ASSERT_EQUAL_STRING("human", event->author);
    TEST_ASSERT_EQUAL_INT64(
        1, tp_session_snapshot_revision(
               tp_session_observation_snapshot(after)));

    tp_session_observation_destroy(after);
    tp_session_observation_destroy(before);
    tp_session_destroy(session);
}

void test_no_change_observe_is_null_and_allocation_free(void) {
    tp_session *session = make_session();
    tp_session_observation_token token;
    tp_session_observation *initial = initial_observation(session, &token);
    tp_session_observation_destroy(initial);
    const size_t allocations =
        tp_session__test_snapshot_allocation_count();

    tp_session_observation *unchanged =
        (tp_session_observation *)(uintptr_t)1U;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &token, &unchanged, &err), err.msg);
    TEST_ASSERT_NULL(unchanged);
    TEST_ASSERT_EQUAL_size_t(
        allocations, tp_session__test_snapshot_allocation_count());
    tp_session_destroy(session);
}

void test_source_only_delta_has_complete_event_without_project_materialization(void) {
    tp_session *session = make_session();
    tp_session_observation_token token;
    tp_session_observation *initial = initial_observation(session, &token);
    tp_session_observation_destroy(initial);
    tp_session__test_reset_snapshot_allocations();

    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_invalidate_sources(session, &err));
    tp_session_observation *delta = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &token, &delta, &err), err.msg);
    TEST_ASSERT_NOT_NULL(delta);
    TEST_ASSERT_NULL(tp_session_observation_snapshot(delta));
    TEST_ASSERT_EQUAL_size_t(1U,
        tp_session_observation_event_count(delta));
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_EVENT_SOURCE_RUNTIME_CHANGED,
        tp_session_observation_event_at(delta, 0U)->kind);
    TEST_ASSERT_EQUAL_size_t(
        0U, tp_session__test_snapshot_allocation_count());
    tp_session_observation_destroy(delta);
    tp_session_destroy(session);
}

void test_event_window_exact_edge_and_overflow_resync_are_complete(void) {
    tp_session *session = make_session();
    tp_error err = {{0}};
    for (size_t i = 0U; i < TP_SESSION_OBSERVATION_EVENT_CAPACITY + 1U;
         ++i) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK, tp_session_invalidate_sources(session, &err));
    }

    tp_session_observation_token edge = {
        .event_sequence = 1U,
        .source_runtime_generation = 1U,
    };
    tp_session_observation *delta = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &edge, &delta, &err), err.msg);
    TEST_ASSERT_NOT_NULL(delta);
    TEST_ASSERT_FALSE(tp_session_observation_resync_required(delta));
    TEST_ASSERT_EQUAL_size_t(
        TP_SESSION_OBSERVATION_EVENT_CAPACITY,
        tp_session_observation_event_count(delta));
    TEST_ASSERT_EQUAL_UINT64(
        2U, tp_session_observation_event_at(delta, 0U)->sequence);
    TEST_ASSERT_EQUAL_UINT64(
        65U,
        tp_session_observation_event_at(
            delta, TP_SESSION_OBSERVATION_EVENT_CAPACITY - 1U)->sequence);
    tp_session_observation_destroy(delta);

    tp_session_observation_token gap = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &gap, &delta, &err), err.msg);
    TEST_ASSERT_TRUE(tp_session_observation_resync_required(delta));
    TEST_ASSERT_EQUAL_size_t(
        0U, tp_session_observation_event_count(delta));
    TEST_ASSERT_NOT_NULL(tp_session_observation_snapshot(delta));
    tp_session_observation_destroy(delta);

    tp_session_observation_token future = {
        .event_sequence = 66U,
        .source_runtime_generation = 65U,
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &future, &delta, &err), err.msg);
    TEST_ASSERT_TRUE(tp_session_observation_resync_required(delta));
    TEST_ASSERT_NOT_NULL(tp_session_observation_snapshot(delta));
    tp_session_observation_destroy(delta);
    tp_session_destroy(session);
}

void test_observation_oom_preserves_caller_token_and_is_retryable(void) {
    tp_session *session = make_session();
    const tp_id128 atlas_id = first_atlas_id(session);
    tp_session_observation_token token;
    tp_session_observation *initial = initial_observation(session, &token);
    tp_session_observation_destroy(initial);
    apply_rename(session, atlas_id,
                 "41000000000000000000000000000002", "oom-retry",
                 "Retry rename", "human", true);

    tp_session__test_fail_snapshot_allocation_after(0U);
    tp_session_observation *delta =
        (tp_session_observation *)(uintptr_t)1U;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        tp_session_observe(session, &token, &delta, &err));
    TEST_ASSERT_NULL(delta);
    TEST_ASSERT_EQUAL_UINT64(0U, token.event_sequence);

    memset(&err, 0, sizeof err);
    tp_session__test_fail_next_generation_owner_allocation();
    delta = (tp_session_observation *)(uintptr_t)1U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        tp_session_observe(session, &token, &delta, &err));
    TEST_ASSERT_NULL(delta);
    TEST_ASSERT_EQUAL_UINT64(0U, token.event_sequence);

    memset(&err, 0, sizeof err);
    tp_session__test_fail_snapshot_allocation_after(1U);
    delta = (tp_session_observation *)(uintptr_t)1U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        tp_session_observe(session, &token, &delta, &err));
    TEST_ASSERT_NULL(delta);
    TEST_ASSERT_EQUAL_UINT64(0U, token.event_sequence);

    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &token, &delta, &err), err.msg);
    TEST_ASSERT_NOT_NULL(delta);
    TEST_ASSERT_EQUAL_UINT64(
        1U, tp_session_observation_token_query(delta).event_sequence);
    tp_session_observation_destroy(delta);
    tp_session_destroy(session);
}

void test_recovery_only_change_advances_token_without_project_materialization(void) {
    tp_session *session = make_session();
    tp_session_observation_token token;
    tp_session_observation *initial = initial_observation(session, &token);
    TEST_ASSERT_TRUE(
        tp_session_observation_recovery_health(initial).available);
    tp_session_observation_destroy(initial);
    tp_session__test_reset_snapshot_allocations();

    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_require_recovery(session, &err));

    tp_session_observation *delta = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &token, &delta, &err), err.msg);
    TEST_ASSERT_NOT_NULL(delta);
    const tp_session_observation_token advanced =
        tp_session_observation_token_query(delta);
    TEST_ASSERT_EQUAL_UINT64(
        token.event_sequence, advanced.event_sequence);
    TEST_ASSERT_EQUAL_UINT64(
        token.recovery_health_generation,
        advanced.recovery_health_generation);
    TEST_ASSERT_EQUAL_UINT64(
        token.recovery_owner_generation + 1U,
        advanced.recovery_owner_generation);
    TEST_ASSERT_EQUAL_size_t(
        0U, tp_session_observation_event_count(delta));
    TEST_ASSERT_NULL(tp_session_observation_snapshot(delta));
    TEST_ASSERT_FALSE(
        tp_session_observation_recovery_health(delta).available);
    TEST_ASSERT_EQUAL_size_t(
        0U, tp_session__test_snapshot_allocation_count());
    tp_session_observation_destroy(delta);

    tp_session_observation *unchanged =
        (tp_session_observation *)(uintptr_t)1U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &advanced, &unchanged, &err), err.msg);
    TEST_ASSERT_NULL(unchanged);
    tp_session_destroy(session);
}

void test_terminal_no_change_publishes_no_event(void) {
    tp_session *session = make_session();
    tp_session_observation_token token;
    tp_session_observation *initial = initial_observation(session, &token);
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(
        tp_session_observation_snapshot(initial), 0);
    TEST_ASSERT_NOT_NULL(atlas);
    char name[128];
    (void)snprintf(name, sizeof name, "%s", atlas->name);
    const tp_id128 atlas_id = atlas->id;
    tp_session_observation_destroy(initial);

    apply_rename(session, atlas_id,
                 "41000000000000000000000000000003", name,
                 "No-op rename", "human", false);
    tp_session_observation *delta =
        (tp_session_observation *)(uintptr_t)1U;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &token, &delta, &err), err.msg);
    TEST_ASSERT_NULL(delta);
    TEST_ASSERT_EQUAL_UINT64(0U, tp_session_event_sequence(session));
    tp_session_destroy(session);
}

/* The observation token carries counters, not a session identity: it is
 * deliberately comparable only against the session that minted it. This test
 * pins BOTH halves of that contract.
 *
 * 1. A token carried over from a session that did real work is never silently
 *    accepted by a fresh session. Its counters sit in the fresh session's
 *    FUTURE, so token_is_future fires and the caller gets a complete
 *    resync-required observation with a snapshot -- not a no-op.
 * 2. The residual hazard is documented, not hidden: two sessions that are BOTH
 *    untouched mint equal (all-zero) tokens, so a token crossed between them
 *    reads as "nothing changed" and returns *out == NULL. That is safe only
 *    because the host never crosses instances -- gui_session_client_prepare
 *    observes every attachment candidate with after == NULL (asserted in
 *    apps/gui/test_gui_session_client.c). Encoding a session id into the token
 *    would trade a host-owned rule for a wider DTO and an extra comparison on
 *    every poll; the rule is cheaper and already enforced. */
void test_token_from_another_session_never_reads_as_a_stale_no_op(void) {
    tp_session *donor = make_session();
    const tp_id128 donor_atlas = first_atlas_id(donor);
    apply_rename(donor, donor_atlas,
                 "41000000000000000000000000000004", "donor-edit",
                 "Donor rename", "human", true);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_invalidate_sources(donor, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_require_recovery(donor, &err));
    tp_session_observation_token donor_token;
    tp_session_observation *donor_observation =
        initial_observation(donor, &donor_token);
    tp_session_observation_destroy(donor_observation);
    TEST_ASSERT_GREATER_THAN_UINT64(0U, donor_token.event_sequence);
    tp_session_destroy(donor);

    tp_session *fresh = make_session();
    tp_session_observation *crossed =
        (tp_session_observation *)(uintptr_t)1U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(fresh, &donor_token, &crossed, &err), err.msg);
    TEST_ASSERT_NOT_NULL(crossed);
    TEST_ASSERT_TRUE(tp_session_observation_resync_required(crossed));
    TEST_ASSERT_NOT_NULL(tp_session_observation_snapshot(crossed));
    TEST_ASSERT_EQUAL_size_t(
        0U, tp_session_observation_event_count(crossed));
    /* The published token is the fresh session's own cut, never the donor's. */
    const tp_session_observation_token adopted =
        tp_session_observation_token_query(crossed);
    TEST_ASSERT_EQUAL_UINT64(0U, adopted.event_sequence);
    TEST_ASSERT_EQUAL_UINT64(0U, adopted.recovery_owner_generation);
    tp_session_observation_destroy(crossed);

    /* Documented residual: untouched-to-untouched is indistinguishable. */
    tp_session *untouched_donor = make_session();
    tp_session_observation_token untouched_token;
    tp_session_observation *untouched_observation =
        initial_observation(untouched_donor, &untouched_token);
    tp_session_observation_destroy(untouched_observation);
    tp_session_destroy(untouched_donor);
    tp_session *untouched_fresh = make_session();
    tp_session_observation *silent =
        (tp_session_observation *)(uintptr_t)1U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(untouched_fresh, &untouched_token, &silent, &err),
        err.msg);
    TEST_ASSERT_NULL(silent);

    tp_session_destroy(untouched_fresh);
    tp_session_destroy(fresh);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(
        test_observation_cut_remains_consistent_when_commit_follows_unlock);
    RUN_TEST(test_no_change_observe_is_null_and_allocation_free);
    RUN_TEST(
        test_source_only_delta_has_complete_event_without_project_materialization);
    RUN_TEST(test_event_window_exact_edge_and_overflow_resync_are_complete);
    RUN_TEST(test_observation_oom_preserves_caller_token_and_is_retryable);
    RUN_TEST(
        test_recovery_only_change_advances_token_without_project_materialization);
    RUN_TEST(test_terminal_no_change_publishes_no_event);
    RUN_TEST(test_token_from_another_session_never_reads_as_a_stale_no_op);
    return UNITY_END();
}
