#include "tp_format_catalog_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nt_assert.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_format_discovery_internal.h"
#include "tp_format_package_internal.h"

struct tp_format_catalog_scan {
    char *root;
    tp_format_catalog_owned_row *rows;
    size_t row_count;
    size_t row_capacity;
    size_t compile_count;
    size_t admitted_bytes;
    tp_format_diagnostic_report *root_diagnostics;
    bool root_missing;
    bool limit_fail_closed;
    tp_format_compile_batch_state compile_state;
};

static void scan_row_destroy(tp_format_catalog_owned_row *row) {
    if (!row) {
        return;
    }
    free(row->key);
    free(row->package_path);
    tp_format_owned_descriptor_destroy(row->owned_descriptor);
    tp_format_diagnostic_report_destroy(row->diagnostics);
    free(row->descriptor_bytes);
    free(row->source_bytes);
    memset(row, 0, sizeof *row);
}

void tp_format_catalog_scan_destroy(tp_format_catalog_scan *scan) {
    if (!scan) {
        return;
    }
    for (size_t i = 0U; i < scan->row_count; ++i) {
        scan_row_destroy(&scan->rows[i]);
    }
    free(scan->rows);
    free(scan->root);
    tp_format_diagnostic_report_destroy(scan->root_diagnostics);
    free(scan);
}

static tp_status report_one(tp_format_diagnostic_report **report,
                            tp_format_diagnostic_code code,
                            const char *format_id, const char *package_path,
                            uint32_t line, uint32_t column,
                            const char *message, tp_error *error) {
    if (!*report) {
        tp_status status =
            tp_format_diagnostic_report_create_internal(report, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    tp_format_diagnostic_phase phase = TP_FORMAT_PHASE_DISCOVERY;
    NT_ASSERT(tp_format_diagnostic_normal_phase_internal(code, &phase));
    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = code,
        .phase = phase,
        .format_id = format_id,
        .package_path = package_path,
        .line = line,
        .column = column,
        .message = message,
    };
    return tp_format_diagnostic_report_append_internal(*report, &diagnostic,
                                                        error);
}

static tp_status logical_diagnostic_path(
    const char *package_path, tp_format_discovery_fault_file fault_file,
    char out[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U],
    const char **out_path, tp_error *error) {
    if (!out_path) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "diagnostic path requires an output");
    }
    *out_path = package_path;
    if (!package_path || fault_file == TP_FORMAT_DISCOVERY_FAULT_PACKAGE) {
        return TP_STATUS_OK;
    }
    const char *file_name =
        fault_file == TP_FORMAT_DISCOVERY_FAULT_DESCRIPTOR ? "format.json"
                                                           : "export.lua";
    const int written = snprintf(
        out, TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U, "%s/%s",
        package_path, file_name);
    if (written < 0 || (size_t)written > TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "logical format diagnostic path exceeds its bound");
    }
    *out_path = out;
    return TP_STATUS_OK;
}

static void best_effort_failure_report(
    const tp_format_discovery_failure *failure,
    tp_format_diagnostic_report **out_report) {
    if (!out_report || !failure || failure->code == 0) {
        return;
    }
    tp_error ignored = {{0}};
    tp_format_diagnostic_report *report = NULL;
    if (report_one(&report, failure->code, NULL, "formats", 0U, 0U,
                   failure->message, &ignored) == TP_STATUS_OK) {
        *out_report = report;
    } else {
        tp_format_diagnostic_report_destroy(report);
    }
}

static int scan_row_compile_compare(const void *left_value,
                                    const void *right_value) {
    const tp_format_catalog_owned_row *left =
        (const tp_format_catalog_owned_row *)left_value;
    const tp_format_catalog_owned_row *right =
        (const tp_format_catalog_owned_row *)right_value;
    if (left->pending_compile != right->pending_compile) {
        return left->pending_compile ? -1 : 1;
    }
    if (left->pending_compile) {
        const char *left_id =
            tp_format_owned_descriptor_view(left->owned_descriptor)->id;
        const char *right_id =
            tp_format_owned_descriptor_view(right->owned_descriptor)->id;
        const int id_order = strcmp(left_id, right_id);
        return id_order != 0 ? id_order : strcmp(left->key, right->key);
    }
    return strcmp(left->key, right->key);
}

