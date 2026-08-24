#include "tp_export_command_report_proto_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_export_command_report_internal.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_job_worker_internal.h"
#include "tp_utf8_internal.h"

#define OUTCOME_PROTO_VERSION 2U
#define REPORT_MAX_COUNT 65536

typedef struct report_writer {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} report_writer;

typedef struct report_reader {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
} report_reader;

static bool writer_reserve(report_writer *writer, size_t addition) {
    if (addition > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES - writer->length) {
        return false;
    }
    const size_t needed = writer->length + addition;
    if (needed <= writer->capacity) {
        return true;
    }
    size_t capacity = writer->capacity ? writer->capacity : 256U;
    while (capacity < needed) {
        if (capacity > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES / 2U) {
            capacity = TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES;
            break;
        }
        capacity *= 2U;
    }
    uint8_t *grown = realloc(writer->bytes, capacity);
    if (!grown) {
        return false;
    }
    writer->bytes = grown;
    writer->capacity = capacity;
    return true;
}

static bool put_bytes(report_writer *writer, const void *bytes, size_t length) {
    if (!writer_reserve(writer, length)) {
        return false;
    }
    if (length > 0U) {
        memcpy(writer->bytes + writer->length, bytes, length);
        writer->length += length;
    }
    return true;
}

static bool put_u32(report_writer *writer, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned int i = 0; i < 4U; ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
    return put_bytes(writer, bytes, sizeof bytes);
}

static bool put_i32(report_writer *writer, int value) {
    return put_u32(writer, (uint32_t)(int32_t)value);
}

static bool put_u64(report_writer *writer, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned int i = 0; i < 8U; ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
    return put_bytes(writer, bytes, sizeof bytes);
}

static bool text_length(const char *text, size_t cap, uint32_t *out) {
    if (!text) {
        *out = UINT32_MAX;
        return true;
    }
    for (size_t i = 0U; i <= cap; ++i) {
        if (text[i] == '\0') {
            *out = (uint32_t)i;
            return true;
        }
    }
    return false;
}

static bool put_text(report_writer *writer, const char *text, size_t cap) {
    uint32_t length = 0U;
    return text_length(text, cap, &length) && put_u32(writer, length) &&
           (length == UINT32_MAX || put_bytes(writer, text, length));
}

static bool put_error(report_writer *writer, const tp_error *error) {
    return error->file_io.phase >= TP_FILE_IO_PHASE_NONE &&
           error->file_io.phase <= TP_FILE_IO_PHASE_ATOMIC_CREATE &&
           put_text(writer, error->msg, sizeof error->msg - 1U) &&
           put_i32(writer, (int)error->file_io.phase) &&
           put_i32(writer, error->file_io.native_code) &&
           put_text(writer, error->file_io.path,
                    sizeof error->file_io.path - 1U);
}

static bool put_format_diagnostics(
    report_writer *writer, const tp_format_diagnostic_report *report) {
    const size_t count = tp_format_diagnostic_report_count(report);
    if (count > TP_FORMAT_DIAGNOSTIC_MAX ||
        !put_u32(writer, (uint32_t)count)) {
        return false;
    }
    for (size_t i = 0U; i < count; ++i) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(report, i);
        if (!diagnostic ||
            diagnostic->frame_count > TP_FORMAT_DIAGNOSTIC_FRAME_MAX ||
            !put_i32(writer, (int)diagnostic->severity) ||
            !put_i32(writer, (int)diagnostic->code) ||
            !put_i32(writer, (int)diagnostic->phase) ||
            !put_u32(writer, diagnostic->line) ||
            !put_u32(writer, diagnostic->column) ||
            !put_u32(writer, (uint32_t)diagnostic->frame_count) ||
            !put_text(writer, diagnostic->format_id, TP_FORMAT_ID_MAX_BYTES) ||
            !put_text(writer, diagnostic->package_path,
                      TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES) ||
            !put_text(writer, diagnostic->message,
                      TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES)) {
            return false;
        }
        for (size_t frame = 0U; frame < diagnostic->frame_count; ++frame) {
            if (!put_u32(writer, diagnostic->frames[frame].line) ||
                !put_text(writer, diagnostic->frames[frame].text,
                          TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES)) {
                return false;
            }
        }
    }
    return true;
}

