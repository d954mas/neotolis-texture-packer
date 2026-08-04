#include "tp_format_discovery_internal.h"

#include <stdlib.h>
#include <string.h>

tp_status tp_format_discovery_visit_resolve(
    tp_format_discovery_visit_result result, bool *out_stop,
    tp_error *error) {
    if (!out_stop) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "format discovery visit requires a stop output");
    }
    *out_stop = false;
    if (result.kind == TP_FORMAT_DISCOVERY_VISIT_CONTINUE &&
        result.status == TP_STATUS_OK) {
        return TP_STATUS_OK;
    }
    if (result.kind == TP_FORMAT_DISCOVERY_VISIT_STOP_SUCCESS &&
        result.status == TP_STATUS_OK) {
        *out_stop = true;
        return TP_STATUS_OK;
    }
    if (result.kind == TP_FORMAT_DISCOVERY_VISIT_ERROR &&
        result.status != TP_STATUS_OK) {
        return result.status;
    }
    return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                        "format discovery visitor returned an invalid result");
}

void tp_format_discovered_candidate_destroy(
    tp_format_discovered_candidate *candidate) {
    if (!candidate) {
        return;
    }
    free(candidate->key);
    free(candidate->package_path);
    free(candidate->descriptor_bytes);
    free(candidate->source_bytes);
    memset(candidate, 0, sizeof *candidate);
}

void tp_format_discovery_result_destroy(tp_format_discovery_result *result) {
    if (!result) {
        return;
    }
    free(result->root);
    memset(result, 0, sizeof *result);
}
