#ifndef TP_BUILD_SRC_TP_LUA_HOST_INTERNAL_H
#define TP_BUILD_SRC_TP_LUA_HOST_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_cancel.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frozen API-v1 execution ceilings from docs/formats/lua-package-v1.md. */
#define TP_LUA_LIVE_BYTES_MAX ((size_t)134217728U)
#define TP_LUA_INSTRUCTION_MAX UINT64_C(250000000)
#define TP_LUA_HOOK_INTERVAL 10000
#define TP_LUA_HOST_CALL_MAX UINT64_C(8388608)
#define TP_LUA_WRITER_CALL_MAX UINT64_C(4194304)
#define TP_LUA_WRITER_ARGUMENT_MAX_BYTES ((size_t)1048576U)
#define TP_LUA_FACT_VALUE_MAX_BYTES ((size_t)4095U)
#define TP_LUA_DOCUMENT_MAX_BYTES ((size_t)67108864U)
#define TP_LUA_DOCUMENT_TOTAL_MAX_BYTES ((size_t)67108864U)
#define TP_LUA_NOTICE_MAX ((size_t)4096U)
#define TP_LUA_NOTICE_MESSAGE_MAX_BYTES ((size_t)1024U)
#define TP_LUA_NOTICE_TOTAL_MAX_BYTES ((size_t)1048576U)

typedef struct tp_lua_document_decl {
    const char *id;
} tp_lua_document_decl;

typedef struct tp_lua_fact_value {
    const char *id;
    const char *value;
} tp_lua_fact_value;

/* This is a prepared projection, not a policy input. The caller has already
 * intersected capabilities and emitted loss notices. These booleans only tell
 * the Lua view which fields the projection hides; the kernel never derives
 * them from a descriptor or project. Page image strings are planned PNG
 * basenames, never paths. */
typedef struct tp_lua_projected_ir {
    const tp_export_ir *value;
    const char *const *page_images;
    size_t page_image_count;
    bool polygons_visible;
    bool pivot_visible;
    bool slice9_visible;
    bool aliases_visible;
} tp_lua_projected_ir;

typedef struct tp_lua_document {
    char *id;
    unsigned char *bytes;
    size_t byte_count;
} tp_lua_document;

typedef struct tp_lua_notice {
    char *message;
} tp_lua_notice;

typedef struct tp_lua_runtime_result {
    tp_lua_document *documents;
    size_t document_count;
    tp_lua_notice *notices;
    size_t notice_count;
    tp_format_diagnostic_report *diagnostics;
} tp_lua_runtime_result;

typedef struct tp_lua_runtime_input {
    const unsigned char *source;
    size_t source_byte_count;
    const char *format_id;
    const char *package_path; /* logical formats/.../export.lua path */
    const tp_lua_projected_ir *projected_ir;
    const tp_lua_document_decl *documents;
    size_t document_count;
    const tp_lua_fact_value *facts;
    size_t fact_count;
    const tp_cancel_token *cancel;
} tp_lua_runtime_input;

/* Text-only compile admission. A successful compile returns OK with a NULL
 * report. A package/source/limit rejection returns INVALID_ARGUMENT with an
 * owned diagnostic report. Genuine allocator/report OOM returns OOM and never
 * relabels itself as a package memory_limit. The chunk is never executed. */
tp_status tp_lua_compile_validate(
    const unsigned char *source, size_t source_byte_count,
    const char *format_id, const char *package_path,
    tp_format_diagnostic_report **out_report, tp_error *error);

/* Executes one exact source snapshot in a fresh state. Success transfers all
 * documents/notices to `out`. A handled package/contract/limit failure returns
 * INVALID_ARGUMENT and transfers its diagnostic report. Cancellation returns
 * CANCELLED. Genuine host OOM returns OOM. No filesystem, pixel, path-planning,
 * capability-policy, PNG, or publication operation is reachable here. */
tp_status tp_lua_runtime_serialize(const tp_lua_runtime_input *input,
                                   tp_lua_runtime_result *out,
                                   tp_error *error);
void tp_lua_runtime_result_destroy(tp_lua_runtime_result *result);

#ifdef TP_ENABLE_TEST_SEAMS
typedef struct tp_lua_test_limits {
    size_t live_bytes;
    uint64_t instructions;
    int hook_interval;
    uint64_t host_calls;
    uint64_t writer_calls;
    size_t writer_argument_bytes;
    size_t document_bytes;
    size_t document_total_bytes;
    size_t notices;
    size_t notice_message_bytes;
    size_t notice_total_bytes;
    bool fail_next_allocation;
    bool fail_runtime_allocation_after_load;
    bool disable_instruction_hook;
} tp_lua_test_limits;

/* Process-local and worker-test-only. NULL restores the frozen production
 * limits. Shipping targets have neither this symbol nor mutable ceilings. */
void tp_lua__test_set_limits(const tp_lua_test_limits *limits);
void tp_lua__test_fail_next_allocation(void);
void tp_lua__test_trigger_panic(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TP_BUILD_SRC_TP_LUA_HOST_INTERNAL_H */
