#ifndef TP_CORE_TP_FORMAT_H
#define TP_CORE_TP_FORMAT_H

/*
 * Immutable export-format descriptors and the owned runtime catalog.
 *
 * The catalog is an explicit runtime dependency.  It is never a mutable
 * process-global registry: callers retain one immutable generation and pass it
 * into sessions/snapshots/jobs.  Runtime package discovery is intentionally a
 * separate internal builder stage; descriptor bytes alone cannot make a Lua
 * row available.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TP_FORMAT_API_VERSION 1U
#define TP_FORMAT_ID_MAX_BYTES 63U
#define TP_FORMAT_DISPLAY_NAME_MAX_BYTES 255U
#define TP_FORMAT_PACKAGE_NAME_MAX_BYTES 255U
#define TP_FORMAT_LOGICAL_ID_MAX_BYTES 63U
#define TP_FORMAT_SUFFIX_MAX_BYTES 32U
#define TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES 4095U
#define TP_FORMAT_DESCRIPTOR_MAX_BYTES 65536U
#define TP_FORMAT_SOURCE_MAX_BYTES 1048576U
#define TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX 67108864U
#define TP_FORMAT_ROOT_ENTRY_MAX 4096U
#define TP_FORMAT_PACKAGE_MAX 256U
#define TP_FORMAT_OUTPUT_MAX 32U
#define TP_FORMAT_HOST_FACT_MAX 8U
#define TP_FORMAT_JSON_DEPTH_MAX 16U
#define TP_FORMAT_JSON_NODE_MAX 2048U
#define TP_FORMAT_JSON_CONTAINER_ENTRY_MAX 256U

#define TP_FORMAT_DIAGNOSTIC_MAX 256U
#define TP_FORMAT_DIAGNOSTIC_DYNAMIC_BYTES_MAX 1048576U
#define TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES 1024U
#define TP_FORMAT_DIAGNOSTIC_FRAME_MAX 16U
#define TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES 256U

/* Canonical format/exporter identifier capacity, including NUL.  Saved project
 * v5 retains the wider historical bound; runtime package descriptors use the
 * narrower TP_FORMAT_ID_MAX_BYTES grammar. */
#define TP_EXPORTER_ID_MAX 256

typedef struct tp_export_caps {
    uint8_t transform_mask;
    bool polygons;
    bool pivot;
    bool slice9;
    bool multipage;
    bool aliases;
    bool animations;
} tp_export_caps;

typedef struct tp_format_artifact_decl {
    const char *id;
    const char *suffix;
} tp_format_artifact_decl;

typedef enum tp_format_host_fact_kind {
    TP_FORMAT_HOST_FACT_PROJECT_RESOURCE = 1,
} tp_format_host_fact_kind;

typedef enum tp_format_host_fact_missing {
    TP_FORMAT_HOST_FACT_MISSING_BASENAME_NOTICE = 1,
} tp_format_host_fact_missing;

typedef struct tp_format_host_fact_decl {
    const char *id;
    tp_format_host_fact_kind kind;
    const char *output_id;
    const char *root_marker;
    tp_format_host_fact_missing missing;
} tp_format_host_fact_decl;

typedef struct tp_format_descriptor {
    /* Zero denotes a trusted compiled-in native descriptor.  Runtime package
     * descriptors carry TP_FORMAT_API_VERSION. */
    uint32_t api_version;
    const char *id;
    const char *display_name;
    tp_export_caps caps;
    const tp_format_artifact_decl *artifacts;
    int artifact_count;
    const tp_format_host_fact_decl *host_facts;
    int host_fact_count;
} tp_format_descriptor;

typedef enum tp_format_diagnostic_severity {
    TP_FORMAT_DIAGNOSTIC_WARNING = 1,
    TP_FORMAT_DIAGNOSTIC_ERROR = 2,
} tp_format_diagnostic_severity;

typedef enum tp_format_diagnostic_phase {
    TP_FORMAT_PHASE_DISCOVERY = 1,
    TP_FORMAT_PHASE_DESCRIPTOR,
    TP_FORMAT_PHASE_COMPILE,
    TP_FORMAT_PHASE_RUNTIME,
    TP_FORMAT_PHASE_LIMIT,
    TP_FORMAT_PHASE_OUTPUT,
} tp_format_diagnostic_phase;

