/* Shared harness for the three action-state trace targets (draft / refresh /
 * job). The corpus was one translation unit; the frame pump, foreign-commit
 * helper, reset order and save path stay in one place so the three binaries
 * observe the same starting state. Definitions live in the .c: the shell reset
 * seam is a single definition, not a per-target stub. */
#ifndef TP_TEST_GUI_ACTION_TRACE_FIXTURE_H
#define TP_TEST_GUI_ACTION_TRACE_FIXTURE_H

#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "gui_actions.h"
#include "gui_actions_internal.h"
#include "gui_pack.h"
#include "gui_pack_internal.h"
#include "gui_project_internal.h"
#include "gui_project_test_driver.h"
#include "gui_rows.h"
#include "gui_state.h"

#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_transaction.h"
#include "tp_test_seams.h"

#include "time/nt_time.h"
#include "unity.h"

/* Project file under this target's own TP_GUI_TRACE_TEST_DIR. */
extern char s_save_path[1024];

void pump_action_frame(void);
void publish_project_frame(void);
void settle_project_job(void);
gui_project_create_result create_animation_observed(
    tp_id128 atlas_id, int64_t expected_revision,
    const char *name, const tp_op_sprite_ref *frames,
    int frame_count);
const tp_snapshot_atlas *atlas_at(int index);
bool trace_animation_ref_at(int atlas_index, int animation_index,
                            gui_animation_ref *out);
bool trace_target_ref_at(int atlas_index, int target_index,
                         gui_target_ref *out);
gui_sprite_ref add_test_sprite_ref(const char *source_path,
                                   const char *source_key);
void apply_foreign_operation(tp_operation *operation,
                             const char *transaction_id);
void reset_public_action_state(void);

#endif /* TP_TEST_GUI_ACTION_TRACE_FIXTURE_H */
