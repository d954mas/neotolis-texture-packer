#ifndef TP_CORE_SRC_TP_PROJECT_INTERNAL_H
#define TP_CORE_SRC_TP_PROJECT_INTERNAL_H

#include <stddef.h>

/* Deterministic save-I/O fault seam for core and GUI self-tests. One-shot; it
 * fails before a temp file is created, so the destination must remain untouched. */
void tp_project__test_fail_next_temp_create(void);

/* One-shot writer-size limit override. Lets tests prove the save-side cap is
 * checked before publishing without constructing a 64 MiB project. */
void tp_project__test_set_save_max_bytes(size_t max_bytes);

/* Deterministic checkpoint pre-materialization probes. The bounded size pass is
 * deliberately not an encode call and performs no serializer allocation. */
void tp_project__test_serialization_stats_reset(void);
size_t tp_project__test_save_buffer_calls(void);
size_t tp_project__test_serializer_allocations(void);
size_t tp_project__test_load_buffer_calls(void);
size_t tp_project__test_size_query_calls(void);

/* Recovery checkpoints are self-contained: relative live source spellings are
 * emitted as absolute paths using their stable runtime source base. */
tp_status tp_project_checkpoint_save_buffer(const tp_project *p, char **out,
                                            size_t *out_len, tp_error *err);
tp_status tp_project_checkpoint_serialized_size_bounded(
    const tp_project *p, size_t limit, size_t *out_len, tp_error *err);

#endif