static bool put_target(report_writer *writer,
                       const tp_export_report_target *target) {
    if (!put_bytes(writer, target->id.bytes, sizeof target->id.bytes) ||
        !put_text(writer, target->exporter_id,
                  TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES) ||
        !put_text(writer, target->out_path,
                  TP_JOB_WORKER_PROTO_MAX_PATH_BYTES) ||
        !put_text(writer, target->error, TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES) ||
        !put_i32(writer, target->written_file_count) ||
        !put_i32(writer, target->would_write_count) ||
        !put_i32(writer, target->pack_run) ||
        !put_i32(writer, target->notice_begin) ||
        !put_i32(writer, target->notice_end) ||
        !put_i32(writer, (int)target->writer_outcome) ||
        !put_u32(writer, target->publication_uncertain ? 1U : 0U) ||
        !put_u32(writer, target->completed ? 1U : 0U) ||
        !put_u32(writer, target->ok ? 1U : 0U) ||
        !put_format_diagnostics(writer, target->format_diagnostics)) {
        return false;
    }
    for (int i = 0; i < target->written_file_count; ++i) {
        if (!put_text(writer, target->written_files[i],
                      TP_JOB_WORKER_PROTO_MAX_PATH_BYTES)) {
            return false;
        }
    }
    for (int i = 0; i < target->would_write_count; ++i) {
        if (!put_text(writer, target->would_write[i],
                      TP_JOB_WORKER_PROTO_MAX_PATH_BYTES)) {
            return false;
        }
    }
    return true;
}

static bool get_u32(report_reader *reader, uint32_t *out) {
    if (reader->length - reader->offset < 4U) return false;
    const uint8_t *bytes = reader->bytes + reader->offset;
    *out = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
    reader->offset += 4U;
    return true;
}

static bool get_i32(report_reader *reader, int *out) {
    uint32_t value = 0U;
    if (!get_u32(reader, &value)) return false;
    *out = (int)(int32_t)value;
    return true;
}

static bool get_u64(report_reader *reader, uint64_t *out) {
    if (reader->length - reader->offset < 8U) return false;
    uint64_t value = 0U;
    for (unsigned int i = 0; i < 8U; ++i) {
        value |= (uint64_t)reader->bytes[reader->offset + i] << (i * 8U);
    }
    reader->offset += 8U;
    *out = value;
    return true;
}

static bool get_bytes(report_reader *reader, void *out, size_t length) {
    if (length > reader->length - reader->offset) {
        return false;
    }
    memcpy(out, reader->bytes + reader->offset, length);
    reader->offset += length;
    return true;
}

static tp_status get_text(report_reader *reader, size_t cap, const char **out,
                          tp_error *err) {
    uint32_t length = 0U;
    if (!get_u32(reader, &length)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "truncated Export command report string");
    }
    *out = NULL;
    if (length == UINT32_MAX) return TP_STATUS_OK;
    if ((size_t)length > cap || (size_t)length > reader->length - reader->offset) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "invalid Export command report string length");
    }
    tp_status status = tp_utf8_validate_text_field(
        reader->bytes + reader->offset, length, "Export command report text",
        err);
    if (status != TP_STATUS_OK) return status;
    char *text = malloc((size_t)length + 1U);
    if (!text) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "Export command report string allocation failed");
    }
    memcpy(text, reader->bytes + reader->offset, length);
    text[length] = '\0';
    reader->offset += length;
    *out = text;
    return TP_STATUS_OK;
}

static bool valid_count(int value) {
    return value >= 0 && value <= REPORT_MAX_COUNT;
}

static tp_status get_string_array(report_reader *reader, int count,
                                  const char *const **out, tp_error *err) {
    *out = NULL;
    if (!valid_count(count)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "invalid Export command report path count");
    }
    if (count == 0) return TP_STATUS_OK;
    const char **items = calloc((size_t)count, sizeof *items);
    if (!items) return tp_error_set(err, TP_STATUS_OOM,
                                    "Export command report path allocation failed");
    *out = items;
    for (int i = 0; i < count; ++i) {
        tp_status status = get_text(reader, TP_JOB_WORKER_PROTO_MAX_PATH_BYTES,
                                    &items[i], err);
        if (status != TP_STATUS_OK) return status;
    }
    return TP_STATUS_OK;
}

