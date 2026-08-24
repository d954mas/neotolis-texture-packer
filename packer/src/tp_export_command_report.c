#include "tp_export_command_report_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tp_format_diagnostic_internal.h"

#ifdef TP_ENABLE_TEST_SEAMS
static bool s_fail_next_adoption;

void tp_export_command_report__test_fail_next_adoption(void) {
    s_fail_next_adoption = true;
}
#endif

static char *report_strdup(const char *text) {
    if (!text) {
        return NULL;
    }
    const size_t length = strlen(text) + 1U;
    char *copy = malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

static void free_strings(const char *const *items, int count) {
    if (!items) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free((void *)items[i]);
    }
    free((void *)items);
}

static void destroy_target(tp_export_report_target *target) {
    if (!target) {
        return;
    }
    free((void *)target->exporter_id);
    free((void *)target->out_path);
    free((void *)target->error);
    tp_format_diagnostic_report_destroy(target->format_diagnostics);
    free_strings(target->written_files, target->written_file_count);
    free_strings(target->would_write, target->would_write_count);
    memset(target, 0, sizeof *target);
}

static void destroy_run(tp_export_report_run *run) {
    if (!run) {
        return;
    }
    free(run->pages);
    memset(run, 0, sizeof *run);
}

static bool run_equal(const tp_export_report_run *left,
                      const tp_export_report_run *right) {
    if (left->page_count != right->page_count ||
        left->sprite_count != right->sprite_count) {
        return false;
    }
    return left->page_count == 0 ||
           memcmp(left->pages, right->pages,
                  (size_t)left->page_count * sizeof *left->pages) == 0;
}

static void destroy_atlas(tp_export_command_atlas_report *atlas) {
    if (!atlas) {
        return;
    }
    free((void *)atlas->name);
    free((void *)atlas->skip_notice_id);
    free((void *)atlas->note);
    for (int i = 0; i < atlas->report.run_count; ++i) {
        free(atlas->report.runs[i].pages);
    }
    free(atlas->report.runs);
    for (int i = 0; i < atlas->report.target_count; ++i) {
        destroy_target(&atlas->report.targets[i]);
    }
    free(atlas->report.targets);
    for (int i = 0; i < atlas->notices.count; ++i) {
        free((void *)atlas->notices.items[i].sprite);
        free((void *)atlas->notices.items[i].target);
    }
    free(atlas->notices.items);
    memset(atlas, 0, sizeof *atlas);
}

void tp_export_command_report_destroy(tp_export_command_report *report) {
    if (!report) {
        return;
    }
    for (int i = 0; i < report->atlas_count; ++i) {
        destroy_atlas(&report->atlases[i]);
    }
    free(report->atlases);
    memset(report, 0, sizeof *report);
}

tp_status tp_export_command_report_allocate(tp_export_command_report *report,
                                            int atlas_count, bool dry_run,
                                            tp_error *err) {
    if (!report || atlas_count < 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid Export command report allocation");
    }
    memset(report, 0, sizeof *report);
    report->dry_run = dry_run;
    if (atlas_count == 0) {
        return TP_STATUS_OK;
    }
    report->atlases = calloc((size_t)atlas_count, sizeof *report->atlases);
    if (!report->atlases) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "Export command report allocation failed");
    }
    report->atlas_count = atlas_count;
    return TP_STATUS_OK;
}

static tp_status clone_string(const char *source, const char **out,
                              tp_error *err) {
    *out = report_strdup(source);
    if (source && !*out) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "Export command report string allocation failed");
    }
    return TP_STATUS_OK;
}

