#ifndef TP_CORE_SRC_TP_PROJECT_INTERNAL_H
#define TP_CORE_SRC_TP_PROJECT_INTERNAL_H

#include <stddef.h>

/* Exact nodes in the calibrated max mixed schema fixture: one atlas with
 * 262,144 tagged sources and canonical sprite records. Together with a
 * 64 MiB input it stays below the benchmark's 257 MiB accounted-byte budget. */
#define TP_PROJECT_JSON_MAX_NODES 2097166U
#define TP_PROJECT_JSON_MAX_CONTAINER_ENTRIES 262144U
#define TP_PROJECT_JSON_MAX_DEPTH 64U

typedef struct tp_project_load_lookup_work {
    size_t source_path_comparisons;
} tp_project_load_lookup_work;

typedef struct tp_project_load_resources {
    size_t source_index_peak_bytes;
    size_t id_refs_bytes;
    size_t id_index_bytes;
} tp_project_load_resources;

typedef struct tp_project_json_limits {
    size_t bytes;
    size_t nodes;
    size_t container_entries;
    size_t depth;
} tp_project_json_limits;

/* Test seams. Every declaration below, its definition, and the production
 * branches that read its state are compiled out of a build that does not define
 * TP_ENABLE_TEST_SEAMS, so a shipped tp_core carries neither the fault knobs nor
 * the accounting. A consumer recompiles the owning TU with the define:
 * tp_project_save.c (save I/O faults + the size cap), tp_project_write.c
 * (serialization probes), tp_project_parse.c (load probes + the JSON-limit
 * writer), tp_project_identity.c (id-validation probe; it also holds the fenced
 * production call that feeds tp_project__test_note_id_resources).
 * The struct definitions above stay unconditional: they are plain value types and
 * a type that appeared only under the fence would make the header's shape depend
 * on the build. */
#ifdef TP_ENABLE_TEST_SEAMS

/* Deterministic save-I/O fault seam for core and GUI self-tests. One-shot; it
 * fails before a temp file is created, so the destination must remain untouched. */
void tp_project__test_fail_next_temp_create(void);

/* Save fault seams around the two durability barriers. File-sync failure is
 * pre-publication (destination unchanged); parent-sync failure is
 * post-publication (FILE_DURABILITY_UNCERTAIN, saved bytes authoritative). */
void tp_project__test_fail_next_file_sync(void);
void tp_project__test_fail_next_parent_sync(void);

/* One-shot exact pre-publication Save phase failure. Production never retries
 * these phases; tests use this seam to pin the public outcome matrix. */
void tp_project__test_fail_next_save_io(tp_file_io_phase phase);

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

void tp_project__test_load_lookup_work_reset(void);
tp_project_load_lookup_work tp_project__test_load_lookup_work_take(void);
void tp_project__test_id_validation_work_reset(void);
size_t tp_project__test_id_validation_work_take(void);
void tp_project__test_load_resources_reset(void);
tp_project_load_resources tp_project__test_load_resources_take(void);
/* Production instrumentation sink: tp_project_identity.c reports the id tables it
 * just allocated, and the call site is fenced with this declaration. */
void tp_project__test_note_id_resources(size_t refs_bytes,
                                        size_t index_bytes);

/* Writes through the same production writer with the admission limits supplied
 * explicitly. The limits are the persisted-project admission contract, so a
 * shipping consumer gets the two fixed-limit entries and no override. */
tp_status tp_project__test_save_buffer_with_json_limits(
    const tp_project *project, bool checkpoint,
    const tp_project_json_limits *limits, char **out, size_t *out_len,
    tp_error *err);

#endif /* TP_ENABLE_TEST_SEAMS */

/* Recovery checkpoints are self-contained: relative live source spellings are
 * emitted as absolute paths using their stable runtime source base. */
tp_status tp_project_checkpoint_save_buffer(const tp_project *p, char **out,
                                            size_t *out_len, tp_error *err);
tp_status tp_project_checkpoint_serialized_size_bounded(
    const tp_project *p, size_t limit, size_t *out_len, tp_error *err);

/* Publishes an already-cloned private model candidate without cloning it again.
 * Serialization temporarily relativizes source paths, then restores their live
 * spellings before returning. On success the candidate retains the new saved
 * directory state and is ready for an allocation-free model pointer swap. */
tp_status tp_project_save_candidate_with_fingerprint(
    tp_project *candidate, const char *path,
    const tp_id128 *expected_fingerprint, bool create_only,
    tp_id128 *out_fingerprint, tp_error *err);

#endif
