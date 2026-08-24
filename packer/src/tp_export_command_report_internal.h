#ifndef TP_EXPORT_COMMAND_REPORT_INTERNAL_H
#define TP_EXPORT_COMMAND_REPORT_INTERNAL_H

#include "tp_core/tp_job.h"

typedef enum tp_export_command_outcome_kind {
    TP_EXPORT_COMMAND_OUTCOME_ATLAS = 1,
    TP_EXPORT_COMMAND_OUTCOME_TARGET,
} tp_export_command_outcome_kind;

/* One bounded worker fact. Atlas outcomes carry final atlas-level state;
 * target outcomes carry exactly one completed target, its notice slice, and at
 * most the one shared pack run it references. Admission identity stays owned
 * by the host report and is validated before adoption. */
typedef struct tp_export_command_outcome {
    tp_export_command_outcome_kind kind;
    int atlas_index;
    tp_id128 atlas_id;
    const char *atlas_name;
    int sprite_count;
    int missing_sources;
    const char *skip_notice_id;
    const char *note;
    tp_status status;
    tp_error error;
    bool report_present;
    bool dry_run;
    bool pack_failed;
    bool report_failed;
    tp_export_input_outcome input_outcome;

    int target_index;
    tp_export_report_target target;
    tp_export_notices notices;
    bool pack_run_present;
    int pack_run_index;
    tp_export_report_run pack_run;
} tp_export_command_outcome;

tp_status tp_export_command_report_allocate(tp_export_command_report *report,
                                            int atlas_count, bool dry_run,
                                            tp_error *err);
void tp_export_command_report_destroy(tp_export_command_report *report);
void tp_export_command_report_recount(tp_export_command_report *report);
void tp_export_command_report_apply_terminal_failure(
    tp_export_command_report *report, tp_status status,
    const tp_error *error);
void tp_export_command_outcome_destroy(tp_export_command_outcome *outcome);
tp_status tp_export_command_report_apply_outcome(
    tp_export_command_report *report,
    tp_export_command_outcome *outcome, tp_error *err);
void tp_export_command_report_finalize(
    tp_export_command_report *report, tp_session_job_state terminal_state,
    bool publication_pending);
tp_status tp_export_command_report_mark_lua_panic(
    tp_export_command_report *report, tp_error *err);

#ifdef TP_ENABLE_TEST_SEAMS
void tp_export_command_report__test_fail_next_adoption(void);
#endif

#endif
