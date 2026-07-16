#ifndef TP_TESTS_TP_BENCH_PROJECT_LOAD_H
#define TP_TESTS_TP_BENCH_PROJECT_LOAD_H

#include <stddef.h>

/* Borrowed JSON; the caller keeps ownership and frees it after return. */
int tp_bench_project_load_run(int iterations,
                              char *shipped_huge_json,
                              size_t shipped_huge_json_bytes);

#endif
