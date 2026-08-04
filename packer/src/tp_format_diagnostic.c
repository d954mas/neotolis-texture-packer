#include "tp_format_diagnostic_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct tp_format_owned_diagnostic {
    tp_format_diagnostic value;
    void *storage;
    size_t storage_bytes;
} tp_format_owned_diagnostic;

struct tp_format_diagnostic_report {
    tp_format_owned_diagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;
    size_t dynamic_bytes;
    bool truncated;
    bool materialization_closed;
    tp_format_diagnostic truncation_marker;
};

#define TP_FORMAT_DIAGNOSTIC_ORDINARY_MAX \
    (TP_FORMAT_DIAGNOSTIC_MAX - 1U)

_Static_assert(TP_FORMAT_DIAGNOSTIC_MAX >= 1U,
               "diagnostic report needs one truncation-marker slot");
_Static_assert(sizeof(tp_format_diagnostic_report) <=
                   TP_FORMAT_DIAGNOSTIC_DYNAMIC_BYTES_MAX,
               "diagnostic report object exceeds its dynamic-byte budget");

#define TP_FORMAT_DIAGNOSTIC_CODE_LIST(X)                                \
    X(CATALOG_LIMIT, "catalog_limit")                                    \
    X(ROOT_NOT_DIRECTORY, "root_not_directory")                          \
    X(ROOT_REPARSE, "root_reparse")                                      \
    X(ROOT_IO, "root_io")                                                \
    X(PACKAGE_NAME_INVALID, "package_name_invalid")                      \
    X(PACKAGE_REPARSE, "package_reparse")                                \
    X(PACKAGE_EXTRA_ENTRY, "package_extra_entry")                        \
    X(DESCRIPTOR_MISSING, "descriptor_missing")                          \
    X(SOURCE_MISSING, "source_missing")                                  \
    X(PACKAGE_FILE_TYPE, "package_file_type")                            \
    X(PACKAGE_FILE_REPARSE, "package_file_reparse")                      \
    X(PACKAGE_FILE_TOO_LARGE, "package_file_too_large")                  \
    X(PACKAGE_READ_FAILED, "package_read_failed")                        \
    X(DESCRIPTOR_INVALID_UTF8, "descriptor_invalid_utf8")                \
    X(DESCRIPTOR_INVALID_JSON, "descriptor_invalid_json")                \
    X(DESCRIPTOR_SCHEMA, "descriptor_schema")                            \
    X(API_UNSUPPORTED, "api_unsupported")                                \
    X(FORMAT_ID_INVALID, "format_id_invalid")                            \
    X(FORMAT_ID_RESERVED, "format_id_reserved")                          \
    X(OUTPUT_INVALID, "output_invalid")                                  \
    X(OUTPUT_CONFLICT, "output_conflict")                                \
    X(HOST_FACT_INVALID, "host_fact_invalid")                            \
    X(DUPLICATE_FORMAT_ID, "duplicate_format_id")                        \
    X(SOURCE_INVALID_UTF8, "source_invalid_utf8")                        \
    X(SOURCE_BINARY, "source_binary")                                    \
    X(COMPILE_ERROR, "compile_error")                                    \
    X(COMPILE_WORKER_FAILED, "compile_worker_failed")                    \
    X(COMPILE_PROTOCOL, "compile_protocol")                              \
    X(COMPILE_BUDGET, "compile_budget")                                  \
    X(HANDLER_CONTRACT, "handler_contract")                              \
    X(HANDLER_FAILED, "handler_failed")                                  \
    X(HANDLER_PANIC, "handler_panic")                                    \
    X(MEMORY_LIMIT, "memory_limit")                                      \
    X(INSTRUCTION_LIMIT, "instruction_limit")                            \
    X(HOST_CALL_LIMIT, "host_call_limit")                                \
    X(OUTPUT_LIMIT, "output_limit")                                      \
    X(NOTICE_LIMIT, "notice_limit")                                      \
    X(DOCUMENT_UNKNOWN, "document_unknown")                              \
    X(DOCUMENT_DUPLICATE, "document_duplicate")                          \
    X(DOCUMENT_UNFINISHED, "document_unfinished")                        \
    X(DOCUMENT_MISSING, "document_missing")                              \
    X(DOCUMENT_WRITE_AFTER_FINISH, "document_write_after_finish")        \
    X(DOCUMENT_INVALID_UTF8, "document_invalid_utf8")                    \
    X(DOCUMENT_CONTAINS_NUL, "document_contains_nul")                    \
    X(DIAGNOSTICS_TRUNCATED, "diagnostics_truncated")