static tp_status mark_duplicate_ids(tp_format_catalog_scan *scan,
                                    tp_error *error) {
    bool duplicate[TP_FORMAT_PACKAGE_MAX] = {false};
    for (size_t i = 0U; i < scan->row_count; ++i) {
        if (!scan->rows[i].owned_descriptor) {
            continue;
        }
        const char *left_id = tp_format_owned_descriptor_view(
                                  scan->rows[i].owned_descriptor)
                                  ->id;
        for (size_t j = i + 1U; j < scan->row_count; ++j) {
            if (!scan->rows[j].owned_descriptor) {
                continue;
            }
            const char *right_id = tp_format_owned_descriptor_view(
                                       scan->rows[j].owned_descriptor)
                                       ->id;
            if (strcmp(left_id, right_id) == 0) {
                duplicate[i] = true;
                duplicate[j] = true;
            }
        }
    }
    for (size_t i = 0U; i < scan->row_count; ++i) {
        if (!duplicate[i]) {
            continue;
        }
        tp_format_catalog_owned_row *row = &scan->rows[i];
        const char *id =
            tp_format_owned_descriptor_view(row->owned_descriptor)->id;
        char diagnostic_path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
        const char *logical_path = NULL;
        tp_status status = logical_diagnostic_path(
            row->package_path, TP_FORMAT_DISCOVERY_FAULT_DESCRIPTOR,
            diagnostic_path, &logical_path, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        status = report_one(
            &row->diagnostics, TP_FORMAT_DIAGNOSTIC_DUPLICATE_FORMAT_ID,
            id, logical_path, 0U, 0U,
            "two or more runtime packages claim the same format id", error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        row->pending_compile = false;
        free(row->descriptor_bytes);
        row->descriptor_bytes = NULL;
        row->descriptor_byte_count = 0U;
        free(row->source_bytes);
        row->source_bytes = NULL;
        row->source_byte_count = 0U;
    }
    return TP_STATUS_OK;
}

static void make_limit_fail_closed(tp_format_catalog_scan *scan) {
    for (size_t i = 0U; i < scan->row_count; ++i) {
        scan_row_destroy(&scan->rows[i]);
    }
    free(scan->rows);
    scan->rows = NULL;
    scan->row_count = 0U;
    scan->row_capacity = 0U;
    scan->compile_count = 0U;
    scan->admitted_bytes = 0U;
    scan->limit_fail_closed = true;
}

static tp_status scan_append_candidate(
    tp_format_catalog_scan *scan, tp_format_discovered_candidate *candidate,
    tp_format_catalog_owned_row **out_row, tp_error *error) {
    *out_row = NULL;
    if (scan->row_count >= TP_FORMAT_PACKAGE_MAX) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "format scan row count exceeds its bound");
    }
    if (scan->row_count == scan->row_capacity) {
        size_t new_capacity = scan->row_capacity == 0U
                                  ? 8U
                                  : scan->row_capacity * 2U;
        if (new_capacity > TP_FORMAT_PACKAGE_MAX) {
            new_capacity = TP_FORMAT_PACKAGE_MAX;
        }
        tp_format_catalog_owned_row *resized =
            (tp_format_catalog_owned_row *)realloc(
                scan->rows, new_capacity * sizeof *resized);
        if (!resized) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "format scan row allocation failed");
        }
        scan->rows = resized;
        scan->row_capacity = new_capacity;
    }

    tp_format_catalog_owned_row *row = &scan->rows[scan->row_count++];
    memset(row, 0, sizeof *row);
    row->implementation = TP_FORMAT_IMPLEMENTATION_LUA;
    row->key = candidate->key;
    candidate->key = NULL;
    row->package_path = candidate->package_path;
    candidate->package_path = NULL;
    row->descriptor_bytes = candidate->descriptor_bytes;
    row->descriptor_byte_count = candidate->descriptor_byte_count;
    candidate->descriptor_bytes = NULL;
    candidate->descriptor_byte_count = 0U;
    row->source_bytes = candidate->source_bytes;
    row->source_byte_count = candidate->source_byte_count;
    candidate->source_bytes = NULL;
    candidate->source_byte_count = 0U;
    *out_row = row;
    return TP_STATUS_OK;
}

