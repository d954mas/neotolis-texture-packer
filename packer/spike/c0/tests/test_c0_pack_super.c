/* C0-03 task 5: Pack supersession / preview-selection state machine. Encodes
 * decision docs/decisions/0004-pack-supersession.md as state-table fixtures --
 * one running job + one latest intent, superseded jobs never become the
 * authoritative preview, every success enters the cache, selection is by hash
 * not by timing, and ownership transfer cancels only the running Pack. The
 * executable form of C0-03-contract.md §5. */

#include "tp_c0/tp_c0_pack_super.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static tp_c0_id128 h(int i) {
    tp_c0_id128 id = tp_c0_id128_nil();
    id.bytes[0] = (uint8_t)(0xA0 + i);
    id.bytes[15] = 0x5A;
    return id;
}

/* Distinct 128-bit hash for large sweeps (two-byte counter, always non-nil). */
static tp_c0_id128 hn(int i) {
    tp_c0_id128 id = tp_c0_id128_nil();
    id.bytes[0] = (uint8_t)(i & 0xFF);
    id.bytes[1] = (uint8_t)((i >> 8) & 0xFF);
    id.bytes[15] = 0x5A;
    return id;
}

void test_outcome_tokens_pinned(void) {
    TEST_ASSERT_EQUAL_STRING("started", tp_c0_pack_outcome_id(TP_C0_PACK_STARTED));
    TEST_ASSERT_EQUAL_STRING("queued", tp_c0_pack_outcome_id(TP_C0_PACK_QUEUED));
    TEST_ASSERT_EQUAL_STRING("became_preview", tp_c0_pack_outcome_id(TP_C0_PACK_BECAME_PREVIEW));
    TEST_ASSERT_EQUAL_STRING("superseded", tp_c0_pack_outcome_id(TP_C0_PACK_SUPERSEDED));
    TEST_ASSERT_EQUAL_STRING("selected", tp_c0_pack_outcome_id(TP_C0_PACK_SELECTED));
    TEST_ASSERT_EQUAL_STRING("miss", tp_c0_pack_outcome_id(TP_C0_PACK_MISS));
    TEST_ASSERT_EQUAL_STRING("cancelled", tp_c0_pack_outcome_id(TP_C0_PACK_CANCELLED));
    TEST_ASSERT_EQUAL_STRING("noop", tp_c0_pack_outcome_id(TP_C0_PACK_NOOP));
}

/* One running job + single latest intent: a request during a running Pack
 * QUEUEs (replaces the pending intent), never spawns a parallel job. */
void test_request_starts_then_queues(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_STARTED, tp_c0_pack_super_request(&s, h(1)));
    TEST_ASSERT_TRUE(s.has_running);
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_QUEUED, tp_c0_pack_super_request(&s, h(2)));
    TEST_ASSERT_TRUE(s.has_running); /* still ONE running job */
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.running_hash, h(1)));
    TEST_ASSERT_TRUE(s.has_pending);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.pending_hash, h(2)));
}

/* A newer request replaces the pending intent; the replaced one never runs. */
void test_pending_intent_is_replaced_not_parallel(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    tp_c0_pack_super_request(&s, h(1)); /* running A */
    tp_c0_pack_super_request(&s, h(2)); /* pending B */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_QUEUED, tp_c0_pack_super_request(&s, h(3))); /* pending C replaces B */
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.pending_hash, h(3)));
    /* Complete A (superseded), C promoted and completes -> preview C. B never ran. */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_SUPERSEDED, tp_c0_pack_super_complete(&s));
    TEST_ASSERT_TRUE(s.has_running);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.running_hash, h(3)));
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_BECAME_PREVIEW, tp_c0_pack_super_complete(&s));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(3)));
    TEST_ASSERT_TRUE(tp_c0_pack_super_in_cache(&s, h(1)));  /* A: cached (ran) */
    TEST_ASSERT_TRUE(tp_c0_pack_super_in_cache(&s, h(3)));  /* C: cached + preview */
    TEST_ASSERT_FALSE(tp_c0_pack_super_in_cache(&s, h(2))); /* B: replaced, never ran */
}

/* Full supersession: the earlier job enters the cache but does NOT become the
 * authoritative preview; the newer job does. */
