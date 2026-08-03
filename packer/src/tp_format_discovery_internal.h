#ifndef TP_CORE_SRC_TP_FORMAT_DISCOVERY_INTERNAL_H
#define TP_CORE_SRC_TP_FORMAT_DISCOVERY_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_format.h"

typedef enum tp_format_discovery_fault_file {
    TP_FORMAT_DISCOVERY_FAULT_PACKAGE = 0,
    TP_FORMAT_DISCOVERY_FAULT_DESCRIPTOR,
    TP_FORMAT_DISCOVERY_FAULT_SOURCE,
} tp_format_discovery_fault_file;

typedef struct tp_format_discovered_candidate {
    char *key;          /* portable package spelling or invalid-name-* hash */
    char *package_path; /* logical formats/<directory>, NULL for invalid name */
    unsigned char *descriptor_bytes;
    size_t descriptor_byte_count;
    unsigned char *source_bytes;
    size_t source_byte_count;
    tp_format_diagnostic_code fault_code; /* zero when both bytes were read */
    tp_format_discovery_fault_file fault_file;
    char fault_message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
} tp_format_discovered_candidate;

typedef struct tp_format_discovery_result {
    char *root;
    size_t candidate_count;
    bool root_missing;
    bool limit_fail_closed;
} tp_format_discovery_result;

typedef struct tp_format_discovery_failure {
    tp_format_diagnostic_code code;
    char message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
} tp_format_discovery_failure;

/* Called synchronously after one candidate's exact bytes are snapshotted and
 * before discovery reads the next candidate. The visitor may retain pointer
 * members by moving them out and setting them to NULL; discovery destroys all
 * members left in candidate after the call. */
typedef tp_status (*tp_format_discovery_candidate_visitor)(
    void *context, tp_format_discovered_candidate *candidate,
    tp_error *error);

/* Platform implementation. Root is explicit UTF-8 and is never derived from
 * CWD/PATH/argv. A missing root and a fail-closed limit are successful complete
 * results. Root identity/enumeration/OOM faults return non-OK, set failure, and
 * leave out empty/ineligible. */
tp_status tp_format_discovery_read_root(
    const char *root, tp_format_discovery_candidate_visitor visit_candidate,
    void *visit_context, tp_format_discovery_result *out,
    tp_format_discovery_failure *failure, tp_error *error);

void tp_format_discovered_candidate_destroy(
    tp_format_discovered_candidate *candidate);
void tp_format_discovery_result_destroy(tp_format_discovery_result *result);

#endif /* TP_CORE_SRC_TP_FORMAT_DISCOVERY_INTERNAL_H */