const char *tp_format_diagnostic_code_id(tp_format_diagnostic_code code) {
    switch (code) {
#define TP_FORMAT_DIAGNOSTIC_CODE_CASE(name, id) \
        case TP_FORMAT_DIAGNOSTIC_##name: return id;
        TP_FORMAT_DIAGNOSTIC_CODE_LIST(TP_FORMAT_DIAGNOSTIC_CODE_CASE)
#undef TP_FORMAT_DIAGNOSTIC_CODE_CASE
    }
    return "unknown_diagnostic_code";
}

const char *tp_format_diagnostic_phase_id(tp_format_diagnostic_phase phase) {
    switch (phase) {
        case TP_FORMAT_PHASE_DISCOVERY: return "discovery";
        case TP_FORMAT_PHASE_DESCRIPTOR: return "descriptor";
        case TP_FORMAT_PHASE_COMPILE: return "compile";
        case TP_FORMAT_PHASE_RUNTIME: return "runtime";
        case TP_FORMAT_PHASE_LIMIT: return "limit";
        case TP_FORMAT_PHASE_OUTPUT: return "output";
    }
    return "unknown_phase";
}

const char *tp_format_diagnostic_severity_id(
    tp_format_diagnostic_severity severity) {
    switch (severity) {
        case TP_FORMAT_DIAGNOSTIC_WARNING: return "warning";
        case TP_FORMAT_DIAGNOSTIC_ERROR: return "error";
    }
    return "unknown_severity";
}

static bool diagnostic_code_valid(tp_format_diagnostic_code code) {
    switch (code) {
#define TP_FORMAT_DIAGNOSTIC_VALID_CASE(name, id) \
        case TP_FORMAT_DIAGNOSTIC_##name: return true;
        TP_FORMAT_DIAGNOSTIC_CODE_LIST(TP_FORMAT_DIAGNOSTIC_VALID_CASE)
#undef TP_FORMAT_DIAGNOSTIC_VALID_CASE
    }
    return false;
}

static bool diagnostic_phase_valid(tp_format_diagnostic_phase phase) {
    switch (phase) {
        case TP_FORMAT_PHASE_DISCOVERY:
        case TP_FORMAT_PHASE_DESCRIPTOR:
        case TP_FORMAT_PHASE_COMPILE:
        case TP_FORMAT_PHASE_RUNTIME:
        case TP_FORMAT_PHASE_LIMIT:
        case TP_FORMAT_PHASE_OUTPUT: return true;
    }
    return false;
}

static bool diagnostic_severity_valid(
    tp_format_diagnostic_severity severity) {
    switch (severity) {
        case TP_FORMAT_DIAGNOSTIC_WARNING:
        case TP_FORMAT_DIAGNOSTIC_ERROR: return true;
    }
    return false;
}

