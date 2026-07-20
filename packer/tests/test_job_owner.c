#include <stdatomic.h>
#include <string.h>

#include "tinycthread.h"

#include "tp_core/tp_identity.h"
#include "tp_core/tp_session.h"
#include "tp_job_owner_internal.h"
#include "unity.h"

typedef struct fake_job {
    tp_session_owned_job owner;
    _Atomic bool acquired;
    _Atomic bool acquire_ok;
    _Atomic bool proceed;
    _Atomic int touched;
    _Atomic int destroyed;
} fake_job;

typedef struct worker_args {
    tp_session *session;
    fake_job *job;
} worker_args;

void setUp(void) {}
void tearDown(void) {}

static int fill_rng(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    memset(out, 0x5A, len);
    return (int)len;
}

static void fake_cancel(tp_session_owned_job *owner) { (void)owner; }

static void fake_destroy(tp_session_owned_job *owner) {
    fake_job *job = (fake_job *)owner;
    atomic_fetch_add_explicit(&job->destroyed, 1, memory_order_relaxed);
}

static int pinned_worker(void *context) {
    worker_args *args = context;
    tp_session_owned_job *owner =
        tp_session_job_acquire_internal(args->session);
    atomic_store_explicit(&args->job->acquire_ok,
                          owner == &args->job->owner,
                          memory_order_relaxed);
    atomic_store_explicit(&args->job->acquired, true, memory_order_release);
    while (!atomic_load_explicit(&args->job->proceed, memory_order_acquire)) {
        thrd_yield();
    }
    atomic_fetch_add_explicit(&args->job->touched, 1, memory_order_relaxed);
    if (owner) {
        tp_session_job_release_internal(owner);
    }
    return 0;
}

void test_detach_cannot_destroy_a_concurrently_pinned_job(void) {
    tp_rng rng = {fill_rng, NULL};
    tp_session *session = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create(&rng, &session, &error));

    fake_job job;
    memset(&job, 0, sizeof job);
    atomic_init(&job.acquired, false);
    atomic_init(&job.acquire_ok, false);
    atomic_init(&job.proceed, false);
    atomic_init(&job.touched, 0);
    atomic_init(&job.destroyed, 0);
    tp_session_owned_job_init(&job.owner, fake_cancel, fake_destroy);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_attach_internal(session, &job.owner,
                                                         &error));

    worker_args args = {session, &job};
    thrd_t thread;
    TEST_ASSERT_EQUAL_INT(thrd_success,
                          thrd_create(&thread, pinned_worker, &args));
    while (!atomic_load_explicit(&job.acquired, memory_order_acquire)) {
        thrd_yield();
    }

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_detach_internal(session, &job.owner,
                                                         &error));
    tp_session_job_release_internal(&job.owner); /* session ownership */
    TEST_ASSERT_EQUAL_INT(0, atomic_load_explicit(&job.destroyed,
                                                  memory_order_relaxed));
    atomic_store_explicit(&job.proceed, true, memory_order_release);
    TEST_ASSERT_EQUAL_INT(thrd_success, thrd_join(thread, NULL));
    TEST_ASSERT_EQUAL_INT(1, atomic_load_explicit(&job.touched,
                                                  memory_order_relaxed));
    TEST_ASSERT_TRUE(atomic_load_explicit(&job.acquire_ok,
                                          memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT(1, atomic_load_explicit(&job.destroyed,
                                                  memory_order_relaxed));
    tp_session_destroy(session);
}

void test_discarded_session_rejects_new_job_ownership(void) {
    tp_rng rng = {fill_rng, NULL};
    tp_session *session = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create(&rng, &session, &error));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_discard(session, &error));

    fake_job job;
    memset(&job, 0, sizeof job);
    atomic_init(&job.destroyed, 0);
    tp_session_owned_job_init(&job.owner, fake_cancel, fake_destroy);

    const tp_status status =
        tp_session_job_attach_internal(session, &job.owner, &error);
    if (status == TP_STATUS_OK) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_detach_internal(
                                  session, &job.owner, &error));
    }
    tp_session_job_release_internal(&job.owner);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, status);
    TEST_ASSERT_EQUAL_INT(1, atomic_load_explicit(&job.destroyed,
                                                  memory_order_relaxed));
    tp_session_destroy(session);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_detach_cannot_destroy_a_concurrently_pinned_job);
    RUN_TEST(test_discarded_session_rejects_new_job_ownership);
    return UNITY_END();
}