static void free_temporary_diagnostic(tp_format_diagnostic *diagnostic) {
    free((void *)diagnostic->format_id);
    free((void *)diagnostic->package_path);
    free((void *)diagnostic->message);
    if (diagnostic->frames) {
        for (size_t i = 0U; i < diagnostic->frame_count; ++i) {
            free((void *)diagnostic->frames[i].text);
        }
    }
    free((void *)diagnostic->frames);
    memset(diagnostic, 0, sizeof *diagnostic);
}

static tp_status get_format_diagnostics(
    report_reader *reader, tp_format_diagnostic_report **out, tp_error *err) {
    *out = NULL;
    uint32_t count = 0U;
    if (!get_u32(reader, &count) || count > TP_FORMAT_DIAGNOSTIC_MAX) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid Export format diagnostic count");
    }
    if (count == 0U) {
        return TP_STATUS_OK;
    }
    tp_format_diagnostic_report *report = NULL;
    tp_status status =
        tp_format_diagnostic_report_create_internal(&report, err);
    bool marker_seen = false;
    for (uint32_t i = 0U; status == TP_STATUS_OK && i < count; ++i) {
        int severity = 0, code = 0, phase = 0;
        uint32_t frame_count = 0U;
        tp_format_diagnostic diagnostic = {0};
        if (!get_i32(reader, &severity) || !get_i32(reader, &code) ||
            !get_i32(reader, &phase) || !get_u32(reader, &diagnostic.line) ||
            !get_u32(reader, &diagnostic.column) ||
            !get_u32(reader, &frame_count) ||
            frame_count > TP_FORMAT_DIAGNOSTIC_FRAME_MAX) {
            status = tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "invalid Export format diagnostic fields");
        }
        diagnostic.severity = (tp_format_diagnostic_severity)severity;
        diagnostic.code = (tp_format_diagnostic_code)code;
        diagnostic.phase = (tp_format_diagnostic_phase)phase;
        if (status == TP_STATUS_OK) {
            status = get_text(reader, TP_FORMAT_ID_MAX_BYTES,
                              &diagnostic.format_id, err);
        }
        if (status == TP_STATUS_OK) {
            status = get_text(reader, TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES,
                              &diagnostic.package_path, err);
        }
        if (status == TP_STATUS_OK) {
            status = get_text(reader, TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES,
                              &diagnostic.message, err);
        }
        tp_format_diagnostic_frame *frames = NULL;
        if (status == TP_STATUS_OK && frame_count > 0U) {
            frames = calloc(frame_count, sizeof *frames);
            if (!frames) {
                status = tp_error_set(
                    err, TP_STATUS_OOM,
                    "Export format diagnostic frame allocation failed");
            }
        }
        diagnostic.frames = frames;
        diagnostic.frame_count = frame_count;
        for (uint32_t frame = 0U;
             status == TP_STATUS_OK && frame < frame_count; ++frame) {
            if (!get_u32(reader, &frames[frame].line) ||
                frames[frame].line == 0U) {
                status = tp_error_set(
                    err, TP_STATUS_INVALID_ARGUMENT,
                    "invalid Export format diagnostic frame");
            } else {
                status = get_text(reader,
                                  TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES,
                                  &frames[frame].text, err);
                if (status == TP_STATUS_OK && !frames[frame].text) {
                    status = tp_error_set(
                        err, TP_STATUS_INVALID_ARGUMENT,
                        "Export format diagnostic frame text is required");
                }
            }
        }
        if (status == TP_STATUS_OK &&
            (!tp_format_diagnostic_semantics_valid_internal(&diagnostic) ||
             marker_seen)) {
            status = tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "invalid Export format diagnostic semantics");
        } else if (status == TP_STATUS_OK &&
                   diagnostic.code ==
                       TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED) {
            if (i + 1U != count ||
                !tp_format_diagnostic_truncation_marker_canonical_internal(
                    &diagnostic)) {
                status = tp_error_set(
                    err, TP_STATUS_INVALID_ARGUMENT,
                    "invalid Export diagnostic truncation marker");
            } else {
                status = tp_format_diagnostic_report_mark_truncated_internal(
                    report, diagnostic.phase, err);
                marker_seen = status == TP_STATUS_OK;
            }
        } else if (status == TP_STATUS_OK) {
            status = tp_format_diagnostic_report_append_internal(
                report, &diagnostic, err);
            if (status == TP_STATUS_OK &&
                tp_format_diagnostic_report_truncated(report)) {
                status = tp_error_set(
                    err, TP_STATUS_OUT_OF_BOUNDS,
                    "Export diagnostics exceed their owned-report cap");
            }
        }
        free_temporary_diagnostic(&diagnostic);
    }
    if (status != TP_STATUS_OK) {
        tp_format_diagnostic_report_destroy(report);
        return status;
    }
    *out = report;
    return TP_STATUS_OK;
}