/* Append-only values matching docs/formats/lua-package-v1.md. */
typedef enum tp_format_diagnostic_code {
    TP_FORMAT_DIAGNOSTIC_CATALOG_LIMIT = 1,
    TP_FORMAT_DIAGNOSTIC_ROOT_NOT_DIRECTORY,
    TP_FORMAT_DIAGNOSTIC_ROOT_REPARSE,
    TP_FORMAT_DIAGNOSTIC_ROOT_IO,
    TP_FORMAT_DIAGNOSTIC_PACKAGE_NAME_INVALID,
    TP_FORMAT_DIAGNOSTIC_PACKAGE_REPARSE,
    TP_FORMAT_DIAGNOSTIC_PACKAGE_EXTRA_ENTRY,
    TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_MISSING,
    TP_FORMAT_DIAGNOSTIC_SOURCE_MISSING,
    TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TYPE,
    TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_REPARSE,
    TP_FORMAT_DIAGNOSTIC_PACKAGE_FILE_TOO_LARGE,
    TP_FORMAT_DIAGNOSTIC_PACKAGE_READ_FAILED,
    TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_UTF8,
    TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_INVALID_JSON,
    TP_FORMAT_DIAGNOSTIC_DESCRIPTOR_SCHEMA,
    TP_FORMAT_DIAGNOSTIC_API_UNSUPPORTED,
    TP_FORMAT_DIAGNOSTIC_FORMAT_ID_INVALID,
    TP_FORMAT_DIAGNOSTIC_FORMAT_ID_RESERVED,
    TP_FORMAT_DIAGNOSTIC_OUTPUT_INVALID,
    TP_FORMAT_DIAGNOSTIC_OUTPUT_CONFLICT,
    TP_FORMAT_DIAGNOSTIC_HOST_FACT_INVALID,
    TP_FORMAT_DIAGNOSTIC_DUPLICATE_FORMAT_ID,
    TP_FORMAT_DIAGNOSTIC_SOURCE_INVALID_UTF8,
    TP_FORMAT_DIAGNOSTIC_SOURCE_BINARY,
    TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
    TP_FORMAT_DIAGNOSTIC_COMPILE_WORKER_FAILED,
    TP_FORMAT_DIAGNOSTIC_COMPILE_PROTOCOL,
    TP_FORMAT_DIAGNOSTIC_COMPILE_BUDGET,
    TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT,
    TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
    TP_FORMAT_DIAGNOSTIC_HANDLER_PANIC,
    TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT,
    TP_FORMAT_DIAGNOSTIC_INSTRUCTION_LIMIT,
    TP_FORMAT_DIAGNOSTIC_HOST_CALL_LIMIT,
    TP_FORMAT_DIAGNOSTIC_OUTPUT_LIMIT,
    TP_FORMAT_DIAGNOSTIC_NOTICE_LIMIT,
    TP_FORMAT_DIAGNOSTIC_DOCUMENT_UNKNOWN,
    TP_FORMAT_DIAGNOSTIC_DOCUMENT_DUPLICATE,
    TP_FORMAT_DIAGNOSTIC_DOCUMENT_UNFINISHED,
    TP_FORMAT_DIAGNOSTIC_DOCUMENT_MISSING,
    TP_FORMAT_DIAGNOSTIC_DOCUMENT_WRITE_AFTER_FINISH,
    TP_FORMAT_DIAGNOSTIC_DOCUMENT_INVALID_UTF8,
    TP_FORMAT_DIAGNOSTIC_DOCUMENT_CONTAINS_NUL,
    TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED,
} tp_format_diagnostic_code;

typedef struct tp_format_diagnostic_frame {
    const char *text;
    uint32_t line;
} tp_format_diagnostic_frame;

typedef struct tp_format_diagnostic {
    tp_format_diagnostic_severity severity;
    tp_format_diagnostic_code code;
    tp_format_diagnostic_phase phase;
    const char *format_id;
    const char *package_path;
    uint32_t line;
    uint32_t column;
    const char *message;
    const tp_format_diagnostic_frame *frames;
    size_t frame_count;
} tp_format_diagnostic;

typedef struct tp_format_diagnostic_report tp_format_diagnostic_report;

const char *tp_format_diagnostic_code_id(tp_format_diagnostic_code code);
const char *tp_format_diagnostic_phase_id(tp_format_diagnostic_phase phase);
const char *tp_format_diagnostic_severity_id(
    tp_format_diagnostic_severity severity);
size_t tp_format_diagnostic_report_count(
    const tp_format_diagnostic_report *report);
