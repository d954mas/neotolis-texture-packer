#ifndef TP_CORE_SRC_TP_FORMAT_CATALOG_INTERNAL_H
#define TP_CORE_SRC_TP_FORMAT_CATALOG_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_format.h"
#include "tp_format_descriptor_internal.h"

typedef struct tp_exporter tp_exporter;

typedef struct tp_format_catalog_owned_row {
    tp_format_implementation_kind implementation;
    bool available;
    char *key;
    char *package_path;
    char fingerprint[33];
    tp_format_owned_descriptor *owned_descriptor;
    tp_format_diagnostic_report *diagnostics;
    unsigned char *descriptor_bytes;
    size_t descriptor_byte_count;
    unsigned char *source_bytes;
    size_t source_byte_count;
    const tp_exporter *native_binding; /* non-NULL only for native/test rows */
    bool pending_compile; /* scan-only; never present in an active catalog */
    uint32_t candidate_index;
} tp_format_catalog_owned_row;

typedef enum tp_format_compile_batch_state {
    TP_FORMAT_COMPILE_BATCH_PENDING = 0,
    TP_FORMAT_COMPILE_BATCH_COMPLETE,
    TP_FORMAT_COMPILE_BATCH_INELIGIBLE,
} tp_format_compile_batch_state;

/* One validated worker result. The report remains caller-owned until the
 * complete batch is accepted, then ownership is transferred into the scan. */
typedef struct tp_format_compile_row_result {
    uint32_t candidate_index;
    bool available;
    tp_format_diagnostic_report *diagnostics;
} tp_format_compile_row_result;

tp_format_compile_batch_state tp_format_catalog_scan_compile_state_internal(
    const tp_format_catalog_scan *scan);
tp_status tp_format_catalog_scan_complete_compile_internal(
    tp_format_catalog_scan *scan, tp_format_compile_row_result *results,
    size_t result_count, tp_error *error);
void tp_format_catalog_scan_invalidate_compile_internal(
    tp_format_catalog_scan *scan);
tp_status tp_format_catalog_scan_finish_compiled_internal(
    tp_format_catalog_scan **owned_scan, tp_format_catalog **out_catalog,
    tp_error *error);

tp_format_catalog *tp_format_catalog_create_owned_internal(
    char *owned_root, tp_format_catalog_owned_row *owned_rows,
    size_t owned_row_count,
    tp_format_diagnostic_report *owned_root_diagnostics,
    bool root_missing, bool limit_fail_closed, tp_error *error);

const tp_exporter *tp_format_catalog_exporter_find(
    const tp_format_catalog *catalog, const char *id);

#ifdef TP_ENABLE_TEST_SEAMS
/* Test-only construction is explicit and catalog-local; it never mutates a
 * process-global registry. Descriptors/exporter objects are borrowed static
 * test fixtures and must outlive the returned catalog. */
tp_status tp_format_catalog__test_create(
    const tp_exporter *const *exporters, size_t exporter_count,
    tp_format_catalog **out, tp_error *error);
#endif

#endif /* TP_CORE_SRC_TP_FORMAT_CATALOG_INTERNAL_H */
