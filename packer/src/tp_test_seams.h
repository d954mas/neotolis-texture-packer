#ifndef TP_TEST_SEAMS_H
#define TP_TEST_SEAMS_H

#include <stdbool.h>

#ifndef TP_ENABLE_TEST_SEAMS
#error "tp_test_seams.h is available only to test-seam builds"
#endif

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

void tp_job__test_fail_next_observation_target_allocation(void);
void tp_job__test_set_worker_timeout_ms(int timeout_ms);
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