static tp_status get_target(report_reader *reader,
                            tp_export_report_target *target, tp_error *err) {
    memset(target, 0, sizeof *target);
    int writer_outcome = 0;
    uint32_t uncertain = 0U, completed = 0U, ok = 0U;
    if (!get_bytes(reader, target->id.bytes, sizeof target->id.bytes)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "truncated Export target identity");
    }
    tp_status status = get_text(reader, TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES,
                                &target->exporter_id, err);
    if (status == TP_STATUS_OK) status = get_text(reader, TP_JOB_WORKER_PROTO_MAX_PATH_BYTES, &target->out_path, err);
    if (status == TP_STATUS_OK) status = get_text(reader, TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES, &target->error, err);
    if (status != TP_STATUS_OK || !get_i32(reader, &target->written_file_count) ||
        !get_i32(reader, &target->would_write_count) ||
        !get_i32(reader, &target->pack_run) ||
        !get_i32(reader, &target->notice_begin) ||
        !get_i32(reader, &target->notice_end) ||
        !get_i32(reader, &writer_outcome) || !get_u32(reader, &uncertain) ||
        !get_u32(reader, &completed) || !get_u32(reader, &ok)) {
        return status != TP_STATUS_OK ? status :
            tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                         "truncated Export command target report");
    }
    if (!valid_count(target->written_file_count) ||
        !valid_count(target->would_write_count) ||
        writer_outcome < TP_EXPORT_WRITER_NOT_ATTEMPTED ||
        writer_outcome > TP_EXPORT_WRITER_FAILED || uncertain > 1U ||
        completed > 1U || ok > 1U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid Export command target report");
    }
    target->writer_outcome = (tp_export_writer_outcome)writer_outcome;
    target->publication_uncertain = uncertain != 0U;
    target->completed = completed != 0U;
    target->ok = ok != 0U;
    status = get_format_diagnostics(reader, &target->format_diagnostics, err);
    if (status == TP_STATUS_OK) {
        status = get_string_array(reader, target->written_file_count,
                              &target->written_files, err);
    }
    if (status == TP_STATUS_OK) {
        status = get_string_array(reader, target->would_write_count,
                                  &target->would_write, err);
    }
    return status;
}

static bool put_outcome_run(report_writer *writer,
                            const tp_export_report_run *run) {
    if (!put_i32(writer, run->page_count) ||
        !put_i32(writer, run->sprite_count)) {
        return false;
    }
    for (int page = 0; page < run->page_count; ++page) {
        uint64_t occupancy = 0U;
        memcpy(&occupancy, &run->pages[page].occupancy_pct,
               sizeof occupancy);
        if (!put_i32(writer, run->pages[page].index) ||
            !put_i32(writer, run->pages[page].w) ||
            !put_i32(writer, run->pages[page].h) ||
            !put_u64(writer, occupancy)) {
            return false;
        }
    }
    return true;
}