static bool diagnostic_normal_phase(tp_format_diagnostic_code code,
                                    tp_format_diagnostic_phase *out_phase) {
    switch (code) {
        case TP_FORMAT_DIAGNOSTIC_CATALOG_LIMIT:
        case TP_FORMAT_DIAGNOSTIC_ROOT_NOT_DIRECTORY:
        case TP_FORMAT_DIAGNOSTIC_ROOT_REPARSE:
        case TP_FORMAT_DIAGNOSTIC_ROOT_IO:
        case TP_FORMAT_DIAGNOSTIC_PACKAGE_NAME_INVALID:
        case TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE:
        case TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY:
        case TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING:
        case TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING:
        case TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE:
        case TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE:
        case TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TOO_LARGE:
        case TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED:
            *out_phase = TP_FORMAT_PHASE_DISCOVERY;
            return true;
        case TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_UTF8:
        case TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON:
        case TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA:
        case TP_FORMAT_DIAGNOSTIC_API_UNSUPPORTED:
        case TP_FORMAT_DIAGNOSTIC_FORMAT_ID_INVALID:
        case TP_FORMAT_DIAGNOSTIC_FORMAT_ID_RESERVED:
        case TP_FORMAT_DIAGNOSTIC_OUTPUT_INVALID:
        case TP_FORMAT_DIAGNOSTIC_OUTPUT_CONFLICT:
        case TP_FORMAT_DIAGNOSTIC_HOST_FACT_INVALID:
        case TP_FORMAT_DIAGNOSTIC_DUPLICATE_FORMAT_ID:
            *out_phase = TP_FORMAT_PHASE_DESCRIPTOR;
            return true;
        case TP_FORMAT_DIAGNOSTIC_SOURCE_INVALID_UTF8:
        case TP_FORMAT_DIAGNOSTIC_SOURCE_BINARY:
        case TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR:
        case TP_FORMAT_DIAGNOSTIC_COMPILE_WORKER_FAILED:
        case TP_FORMAT_DIAGNOSTIC_COMPILE_PROTOCOL:
        case TP_FORMAT_DIAGNOSTIC_COMPILE_BUDGET:
            *out_phase = TP_FORMAT_PHASE_COMPILE;
            return true;
        case TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT:
        case TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED:
        case TP_FORMAT_DIAGNOSTIC_HANDLER_PANIC:
            *out_phase = TP_FORMAT_PHASE_RUNTIME;
            return true;
        case TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT:
        case TP_FORMAT_DIAGNOSTIC_INSTRUCTION_LIMIT:
        case TP_FORMAT_DIAGNOSTIC_HOST_CALL_LIMIT:
        case TP_FORMAT_DIAGNOSTIC_OUTPUT_LIMIT:
        case TP_FORMAT_DIAGNOSTIC_NOTICE_LIMIT:
            *out_phase = TP_FORMAT_PHASE_LIMIT;
            return true;
        case TP_FORMAT_DIAGNOSTIC_DOCUMENT_UNKNOWN:
        case TP_FORMAT_DIAGNOSTIC_DOCUMENT_DUPLICATE:
        case TP_FORMAT_DIAGNOSTIC_DOCUMENT_UNFINISHED:
        case TP_FORMAT_DIAGNOSTIC_DOCUMENT_MISSING:
        case TP_FORMAT_DIAGNOSTIC_DOCUMENT_WRITE_AFTER_FINISH:
        case TP_FORMAT_DIAGNOSTIC_DOCUMENT_INVALID_UTF8:
        case TP_FORMAT_DIAGNOSTIC_DOCUMENT_CONTAINS_NUL:
            *out_phase = TP_FORMAT_PHASE_OUTPUT;
            return true;
        case TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED: return false;
    }
    return false;
}

bool tp_format_diagnostic_semantics_valid_internal(
    const tp_format_diagnostic *diagnostic) {
    if (!diagnostic || !diagnostic_code_valid(diagnostic->code) ||
        !diagnostic_phase_valid(diagnostic->phase) ||
        !diagnostic_severity_valid(diagnostic->severity)) {
        return false;
    }
    if (diagnostic->code == TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED) {
        return diagnostic->severity == TP_FORMAT_DIAGNOSTIC_WARNING;
    }
    tp_format_diagnostic_phase normal_phase = TP_FORMAT_PHASE_DISCOVERY;
    return diagnostic_normal_phase(diagnostic->code, &normal_phase) &&
           diagnostic->severity == TP_FORMAT_DIAGNOSTIC_ERROR &&
           diagnostic->phase == normal_phase;
}

