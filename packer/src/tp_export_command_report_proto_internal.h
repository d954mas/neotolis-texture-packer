#ifndef TP_EXPORT_COMMAND_REPORT_PROTO_INTERNAL_H
#define TP_EXPORT_COMMAND_REPORT_PROTO_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "tp_export_command_report_internal.h"

tp_status tp_export_command_outcome_proto_encode(
    const tp_export_command_outcome *outcome, uint8_t **out_bytes,
    size_t *out_length, tp_error *err);
tp_status tp_export_command_outcome_proto_decode(
    const uint8_t *bytes, size_t length, tp_export_command_outcome *out,
    tp_error *err);

#endif