static bool put_outcome_notices(report_writer *writer,
                                const tp_export_notices *notices) {
    if (!put_i32(writer, notices->count)) {
        return false;
    }
    for (int i = 0; i < notices->count; ++i) {
        const tp_export_notice *notice = &notices->items[i];
        if (!put_i32(writer, notice->field_id) ||
            !put_i32(writer, notice->reason_id) ||
            !put_text(writer, notice->sprite,
                      TP_JOB_WORKER_PROTO_MAX_NAME_BYTES) ||
            !put_text(writer, notice->target,
                      TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES) ||
            !put_text(writer, notice->msg,
                      TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES)) {
            return false;
        }
    }
    return true;
}

tp_status tp_export_command_outcome_proto_encode(
    const tp_export_command_outcome *outcome, uint8_t **out_bytes,
    size_t *out_length, tp_error *err) {
    if (!outcome || !out_bytes || !out_length || outcome->atlas_index < 0 ||
        !outcome->atlas_name ||
        (outcome->kind != TP_EXPORT_COMMAND_OUTCOME_ATLAS &&
         outcome->kind != TP_EXPORT_COMMAND_OUTCOME_TARGET) ||
        (outcome->kind == TP_EXPORT_COMMAND_OUTCOME_TARGET &&
         (outcome->target_index < 0 || !outcome->target.completed ||
          outcome->target.notice_begin != 0 ||
          outcome->target.notice_end != outcome->notices.count ||
          ((outcome->target.pack_run >= 0) != outcome->pack_run_present) ||
          (outcome->pack_run_present &&
           outcome->target.pack_run != outcome->pack_run_index)))) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid Export command outcome");
    }
    *out_bytes = NULL;
    *out_length = 0U;
    report_writer writer = {0};
    bool ok = put_u32(&writer, OUTCOME_PROTO_VERSION) &&
              put_i32(&writer, (int)outcome->kind) &&
              put_i32(&writer, outcome->atlas_index) &&
              put_bytes(&writer, outcome->atlas_id.bytes,
                        sizeof outcome->atlas_id.bytes) &&
              put_text(&writer, outcome->atlas_name,
                       TP_JOB_WORKER_PROTO_MAX_NAME_BYTES) &&
              put_i32(&writer, outcome->sprite_count) &&
              put_i32(&writer, outcome->missing_sources) &&
              put_text(&writer, outcome->skip_notice_id,
                       TP_JOB_WORKER_PROTO_MAX_NAME_BYTES) &&
              put_text(&writer, outcome->note,
                       TP_JOB_WORKER_PROTO_MAX_NAME_BYTES) &&
              put_i32(&writer, (int)outcome->status) &&
              put_error(&writer, &outcome->error) &&
              put_u32(&writer, outcome->report_present ? 1U : 0U) &&
              put_u32(&writer, outcome->dry_run ? 1U : 0U) &&
              put_u32(&writer, outcome->pack_failed ? 1U : 0U) &&
              put_u32(&writer, outcome->report_failed ? 1U : 0U) &&
              put_i32(&writer, (int)outcome->input_outcome);
    if (ok && outcome->kind == TP_EXPORT_COMMAND_OUTCOME_TARGET) {
        ok = put_i32(&writer, outcome->target_index) &&
             put_target(&writer, &outcome->target) &&
             put_outcome_notices(&writer, &outcome->notices) &&
             put_u32(&writer, outcome->pack_run_present ? 1U : 0U) &&
             put_i32(&writer, outcome->pack_run_index) &&
             (!outcome->pack_run_present ||
              put_outcome_run(&writer, &outcome->pack_run));
    }
    if (!ok) {
        free(writer.bytes);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "Export command outcome exceeds protocol bounds");
    }
    *out_bytes = writer.bytes;
    *out_length = writer.length;
    return TP_STATUS_OK;
}