bool tp_format_diagnostic_truncation_marker_canonical_internal(
    const tp_format_diagnostic *diagnostic) {
    static const char marker_message[] =
        "diagnostic report truncated by a hard limit";
    return tp_format_diagnostic_semantics_valid_internal(diagnostic) &&
           diagnostic->code ==
               TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED &&
           !diagnostic->format_id && !diagnostic->package_path &&
           diagnostic->line == 0U && diagnostic->column == 0U &&
           diagnostic->message &&
           strcmp(diagnostic->message, marker_message) == 0 &&
           !diagnostic->frames && diagnostic->frame_count == 0U;
}

static bool size_add(size_t *value, size_t increment) {
    if (*value > SIZE_MAX - increment) {
        return false;
    }
    *value += increment;
    return true;
}

typedef struct tp_format_bounded_string {
    const char *source;
    size_t copied_bytes;
    bool truncated;
} tp_format_bounded_string;

static tp_format_bounded_string bounded_string(const char *source,
                                                size_t max_bytes) {
    tp_format_bounded_string result = {source, 0U, false};
    if (!source) {
        return result;
    }
    const size_t length = strlen(source);
    result.copied_bytes = length <= max_bytes ? length : max_bytes;
    result.truncated = length > max_bytes;
    return result;
}

static bool add_string_storage(size_t *bytes,
                               const tp_format_bounded_string *string) {
    return !string->source || size_add(bytes, string->copied_bytes + 1U);
}

static const char *copy_bounded_string(
    char **cursor, const tp_format_bounded_string *string) {
    if (!string->source) {
        return NULL;
    }
    char *destination = *cursor;
    if (string->copied_bytes > 0U) {
        memcpy(destination, string->source, string->copied_bytes);
    }
    destination[string->copied_bytes] = '\0';
    if (string->truncated) {
        tp_error_trim_partial_utf8(destination);
    }
    *cursor += string->copied_bytes + 1U;
    return destination;
}

static void mark_truncated(tp_format_diagnostic_report *report,
                           tp_format_diagnostic_phase phase) {
    if (report->truncated) {
        return;
    }
    report->truncated = true;
    report->truncation_marker.severity = TP_FORMAT_DIAGNOSTIC_WARNING;
    report->truncation_marker.code =
        TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED;
    report->truncation_marker.phase = phase;
    report->truncation_marker.message =
        "diagnostic report truncated by a hard limit";
}

tp_status tp_format_diagnostic_report_create_internal(
    tp_format_diagnostic_report **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic report output is required");
    }
    *out = NULL;
    tp_format_diagnostic_report *report =
        (tp_format_diagnostic_report *)calloc(1U, sizeof *report);
    if (!report) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "diagnostic report allocation failed");
    }
    report->dynamic_bytes = sizeof *report;
    *out = report;
    return TP_STATUS_OK;
}

tp_status tp_format_diagnostic_report_mark_truncated_internal(
    tp_format_diagnostic_report *report,
    tp_format_diagnostic_phase phase, tp_error *err) {
    if (!report || !diagnostic_phase_valid(phase)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic report and valid phase are required");
    }
    mark_truncated(report, phase);
    return TP_STATUS_OK;
}

static tp_status validate_append_input(
    const tp_format_diagnostic_report *report,
    const tp_format_diagnostic *diagnostic, tp_error *err) {
    if (!report || !diagnostic) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic report and diagnostic are required");
    }
    if (!diagnostic_severity_valid(diagnostic->severity) ||
        !diagnostic_code_valid(diagnostic->code) ||
        !diagnostic_phase_valid(diagnostic->phase)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic has invalid severity, code, or phase");
    }
    if (diagnostic->code == TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostics_truncated is report-owned");
    }
    if (diagnostic->frame_count > 0U && !diagnostic->frames) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic frames are required for a nonzero count");
    }
    return TP_STATUS_OK;
}

static size_t next_capacity(const tp_format_diagnostic_report *report) {
    size_t capacity = report->diagnostic_capacity > 0U
                          ? report->diagnostic_capacity * 2U
                          : 8U;
    if (capacity > (size_t)TP_FORMAT_DIAGNOSTIC_ORDINARY_MAX) {
        capacity = (size_t)TP_FORMAT_DIAGNOSTIC_ORDINARY_MAX;
    }
    return capacity;
}