void tp_export_command_report_recount(tp_export_command_report *report) {
    if (!report) {
        return;
    }
    report->targets_ok = 0;
    report->targets_failed = 0;
    report->files_written = 0;
    report->notices = 0;
    report->atlases_ok = 0;
    report->atlases_failed = 0;
    report->atlases_skipped = 0;
    report->first_error[0] = '\0';
    bool uncertain = report->publication_uncertain;
    report->had_pack_failure = false;
    report->had_export_failure = false;
    for (int atlas = 0; atlas < report->atlas_count; ++atlas) {
        const tp_export_command_atlas_report *row = &report->atlases[atlas];
        if (!row->name) {
            continue;
        }
        const bool atlas_pack_failure =
            row->report.pack_failed ||
            (!row->report_present && row->status != TP_STATUS_OK);
        report->had_pack_failure |= atlas_pack_failure;
        report->had_export_failure |=
            row->report.report_failed ||
            (row->status != TP_STATUS_OK && !atlas_pack_failure);
        report->notices += row->notices.count;
        bool atlas_target_failure = false;
        const int target_count =
            row->report_present ? row->report.target_count : 0;
        for (int target = 0; target < target_count; ++target) {
            const tp_export_report_target *outcome =
                &row->report.targets[target];
            uncertain |= outcome->publication_uncertain;
            if (!outcome->completed) {
                report->targets_failed++;
                atlas_target_failure = true;
            } else if (outcome->ok) {
                report->targets_ok++;
                report->files_written += outcome->written_file_count;
            } else {
                report->targets_failed++;
                atlas_target_failure = true;
                report->had_export_failure |= !atlas_pack_failure;
            }
            if (report->first_error[0] == '\0' && outcome->completed &&
                !outcome->ok && outcome->error) {
                (void)snprintf(report->first_error,
                               sizeof report->first_error, "%s: %s",
                               row->name ? row->name : "?", outcome->error);
            }
        }
        if (row->skip_notice_id) {
            report->atlases_skipped++;
        } else if (atlas_pack_failure || row->status != TP_STATUS_OK ||
                   row->report.report_failed || atlas_target_failure) {
            report->atlases_failed++;
        } else {
            report->atlases_ok++;
        }
        if (report->first_error[0] == '\0' && row->status != TP_STATUS_OK) {
            const char *detail = row->error.msg[0]
                                     ? row->error.msg
                                     : row->note ? row->note
                                                 : tp_status_str(row->status);
            (void)snprintf(report->first_error, sizeof report->first_error,
                           "%s: %s", row->name ? row->name : "?", detail);
        }
    }
    report->publication_uncertain = uncertain;
}

void tp_export_command_report_apply_terminal_failure(
    tp_export_command_report *report, tp_status status,
    const tp_error *error) {
    if (!report || !error) {
        return;
    }
    for (int i = 0; i < report->atlas_count; ++i) {
        tp_export_command_atlas_report *atlas = &report->atlases[i];
        if (atlas->outcome_received) {
            continue;
        }
        free((void *)atlas->note);
        atlas->note = NULL;
        atlas->status = status;
        atlas->error = *error;
        return;
    }
}

void tp_export_command_report_finalize(
    tp_export_command_report *report, tp_session_job_state terminal_state,
    bool publication_pending) {
    if (!report) {
        return;
    }
    report->publication_uncertain |= !report->dry_run && publication_pending;
    if (!report->dry_run && publication_pending) {
        bool attributed = false;
        for (int atlas = 0;
             !attributed && atlas < report->atlas_count; ++atlas) {
            tp_export_command_atlas_report *row = &report->atlases[atlas];
            for (int target = 0; target < row->report.target_count; ++target) {
                tp_export_report_target *outcome =
                    &row->report.targets[target];
                if (outcome->completed) {
                    continue;
                }
                outcome->completed = true;
                outcome->ok = false;
                outcome->writer_outcome = TP_EXPORT_WRITER_FAILED;
                outcome->publication_uncertain = true;
                attributed = true;
                break;
            }
        }
    }
    tp_export_command_report_recount(report);
    report->partial_publication =
        !report->dry_run && terminal_state != TP_SESSION_JOB_SUCCEEDED &&
        (report->targets_ok > 0 || report->publication_uncertain);
}

