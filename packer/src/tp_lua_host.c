#include "tp_lua_host_private_internal.h"
#include "tp_format_diagnostic_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nt_assert.h"
#include "lauxlib.h"

#include "tp_format_package_internal.h"
#include "tp_utf8_internal.h"

_Static_assert(sizeof(lua_Integer) == 8U,
               "Lua API v1 requires a 64-bit lua_Integer");
_Static_assert(sizeof(lua_Number) == 8U && DBL_MANT_DIG == 53 &&
                   DBL_MAX_EXP == 1024,
               "Lua API v1 requires IEEE-754 binary64 lua_Number");
_Static_assert(TP_LUA_HOOK_INTERVAL > 0,
               "Lua instruction hook interval must be positive");

static const unsigned char g_lua_failure_sentinel;

#ifdef TP_ENABLE_TEST_SEAMS
static tp_lua_test_limits g_test_limits;

void tp_lua__test_set_limits(const tp_lua_test_limits *limits) {
    g_test_limits = limits ? *limits : (tp_lua_test_limits){0};
}

void tp_lua__test_fail_next_allocation(void) {
    g_test_limits.fail_next_allocation = true;
}
#endif

tp_lua_effective_limits tp_lua_effective_limits_get(void) {
    tp_lua_effective_limits result = {
        .live_bytes = TP_LUA_LIVE_BYTES_MAX,
        .instructions = TP_LUA_INSTRUCTION_MAX,
        .hook_interval = TP_LUA_HOOK_INTERVAL,
        .host_calls = TP_LUA_HOST_CALL_MAX,
        .writer_calls = TP_LUA_WRITER_CALL_MAX,
        .writer_argument_bytes = TP_LUA_WRITER_ARGUMENT_MAX_BYTES,
        .document_bytes = TP_LUA_DOCUMENT_MAX_BYTES,
        .document_total_bytes = TP_LUA_DOCUMENT_TOTAL_MAX_BYTES,
        .notices = TP_LUA_NOTICE_MAX,
        .notice_message_bytes = TP_LUA_NOTICE_MESSAGE_MAX_BYTES,
        .notice_total_bytes = TP_LUA_NOTICE_TOTAL_MAX_BYTES,
        .instruction_hook_enabled = true,
    };
#ifdef TP_ENABLE_TEST_SEAMS
#define TP_TEST_LIMIT(field)                 \
    do {                                     \
        if (g_test_limits.field != 0U)       \
            result.field = g_test_limits.field; \
    } while (0)
    TP_TEST_LIMIT(live_bytes);
    TP_TEST_LIMIT(instructions);
    if (g_test_limits.hook_interval > 0)
        result.hook_interval = g_test_limits.hook_interval;
    TP_TEST_LIMIT(host_calls);
    TP_TEST_LIMIT(writer_calls);
    TP_TEST_LIMIT(writer_argument_bytes);
    TP_TEST_LIMIT(document_bytes);
    TP_TEST_LIMIT(document_total_bytes);
    TP_TEST_LIMIT(notices);
    TP_TEST_LIMIT(notice_message_bytes);
    TP_TEST_LIMIT(notice_total_bytes);
    if (g_test_limits.disable_instruction_hook)
        result.instruction_hook_enabled = false;
#undef TP_TEST_LIMIT
#endif
    return result;
}

static bool allocator_pending_matches(const tp_lua_allocator *allocator,
                                      void *pointer, size_t old_size,
                                      size_t new_size) {
    return allocator->failure_pending &&
           allocator->pending_pointer == pointer &&
           allocator->pending_old_size == old_size &&
           allocator->pending_new_size == new_size;
}

static void allocator_record_failure(tp_lua_allocator *allocator,
                                     void *pointer, size_t old_size,
                                     size_t new_size, bool limit_hit) {
    if (!allocator_pending_matches(allocator, pointer, old_size, new_size)) {
        allocator->pending_prior_limit_hit = allocator->limit_hit;
        allocator->pending_prior_host_oom = allocator->host_oom;
    }
    allocator->limit_hit = allocator->limit_hit || limit_hit;
    allocator->host_oom = allocator->host_oom || !limit_hit;
    allocator->failure_pending = true;
    allocator->pending_pointer = pointer;
    allocator->pending_old_size = old_size;
    allocator->pending_new_size = new_size;
}

