#include "tp_format_discovery_internal.h"

#include <stdlib.h>
#include <string.h>

void tp_format_discovery_result_destroy(tp_format_discovery_result *result) {
    if (!result) {
        return;
    }
    for (size_t i = 0U; i < result->candidate_count; ++i) {
        tp_format_discovered_candidate *candidate = &result->candidates[i];
        free(candidate->key);
        free(candidate->package_path);
        free(candidate->descriptor_bytes);
        free(candidate->source_bytes);
    }
    free(result->candidates);
    free(result->root);
    memset(result, 0, sizeof *result);
}