tp_status tp_export_command_report_mark_lua_panic(
    tp_export_command_report *report, tp_error *err) {
    static const char message[] = "Lua handler panicked in the job worker";
    if (!report) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Lua panic report is required");
    }
    tp_export_report_target *target = NULL;
    for (int atlas = 0; !target && atlas < report->atlas_count; ++atlas) {
        tp_export_command_atlas_report *row = &report->atlases[atlas];
        if (row->skip_notice_id || !row->report_present) {
            continue;
        }
        for (int index = 0; index < row->report.target_count; ++index) {
            if (!row->report.targets[index].completed) {
                target = &row->report.targets[index];
                break;
            }
        }
    }
    if (!target) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Lua panic has no admitted target");
    }
    char *owned_error = report_strdup(message);
    if (!owned_error) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "Lua panic target error allocation failed");
    }
    tp_format_diagnostic_report *diagnostics = NULL;
    tp_status status = tp_format_diagnostic_report_create_internal(
        &diagnostics, err);
    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_HANDLER_PANIC,
        .phase = TP_FORMAT_PHASE_RUNTIME,
        .format_id = target->exporter_id,
        .message = message,
    };
    if (status == TP_STATUS_OK) {
        status = tp_format_diagnostic_report_append_internal(
            diagnostics, &diagnostic, err);
    }
    if (status != TP_STATUS_OK) {
        free(owned_error);
        tp_format_diagnostic_report_destroy(diagnostics);
        return status;
    }
    free((void *)target->error);
    tp_format_diagnostic_report_destroy(target->format_diagnostics);
    target->error = owned_error;
    target->format_diagnostics = diagnostics;
    target->writer_outcome = TP_EXPORT_WRITER_FAILED;
    target->completed = true;
    target->ok = false;
    target->publication_uncertain = false;
    tp_export_command_report_recount(report);
    return TP_STATUS_OK;
}

static void destroy_notices(tp_export_notices *notices) {
    if (!notices) {
        return;
    }
    for (int i = 0; i < notices->count; ++i) {
        free((void *)notices->items[i].sprite);
        free((void *)notices->items[i].target);
    }
    free(notices->items);
    memset(notices, 0, sizeof *notices);
}

void tp_export_command_outcome_destroy(tp_export_command_outcome *outcome) {
    if (!outcome) {
        return;
    }
    free((void *)outcome->atlas_name);
    free((void *)outcome->skip_notice_id);
    free((void *)outcome->note);
    destroy_target(&outcome->target);
    destroy_notices(&outcome->notices);
    destroy_run(&outcome->pack_run);
    memset(outcome, 0, sizeof *outcome);
}

static tp_status adopt_atlas_text(tp_export_command_atlas_report *atlas,
                                  const tp_export_command_outcome *outcome,
                                  tp_error *err) {
    const char *skip = NULL;
    const char *note = NULL;
    tp_status status = clone_string(outcome->skip_notice_id, &skip, err);
    if (status == TP_STATUS_OK) {
        status = clone_string(outcome->note, &note, err);
    }
    if (status != TP_STATUS_OK) {
        free((void *)skip);
        free((void *)note);
        return status;
    }
    free((void *)atlas->skip_notice_id);
    free((void *)atlas->note);
    atlas->skip_notice_id = skip;
    atlas->note = note;
    atlas->sprite_count = outcome->sprite_count;
    atlas->missing_sources = outcome->missing_sources;
    atlas->status = outcome->status;
    atlas->error = outcome->error;
    atlas->report_present = outcome->report_present;
    if (outcome->report_present) {
        atlas->report.dry_run = outcome->dry_run;
        atlas->report.pack_failed = outcome->pack_failed;
        atlas->report.report_failed = outcome->report_failed;
        atlas->report.input_outcome = outcome->input_outcome;
    }
    return TP_STATUS_OK;
}