static tp_status get_outcome_notices(report_reader *reader,
                                     tp_export_notices *notices,
                                     tp_error *err) {
    if (!get_i32(reader, &notices->count) ||
        !valid_count(notices->count)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid Export outcome notice count");
    }
    notices->cap = notices->count;
    if (notices->count > 0) {
        notices->items = calloc((size_t)notices->count,
                                sizeof *notices->items);
        if (!notices->items) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "Export outcome notice allocation failed");
        }
    }
    for (int i = 0; i < notices->count; ++i) {
        tp_export_notice *notice = &notices->items[i];
        if (!get_i32(reader, &notice->field_id) ||
            !get_i32(reader, &notice->reason_id)) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "truncated Export outcome notice");
        }
        tp_status status = get_text(
            reader, TP_JOB_WORKER_PROTO_MAX_NAME_BYTES, &notice->sprite, err);
        if (status == TP_STATUS_OK) {
            status = get_text(reader,
                              TP_JOB_WORKER_PROTO_MAX_EXPORTER_ID_BYTES,
                              &notice->target, err);
        }
        const char *message = NULL;
        if (status == TP_STATUS_OK) {
            status = get_text(reader, TP_JOB_WORKER_PROTO_MAX_ERROR_BYTES,
                              &message, err);
        }
        if (status != TP_STATUS_OK) {
            free((void *)message);
            return status;
        }
        if (message) {
            (void)snprintf(notice->msg, sizeof notice->msg, "%s", message);
        }
        free((void *)message);
    }
    return TP_STATUS_OK;
}

static tp_status get_outcome_run(report_reader *reader,
                                 tp_export_report_run *run, tp_error *err) {
    if (!get_i32(reader, &run->page_count) ||
        !get_i32(reader, &run->sprite_count) ||
        !valid_count(run->page_count)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid Export outcome pack run");
    }
    if (run->page_count > 0) {
        run->pages = calloc((size_t)run->page_count, sizeof *run->pages);
        if (!run->pages) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "Export outcome page allocation failed");
        }
    }
    for (int i = 0; i < run->page_count; ++i) {
        uint64_t occupancy = 0U;
        if (!get_i32(reader, &run->pages[i].index) ||
            !get_i32(reader, &run->pages[i].w) ||
            !get_i32(reader, &run->pages[i].h) ||
            !get_u64(reader, &occupancy)) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "truncated Export outcome page");
        }
        memcpy(&run->pages[i].occupancy_pct, &occupancy, sizeof occupancy);
    }
    return TP_STATUS_OK;
}

