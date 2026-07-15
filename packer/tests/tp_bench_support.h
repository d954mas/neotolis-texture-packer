#ifndef TP_BENCH_SUPPORT_H
#define TP_BENCH_SUPPORT_H

/* M0 benchmark-only measurement helpers. This header deliberately stays in
 * packer/tests: it is not a production profiler, allocator wrapper, or generic
 * timing service. */

#include <stdbool.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "time/nt_time.h"

#define TP_BENCH_MAX_SAMPLES 128U

typedef struct tp_bench_samples {
    double values[TP_BENCH_MAX_SAMPLES];
    size_t count;
    size_t failed;
} tp_bench_samples;

static inline void tp_bench_samples_init(tp_bench_samples *samples) {
    if (samples) {
        memset(samples, 0, sizeof *samples);
    }
}

static inline bool tp_bench_samples_accept(tp_bench_samples *samples, double elapsed_ms) {
    if (!samples || !isfinite(elapsed_ms) || elapsed_ms < 0.0 || samples->count >= TP_BENCH_MAX_SAMPLES) {
        if (samples) {
            samples->failed++;
        }
        return false;
    }
    samples->values[samples->count++] = elapsed_ms;
    return true;
}

static inline bool tp_bench_samples_record(tp_bench_samples *samples, bool operation_ok, double elapsed_ms) {
    if (!operation_ok) {
        if (samples) {
            samples->failed++;
        }
        return false;
    }
    return tp_bench_samples_accept(samples, elapsed_ms);
}

static inline bool tp_bench_samples_valid(const tp_bench_samples *samples) {
    return samples && samples->count > 0U && samples->failed == 0U;
}

/* Exact nearest-rank percentile. For N=20 this makes p50 sample 10 and p95
 * sample 19. Sorting in place is intentional: callers print individual samples
 * before requesting summary percentiles. */
static inline double tp_bench_samples_percentile(tp_bench_samples *samples, unsigned percentile) {
    if (!samples || samples->count == 0U || percentile == 0U || percentile > 100U) {
        return 0.0;
    }
    for (size_t i = 1U; i < samples->count; i++) {
        const double value = samples->values[i];
        size_t j = i;
        while (j > 0U && samples->values[j - 1U] > value) {
            samples->values[j] = samples->values[j - 1U];
            j--;
        }
        samples->values[j] = value;
    }
    size_t rank = ((size_t)percentile * samples->count + 99U) / 100U;
    return samples->values[rank - 1U];
}

static inline double tp_bench_now_ms(void) { return nt_time_now() * 1000.0; }

#endif /* TP_BENCH_SUPPORT_H */
