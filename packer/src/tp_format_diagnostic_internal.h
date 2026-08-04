#ifndef TP_CORE_SRC_TP_FORMAT_DIAGNOSTIC_INTERNAL_H
#define TP_CORE_SRC_TP_FORMAT_DIAGNOSTIC_INTERNAL_H

#include <stddef.h>

#include "tp_core/tp_format.h"

/* Exact stable vocabulary semantics from lua-package-v1.md. Ordinary codes
 * have one normal error phase; diagnostics_truncated is a warning in any
 * valid phase. The canonical marker helper additionally validates the fixed
 * allocation-free marker shape used by owned reports and wire protocols. */
bool tp_format_diagnostic_semantics_valid_internal(
    const tp_format_diagnostic *diagnostic);
bool tp_format_diagnostic_truncation_marker_canonical_internal(
    const tp_format_diagnostic *diagnostic);

/* Internal owned-report builder. Input diagnostics and every string/frame they
 * reference are borrowed only for the duration of append; the report stores a
 * deep copy. Nullable strings remain NULL in the owned public view.
 *
 * A hard diagnostic limit is not an operation failure. Append keeps the bounded
 * prefix when an individual string/frame list is too large, or omits the whole
 * diagnostic when the report count/byte budget is exhausted, and exposes one
 * allocation-free diagnostics_truncated marker as the final public row.
 */
tp_status tp_format_diagnostic_report_create_internal(
    tp_format_diagnostic_report **out, tp_error *err);

tp_status tp_format_diagnostic_report_append_internal(
    tp_format_diagnostic_report *report,
    const tp_format_diagnostic *diagnostic, tp_error *err);

/* Explicitly records an omission made by an enclosing catalog/job budget. This
 * never allocates and preserves the phase of the first truncation. */
tp_status tp_format_diagnostic_report_mark_truncated_internal(
    tp_format_diagnostic_report *report,
    tp_format_diagnostic_phase phase, tp_error *err);

/* Deep-copy helpers for transferring reports between independently owned rows
 * and job results. Merge preserves source insertion order. An allocation error
 * leaves a valid, possibly prefix-extended destination for the caller to free. */
tp_status tp_format_diagnostic_report_clone_internal(
    const tp_format_diagnostic_report *source,
    tp_format_diagnostic_report **out, tp_error *err);
tp_status tp_format_diagnostic_report_merge_internal(
    tp_format_diagnostic_report *destination,
    const tp_format_diagnostic_report *source, tp_error *err);

/* Exact heap bytes currently owned by this report, including the opaque report
 * object, vector capacity, and per-diagnostic storage blocks. Catalog/job
 * builders use this for their wider aggregate diagnostic budgets. */
size_t tp_format_diagnostic_report_dynamic_bytes_internal(
    const tp_format_diagnostic_report *report);

#endif /* TP_CORE_SRC_TP_FORMAT_DIAGNOSTIC_INTERNAL_H */
