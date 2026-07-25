#ifndef TP_TEST_GATE_H
#define TP_TEST_GATE_H

#include <stdbool.h>
#include <stdatomic.h>

typedef enum tp_test_gate_state {
    TP_TEST_GATE_IDLE = 0,
    TP_TEST_GATE_ARMED,
    TP_TEST_GATE_ENTERED,
    TP_TEST_GATE_RELEASING
} tp_test_gate_state;

typedef struct tp_test_gate {
    atomic_int state;
} tp_test_gate;

#define TP_TEST_GATE_INIT {TP_TEST_GATE_IDLE}

typedef void (*tp_test_gate_yield_fn)(void);

static inline void tp_test_gate_yield(tp_test_gate_yield_fn yield_fn) {
    if (yield_fn) {
        yield_fn();
    }
}

static inline void tp_test_gate_arm(tp_test_gate *gate,
                                    tp_test_gate_yield_fn yield_fn) {
    for (;;) {
        int expected = TP_TEST_GATE_IDLE;
        if (atomic_compare_exchange_weak(&gate->state, &expected,
                                         TP_TEST_GATE_ARMED)) {
            return;
        }
        /* A previous waiter owns RELEASING until it acknowledges the release.
         * Waiting here prevents a new arm from being overwritten by its final
         * IDLE store. Other states indicate overlapping test use and likewise
         * remain serialized instead of corrupting the gate. */
        tp_test_gate_yield(yield_fn);
    }
}

static inline bool tp_test_gate_entered(const tp_test_gate *gate) {
    return atomic_load(&gate->state) == TP_TEST_GATE_ENTERED;
}

static inline void tp_test_gate_release(tp_test_gate *gate,
                                        tp_test_gate_yield_fn yield_fn) {
    int observed = atomic_load(&gate->state);
    for (;;) {
        if (observed == TP_TEST_GATE_IDLE) {
            return;
        }
        if (observed == TP_TEST_GATE_RELEASING) {
            while (atomic_load(&gate->state) == TP_TEST_GATE_RELEASING) {
                tp_test_gate_yield(yield_fn);
            }
            return;
        }
        const int desired = observed == TP_TEST_GATE_ARMED
                                ? TP_TEST_GATE_IDLE
                                : TP_TEST_GATE_RELEASING;
        if (atomic_compare_exchange_weak(&gate->state, &observed, desired)) {
            if (desired == TP_TEST_GATE_RELEASING) {
                while (atomic_load(&gate->state) == TP_TEST_GATE_RELEASING) {
                    tp_test_gate_yield(yield_fn);
                }
            }
            return;
        }
    }
}

static inline void tp_test_gate_wait(tp_test_gate *gate,
                                     tp_test_gate_yield_fn yield_fn) {
    int expected = TP_TEST_GATE_ARMED;
    if (!atomic_compare_exchange_strong(&gate->state, &expected,
                                        TP_TEST_GATE_ENTERED)) {
        return;
    }
    while (atomic_load(&gate->state) == TP_TEST_GATE_ENTERED) {
        tp_test_gate_yield(yield_fn);
    }
    expected = TP_TEST_GATE_RELEASING;
    while (!atomic_compare_exchange_weak(&gate->state, &expected,
                                         TP_TEST_GATE_IDLE)) {
        expected = TP_TEST_GATE_RELEASING;
        tp_test_gate_yield(yield_fn);
    }
}

static inline void tp_test_gate_reset(tp_test_gate *gate,
                                      tp_test_gate_yield_fn yield_fn) {
    tp_test_gate_release(gate, yield_fn);
}

#endif
