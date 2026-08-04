#ifndef TP_CORE_SRC_TP_FORMAT_COMPILE_WORKER_INTERNAL_H
#define TP_CORE_SRC_TP_FORMAT_COMPILE_WORKER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_format.h"

/* Allocation failure before a compile frame can be represented is carried by
 * process status. It is reserved to this private worker and never becomes a
 * public CLI exit code. */
#define TP_FORMAT_COMPILE_WORKER_EXIT_OOM 86

typedef struct tp_format_compile_worker_options {
    const char *worker_exe;
    int timeout_ms;
} tp_format_compile_worker_options;

/* Validate every pending candidate through one persistent self-reexec worker.
 * Row outcomes remain detached until the worker proves END -> COMPLETE; only
 * then are they atomically consumed by the scan. Any global fault invalidates
 * the scan and destroys the detached prefix. */
tp_status tp_format_compile_worker_run(
    tp_format_catalog_scan *scan,
    const tp_format_compile_worker_options *options, tp_error *error);

/* Child-side service selected by the shared __build-worker dispatcher after it
 * has read the first complete REQUEST frame. The service retains stdin/stdout
 * for the persistent REQUEST/ANNOUNCE/RESULT ... END/COMPLETE exchange. */
int tp_format_compile_worker_main_request(const uint8_t *bytes, size_t length);

#ifdef TP_ENABLE_TEST_SEAMS
typedef struct tp_format_compile_worker_test_budget {
    size_t frame_count;
    size_t request_bytes;
    size_t response_bytes;
    size_t source_bytes;
} tp_format_compile_worker_test_budget;

tp_status tp_format_compile_worker__test_reserve_request(
    tp_format_compile_worker_test_budget *budget, size_t frame_bytes,
    tp_error *error);
tp_status tp_format_compile_worker__test_charge_source_bytes(
    tp_format_compile_worker_test_budget *budget, size_t source_bytes,
    tp_error *error);
tp_status tp_format_compile_worker__test_reserve_response_frame(
    tp_format_compile_worker_test_budget *budget, tp_error *error);
tp_status tp_format_compile_worker__test_charge_response_bytes(
    tp_format_compile_worker_test_budget *budget, size_t byte_count,
    tp_error *error);
#endif

#endif /* TP_CORE_SRC_TP_FORMAT_COMPILE_WORKER_INTERNAL_H */