static tp_status validate_target_outcome_semantics(
    const tp_export_command_outcome *outcome, tp_error *err) {
    const tp_export_report_target *target = &outcome->target;
    if (outcome->status != TP_STATUS_OK || outcome->pack_failed ||
        outcome->input_outcome != TP_EXPORT_INPUT_READY) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export target outcome has invalid atlas state");
    }
    if (outcome->dry_run) {
        if (target->written_file_count != 0 ||
            target->writer_outcome == TP_EXPORT_WRITER_SUCCEEDED ||
            target->publication_uncertain) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "Export dry-run target outcome has publication state");
        }
    } else if (target->would_write_count != 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export wet target outcome has dry-run files");
    }
    if (target->ok) {
        if ((target->error && target->error[0] != '\0') ||
            target->publication_uncertain ||
            target->writer_outcome !=
                (outcome->dry_run
                     ? TP_EXPORT_WRITER_NOT_ATTEMPTED
                     : TP_EXPORT_WRITER_SUCCEEDED)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "Export successful target outcome is contradictory");
        }
    } else if (target->writer_outcome == TP_EXPORT_WRITER_SUCCEEDED) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export failed target outcome reports a successful writer");
    }
    return TP_STATUS_OK;
}

tp_status tp_export_command_report_apply_outcome(
    tp_export_command_report *report,
    tp_export_command_outcome *outcome, tp_error *err) {
    if (!report || !outcome || outcome->atlas_index < 0 ||
        outcome->sprite_count < 0 || outcome->missing_sources < 0 ||
        outcome->atlas_index >= report->atlas_count) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export outcome atlas is outside admission");
    }
    tp_export_command_atlas_report *atlas =
        &report->atlases[outcome->atlas_index];
    if (!tp_id128_eq(atlas->id, outcome->atlas_id) || !atlas->name ||
        !outcome->atlas_name || strcmp(atlas->name, outcome->atlas_name) != 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export outcome atlas identity does not match admission");
    }
    if (outcome->kind == TP_EXPORT_COMMAND_OUTCOME_ATLAS) {
        if (atlas->outcome_received) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "Export atlas outcome was already received");
        }
        bool has_completed_target = false;
        bool has_uncompleted_target = false;
        for (int i = 0; i < atlas->report.target_count; ++i) {
            if (atlas->report.targets[i].completed) {
                has_completed_target = true;
            } else {
                has_uncompleted_target = true;
            }
        }
        if ((!outcome->report_present && has_completed_target) ||
            (!outcome->report_present && outcome->status == TP_STATUS_OK &&
             !outcome->skip_notice_id) ||
            (outcome->report_present &&
             (outcome->dry_run != report->dry_run ||
              outcome->skip_notice_id ||
              (outcome->status == TP_STATUS_OK && has_uncompleted_target) ||
              (has_completed_target &&
               (outcome->pack_failed != atlas->report.pack_failed ||
                outcome->report_failed != atlas->report.report_failed ||
                outcome->input_outcome != atlas->report.input_outcome))))) {
            return tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "Export atlas outcome conflicts with accepted target results");
        }
        const tp_status status = adopt_atlas_text(atlas, outcome, err);
        if (status == TP_STATUS_OK) {
            atlas->outcome_received = true;
        }
        return status;
    }
    if (outcome->kind != TP_EXPORT_COMMAND_OUTCOME_TARGET ||
        atlas->outcome_received ||
        outcome->target_index < 0 ||
        outcome->target_index >= atlas->report.target_count ||
        !outcome->target.completed || !outcome->report_present ||
        outcome->dry_run != report->dry_run) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export target outcome is invalid");
    }
    const tp_status semantics_status =
        validate_target_outcome_semantics(outcome, err);
    if (semantics_status != TP_STATUS_OK) {
        return semantics_status;
    }
    tp_export_report_target *target =
        &atlas->report.targets[outcome->target_index];
    const bool failed_before_output_resolution =
        !outcome->target.ok && outcome->target.pack_run < 0 &&
        !outcome->target.out_path;
    if (target->completed || !tp_id128_eq(target->id, outcome->target.id) ||
        !target->exporter_id || !outcome->target.exporter_id ||
        strcmp(target->exporter_id, outcome->target.exporter_id) != 0 ||
        (!outcome->target.out_path && !failed_before_output_resolution) ||
        outcome->target.notice_begin != 0 ||
        outcome->target.notice_end != outcome->notices.count) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export target outcome identity does not match admission");
    }
    if ((outcome->target.pack_run >= 0) != outcome->pack_run_present ||
        (outcome->pack_run_present &&
         outcome->target.pack_run != outcome->pack_run_index) ||
        outcome->pack_run_index > atlas->report.run_count) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export target outcome pack run is invalid");
    }
    if (outcome->pack_run_present &&
        outcome->pack_run_index < atlas->report.run_count &&
        !run_equal(&atlas->report.runs[outcome->pack_run_index],
                   &outcome->pack_run)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export target outcome pack run changed");
    }