void test_superseded_job_not_authoritative(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    tp_c0_pack_super_request(&s, h(1)); /* running A (seq1) */
    tp_c0_pack_super_request(&s, h(2)); /* pending B (seq2), latest=2 */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_SUPERSEDED, tp_c0_pack_super_complete(&s)); /* A: seq1 < 2 */
    TEST_ASSERT_FALSE(s.has_preview);                                           /* A did NOT become preview */
    TEST_ASSERT_TRUE(tp_c0_pack_super_in_cache(&s, h(1)));                       /* but it IS cached */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_BECAME_PREVIEW, tp_c0_pack_super_complete(&s)); /* B: seq2 == 2 */
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(2)));
}

/* A late-finishing superseded job must not overwrite the authoritative preview.
 * Construct the straggler directly: a running job whose request seq is older
 * than the latest, while a newer preview is already set. */
void test_late_superseded_does_not_overwrite_preview(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    s.latest_seq = 5;
    s.has_preview = true;
    s.preview_hash = h(2); /* authoritative preview from the newer request */
    s.has_running = true;
    s.running_seq = 2; /* older than latest (5) -> superseded */
    s.running_hash = h(1);
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_SUPERSEDED, tp_c0_pack_super_complete(&s));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(2))); /* preview UNCHANGED */
    TEST_ASSERT_TRUE(tp_c0_pack_super_in_cache(&s, h(1)));  /* straggler still cached */
}

/* Explicit selection and Undo cache-hit pick BY hash, not by completion time. */
void test_selection_by_hash_not_timing(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    tp_c0_pack_super_request(&s, h(1));                                             /* running A */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_BECAME_PREVIEW, tp_c0_pack_super_complete(&s)); /* preview A */
    tp_c0_pack_super_request(&s, h(2));                                             /* running B */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_BECAME_PREVIEW, tp_c0_pack_super_complete(&s)); /* preview B (later) */
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(2)));
    /* Explicitly select the EARLIER-completed A by hash (or Undo makes A current). */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_SELECTED, tp_c0_pack_super_select(&s, h(1)));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(1))); /* chosen by hash, ignoring timing */
}

/* Undo/selection cache MISS: preview unchanged (out of date), no auto-pack. */
void test_select_miss_marks_stale_no_autopack(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    tp_c0_pack_super_request(&s, h(1));
    tp_c0_pack_super_complete(&s); /* preview A, idle */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_MISS, tp_c0_pack_super_select(&s, h(7))); /* h(7) not cached */
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(1)));                    /* preview kept */
    TEST_ASSERT_FALSE(s.has_running);                                         /* no Pack auto-started */
    TEST_ASSERT_FALSE(tp_c0_pack_super_is_fresh(&s, h(7)));                    /* stale vs current h(7) */
}

/* Ownership transfer drops the session pack INTENT: it cancels the running Pack
 * AND the never-run pending intent; only preview + cache survive (§59 item 24,
 * F3). A stale pending must not resurrect as an unrequested Pack. */
void test_transfer_drops_running_and_pending(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    tp_c0_pack_super_request(&s, h(1));
    tp_c0_pack_super_complete(&s);      /* preview A, cached */
    tp_c0_pack_super_request(&s, h(2)); /* running B */
    tp_c0_pack_super_request(&s, h(3)); /* pending C */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_CANCELLED, tp_c0_pack_super_transfer(&s));
    TEST_ASSERT_FALSE(s.has_running);                      /* running B cancelled */
    TEST_ASSERT_FALSE(s.has_pending);                      /* pending C dropped (F3) */
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(1))); /* preview A preserved */
    TEST_ASSERT_TRUE(tp_c0_pack_super_in_cache(&s, h(1))); /* cache preserved */

    /* After transfer, a fresh request D starts a NEW job and completes as the
     * preview -- the dropped pending C must NOT reappear as a SUPERSEDED straggler. */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_STARTED, tp_c0_pack_super_request(&s, h(4)));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.running_hash, h(4)));
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_BECAME_PREVIEW, tp_c0_pack_super_complete(&s));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(4)));
    TEST_ASSERT_FALSE(s.has_running);                        /* no straggler promoted */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_NOOP, tp_c0_pack_super_complete(&s)); /* nothing left */
    TEST_ASSERT_FALSE(tp_c0_pack_super_in_cache(&s, h(3)));  /* C never ran, never cached */

    /* Transfer with nothing running is a no-op. */
    tp_c0_pack_super_init(&s);
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_NOOP, tp_c0_pack_super_transfer(&s));
}