void *tp_lua_allocator_fn(void *userdata, void *pointer, size_t old_size,
                          size_t new_size) {
    tp_lua_allocator *allocator = (tp_lua_allocator *)userdata;
    NT_ASSERT(allocator);
    if (new_size == 0U) {
        if (pointer) {
            NT_ASSERT(old_size <= allocator->live_bytes);
            allocator->live_bytes -= old_size;
        }
        free(pointer);
        return NULL;
    }
    const bool retries_pending_failure = allocator_pending_matches(
        allocator, pointer, old_size, new_size);
    if (allocator->fail_next) {
        allocator->fail_next = false;
        allocator_record_failure(allocator, pointer, old_size, new_size,
                                 false);
        return NULL;
    }
    const size_t retained = pointer ? old_size : 0U;
    NT_ASSERT(retained <= allocator->live_bytes);
    if (new_size > SIZE_MAX - (allocator->live_bytes - retained) ||
        allocator->live_bytes - retained + new_size > allocator->limit) {
        allocator_record_failure(allocator, pointer, old_size, new_size,
                                 true);
        return NULL;
    }
    void *resized = realloc(pointer, new_size);
    if (!resized) {
        allocator_record_failure(allocator, pointer, old_size, new_size,
                                 false);
        return NULL;
    }
    allocator->live_bytes = allocator->live_bytes - retained + new_size;
    if (retries_pending_failure) {
        allocator->limit_hit = allocator->pending_prior_limit_hit;
        allocator->host_oom = allocator->pending_prior_host_oom;
        allocator->failure_pending = false;
        allocator->pending_pointer = NULL;
        allocator->pending_old_size = 0U;
        allocator->pending_new_size = 0U;
    }
    return resized;
}

int tp_lua_panic(lua_State *state) {
    (void)state;
    abort();
}

tp_lua_runtime_context *tp_lua_context(lua_State *state) {
    if (!state) return NULL;
    return *(tp_lua_runtime_context **)lua_getextraspace(state);
}

static void copy_failure_message(char *destination, const char *message) {
    const char *source = message ? message : "Lua handler failed";
    (void)snprintf(destination,
                   TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U, "%s",
                   source);
    tp_error_trim_partial_utf8(destination);
}

void tp_lua_fail(tp_lua_runtime_context *context,
                 tp_format_diagnostic_code code,
                 tp_format_diagnostic_phase phase, const char *message) {
    if (!context || context->failure_set) return;
    context->failure_set = true;
    context->failure_code = code;
    context->failure_phase = phase;
    copy_failure_message(context->failure_message, message);
}

static void push_sentinel(lua_State *state) {
    lua_pushlightuserdata(state, (void *)&g_lua_failure_sentinel);
}

int tp_lua_raise(lua_State *state, tp_format_diagnostic_code code,
                 tp_format_diagnostic_phase phase, const char *message) {
    tp_lua_fail(tp_lua_context(state), code, phase, message);
    push_sentinel(state);
    return lua_error(state);
}

bool tp_lua_text_valid(const char *text, size_t byte_count) {
    return text && !memchr(text, '\0', byte_count) &&
           tp_utf8_validate_bytes(text, byte_count, TP_STATUS_INVALID_UTF8,
                                  "Lua text", NULL) == TP_STATUS_OK;
}

bool tp_lua_gate(lua_State *state, bool writer_call) {
    tp_lua_runtime_context *context = tp_lua_context(state);
    if (!context) {
        push_sentinel(state);
        return false;
    }
    if (tp_cancel_requested(context->input->cancel)) {
        context->cancelled = true;
        push_sentinel(state);
        return false;
    }
    if (context->host_calls >= context->limits.host_calls) {
        tp_lua_fail(context, TP_FORMAT_DIAGNOSTIC_HOST_CALL_LIMIT,
                    TP_FORMAT_PHASE_LIMIT,
                    "Lua host-call count exceeded its fixed limit");
        push_sentinel(state);
        return false;
    }
    context->host_calls++;
    if (writer_call) {
        if (context->writer_calls >= context->limits.writer_calls) {
            tp_lua_fail(context, TP_FORMAT_DIAGNOSTIC_HOST_CALL_LIMIT,
                        TP_FORMAT_PHASE_LIMIT,
                        "Lua writer-call count exceeded its fixed limit");
            push_sentinel(state);
            return false;
        }
        context->writer_calls++;
    }
    return true;
}

