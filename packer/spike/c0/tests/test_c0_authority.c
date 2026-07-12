/* C0-03 task 3: session-authority vocabulary + the permission predicate table.
 * Pins the tokens and the full (state x capability) matrix -- the executable form
 * of C0-03-contract.md §3. No OS claim/cutover is modelled (open per §60). */

#include "tp_c0/tp_c0_authority.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_state_tokens_pinned(void) {
    TEST_ASSERT_EQUAL_STRING("owner", tp_c0_authority_id(TP_C0_AUTH_OWNER));
    TEST_ASSERT_EQUAL_STRING("observer", tp_c0_authority_id(TP_C0_AUTH_OBSERVER));
    TEST_ASSERT_EQUAL_STRING("transfer_pending", tp_c0_authority_id(TP_C0_AUTH_TRANSFER_PENDING));
    TEST_ASSERT_EQUAL_STRING("released", tp_c0_authority_id(TP_C0_AUTH_RELEASED));
}

void test_cap_tokens_pinned(void) {
    TEST_ASSERT_EQUAL_STRING("apply", tp_c0_authority_cap_id(TP_C0_AUTH_CAP_APPLY));
    TEST_ASSERT_EQUAL_STRING("publish", tp_c0_authority_cap_id(TP_C0_AUTH_CAP_PUBLISH));
    TEST_ASSERT_EQUAL_STRING("pack", tp_c0_authority_cap_id(TP_C0_AUTH_CAP_PACK));
}

/* The full predicate table. OWNER permits all three; every other state permits
 * none -- singular authority: at most one host accepts writes/publish/pack. */
void test_permission_matrix(void) {
    typedef struct {
        tp_c0_authority_state s;
        bool apply, publish, pack;
    } row;
    const row table[] = {
        {TP_C0_AUTH_OWNER, true, true, true},
        {TP_C0_AUTH_OBSERVER, false, false, false},
        {TP_C0_AUTH_TRANSFER_PENDING, false, false, false},
        {TP_C0_AUTH_RELEASED, false, false, false},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        const row *r = &table[i];
        const char *name = tp_c0_authority_id(r->s);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->apply, tp_c0_authority_permits(r->s, TP_C0_AUTH_CAP_APPLY), name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->publish, tp_c0_authority_permits(r->s, TP_C0_AUTH_CAP_PUBLISH), name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(r->pack, tp_c0_authority_permits(r->s, TP_C0_AUTH_CAP_PACK), name);
    }
}

/* Exactly one state is authoritative (accepts writes). */
void test_single_authoritative_state(void) {
    int authoritative = 0;
    for (int s = 0; s < TP_C0_AUTH_STATE_COUNT; s++) {
        if (tp_c0_authority_is_authoritative((tp_c0_authority_state)s)) {
            authoritative++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, authoritative);
    TEST_ASSERT_TRUE(tp_c0_authority_is_authoritative(TP_C0_AUTH_OWNER));
}

/* Out-of-range capability never aborts; returns false. */
void test_out_of_range_cap(void) {
    TEST_ASSERT_FALSE(tp_c0_authority_permits(TP_C0_AUTH_OWNER, TP_C0_AUTH_CAP_COUNT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_state_tokens_pinned);
    RUN_TEST(test_cap_tokens_pinned);
    RUN_TEST(test_permission_matrix);
    RUN_TEST(test_single_authoritative_state);
    RUN_TEST(test_out_of_range_cap);
    return UNITY_END();
}
