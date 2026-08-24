#include "cli_exit.h"

#include "core/nt_assert.h"

int cli_exit_for_rejected_status(tp_status status) {
    switch (status) {
    case TP_STATUS_OOM:
    case TP_STATUS_RNG_FAILED:
    case TP_STATUS_DUPLICATE_ID:
        return CLI_EXIT_INTERNAL;
    case TP_STATUS_NOT_FOUND:
    case TP_STATUS_OUT_OF_BOUNDS:
        return CLI_EXIT_PROJECT;
    default:
        return CLI_EXIT_USAGE;
    }
}

int cli_exit_for_save_status(tp_status status) {
    if (status == TP_STATUS_FILE_IO_FAILED) {
        return CLI_EXIT_FILE_IO;
    }
    if (status == TP_STATUS_OOM || status == TP_STATUS_RNG_FAILED ||
        status == TP_STATUS_DUPLICATE_ID) {
        return CLI_EXIT_INTERNAL;
    }
    return CLI_EXIT_PROJECT;
}

int cli_exit_for_export_result(
    tp_session_job_state state, tp_status status,
    const tp_export_command_report *report) {
    if (state != TP_SESSION_JOB_SUCCEEDED || status != TP_STATUS_OK) {
        if (report &&
            (report->targets_ok > 0 || report->partial_publication)) {
            return CLI_EXIT_PARTIAL;
        }
        if (status == TP_STATUS_OOM) {
            return CLI_EXIT_INTERNAL;
        }
        return report && report->had_pack_failure
                   ? CLI_EXIT_PACK
                   : CLI_EXIT_EXPORT;
    }
    NT_ASSERT(report);
    if (!report->had_pack_failure && !report->had_export_failure) {
        return CLI_EXIT_OK;
    }
    if (report->targets_ok > 0) {
        return CLI_EXIT_PARTIAL;
    }
    return report->had_pack_failure ? CLI_EXIT_PACK : CLI_EXIT_EXPORT;
}
