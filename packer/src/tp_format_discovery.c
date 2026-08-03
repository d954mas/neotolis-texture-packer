#include "tp_format_discovery_internal.h"

#include <stdlib.h>
#include <string.h>

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