#ifdef TP_ENABLE_TEST_SEAMS
    if (s_fail_next_adoption) {
        s_fail_next_adoption = false;
        return tp_error_set(err, TP_STATUS_OOM,
                            "Export outcome adoption allocation failed");
    }
#endif

    const bool append_run = outcome->pack_run_present &&
                            outcome->pack_run_index == atlas->report.run_count;
    const char *admitted_out_path = NULL;
    if (failed_before_output_resolution) {
        tp_status status = clone_string(
            target->out_path, &admitted_out_path, err);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    tp_export_report_run *new_runs = NULL;
    if (append_run) {
        new_runs = calloc((size_t)atlas->report.run_count + 1U,
                          sizeof *new_runs);
        if (!new_runs) {
            free((void *)admitted_out_path);
            return tp_error_set(err, TP_STATUS_OOM,
                                "Export report run adoption failed");
        } else if (atlas->report.run_count > 0) {
            memcpy(new_runs, atlas->report.runs,
                   (size_t)atlas->report.run_count * sizeof *new_runs);
        }
    }
    tp_export_notice *new_notices = NULL;
    const int notice_total = atlas->notices.count + outcome->notices.count;
    if (outcome->notices.count > 0 && atlas->notices.count > 0) {
        new_notices = calloc((size_t)notice_total, sizeof *new_notices);
        if (!new_notices) {
            free((void *)admitted_out_path);
            free(new_runs);
            return tp_error_set(err, TP_STATUS_OOM,
                                "Export report notice adoption failed");
        }
        memcpy(new_notices, atlas->notices.items,
               (size_t)atlas->notices.count * sizeof *new_notices);
        memcpy(new_notices + atlas->notices.count, outcome->notices.items,
               (size_t)outcome->notices.count * sizeof *new_notices);
    }

    if (append_run) {
        new_runs[atlas->report.run_count] = outcome->pack_run;
        memset(&outcome->pack_run, 0, sizeof outcome->pack_run);
        outcome->pack_run_present = false;
        free(atlas->report.runs);
        atlas->report.runs = new_runs;
        atlas->report.run_count++;
    }
    const int notice_begin = atlas->notices.count;
    if (outcome->notices.count > 0) {
        if (atlas->notices.count == 0) {
            new_notices = outcome->notices.items;
        } else {
            free(outcome->notices.items);
        }
        free(atlas->notices.items);
        atlas->notices.items = new_notices;
        atlas->notices.count = notice_total;
        atlas->notices.cap = notice_total;
        memset(&outcome->notices, 0, sizeof outcome->notices);
    }
    tp_export_report_target adopted_target = outcome->target;
    memset(&outcome->target, 0, sizeof outcome->target);
    adopted_target.out_path = failed_before_output_resolution
                                  ? admitted_out_path
                                  : adopted_target.out_path;
    adopted_target.notice_begin = notice_begin;
    adopted_target.notice_end = notice_total;
    destroy_target(target);
    *target = adopted_target;
    atlas->report_present = true;
    atlas->report.dry_run = outcome->dry_run;
    atlas->report.pack_failed = outcome->pack_failed;
    atlas->report.report_failed = outcome->report_failed;
    atlas->report.input_outcome = outcome->input_outcome;
    tp_export_command_report_recount(report);
    return TP_STATUS_OK;
}