static void instruction_hook(lua_State *state, lua_Debug *debug) {
    (void)debug;
    tp_lua_runtime_context *context = tp_lua_context(state);
    if (!context) {
        push_sentinel(state);
        (void)lua_error(state);
        return;
    }
    if (tp_cancel_requested(context->input->cancel)) {
        context->cancelled = true;
        push_sentinel(state);
        (void)lua_error(state);
        return;
    }
    const uint64_t interval = (uint64_t)context->limits.hook_interval;
    if (context->instructions > UINT64_MAX - interval ||
        context->instructions + interval >= context->limits.instructions) {
        context->instructions = context->limits.instructions;
        tp_lua_fail(context, TP_FORMAT_DIAGNOSTIC_INSTRUCTION_LIMIT,
                    TP_FORMAT_PHASE_LIMIT,
                    "Lua instruction count exceeded its fixed limit");
        push_sentinel(state);
        (void)lua_error(state);
        return;
    }
    context->instructions += interval;
}

static void sanitize_label(char *destination, size_t capacity,
                           const char *source) {
    if (!destination || capacity == 0U) return;
    if (!source || !source[0]) source = "?";
    size_t written = 0U;
    while (source[written] && written + 1U < capacity) {
        const unsigned char value = (unsigned char)source[written];
        destination[written] =
            ((value >= 'a' && value <= 'z') ||
             (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9') || value == '_' || value == '.' ||
             value == '-')
                ? (char)value
                : '_';
        written++;
    }
    destination[written] = '\0';
}

static int runtime_error_handler(lua_State *state) {
    tp_lua_runtime_context *context = tp_lua_context(state);
    if (!context) return 1;
    context->failure_frame_count = 0U;
    char target_source[TP_FORMAT_ID_MAX_BYTES + 32U];
    (void)snprintf(target_source, sizeof target_source,
                   "@formats/%s/export.lua", context->input->format_id);
    for (int level = 0;
         context->failure_frame_count < TP_FORMAT_DIAGNOSTIC_FRAME_MAX;
         ++level) {
        lua_Debug frame;
        memset(&frame, 0, sizeof frame);
        if (!lua_getstack(state, level, &frame)) break;
        if (!lua_getinfo(state, "nSl", &frame)) continue;
        if (frame.currentline <= 0 || !frame.source ||
            strcmp(frame.source, target_source) != 0 || !frame.what ||
            strcmp(frame.what, "C") == 0) {
            continue;
        }
        char label[96];
        sanitize_label(label, sizeof label, frame.name);
        char *text =
            context->failure_frame_text[context->failure_frame_count];
        (void)snprintf(text, TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES + 1U,
                       "@formats/%s/export.lua:%s",
                       context->input->format_id, label);
        tp_format_diagnostic_frame *output =
            &context->failure_frames[context->failure_frame_count];
        output->text = text;
        output->line =
            frame.currentline > 0 ? (uint32_t)frame.currentline : 0U;
        context->failure_frame_count++;
    }
    return 1;
}

static tp_status append_report(
    tp_format_diagnostic_report **out_report,
    tp_format_diagnostic_code code, tp_format_diagnostic_phase phase,
    const char *format_id, const char *package_path, uint32_t line,
    const char *message, const tp_format_diagnostic_frame *frames,
    size_t frame_count, tp_error *error) {
    tp_format_diagnostic_report *report = NULL;
    tp_status status =
        tp_format_diagnostic_report_create_internal(&report, error);
    if (status != TP_STATUS_OK) return status;
    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = code,
        .phase = phase,
        .format_id = format_id,
        .package_path = package_path,
        .line = line,
        .message = message,
        .frames = frames,
        .frame_count = frame_count,
    };
    status =
        tp_format_diagnostic_report_append_internal(report, &diagnostic, error);
    if (status != TP_STATUS_OK) {
        tp_format_diagnostic_report_destroy(report);
        return status;
    }
    *out_report = report;
    return TP_STATUS_OK;
}