static tp_format_discovery_visit_result scan_visit_result(
    tp_format_discovery_visit_kind kind, tp_status status) {
    return (tp_format_discovery_visit_result){kind, status};
}

static tp_format_discovery_visit_result scan_visit_candidate(
    void *context, tp_format_discovered_candidate *candidate,
    tp_error *error) {
    tp_format_catalog_scan *scan = (tp_format_catalog_scan *)context;
    if (scan->limit_fail_closed) {
        return scan_visit_result(TP_FORMAT_DISCOVERY_VISIT_STOP_SUCCESS,
                                 TP_STATUS_OK);
    }

    tp_format_catalog_owned_row *row = NULL;
    tp_status status = scan_append_candidate(scan, candidate, &row, error);
    if (status != TP_STATUS_OK) {
        return scan_visit_result(TP_FORMAT_DISCOVERY_VISIT_ERROR, status);
    }

    if (candidate->fault_code != 0) {
        char diagnostic_path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
        const char *logical_path = NULL;
        status = logical_diagnostic_path(
            row->package_path, candidate->fault_file, diagnostic_path,
            &logical_path, error);
        if (status != TP_STATUS_OK) {
            return scan_visit_result(TP_FORMAT_DISCOVERY_VISIT_ERROR,
                                     status);
        }
        status = report_one(&row->diagnostics, candidate->fault_code, NULL,
                            logical_path, 0U, 0U, candidate->fault_message,
                            error);
        return scan_visit_result(
            status == TP_STATUS_OK ? TP_FORMAT_DISCOVERY_VISIT_CONTINUE
                                   : TP_FORMAT_DISCOVERY_VISIT_ERROR,
            status);
    }

    tp_format_descriptor_parse_result parsed;
    status = tp_format_descriptor_v1_parse(
        row->descriptor_bytes, row->descriptor_byte_count, &parsed, error);
    if (status != TP_STATUS_OK) {
        return scan_visit_result(TP_FORMAT_DISCOVERY_VISIT_ERROR, status);
    }
    if (parsed.outcome == TP_FORMAT_DESCRIPTOR_REJECTED) {
        char diagnostic_path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
        const char *logical_path = NULL;
        status = logical_diagnostic_path(
            row->package_path, TP_FORMAT_DISCOVERY_FAULT_DESCRIPTOR,
            diagnostic_path, &logical_path, error);
        if (status == TP_STATUS_OK) {
            status = report_one(&row->diagnostics, parsed.rejection_code,
                                NULL, logical_path, parsed.line,
                                parsed.column, parsed.message, error);
        }
        free(row->descriptor_bytes);
        row->descriptor_bytes = NULL;
        row->descriptor_byte_count = 0U;
        free(row->source_bytes);
        row->source_bytes = NULL;
        row->source_byte_count = 0U;
        return scan_visit_result(
            status == TP_STATUS_OK ? TP_FORMAT_DISCOVERY_VISIT_CONTINUE
                                   : TP_FORMAT_DISCOVERY_VISIT_ERROR,
            status);
    }
    row->owned_descriptor = parsed.owned_descriptor;

    char source_message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U] = {0};
    const tp_format_diagnostic_code source_code =
        tp_format_package_v1_source_admission_internal(
            row->source_bytes, row->source_byte_count, source_message,
            sizeof source_message);
    if (source_code != 0) {
        char diagnostic_path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
        const char *logical_path = NULL;
        status = logical_diagnostic_path(
            row->package_path, TP_FORMAT_DISCOVERY_FAULT_SOURCE,
            diagnostic_path, &logical_path, error);
        if (status == TP_STATUS_OK) {
            status = report_one(
                &row->diagnostics, source_code,
                tp_format_owned_descriptor_view(row->owned_descriptor)->id,
                logical_path, 0U, 0U, source_message, error);
        }
        free(row->descriptor_bytes);
        row->descriptor_bytes = NULL;
        row->descriptor_byte_count = 0U;
        free(row->source_bytes);
        row->source_bytes = NULL;
        row->source_byte_count = 0U;
        return scan_visit_result(
            status == TP_STATUS_OK ? TP_FORMAT_DISCOVERY_VISIT_CONTINUE
                                   : TP_FORMAT_DISCOVERY_VISIT_ERROR,
            status);
    }

    if (row->descriptor_byte_count >
            TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX - scan->admitted_bytes ||
        row->source_byte_count >
            TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX - scan->admitted_bytes -
                row->descriptor_byte_count) {
        make_limit_fail_closed(scan);
        return scan_visit_result(TP_FORMAT_DISCOVERY_VISIT_STOP_SUCCESS,
                                 TP_STATUS_OK);
    }
    scan->admitted_bytes +=
        row->descriptor_byte_count + row->source_byte_count;
    const tp_format_descriptor *descriptor =
        tp_format_owned_descriptor_view(row->owned_descriptor);
    tp_format_package_fingerprint_internal(
        descriptor->api_version, row->descriptor_bytes,
        row->descriptor_byte_count, row->source_bytes, row->source_byte_count,
        row->fingerprint);
    row->pending_compile = true;
    return scan_visit_result(TP_FORMAT_DISCOVERY_VISIT_CONTINUE,
                             TP_STATUS_OK);
}