tp_status tp_export_command_outcome_proto_decode(
    const uint8_t *bytes, size_t length, tp_export_command_outcome *out,
    tp_error *err) {
    if (!bytes || !out || length > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid Export command outcome bytes");
    }
    memset(out, 0, sizeof *out);
    report_reader reader = {bytes, length, 0U};
    uint32_t version = 0U, present = 0U, dry = 0U, pack_failed = 0U,
             report_failed = 0U;
    int kind = 0, status_value = 0, input_outcome = 0;
    if (!get_u32(&reader, &version) || !get_i32(&reader, &kind) ||
        !get_i32(&reader, &out->atlas_index) ||
        !get_bytes(&reader, out->atlas_id.bytes, sizeof out->atlas_id.bytes)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "truncated Export command outcome header");
    }
    tp_status status = get_text(&reader, TP_JOB_WORKER_PROTO_MAX_NAME_BYTES,
                                &out->atlas_name, err);
    if (status != TP_STATUS_OK || !get_i32(&reader, &out->sprite_count) ||
        !get_i32(&reader, &out->missing_sources)) {
        status = status != TP_STATUS_OK
                     ? status
                     : tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "truncated Export command outcome atlas");
        goto fail;
    }
    status = get_text(&reader, TP_JOB_WORKER_PROTO_MAX_NAME_BYTES,
                      &out->skip_notice_id, err);
    if (status == TP_STATUS_OK) {
        status = get_text(&reader, TP_JOB_WORKER_PROTO_MAX_NAME_BYTES,
                          &out->note, err);
    }
    const char *error_message = NULL;
    const char *error_path = NULL;
    int error_phase = 0, error_native_code = 0;
    if (status != TP_STATUS_OK || !get_i32(&reader, &status_value)) {
        status = status != TP_STATUS_OK
                     ? status
                     : tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "truncated Export outcome status");
        goto fail;
    }
    status = get_text(&reader, sizeof out->error.msg - 1U,
                      &error_message, err);
    if (status == TP_STATUS_OK &&
        (!get_i32(&reader, &error_phase) ||
         !get_i32(&reader, &error_native_code))) {
        status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                              "truncated Export outcome error context");
    }
    if (status == TP_STATUS_OK) {
        status = get_text(&reader, TP_FILE_IO_PATH_MAX - 1U,
                          &error_path, err);
    }
    if (status != TP_STATUS_OK || !get_u32(&reader, &present) ||
        !get_u32(&reader, &dry) || !get_u32(&reader, &pack_failed) ||
        !get_u32(&reader, &report_failed) ||
        !get_i32(&reader, &input_outcome)) {
        free((void *)error_message);
        free((void *)error_path);
        status = status != TP_STATUS_OK
                     ? status
                     : tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "truncated Export outcome report header");
        goto fail;
    }
    if (version != OUTCOME_PROTO_VERSION || out->atlas_index < 0 ||
        kind < TP_EXPORT_COMMAND_OUTCOME_ATLAS ||
        kind > TP_EXPORT_COMMAND_OUTCOME_TARGET ||
        status_value < TP_STATUS_OK || status_value > TP_STATUS_CANCELLED ||
        present > 1U || dry > 1U || pack_failed > 1U ||
        report_failed > 1U ||
        error_phase < TP_FILE_IO_PHASE_NONE ||
        error_phase > TP_FILE_IO_PHASE_ATOMIC_CREATE || !error_path ||
        input_outcome < TP_EXPORT_INPUT_NOT_EVALUATED ||
        input_outcome > TP_EXPORT_INPUT_FAILED || !out->atlas_name) {
        free((void *)error_message);
        free((void *)error_path);
        status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                              "invalid Export command outcome header");
        goto fail;
    }
    out->kind = (tp_export_command_outcome_kind)kind;
    out->status = (tp_status)status_value;
    out->report_present = present != 0U;
    out->dry_run = dry != 0U;
    out->pack_failed = pack_failed != 0U;
    out->report_failed = report_failed != 0U;
    out->input_outcome = (tp_export_input_outcome)input_outcome;
    if (error_message) {
        (void)snprintf(out->error.msg, sizeof out->error.msg, "%s",
                       error_message);
    }
    out->error.file_io.phase = (tp_file_io_phase)error_phase;
    out->error.file_io.native_code = error_native_code;
    (void)snprintf(out->error.file_io.path,
                   sizeof out->error.file_io.path, "%s", error_path);
    free((void *)error_message);
    free((void *)error_path);
    if (out->kind == TP_EXPORT_COMMAND_OUTCOME_TARGET) {
        uint32_t run_present = 0U;
        if (!get_i32(&reader, &out->target_index)) {
            status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                  "truncated Export target outcome index");
            goto fail;
        }
        status = get_target(&reader, &out->target, err);
        if (status == TP_STATUS_OK) {
            status = get_outcome_notices(&reader, &out->notices, err);
        }
        if (status != TP_STATUS_OK || !get_u32(&reader, &run_present) ||
            !get_i32(&reader, &out->pack_run_index)) {
            status = status != TP_STATUS_OK
                         ? status
                         : tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                        "truncated Export outcome run header");
            goto fail;
        }
        if (run_present > 1U) {
            status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                  "invalid Export outcome run presence");
            goto fail;
        }
        out->pack_run_present = run_present != 0U;
        if (out->pack_run_present) {
            status = get_outcome_run(&reader, &out->pack_run, err);
            if (status != TP_STATUS_OK) {
                goto fail;
            }
        }
        if (out->target_index < 0 || !out->target.completed ||
            out->target.notice_begin != 0 ||
            out->target.notice_end != out->notices.count ||
            ((out->target.pack_run >= 0) != out->pack_run_present) ||
            (out->pack_run_present &&
             out->target.pack_run != out->pack_run_index)) {
            status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                  "invalid Export target outcome cross-reference");
            goto fail;
        }
    }
    if (reader.offset != reader.length) {
        status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                              "trailing Export command outcome bytes");
        goto fail;
    }
    return TP_STATUS_OK;

fail:
    tp_export_command_outcome_destroy(out);
    return status;
}
