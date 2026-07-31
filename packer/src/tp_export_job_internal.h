#ifndef TP_CORE_SRC_TP_EXPORT_JOB_INTERNAL_H
#define TP_CORE_SRC_TP_EXPORT_JOB_INTERNAL_H

#include "tp_core/tp_export_run.h"
#include "tp_core/tp_project.h"

/* Shared immutable Build workflow constructor. The saved-file CLI reaches it
 * through the public snapshot wrapper; the live job worker already owns a
 * validated immutable project and enters here directly. Both clone once into
 * the same job owner before target filtering/execution. */
tp_status tp_export_project_job_create_internal(
    const tp_project *project, const char *work_dir,
    const tp_export_snapshot_job_opts *opts,
    tp_export_snapshot_job **out, tp_error *err);

#endif /* TP_CORE_SRC_TP_EXPORT_JOB_INTERNAL_H */
