#ifndef TP_CORE_SRC_TP_EXPORT_JOB_INTERNAL_H
#define TP_CORE_SRC_TP_EXPORT_JOB_INTERNAL_H

#include "tp_core/tp_export_run.h"
#include "tp_core/tp_project.h"

typedef struct tp_export_run_opts {
    tp_export_report *report;
    bool dry_run;
    const tp_cancel_token *cancel;
    tp_export_terminal_boundary_fn terminal_boundary;
    void *terminal_boundary_context;
} tp_export_run_opts;

tp_status tp_export_run(
    const tp_project *project, int atlas_index,
    const struct tp_pack_sprite_desc *sprites, int sprite_count,
    const char *work_dir, struct tp_arena *arena,
    struct tp_export_notices *notices, int *out_pack_runs, tp_error *err);

tp_status tp_export_run_ex(
    const tp_project *project, int atlas_index,
    const struct tp_pack_sprite_desc *sprites, int sprite_count,
    const char *work_dir, struct tp_arena *arena,
    struct tp_export_notices *notices, int *out_pack_runs,
    const tp_export_run_opts *opts, tp_error *err);

/* Shared immutable Build workflow constructor. The saved-file CLI reaches it
 * through the public snapshot wrapper; the live job worker already owns a
 * validated immutable project and enters here directly. Both clone once into
 * the same job owner before target filtering/execution. */
tp_status tp_export_project_job_create_internal(
    const tp_project *project, const char *work_dir,
    const tp_export_snapshot_job_opts *opts,
    tp_export_snapshot_job **out, tp_error *err);

#endif /* TP_CORE_SRC_TP_EXPORT_JOB_INTERNAL_H */