tp_status tp_format_catalog_scan_root(
    const char *explicit_root, tp_format_catalog_scan **out_scan,
    tp_format_diagnostic_report **out_failure_diagnostics, tp_error *error) {
    if (!out_scan) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format scan requires an output");
    }
    *out_scan = NULL;
    if (out_failure_diagnostics) {
        *out_failure_diagnostics = NULL;
    }

    tp_format_catalog_scan *scan =
        (tp_format_catalog_scan *)calloc(1, sizeof *scan);
    if (!scan) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "format scan allocation failed");
    }
    scan->compile_state = TP_FORMAT_COMPILE_BATCH_PENDING;

    tp_format_discovery_result discovered;
    tp_format_discovery_failure failure;
    memset(&discovered, 0, sizeof discovered);
    memset(&failure, 0, sizeof failure);
    const tp_status discovery_status = tp_format_discovery_read_root(
        explicit_root, scan_visit_candidate, scan, &discovered, &failure,
        error);
    if (discovery_status != TP_STATUS_OK) {
        best_effort_failure_report(&failure, out_failure_diagnostics);
        tp_format_discovery_result_destroy(&discovered);
        tp_format_catalog_scan_destroy(scan);
        return discovery_status;
    }

    scan->root = discovered.root;
    discovered.root = NULL;
    scan->root_missing = discovered.root_missing;
    if (discovered.limit_fail_closed) {
        if (!scan->limit_fail_closed) {
            make_limit_fail_closed(scan);
        }
    }
    if (scan->limit_fail_closed) {
        const char *message =
            discovered.limit_fail_closed
                ? "format discovery exceeded a hard catalog limit; using native formats only"
                : "admitted format package bytes exceed the catalog limit; using native formats only";
        const tp_status status = report_one(
            &scan->root_diagnostics, TP_FORMAT_DIAGNOSTIC_CATALOG_LIMIT,
            NULL, "formats", 0U, 0U, message, error);
        tp_format_discovery_result_destroy(&discovered);
        if (status != TP_STATUS_OK) {
            tp_format_catalog_scan_destroy(scan);
            return status;
        }
        *out_scan = scan;
        return TP_STATUS_OK;
    }
    tp_format_discovery_result_destroy(&discovered);

    tp_status status = mark_duplicate_ids(scan, error);
    if (status != TP_STATUS_OK) {
        tp_format_catalog_scan_destroy(scan);
        return status;
    }
    if (scan->row_count > 1U) {
        qsort(scan->rows, scan->row_count, sizeof *scan->rows,
              scan_row_compile_compare);
    }
    scan->compile_count = 0U;
    for (size_t i = 0U; i < scan->row_count; ++i) {
        if (scan->rows[i].pending_compile) {
            scan->rows[i].candidate_index = (uint32_t)scan->compile_count++;
        }
    }
    if (scan->compile_count == 0U) {
        scan->compile_state = TP_FORMAT_COMPILE_BATCH_COMPLETE;
    }
    *out_scan = scan;
    return TP_STATUS_OK;
}