static uint32_t compile_error_line(const char *message) {
    if (!message) return 0U;
    const char *cursor = strchr(message, ':');
    while (cursor) {
        cursor++;
        if (*cursor >= '0' && *cursor <= '9') {
            uint64_t value = 0U;
            while (*cursor >= '0' && *cursor <= '9') {
                value = value * 10U + (uint64_t)(*cursor - '0');
                if (value > UINT32_MAX) return 0U;
                cursor++;
            }
            if (*cursor == ':') return (uint32_t)value;
        }
        cursor = strchr(cursor, ':');
    }
    return 0U;
}

static void lua_error_message(lua_State *state, char *out, size_t capacity,
                              const char *fallback) {
    size_t length = 0U;
    const char *message = lua_type(state, -1) == LUA_TSTRING
                              ? lua_tolstring(state, -1, &length)
                              : NULL;
    if (!message || length == 0U || length > TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES ||
        length + 1U > capacity || !tp_lua_text_valid(message, length)) {
        copy_failure_message(out, fallback);
        return;
    }
    memcpy(out, message, length);
    out[length] = '\0';
}

static lua_State *new_state(tp_lua_allocator *allocator) {
#ifdef TP_ENABLE_TEST_SEAMS
    allocator->fail_next = g_test_limits.fail_next_allocation;
    g_test_limits.fail_next_allocation = false;
#endif
    lua_State *state = lua_newstate(tp_lua_allocator_fn, allocator, 0U);
    if (state) {
        (void)lua_atpanic(state, tp_lua_panic);
    }
    return state;
}

tp_status tp_lua_compile_validate(
    const unsigned char *source, size_t source_byte_count,
    const char *format_id, const char *package_path,
    tp_format_diagnostic_report **out_report, tp_error *error) {
    if (!out_report) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua compile validation requires a report output");
    }
    *out_report = NULL;
    if ((!source && source_byte_count != 0U) || !format_id || !package_path ||
        source_byte_count > TP_FORMAT_SOURCE_MAX_BYTES) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua compile validation input is invalid");
    }
    char message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
    const tp_format_diagnostic_code admission =
        tp_format_package_v1_source_admission_internal(
            source, source_byte_count, message, sizeof message);
    if (admission != 0) {
        const tp_status report_status = append_report(
            out_report, admission, TP_FORMAT_PHASE_COMPILE, format_id,
            package_path, 0U, message, NULL, 0U, error);
        return report_status == TP_STATUS_OK ? TP_STATUS_INVALID_ARGUMENT
                                             : report_status;
    }
    tp_lua_effective_limits limits = tp_lua_effective_limits_get();
    tp_lua_allocator allocator = {.limit = limits.live_bytes};
    lua_State *state = new_state(&allocator);
    if (!state) {
        if (allocator.host_oom)
            return tp_error_set(error, TP_STATUS_OOM,
                                "Lua state allocation failed");
        const tp_status report_status = append_report(
            out_report, TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT,
            TP_FORMAT_PHASE_LIMIT, format_id, package_path, 0U,
            "Lua compile allocator exceeded its fixed live-byte limit",
            NULL, 0U, error);
        return report_status == TP_STATUS_OK ? TP_STATUS_INVALID_ARGUMENT
                                             : report_status;
    }
    char chunk_name[TP_FORMAT_ID_MAX_BYTES + 32U];
    (void)snprintf(chunk_name, sizeof chunk_name,
                   "@formats/%s/export.lua", format_id);
    const unsigned char *source_text =
        source ? source : (const unsigned char *)"";
    const int load_status = luaL_loadbufferx(
        state, (const char *)source_text, source_byte_count, chunk_name, "t");
    if (load_status != LUA_OK) {
        lua_error_message(state, message, sizeof message,
                          "Lua text compilation failed");
    }
    const bool limit_hit = allocator.limit_hit;
    const bool host_oom = allocator.host_oom;
    lua_close(state);
    if (load_status == LUA_OK) return TP_STATUS_OK;
    if (host_oom)
        return tp_error_set(error, TP_STATUS_OOM,
                            "Lua compile allocation failed");
    const tp_format_diagnostic_code code =
        limit_hit ? TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT
                  : TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR;
    const tp_format_diagnostic_phase phase =
        limit_hit ? TP_FORMAT_PHASE_LIMIT : TP_FORMAT_PHASE_COMPILE;
    const tp_status report_status = append_report(
        out_report, code, phase, format_id, package_path,
        limit_hit ? 0U : compile_error_line(message), message, NULL, 0U,
        error);
    return report_status == TP_STATUS_OK ? TP_STATUS_INVALID_ARGUMENT
                                         : report_status;
}