bool tp_format_diagnostic_report_truncated(
    const tp_format_diagnostic_report *report);
const tp_format_diagnostic *tp_format_diagnostic_report_at(
    const tp_format_diagnostic_report *report, size_t index);
void tp_format_diagnostic_report_destroy(
    tp_format_diagnostic_report *report);

typedef enum tp_format_implementation_kind {
    TP_FORMAT_IMPLEMENTATION_NATIVE = 1,
    TP_FORMAT_IMPLEMENTATION_LUA = 2,
} tp_format_implementation_kind;

typedef struct tp_format_catalog tp_format_catalog;

typedef struct tp_format_catalog_row {
    tp_format_implementation_kind implementation;
    bool available;
    const char *key;
    const char *package_path;
    const char *fingerprint;
    const tp_format_descriptor *descriptor;
    const tp_format_diagnostic_report *diagnostics;
} tp_format_catalog_row;

typedef enum tp_format_resolution_state {
    TP_FORMAT_RESOLUTION_ABSENT = 0,
    TP_FORMAT_RESOLUTION_UNAVAILABLE,
    TP_FORMAT_RESOLUTION_AVAILABLE,
} tp_format_resolution_state;

typedef struct tp_format_resolution {
    tp_format_resolution_state state;
    tp_format_implementation_kind implementation;
    const tp_format_descriptor *descriptor;
    const tp_format_diagnostic_report *diagnostics;
} tp_format_resolution;

/* Immortal, allocation-free startup baseline containing every compiled-in
 * native format.  Retain/release are valid and explicit (release is a no-op for
 * this one immortal generation). */
tp_format_catalog *tp_format_catalog_native(void);
tp_format_catalog *tp_format_catalog_retain(tp_format_catalog *catalog);
void tp_format_catalog_release(tp_format_catalog *catalog);

size_t tp_format_catalog_row_count(const tp_format_catalog *catalog);
bool tp_format_catalog_row_at(const tp_format_catalog *catalog, size_t index,
                              tp_format_catalog_row *out);
const tp_format_descriptor *tp_format_catalog_find_available(
    const tp_format_catalog *catalog, const char *id);
tp_status tp_format_catalog_resolve(const tp_format_catalog *catalog,
                                    const char *id,
                                    tp_format_resolution *out,
                                    tp_error *err);
const tp_format_diagnostic_report *tp_format_catalog_root_diagnostics(
    const tp_format_catalog *catalog);
const char *tp_format_catalog_root(const tp_format_catalog *catalog);
bool tp_format_catalog_root_missing(const tp_format_catalog *catalog);
bool tp_format_catalog_limit_fail_closed(const tp_format_catalog *catalog);

/* Host byte/descriptor scan handoff.  It is deliberately not an installable
 * catalog while one or more candidates still require isolated Lua compilation.
 * Packet 2 consumes the same snapshots through its compile-validator; Packet 1
 * may finish only missing/limit/broken-only roots. */
typedef struct tp_format_catalog_scan tp_format_catalog_scan;

typedef struct tp_format_compile_candidate {
    uint32_t candidate_index;
    const char *package_path;
    const tp_format_descriptor *descriptor;
    const char *fingerprint;
    const unsigned char *descriptor_bytes;
    size_t descriptor_byte_count;
    const unsigned char *source_bytes;
    size_t source_byte_count;
} tp_format_compile_candidate;

tp_status tp_format_catalog_scan_root(
    const char *explicit_root, tp_format_catalog_scan **out_scan,
    tp_format_diagnostic_report **out_failure_diagnostics, tp_error *err);
void tp_format_catalog_scan_destroy(tp_format_catalog_scan *scan);
size_t tp_format_catalog_scan_compile_count(
    const tp_format_catalog_scan *scan);
bool tp_format_catalog_scan_compile_at(
    const tp_format_catalog_scan *scan, size_t index,
    tp_format_compile_candidate *out);
tp_status tp_format_catalog_scan_finish_without_compile(
    tp_format_catalog_scan **owned_scan, tp_format_catalog **out_catalog,
    tp_error *err);

/* Shared real-image path boundary used by both clients and worker reexec. */
tp_status tp_executable_path(char *out, size_t out_cap, tp_error *err);
tp_status tp_executable_directory(char *out, size_t out_cap, tp_error *err);
tp_status tp_format_root_from_executable(char *out, size_t out_cap,
                                         tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_FORMAT_H */
