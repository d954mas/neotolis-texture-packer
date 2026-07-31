#ifndef TP_TEST_SEAMS_H
#define TP_TEST_SEAMS_H

#include <stdbool.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_job.h"

#ifndef TP_ENABLE_TEST_SEAMS
#error "tp_test_seams.h is available only to test-seam builds"
#endif

struct tp_pack_result_cache;

/* Damages the first page blob of the COLD entry under `hash` so its next promote
 * fails. There is no production route to a damaged blob -- the store both writes
 * and reads it in-process, within one arena's lifetime -- but the containment
 * path it feeds (drop the entry, keep the store usable, fall back to the next
 * candidate) is a real contract that has to stay executable. False if `hash` is
 * absent or its entry is not cold. */
bool tp_pack_result_cache__test_damage_cold_blob(
    struct tp_pack_result_cache *cache, tp_id128 hash);

void tp_scan__test_set_alloc_fail(int nth);
void tp_scan__test_set_stat_error(int error);
void tp_scan__test_reset_sort_finished(void);
bool tp_scan__test_sort_started(void);
bool tp_scan__test_sort_finished(void);
void tp_scan__test_arm_walk_gate(void);
bool tp_scan__test_walk_gate_entered(void);
void tp_scan__test_release_walk_gate(void);
void tp_scan__test_arm_post_entry_gate(void);
bool tp_scan__test_post_entry_gate_entered(void);
void tp_scan__test_release_post_entry_gate(void);
int tp_scan__test_visited_entries(void);
void tp_scan__test_reset_all(void);

int tp_refresh_job__test_active_workers(void);
void tp_refresh_job__test_fail_next_start(void);

void tp_job__test_fail_next_observation_target_allocation(void);
void tp_job__test_fail_next_artifact_path_allocation(void);
void tp_job__test_set_worker_timeout_ms(int timeout_ms);
void tp_job__test_set_worker_cancel_grace_ms(int grace_ms);
void tp_job__test_release_export_payload(tp_session_job_result *result);
void tp_job__test_reset_all(void);

void tp_export_run__test_set_report_alloc_fail(int nth);
void tp_export_run__test_fail_next_error_copy(void);
void tp_export_run__test_arm_before_write_gate(void);
bool tp_export_run__test_before_write_gate_entered(void);
void tp_export_run__test_release_before_write_gate(void);
void tp_export_run__test_arm_after_terminal_boundary_gate(void);
bool tp_export_run__test_after_terminal_boundary_gate_entered(void);
void tp_export_run__test_release_after_terminal_boundary_gate(void);
void tp_export_run__test_reset_all(void);

#endif