static bool c_text_valid(const char *text, size_t max_bytes) {
    if (!text) return false;
    const size_t length = strlen(text);
    return length <= max_bytes &&
           tp_lua_text_valid(text, length);
}

static tp_status validate_runtime_input(const tp_lua_runtime_input *input,
                                        tp_error *error) {
    if (!input || (!input->source && input->source_byte_count != 0U) ||
        !input->format_id ||
        !input->package_path || !input->projected_ir ||
        !input->projected_ir->value ||
        input->source_byte_count > TP_FORMAT_SOURCE_MAX_BYTES ||
        input->document_count == 0U ||
        input->document_count > TP_FORMAT_OUTPUT_MAX ||
        !input->documents || input->fact_count > TP_FORMAT_HOST_FACT_MAX ||
        (input->fact_count > 0U && !input->facts)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua runtime input is incomplete or out of bounds");
    }
    if (!c_text_valid(input->format_id, TP_FORMAT_ID_MAX_BYTES) ||
        !c_text_valid(input->package_path,
                      TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua runtime identity text is invalid");
    }
    tp_status status =
        tp_export_ir_validate(input->projected_ir->value, error);
    if (status != TP_STATUS_OK) return status;
    if (input->projected_ir->page_image_count !=
            (size_t)input->projected_ir->value->page_count ||
        (input->projected_ir->page_image_count > 0U &&
         !input->projected_ir->page_images)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua projected IR page image table is invalid");
    }
    for (size_t i = 0U; i < input->projected_ir->page_image_count; ++i) {
        const char *image = input->projected_ir->page_images[i];
        if (!c_text_valid(image, TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES) ||
            strchr(image, '/') || strchr(image, '\\') || strchr(image, ':')) {
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "Lua page image must be a prepared basename");
        }
    }
    for (size_t i = 0U; i < input->document_count; ++i) {
        if (!c_text_valid(input->documents[i].id,
                          TP_FORMAT_LOGICAL_ID_MAX_BYTES)) {
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "Lua document id is invalid");
        }
        for (size_t j = 0U; j < i; ++j) {
            if (strcmp(input->documents[i].id,
                       input->documents[j].id) == 0) {
                return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                    "Lua document ids must be unique");
            }
        }
    }
    for (size_t i = 0U; i < input->fact_count; ++i) {
        if (!c_text_valid(input->facts[i].id,
                          TP_FORMAT_LOGICAL_ID_MAX_BYTES) ||
            !c_text_valid(input->facts[i].value,
                          TP_LUA_FACT_VALUE_MAX_BYTES)) {
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "Lua prepared host fact is invalid");
        }
        for (size_t j = 0U; j < i; ++j) {
            if (strcmp(input->facts[i].id, input->facts[j].id) == 0) {
                return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                    "Lua host fact ids must be unique");
            }
        }
    }
    char admission_message[128];
    if (tp_format_package_v1_source_admission_internal(
            input->source, input->source_byte_count, admission_message,
            sizeof admission_message) != 0) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT, "%s",
                            admission_message);
    }
    return TP_STATUS_OK;
}