/* An explicit user selection is STICKY: a job that completes while the preview is
 * user-pinned enters the cache but does NOT become the preview; a NEW request
 * clears the pin so its result IS adopted (F4, refinement of decision 0004). */
void test_explicit_selection_is_sticky(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    tp_c0_pack_super_request(&s, h(1)); /* running A */
    tp_c0_pack_super_complete(&s);      /* preview A */
    tp_c0_pack_super_request(&s, h(2)); /* running B (freshest intent) */
    /* User explicitly selects the older cached A while B still runs. */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_SELECTED, tp_c0_pack_super_select(&s, h(1)));
    TEST_ASSERT_TRUE(s.preview_is_explicit);
    /* B completes: it IS the freshest intent, but the preview is user-pinned ->
     * cached only, preview stays A. */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_SUPERSEDED, tp_c0_pack_super_complete(&s));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(1))); /* still A */
    TEST_ASSERT_TRUE(tp_c0_pack_super_in_cache(&s, h(2)));  /* B cached, not preview */
    /* A NEW request clears the pin; its result is wanted -> becomes the preview. */
    tp_c0_pack_super_request(&s, h(3)); /* running C */
    TEST_ASSERT_FALSE(s.preview_is_explicit);
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_BECAME_PREVIEW, tp_c0_pack_super_complete(&s));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, h(3))); /* preview := C */
}

/* Freshness derivation (§10.1): preview current iff preview_hash == current. */
void test_freshness(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    TEST_ASSERT_FALSE(tp_c0_pack_super_is_fresh(&s, h(1))); /* no preview */
    tp_c0_pack_super_request(&s, h(1));
    tp_c0_pack_super_complete(&s);
    TEST_ASSERT_TRUE(tp_c0_pack_super_is_fresh(&s, h(1)));
    TEST_ASSERT_FALSE(tp_c0_pack_super_is_fresh(&s, h(2))); /* current input changed */
}

/* F6: after more distinct packs than the done ring holds, the newest (current)
 * preview is still a cache member and selectable -- the ring evicts the OLDEST,
 * and the current preview is always reported present. */
void test_done_ring_keeps_newest_preview(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    int total = TP_C0_PACK_SUPER_MAX_DONE + 1; /* one more than the ring holds */
    for (int i = 0; i < total; i++) {
        TEST_ASSERT_EQUAL_INT(TP_C0_PACK_STARTED, tp_c0_pack_super_request(&s, hn(i)));
        TEST_ASSERT_EQUAL_INT(TP_C0_PACK_BECAME_PREVIEW, tp_c0_pack_super_complete(&s));
    }
    tp_c0_id128 newest = hn(total - 1);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(s.preview_hash, newest));
    TEST_ASSERT_TRUE(tp_c0_pack_super_in_cache(&s, newest));                 /* still a member */
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_SELECTED, tp_c0_pack_super_select(&s, newest)); /* selectable */
    TEST_ASSERT_FALSE(tp_c0_pack_super_in_cache(&s, hn(0)));                 /* oldest fell out */
}

/* complete() with no running job is a no-op, not an abort. */
void test_complete_noop_when_idle(void) {
    tp_c0_pack_super s;
    tp_c0_pack_super_init(&s);
    TEST_ASSERT_EQUAL_INT(TP_C0_PACK_NOOP, tp_c0_pack_super_complete(&s));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_outcome_tokens_pinned);
    RUN_TEST(test_request_starts_then_queues);
    RUN_TEST(test_pending_intent_is_replaced_not_parallel);
    RUN_TEST(test_superseded_job_not_authoritative);
    RUN_TEST(test_late_superseded_does_not_overwrite_preview);
    RUN_TEST(test_selection_by_hash_not_timing);
    RUN_TEST(test_select_miss_marks_stale_no_autopack);
    RUN_TEST(test_transfer_drops_running_and_pending);
    RUN_TEST(test_explicit_selection_is_sticky);
    RUN_TEST(test_freshness);
    RUN_TEST(test_done_ring_keeps_newest_preview);
    RUN_TEST(test_complete_noop_when_idle);
    return UNITY_END();
}