static int scan_row_catalog_compare(const void *left_value,
                                    const void *right_value) {
    const tp_format_catalog_owned_row *left =
        (const tp_format_catalog_owned_row *)left_value;
    const tp_format_catalog_owned_row *right =
        (const tp_format_catalog_owned_row *)right_value;
    if (left->available != right->available) {
        return left->available ? -1 : 1;
    }
    if (left->available) {
        const char *left_id =
            tp_format_owned_descriptor_view(left->owned_descriptor)->id;
        const char *right_id =
            tp_format_owned_descriptor_view(right->owned_descriptor)->id;
        const int id_order = strcmp(left_id, right_id);
        return id_order != 0 ? id_order : strcmp(left->key, right->key);
    }
    return strcmp(left->key, right->key);
}

size_t tp_format_catalog_scan_compile_count(
    const tp_format_catalog_scan *scan) {
    return scan ? scan->compile_count : 0U;
}

bool tp_format_catalog_scan_compile_at(
    const tp_format_catalog_scan *scan, size_t index,
    tp_format_compile_candidate *out) {
    if (!scan || !out || index >= scan->compile_count) {
        return false;
    }
    for (size_t i = 0U; i < scan->row_count; ++i) {
        const tp_format_catalog_owned_row *row = &scan->rows[i];
        if (!row->pending_compile || row->candidate_index != index) {
            continue;
        }
        *out = (tp_format_compile_candidate){
            .candidate_index = row->candidate_index,
            .package_path = row->package_path,
            .descriptor = tp_format_owned_descriptor_view(
                row->owned_descriptor),
            .fingerprint = row->fingerprint,
            .descriptor_bytes = row->descriptor_bytes,
            .descriptor_byte_count = row->descriptor_byte_count,
            .source_bytes = row->source_bytes,
            .source_byte_count = row->source_byte_count,
        };
        return true;
    }
    return false;
}

tp_format_compile_batch_state tp_format_catalog_scan_compile_state_internal(
    const tp_format_catalog_scan *scan) {
    return scan ? scan->compile_state : TP_FORMAT_COMPILE_BATCH_INELIGIBLE;
}

void tp_format_catalog_scan_invalidate_compile_internal(
    tp_format_catalog_scan *scan) {
    if (scan && scan->compile_state == TP_FORMAT_COMPILE_BATCH_PENDING) {
        scan->compile_state = TP_FORMAT_COMPILE_BATCH_INELIGIBLE;
    }
}