tp_status tp_format_diagnostic_report_append_internal(
    tp_format_diagnostic_report *report,
    const tp_format_diagnostic *diagnostic, tp_error *err) {
    tp_status status = validate_append_input(report, diagnostic, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (report->materialization_closed ||
        report->diagnostic_count >=
            (size_t)TP_FORMAT_DIAGNOSTIC_ORDINARY_MAX) {
        report->materialization_closed = true;
        mark_truncated(report, diagnostic->phase);
        return TP_STATUS_OK;
    }

    tp_format_bounded_string format_id = bounded_string(
        diagnostic->format_id, TP_FORMAT_ID_MAX_BYTES);
    tp_format_bounded_string package_path = bounded_string(
        diagnostic->package_path, TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES);
    tp_format_bounded_string message = bounded_string(
        diagnostic->message, TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES);
    const size_t frame_count =
        diagnostic->frame_count <= TP_FORMAT_DIAGNOSTIC_FRAME_MAX
            ? diagnostic->frame_count
            : TP_FORMAT_DIAGNOSTIC_FRAME_MAX;
    tp_format_bounded_string frame_text[TP_FORMAT_DIAGNOSTIC_FRAME_MAX];
    bool field_truncated = format_id.truncated || package_path.truncated ||
                           message.truncated ||
                           diagnostic->frame_count > frame_count;

    size_t storage_bytes = frame_count * sizeof(tp_format_diagnostic_frame);
    if (!add_string_storage(&storage_bytes, &format_id) ||
        !add_string_storage(&storage_bytes, &package_path) ||
        !add_string_storage(&storage_bytes, &message)) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "diagnostic storage size overflow");
    }
    for (size_t i = 0U; i < frame_count; ++i) {
        frame_text[i] = bounded_string(
            diagnostic->frames[i].text,
            TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES);
        field_truncated = field_truncated || frame_text[i].truncated;
        if (!add_string_storage(&storage_bytes, &frame_text[i])) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "diagnostic frame storage size overflow");
        }
    }

    size_t capacity = report->diagnostic_capacity;
    if (report->diagnostic_count == capacity) {
        capacity = next_capacity(report);
    }
    const size_t old_vector_bytes =
        report->diagnostic_capacity * sizeof(tp_format_owned_diagnostic);
    const size_t new_vector_bytes =
        capacity * sizeof(tp_format_owned_diagnostic);
    size_t prospective_bytes = report->dynamic_bytes - old_vector_bytes;
    if (!size_add(&prospective_bytes, new_vector_bytes) ||
        !size_add(&prospective_bytes, storage_bytes) ||
        prospective_bytes > TP_FORMAT_DIAGNOSTIC_DYNAMIC_BYTES_MAX) {
        report->materialization_closed = true;
        mark_truncated(report, diagnostic->phase);
        return TP_STATUS_OK;
    }

    void *storage = storage_bytes > 0U ? malloc(storage_bytes) : NULL;
    if (storage_bytes > 0U && !storage) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "diagnostic storage allocation failed");
    }
    tp_format_owned_diagnostic *owned = report->diagnostics;
    if (capacity != report->diagnostic_capacity) {
        owned = (tp_format_owned_diagnostic *)realloc(
            report->diagnostics, capacity * sizeof *owned);
        if (!owned) {
            free(storage);
            return tp_error_set(err, TP_STATUS_OOM,
                                "diagnostic vector allocation failed");
        }
    }

    report->diagnostics = owned;
    report->diagnostic_capacity = capacity;
    report->dynamic_bytes = prospective_bytes;
    tp_format_owned_diagnostic *entry =
        &report->diagnostics[report->diagnostic_count];
    memset(entry, 0, sizeof *entry);
    entry->storage = storage;
    entry->storage_bytes = storage_bytes;
    entry->value.severity = diagnostic->severity;
    entry->value.code = diagnostic->code;
    entry->value.phase = diagnostic->phase;
    entry->value.line = diagnostic->line;
    entry->value.column = diagnostic->column;

    tp_format_diagnostic_frame *frames =
        frame_count > 0U ? (tp_format_diagnostic_frame *)storage : NULL;
    char *cursor = storage
                       ? (char *)storage +
                             frame_count * sizeof(tp_format_diagnostic_frame)
                       : NULL;
    entry->value.format_id = copy_bounded_string(&cursor, &format_id);
    entry->value.package_path = copy_bounded_string(&cursor, &package_path);
    entry->value.message = copy_bounded_string(&cursor, &message);
    for (size_t i = 0U; i < frame_count; ++i) {
        frames[i].text = copy_bounded_string(&cursor, &frame_text[i]);
        frames[i].line = diagnostic->frames[i].line;
    }
    entry->value.frames = frames;
    entry->value.frame_count = frame_count;
    report->diagnostic_count++;
    if (field_truncated) {
        mark_truncated(report, diagnostic->phase);
    }
    return TP_STATUS_OK;
}