static int install_thunk(lua_State *state) {
    tp_lua_runtime_context *context = tp_lua_context(state);
    tp_error error = {{0}};
    int environment = 0;
    if (tp_lua_bindings_install(state, context, &environment, &error) !=
        TP_STATUS_OK) {
        context->allocator.host_oom = true;
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                            TP_FORMAT_PHASE_RUNTIME,
                            error.msg[0] ? error.msg
                                         : "Lua sandbox setup failed");
    }
    lua_pushvalue(state, environment);
    return 1;
}

/* The only unprotected calls in the controller manipulate already-reserved
 * stack slots. Userdata construction and the handler itself execute inside
 * this C frame, which is invoked by an outer lua_pcall. */
static int invoke_handler_thunk(lua_State *state) {
    tp_lua_runtime_context *context = tp_lua_context(state);
    if (!context || lua_gettop(state) != 1 ||
        lua_type(state, 1) != LUA_TFUNCTION) {
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT,
                            TP_FORMAT_PHASE_RUNTIME,
                            "Lua handler invocation is invalid");
    }
    if (!lua_checkstack(state, 3)) {
        if (context->allocator.limit_hit || context->allocator.host_oom) {
            push_sentinel(state);
            return lua_error(state);
        }
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                            TP_FORMAT_PHASE_RUNTIME,
                            "Lua handler stack allocation failed");
    }
    lua_pushvalue(state, 1);
    tp_lua_push_atlas(state, context);
    tp_lua_push_host(state, context);
    lua_call(state, 2, LUA_MULTRET);
    if (lua_gettop(state) != 1) {
        lua_settop(state, 1);
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT,
                            TP_FORMAT_PHASE_RUNTIME,
                            "Lua handler must return no values");
    }
    return 0;
}

static void context_destroy(tp_lua_runtime_context *context) {
    if (!context) return;
    for (size_t i = 0U; i < context->document_count; ++i)
        tp_sb_free(&context->documents[i].bytes);
    for (size_t i = 0U; i < context->notice_count; ++i)
        free(context->notices[i].message);
    free(context->notices);
    context->notices = NULL;
}

static tp_status transfer_success(tp_lua_runtime_context *context,
                                  tp_lua_runtime_result *out,
                                  tp_error *error) {
    tp_lua_document *documents = (tp_lua_document *)calloc(
        context->document_count, sizeof *documents);
    if (!documents)
        return tp_error_set(error, TP_STATUS_OOM,
                            "Lua document result allocation failed");
    tp_lua_notice *notices = NULL;
    if (context->notice_count > 0U) {
        notices = (tp_lua_notice *)calloc(context->notice_count,
                                          sizeof *notices);
        if (!notices) {
            free(documents);
            return tp_error_set(error, TP_STATUS_OOM,
                                "Lua notice result allocation failed");
        }
    }
    for (size_t i = 0U; i < context->document_count; ++i) {
        const size_t id_length = strlen(context->documents[i].id);
        documents[i].id = (char *)malloc(id_length + 1U);
        if (!documents[i].id) {
            tp_lua_runtime_result partial = {
                .documents = documents,
                .document_count = context->document_count,
                .notices = notices,
                .notice_count = context->notice_count,
            };
            tp_lua_runtime_result_destroy(&partial);
            return tp_error_set(error, TP_STATUS_OOM,
                                "Lua document id allocation failed");
        }
        memcpy(documents[i].id, context->documents[i].id, id_length + 1U);
        documents[i].bytes =
            (unsigned char *)context->documents[i].bytes.buf;
        documents[i].byte_count = context->documents[i].bytes.len;
        context->documents[i].bytes.buf = NULL;
        context->documents[i].bytes.len = 0U;
        context->documents[i].bytes.cap = 0U;
    }
    for (size_t i = 0U; i < context->notice_count; ++i) {
        notices[i].message = context->notices[i].message;
        context->notices[i].message = NULL;
    }
    out->documents = documents;
    out->document_count = context->document_count;
    out->notices = notices;
    out->notice_count = context->notice_count;
    return TP_STATUS_OK;
}

