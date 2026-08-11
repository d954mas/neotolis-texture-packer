#ifndef TP_BUILD_SRC_TP_LUA_HOST_PRIVATE_INTERNAL_H
#define TP_BUILD_SRC_TP_LUA_HOST_PRIVATE_INTERNAL_H

#include "tp_lua_host_internal.h"

#include "lua.h"

#include "tp_core/tp_sb.h"

typedef struct tp_lua_effective_limits {
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
    bool instruction_hook_enabled;
} tp_lua_effective_limits;

typedef struct tp_lua_allocator {
    size_t live_bytes;
    size_t limit;
    bool limit_hit;
    bool host_oom;
    bool fail_next;
    bool failure_pending;
    bool pending_prior_limit_hit;
    bool pending_prior_host_oom;
    void *pending_pointer;
    size_t pending_old_size;
    size_t pending_new_size;
} tp_lua_allocator;

typedef struct tp_lua_document_state {
    const char *id;
    bool opened;
    bool finished;
    tp_sb bytes;
} tp_lua_document_state;

typedef struct tp_lua_notice_state {
    char *message;
    size_t byte_count;
} tp_lua_notice_state;

typedef struct tp_lua_runtime_context {
    const tp_lua_runtime_input *input;
    tp_lua_effective_limits limits;
    tp_lua_allocator allocator;
    tp_lua_document_state documents[TP_FORMAT_OUTPUT_MAX];
    size_t document_count;
    size_t document_total_bytes;
    tp_lua_notice_state *notices;
    size_t notice_count;
    size_t notice_capacity;
    size_t notice_total_bytes;
    uint64_t instructions;
    uint64_t host_calls;
    uint64_t writer_calls;
    bool cancelled;
    bool failure_set;
    tp_format_diagnostic_code failure_code;
    tp_format_diagnostic_phase failure_phase;
    uint32_t failure_line;
    char failure_message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
    tp_format_diagnostic_frame failure_frames[TP_FORMAT_DIAGNOSTIC_FRAME_MAX];
    char failure_frame_text[TP_FORMAT_DIAGNOSTIC_FRAME_MAX]
                           [TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES + 1U];
    size_t failure_frame_count;
} tp_lua_runtime_context;

typedef enum tp_lua_view_kind {
    TP_LUA_VIEW_ATLAS = 1,
    TP_LUA_VIEW_PAGE,
    TP_LUA_VIEW_SPRITE,
    TP_LUA_VIEW_ANIMATION,
    TP_LUA_VIEW_HOST,
    TP_LUA_VIEW_WRITER,
} tp_lua_view_kind;

typedef struct tp_lua_view {
    tp_lua_runtime_context *context;
    size_t index;
    tp_lua_view_kind kind;
} tp_lua_view;

tp_lua_effective_limits tp_lua_effective_limits_get(void);
void *tp_lua_allocator_fn(void *userdata, void *pointer, size_t old_size,
                          size_t new_size);
int tp_lua_panic(lua_State *state);
tp_lua_runtime_context *tp_lua_context(lua_State *state);

void tp_lua_fail(tp_lua_runtime_context *context,
                 tp_format_diagnostic_code code,
                 tp_format_diagnostic_phase phase, const char *message);
int tp_lua_raise(lua_State *state, tp_format_diagnostic_code code,
                 tp_format_diagnostic_phase phase, const char *message);
bool tp_lua_gate(lua_State *state, bool writer_call);
bool tp_lua_text_valid(const char *text, size_t byte_count);

tp_status tp_lua_bindings_install(lua_State *state,
                                  tp_lua_runtime_context *context,
                                  int *out_environment_index,
                                  tp_error *error);
void tp_lua_push_atlas(lua_State *state, tp_lua_runtime_context *context);
void tp_lua_push_host(lua_State *state, tp_lua_runtime_context *context);

#endif /* TP_BUILD_SRC_TP_LUA_HOST_PRIVATE_INTERNAL_H */
