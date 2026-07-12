/* C0-03 task 2: the commit acknowledgement boundary. Pins the ordering
 * apply -> append -> publish, rollback-on-append-failure, and the invariant that
 * PUBLISH is reachable ONLY from JOURNALED. The executable form of
 * C0-03-contract.md §2. */

#include "tp_c0/tp_c0_ack.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- token pins ---------------------------------------------------------- */

void test_phase_tokens_pinned(void) {
    TEST_ASSERT_EQUAL_STRING("received", tp_c0_ack_phase_id(TP_C0_ACK_RECEIVED));
    TEST_ASSERT_EQUAL_STRING("validated", tp_c0_ack_phase_id(TP_C0_ACK_VALIDATED));
    TEST_ASSERT_EQUAL_STRING("applied", tp_c0_ack_phase_id(TP_C0_ACK_APPLIED));
    TEST_ASSERT_EQUAL_STRING("journaled", tp_c0_ack_phase_id(TP_C0_ACK_JOURNALED));
    TEST_ASSERT_EQUAL_STRING("published", tp_c0_ack_phase_id(TP_C0_ACK_PUBLISHED));
    TEST_ASSERT_EQUAL_STRING("rejected", tp_c0_ack_phase_id(TP_C0_ACK_REJECTED));
    TEST_ASSERT_EQUAL_STRING("rolled_back", tp_c0_ack_phase_id(TP_C0_ACK_ROLLED_BACK));
}

void test_event_tokens_pinned(void) {
    TEST_ASSERT_EQUAL_STRING("validate_ok", tp_c0_ack_event_id(TP_C0_ACK_EV_VALIDATE_OK));
    TEST_ASSERT_EQUAL_STRING("validate_fail", tp_c0_ack_event_id(TP_C0_ACK_EV_VALIDATE_FAIL));
    TEST_ASSERT_EQUAL_STRING("apply_ok", tp_c0_ack_event_id(TP_C0_ACK_EV_APPLY_OK));
    TEST_ASSERT_EQUAL_STRING("apply_fail", tp_c0_ack_event_id(TP_C0_ACK_EV_APPLY_FAIL));
    TEST_ASSERT_EQUAL_STRING("append_ok", tp_c0_ack_event_id(TP_C0_ACK_EV_APPEND_OK));
    TEST_ASSERT_EQUAL_STRING("append_fail", tp_c0_ack_event_id(TP_C0_ACK_EV_APPEND_FAIL));
    TEST_ASSERT_EQUAL_STRING("publish", tp_c0_ack_event_id(TP_C0_ACK_EV_PUBLISH));
}

/* ---- fixture table: {event sequence} -> {phase, published?, rolled_back?} -- */

typedef struct {
    const char *name;
    tp_c0_ack_event seq[8];
    int n;
    tp_c0_ack_phase expect_phase;
    bool published;
    bool rolled_back;
    bool legal; /* whether every event was legal from the running phase */
} ack_case;

void test_commit_pipeline_table(void) {
    const ack_case cases[] = {
        {"commit",
         {TP_C0_ACK_EV_VALIDATE_OK, TP_C0_ACK_EV_APPLY_OK, TP_C0_ACK_EV_APPEND_OK, TP_C0_ACK_EV_PUBLISH},
         4,
         TP_C0_ACK_PUBLISHED,
         true,
         false,
         true},
        {"append_fail_rolls_back",
         {TP_C0_ACK_EV_VALIDATE_OK, TP_C0_ACK_EV_APPLY_OK, TP_C0_ACK_EV_APPEND_FAIL},
         3,
         TP_C0_ACK_ROLLED_BACK,
         false,
         true,
         true},
        {"validate_fail_rejects", {TP_C0_ACK_EV_VALIDATE_FAIL}, 1, TP_C0_ACK_REJECTED, false, false, true},
        {"apply_fail_rejects",
         {TP_C0_ACK_EV_VALIDATE_OK, TP_C0_ACK_EV_APPLY_FAIL},
         2,
         TP_C0_ACK_REJECTED,
         false,
         false,
         true},
        /* Publishing before the journal append is ILLEGAL: the machine refuses to
         * advance, so nothing is published and the phase stays APPLIED. */
        {"publish_before_journal_illegal",
         {TP_C0_ACK_EV_VALIDATE_OK, TP_C0_ACK_EV_APPLY_OK, TP_C0_ACK_EV_PUBLISH},
         3,
         TP_C0_ACK_APPLIED,
         false,
         false,
         false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const ack_case *c = &cases[i];
        bool ok = false;
        tp_c0_ack_phase p = tp_c0_ack_run(c->seq, c->n, &ok);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->expect_phase, p, c->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->published, tp_c0_ack_is_published(p), c->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->rolled_back, tp_c0_ack_is_rolled_back(p), c->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->legal, ok, c->name);
        /* Revision is committed (retained/visible) iff published. */
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->published, tp_c0_ack_revision_committed(p), c->name);
    }
}

/* ---- load-bearing invariant: PUBLISHED only from JOURNALED + PUBLISH ------ */

void test_publish_only_reachable_after_journaled(void) {
    for (int p = 0; p < TP_C0_ACK_PHASE_COUNT; p++) {
        for (int e = 0; e < TP_C0_ACK_EV_COUNT; e++) {
            bool legal = false;
            tp_c0_ack_phase next = tp_c0_ack_next((tp_c0_ack_phase)p, (tp_c0_ack_event)e, &legal);
            if (legal) {
                /* The ONLY legal edge INTO published is JOURNALED + PUBLISH. */
                if (next == TP_C0_ACK_PUBLISHED) {
                    TEST_ASSERT_EQUAL_INT(TP_C0_ACK_JOURNALED, p);
                    TEST_ASSERT_EQUAL_INT(TP_C0_ACK_EV_PUBLISH, e);
                }
            } else {
                /* Illegal transitions never mutate the phase and never abort. */
                TEST_ASSERT_EQUAL_INT(p, next);
            }
        }
    }
}

/* Terminal phases accept no further events. */
void test_terminal_phases_are_stuck(void) {
    const tp_c0_ack_phase terms[] = {TP_C0_ACK_PUBLISHED, TP_C0_ACK_REJECTED, TP_C0_ACK_ROLLED_BACK};
    for (size_t i = 0; i < sizeof terms / sizeof terms[0]; i++) {
        TEST_ASSERT_TRUE(tp_c0_ack_is_terminal(terms[i]));
        for (int e = 0; e < TP_C0_ACK_EV_COUNT; e++) {
            bool legal = true;
            tp_c0_ack_phase next = tp_c0_ack_next(terms[i], (tp_c0_ack_event)e, &legal);
            TEST_ASSERT_FALSE(legal);
            TEST_ASSERT_EQUAL_INT(terms[i], next);
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase_tokens_pinned);
    RUN_TEST(test_event_tokens_pinned);
    RUN_TEST(test_commit_pipeline_table);
    RUN_TEST(test_publish_only_reachable_after_journaled);
    RUN_TEST(test_terminal_phases_are_stuck);
    return UNITY_END();
}