tp_status tp_lua_runtime_serialize(const tp_lua_runtime_input *input,
                                   tp_lua_runtime_result *out,
                                   tp_error *error) {
    if (!out)
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua runtime requires a result output");
    memset(out, 0, sizeof *out);
    tp_status status = validate_runtime_input(input, error);
    if (status != TP_STATUS_OK) return status;
    if (tp_cancel_requested(input->cancel))
        return tp_error_set(error, TP_STATUS_CANCELLED,
                            "Lua serialization cancelled");

    tp_lua_runtime_context context = {
        .input = input,
        .limits = tp_lua_effective_limits_get(),
        .document_count = input->document_count,
    };
    context.allocator.limit = context.limits.live_bytes;
    for (size_t i = 0U; i < input->document_count; ++i) {
        context.documents[i].id = input->documents[i].id;
        context.documents[i].bytes.limit = context.limits.document_bytes;
    }
    lua_State *state = new_state(&context.allocator);
    if (!state) {
        if (context.allocator.host_oom)
            return tp_error_set(error, TP_STATUS_OOM,
                                "Lua runtime state allocation failed");
        status = append_report(
            &out->diagnostics, TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT,
            TP_FORMAT_PHASE_LIMIT, input->format_id, input->package_path, 0U,
            "Lua runtime allocator exceeded its fixed live-byte limit",
            NULL, 0U, error);
        return status == TP_STATUS_OK ? TP_STATUS_INVALID_ARGUMENT : status;
    }
    *(tp_lua_runtime_context **)lua_getextraspace(state) = &context;

    lua_pushcfunction(state, install_thunk);
    int lua_status = lua_pcall(state, 0, 1, 0);
    if (lua_status == LUA_OK) {
        lua_pushcfunction(state, runtime_error_handler);
        const int message_handler = lua_gettop(state);
        char chunk_name[TP_FORMAT_ID_MAX_BYTES + 32U];
        (void)snprintf(chunk_name, sizeof chunk_name,
                       "@formats/%s/export.lua", input->format_id);
        if (tp_cancel_requested(input->cancel)) {
            context.cancelled = true;
            lua_status = LUA_ERRRUN;
        } else {
            const unsigned char *source_text =
                input->source ? input->source : (const unsigned char *)"";
            lua_status = luaL_loadbufferx(
                state, (const char *)source_text,
                input->source_byte_count, chunk_name, "t");
#ifdef TP_ENABLE_TEST_SEAMS
            if (lua_status == LUA_OK &&
                g_test_limits.fail_runtime_allocation_after_load) {
                context.allocator.fail_next = true;
                g_test_limits.fail_runtime_allocation_after_load = false;
            }
#endif
            if (tp_cancel_requested(input->cancel)) {
                context.cancelled = true;
                lua_status = LUA_ERRRUN;
            }
        }
        if (lua_status == LUA_OK) {
            lua_pushvalue(state, 1); /* sandbox environment */
            (void)lua_setupvalue(state, -2, 1);
            if (context.limits.instruction_hook_enabled) {
                lua_sethook(state, instruction_hook, LUA_MASKCOUNT,
                            context.limits.hook_interval);
            }
            lua_status = lua_pcall(state, 0, LUA_MULTRET, message_handler);
            if (lua_status == LUA_OK &&
                (lua_gettop(state) != message_handler + 1 ||
                 lua_type(state, -1) != LUA_TFUNCTION)) {
                tp_lua_fail(&context, TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT,
                            TP_FORMAT_PHASE_RUNTIME,
                            "export.lua must return exactly one function");
                lua_settop(state, message_handler);
                lua_status = LUA_ERRRUN;
            }
            if (lua_status == LUA_OK) {
                if (!lua_checkstack(state, 1)) {
                    if (!context.allocator.limit_hit &&
                        !context.allocator.host_oom) {
                        tp_lua_fail(&context,
                                    TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                                    TP_FORMAT_PHASE_RUNTIME,
                                    "Lua invocation stack allocation failed");
                    }
                    lua_status = LUA_ERRMEM;
                } else {
                    lua_pushcfunction(state, invoke_handler_thunk);
                    lua_insert(state, -2); /* thunk(handler) */
                    lua_status =
                        lua_pcall(state, 1, 0, message_handler);
                }
            }
        }
        if (lua_status != LUA_OK && !context.failure_set &&
            !context.cancelled && !context.allocator.limit_hit &&
            !context.allocator.host_oom) {
            char message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
            lua_error_message(state, message, sizeof message,
                              "Lua handler raised a non-text error");
            tp_lua_fail(&context,
                        lua_status == LUA_ERRSYNTAX
                            ? TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR
                            : TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                        lua_status == LUA_ERRSYNTAX ? TP_FORMAT_PHASE_COMPILE
                                                   : TP_FORMAT_PHASE_RUNTIME,
                        message);
        }
    }

    if (lua_status == LUA_OK) {
        for (size_t i = 0U; i < context.document_count; ++i) {
            const tp_lua_document_state *document = &context.documents[i];
            if (!document->opened) {
                tp_lua_fail(&context,
                            TP_FORMAT_DIAGNOSTIC_DOCUMENT_MISSING,
                            TP_FORMAT_PHASE_OUTPUT,
                            "declared document was never opened");
                break;
            }
            if (!document->finished) {
                tp_lua_fail(&context,
                            TP_FORMAT_DIAGNOSTIC_DOCUMENT_UNFINISHED,
                            TP_FORMAT_PHASE_OUTPUT,
                            "declared document writer was not finished");
                break;
            }
            if (!tp_lua_text_valid(document->bytes.buf
                                       ? document->bytes.buf
                                       : "",
                                   document->bytes.len)) {
                tp_lua_fail(
                    &context,
                    memchr(document->bytes.buf, '\0', document->bytes.len)
                        ? TP_FORMAT_DIAGNOSTIC_DOCUMENT_CONTAINS_NUL
                        : TP_FORMAT_DIAGNOSTIC_DOCUMENT_INVALID_UTF8,
                    TP_FORMAT_PHASE_OUTPUT,
                    "declared document is not valid NUL-free UTF-8 text");
                break;
            }
        }
    }

    const bool host_oom = context.allocator.host_oom;
    const bool limit_hit = context.allocator.limit_hit;
    if (limit_hit && !context.failure_set) {
        tp_lua_fail(&context, TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT,
                    TP_FORMAT_PHASE_LIMIT,
                    "Lua allocator exceeded its fixed live-byte limit");
    }
    lua_close(state);

    if (host_oom) {
        context_destroy(&context);
        return tp_error_set(error, TP_STATUS_OOM,
                            "Lua runtime host allocation failed");
    }
    if (context.cancelled) {
        context_destroy(&context);
        return tp_error_set(error, TP_STATUS_CANCELLED,
                            "Lua serialization cancelled");
    }
    if (context.failure_set) {
        status = append_report(
            &out->diagnostics, context.failure_code, context.failure_phase,
            input->format_id, input->package_path, context.failure_line,
            context.failure_message, context.failure_frames,
            context.failure_frame_count, error);
        context_destroy(&context);
        return status == TP_STATUS_OK ? TP_STATUS_INVALID_ARGUMENT : status;
    }
    status = transfer_success(&context, out, error);
    context_destroy(&context);
    return status;
}

void tp_lua_runtime_result_destroy(tp_lua_runtime_result *result) {
    if (!result) return;
    for (size_t i = 0U; i < result->document_count; ++i) {
        free(result->documents[i].id);
        free(result->documents[i].bytes);
    }
    for (size_t i = 0U; i < result->notice_count; ++i)
        free(result->notices[i].message);
    free(result->documents);
    free(result->notices);
    tp_format_diagnostic_report_destroy(result->diagnostics);
    memset(result, 0, sizeof *result);
}

#ifdef TP_ENABLE_TEST_SEAMS
void tp_lua__test_trigger_panic(void) {
    tp_lua_effective_limits limits = tp_lua_effective_limits_get();
    tp_lua_allocator allocator = {.limit = limits.live_bytes};
    lua_State *state = new_state(&allocator);
    if (!state) abort();
    lua_pushliteral(state, "intentional Lua panic test");
    (void)lua_error(state);
    abort();
}
#endif
