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
size_t tp_project__test_serializer_peak_capacity(void);
size_t tp_project__test_load_buffer_calls(void);
size_t tp_project__test_size_query_calls(void);

typedef struct tp_project_load_lookup_work {
    size_t source_path_comparisons;
    size_t pending_name_comparisons;
} tp_project_load_lookup_work;

void tp_project__test_load_lookup_work_reset(void);
tp_project_load_lookup_work tp_project__test_load_lookup_work_take(void);
void tp_project__test_id_validation_work_reset(void);
size_t tp_project__test_id_validation_work_take(void);
void tp_project__test_legacy_id_work_reset(void);
size_t tp_project__test_legacy_id_work_take(void);
void tp_project__test_fail_next_legacy_id_index_alloc(void);

typedef struct tp_project_json_limits {
    size_t bytes;
    size_t nodes;
    size_t container_entries;
    size_t depth;
} tp_project_json_limits;

/* Pure component probes: production and tests use the same admission/writer
 * path, with limits passed explicitly rather than hidden mutable test state. */
tp_status tp_project__test_json_admit(const char *text, size_t len,
                                      const tp_project_json_limits *limits,
                                      tp_error *err);
tp_status tp_project__test_save_buffer_with_json_limits(
    const tp_project *project, bool checkpoint,
    const tp_project_json_limits *limits, char **out, size_t *out_len,
    tp_error *err);

/* Recovery checkpoints are self-contained: relative live source spellings are
 * emitted as absolute paths using their stable runtime source base. */
tp_status tp_project_checkpoint_save_buffer(const tp_project *p, char **out,
                                            size_t *out_len, tp_error *err);
tp_status tp_project_checkpoint_serialized_size_bounded(
    const tp_project *p, size_t limit, size_t *out_len, tp_error *err);

#endif