tp_status tp_format_diagnostic_report_clone_internal(
    const tp_format_diagnostic_report *source,
    tp_format_diagnostic_report **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic clone requires source and output");
    }
    *out = NULL;
    if (!source) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic clone requires source and output");
    }
    tp_format_diagnostic_report *clone = NULL;
    tp_status status =
        tp_format_diagnostic_report_create_internal(&clone, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    for (size_t i = 0U; i < source->diagnostic_count; ++i) {
        status = tp_format_diagnostic_report_append_internal(
            clone, &source->diagnostics[i].value, err);
        if (status != TP_STATUS_OK) {
            tp_format_diagnostic_report_destroy(clone);
            return status;
        }
    }
    if (source->truncated) {
        mark_truncated(clone, source->truncation_marker.phase);
    }
    clone->materialization_closed = source->materialization_closed;
    *out = clone;
    return TP_STATUS_OK;
}

tp_status tp_format_diagnostic_report_merge_internal(
    tp_format_diagnostic_report *destination,
    const tp_format_diagnostic_report *source, tp_error *err) {
    if (!destination || !source) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic merge requires destination and source");
    }
    if (destination == source) {
        tp_format_diagnostic_report *copy = NULL;
        tp_status status = tp_format_diagnostic_report_clone_internal(
            source, &copy, err);
        if (status != TP_STATUS_OK) {
            return status;
        }
        status = tp_format_diagnostic_report_merge_internal(
            destination, copy, err);
        tp_format_diagnostic_report_destroy(copy);
        return status;
    }
    for (size_t i = 0U; i < source->diagnostic_count; ++i) {
        const tp_status status = tp_format_diagnostic_report_append_internal(
            destination, &source->diagnostics[i].value, err);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    if (source->truncated) {
        mark_truncated(destination, source->truncation_marker.phase);
    }
    return TP_STATUS_OK;
}

size_t tp_format_diagnostic_report_dynamic_bytes_internal(
    const tp_format_diagnostic_report *report) {
    return report ? report->dynamic_bytes : 0U;
}

size_t tp_format_diagnostic_report_count(
    const tp_format_diagnostic_report *report) {
    return report ? report->diagnostic_count + (report->truncated ? 1U : 0U)
                  : 0U;
}

bool tp_format_diagnostic_report_truncated(
    const tp_format_diagnostic_report *report) {
    return report && report->truncated;
}

const tp_format_diagnostic *tp_format_diagnostic_report_at(
    const tp_format_diagnostic_report *report, size_t index) {
    if (!report) {
        return NULL;
    }
    if (index < report->diagnostic_count) {
        return &report->diagnostics[index].value;
    }
    if (report->truncated && index == report->diagnostic_count) {
        return &report->truncation_marker;
    }
    return NULL;
}

void tp_format_diagnostic_report_destroy(
    tp_format_diagnostic_report *report) {
    if (!report) {
        return;
    }
    for (size_t i = 0U; i < report->diagnostic_count; ++i) {
        free(report->diagnostics[i].storage);
    }
    free(report->diagnostics);
    free(report);
}