tp_status tp_format_catalog_scan_complete_compile_internal(
    tp_format_catalog_scan *scan, tp_format_compile_row_result *results,
    size_t result_count, tp_error *error) {
    if (!scan || (result_count != 0U && !results)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile batch requires a scan and results");
    }
    if (scan->compile_state != TP_FORMAT_COMPILE_BATCH_PENDING) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile batch is not pending");
    }
    if (result_count != scan->compile_count) {
        scan->compile_state = TP_FORMAT_COMPILE_BATCH_INELIGIBLE;
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile batch result count does not match candidates");
    }

    bool seen[TP_FORMAT_PACKAGE_MAX] = {false};
    for (size_t i = 0U; i < result_count; ++i) {
        const uint32_t index = results[i].candidate_index;
        const bool has_diagnostics = results[i].diagnostics != NULL;
        if (index >= scan->compile_count || seen[index] ||
            results[i].available == has_diagnostics) {
            scan->compile_state = TP_FORMAT_COMPILE_BATCH_INELIGIBLE;
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "compile batch contains an invalid row result");
        }
        seen[index] = true;
    }

    /* Validation above is allocation-free and precedes every mutation. */
    for (size_t i = 0U; i < scan->row_count; ++i) {
        tp_format_catalog_owned_row *row = &scan->rows[i];
        if (!row->pending_compile) {
            continue;
        }
        tp_format_compile_row_result *result = NULL;
        for (size_t j = 0U; j < result_count; ++j) {
            if (results[j].candidate_index == row->candidate_index) {
                result = &results[j];
                break;
            }
        }
        NT_ASSERT(result); /* Proven by exact count/unique/range validation. */
        tp_format_diagnostic_report_destroy(row->diagnostics);
        row->diagnostics = result->diagnostics;
        result->diagnostics = NULL;
        row->available = result->available;
        row->pending_compile = false;
    }
    if (scan->row_count > 1U) {
        qsort(scan->rows, scan->row_count, sizeof *scan->rows,
              scan_row_catalog_compare);
    }
    scan->compile_state = TP_FORMAT_COMPILE_BATCH_COMPLETE;
    return TP_STATUS_OK;
}

static tp_status finish_scan(tp_format_catalog_scan **owned_scan,
                             tp_format_catalog **out_catalog,
                             tp_error *error) {
    tp_format_catalog_scan *scan = *owned_scan;
    tp_format_catalog *catalog = tp_format_catalog_create_owned_internal(
        scan->root, scan->rows, scan->row_count, scan->root_diagnostics,
        scan->root_missing, scan->limit_fail_closed, error);
    if (!catalog) {
        return TP_STATUS_OOM;
    }
    scan->root = NULL;
    scan->rows = NULL;
    scan->row_count = 0U;
    scan->root_diagnostics = NULL;
    free(scan);
    *owned_scan = NULL;
    *out_catalog = catalog;
    return TP_STATUS_OK;
}

tp_status tp_format_catalog_scan_finish_compiled_internal(
    tp_format_catalog_scan **owned_scan, tp_format_catalog **out_catalog,
    tp_error *error) {
    if (!owned_scan || !*owned_scan || !out_catalog) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compiled scan finish requires owned scan and output");
    }
    *out_catalog = NULL;
    if ((*owned_scan)->compile_count == 0U ||
        (*owned_scan)->compile_state != TP_FORMAT_COMPILE_BATCH_COMPLETE) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compiled scan is not eligible for finalization");
    }
    return finish_scan(owned_scan, out_catalog, error);
}

tp_status tp_format_catalog_scan_finish_without_compile(
    tp_format_catalog_scan **owned_scan, tp_format_catalog **out_catalog,
    tp_error *error) {
    if (!owned_scan || !*owned_scan || !out_catalog) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format scan finish requires owned scan and output");
    }
    *out_catalog = NULL;
    tp_format_catalog_scan *scan = *owned_scan;
    if (scan->compile_count != 0U) {
        return tp_error_set(
            error, TP_STATUS_UNIMPLEMENTED,
            "format scan has %zu candidates awaiting isolated Lua compilation",
            scan->compile_count);
    }
    return finish_scan(owned_scan, out_catalog, error);
}
