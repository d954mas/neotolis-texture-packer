#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "tp_lua_host_internal.h"
#include "tp_proc_internal.h"
#include "unity.h"

#define TP_LUA_LIMIT_CHILD_ARG "--tp-test-lua-limit-child"

typedef struct child_reply {
    int32_t status;
    int32_t diagnostic;
} child_reply;

typedef struct child_observation {
    child_reply reply;
    tp_proc_result process;
    size_t reply_length;
    bool eof;
    bool watchdog_expired;
} child_observation;

void setUp(void) {}
void tearDown(void) {}

static bool always_cancel(void *context) {
    (void)context;
    return true;
}

static bool cancel_on_third_poll(void *context) {
    unsigned *polls = (unsigned *)context;
    (*polls)++;
    return *polls >= 3U;
}

static tp_lua_runtime_input fixture_input(const char *source,
                                          const tp_cancel_token *cancel,
                                          size_t document_count) {
    static const tp_export_page page = {
        .artifact_id = 0, .w = 8, .h = 8, .premultiplied = false};
    static const tp_export_ir ir = {
        .version = TP_EXPORT_IR_VERSION,
        .atlas_name = "atlas",
        .pixels_per_unit = 1.0F,
        .pages = (tp_export_page *)&page,
        .page_count = 1,
    };
    static const char *const images[] = {"atlas.png"};
    static const tp_lua_projected_ir projected = {
        .value = &ir, .page_images = images, .page_image_count = 1U};
    static const tp_lua_document_decl documents[] = {
        {.id = "meta"}, {.id = "extra"}};
    return (tp_lua_runtime_input){
        .source = (const unsigned char *)source,
        .source_byte_count = strlen(source),
        .format_id = "limit-test",
        .package_path = "formats/limit-test/export.lua",
        .projected_ir = &projected,
        .documents = documents,
        .document_count = document_count,
        .cancel = cancel,
    };
}

static tp_format_diagnostic_code first_code(
    const tp_format_diagnostic_report *report) {
    const tp_format_diagnostic *diagnostic =
        report && tp_format_diagnostic_report_count(report) > 0U
            ? tp_format_diagnostic_report_at(report, 0U)
            : NULL;
    return diagnostic ? diagnostic->code : 0;
}

static int write_runtime_reply(const char *source,
                               const tp_cancel_token *cancel) {
    const tp_lua_runtime_input input = fixture_input(source, cancel, 1U);
    tp_lua_runtime_result result = {0};
    tp_error error = {{0}};
    const tp_status status = tp_lua_runtime_serialize(&input, &result, &error);
    const child_reply reply = {
        .status = (int32_t)status,
        .diagnostic = (int32_t)first_code(result.diagnostics),
    };
    const bool wrote = fwrite(&reply, 1U, sizeof reply, stdout) == sizeof reply &&
                       fflush(stdout) == 0;
    tp_lua_runtime_result_destroy(&result);
    return wrote ? 0 : 3;
}

static int write_runtime_reply_with_documents(const char *source,
                                               size_t document_count) {
    const tp_lua_runtime_input input =
        fixture_input(source, NULL, document_count);
    tp_lua_runtime_result result = {0};
    tp_error error = {{0}};
    const tp_status status = tp_lua_runtime_serialize(&input, &result, &error);
    const child_reply reply = {
        .status = (int32_t)status,
        .diagnostic = (int32_t)first_code(result.diagnostics),
    };
    const bool wrote = fwrite(&reply, 1U, sizeof reply, stdout) == sizeof reply &&
                       fflush(stdout) == 0;
    tp_lua_runtime_result_destroy(&result);
    return wrote ? 0 : 3;
}

static int run_child(void) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                       SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const int scenario = fgetc(stdin);
    tp_lua_test_limits limits = {0};
    switch (scenario) {
        case 'I':
            limits.instructions = 1000U;
            limits.hook_interval = 100;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) while true do end end\n", NULL);
        case 'Z':
            limits.disable_instruction_hook = true;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) while true do end end\n", NULL);
        case 'H':
            limits.host_calls = 2U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) atlas:name(); atlas:name(); "
                "atlas:name() end\n", NULL);
        case 'W':
            limits.writer_calls = 2U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) local w=host:document('meta'); "
                "w:write('a'); w:write('b'); w:write('c') end\n", NULL);
        case 'O':
            limits.writer_argument_bytes = 3U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) local w=host:document('meta'); "
                "w:write('four') end\n", NULL);
        case 'd':
            limits.document_bytes = 4U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) local w=host:document('meta'); "
                "w:write('four'); w:finish() end\n", NULL);
        case 'D':
            limits.document_bytes = 4U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) local w=host:document('meta'); "
                "w:write('five!') end\n", NULL);
        case 't':
            limits.document_total_bytes = 6U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply_with_documents(
                "return function(atlas, host) "
                "local a=host:document('meta'); a:write('abc'); a:finish(); "
                "local b=host:document('extra'); b:write('def'); b:finish() end\n",
                2U);
        case 'T':
            limits.document_total_bytes = 5U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply_with_documents(
                "return function(atlas, host) "
                "local a=host:document('meta'); a:write('abc'); a:finish(); "
                "local b=host:document('extra'); b:write('def') end\n",
                2U);
        case 'N':
            limits.notices = 1U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) host:notice('one'); "
                "host:notice('two') end\n", NULL);
        case 'n':
            limits.notice_message_bytes = 3U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) host:notice('abc'); "
                "local w=host:document('meta'); w:finish() end\n", NULL);
        case 'Q':
            limits.notice_message_bytes = 3U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) host:notice('abcd') end\n", NULL);
        case 'q':
            limits.notice_total_bytes = 6U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) host:notice('abc'); "
                "host:notice('def'); local w=host:document('meta'); "
                "w:finish() end\n", NULL);
        case 'R':
            limits.notice_total_bytes = 5U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) host:notice('abc'); "
                "host:notice('def') end\n", NULL);
        case 'C': {
            const tp_cancel_token cancel = {
                .cancel_requested = always_cancel, .ctx = NULL};
            return write_runtime_reply(
                "return function(atlas, host) end\n", &cancel);
        }
        case 'c': {
            unsigned polls = 0U;
            const tp_cancel_token cancel = {
                .cancel_requested = cancel_on_third_poll, .ctx = &polls};
            return write_runtime_reply(
                "return function(atlas, host) end\n", &cancel);
        }
        case 'M': {
            limits.live_bytes = 1024U;
            tp_lua__test_set_limits(&limits);
            tp_format_diagnostic_report *report = NULL;
            tp_error error = {{0}};
            static const unsigned char source[] =
                "return function(atlas, host) end\n";
            const tp_status status = tp_lua_compile_validate(
                source, sizeof source - 1U, "limit-test",
                "formats/limit-test/export.lua", &report, &error);
            const child_reply reply = {
                .status = (int32_t)status,
                .diagnostic = (int32_t)first_code(report),
            };
            const bool wrote =
                fwrite(&reply, 1U, sizeof reply, stdout) == sizeof reply &&
                fflush(stdout) == 0;
            tp_format_diagnostic_report_destroy(report);
            return wrote ? 0 : 3;
        }
        case 'A': {
            tp_lua__test_fail_next_allocation();
            tp_format_diagnostic_report *report = NULL;
            tp_error error = {{0}};
            static const unsigned char source[] =
                "return function(atlas, host) end\n";
            const tp_status status = tp_lua_compile_validate(
                source, sizeof source - 1U, "limit-test",
                "formats/limit-test/export.lua", &report, &error);
            const child_reply reply = {
                .status = (int32_t)status,
                .diagnostic = (int32_t)first_code(report),
            };
            const bool wrote =
                fwrite(&reply, 1U, sizeof reply, stdout) == sizeof reply &&
                fflush(stdout) == 0;
            tp_format_diagnostic_report_destroy(report);
            return wrote ? 0 : 3;
        }
        case 'a':
            tp_lua__test_fail_next_allocation();
            return write_runtime_reply(
                "return function(atlas, host) end\n", NULL);
        case 'm':
            limits.live_bytes = 1024U;
            tp_lua__test_set_limits(&limits);
            return write_runtime_reply(
                "return function(atlas, host) end\n", NULL);
        case 'f':
        case 'F': {
            char fact_value[TP_LUA_FACT_VALUE_MAX_BYTES + 2U];
            const size_t fact_length =
                scenario == 'f' ? TP_LUA_FACT_VALUE_MAX_BYTES
                                : TP_LUA_FACT_VALUE_MAX_BYTES + 1U;
            memset(fact_value, 'x', fact_length);
            fact_value[fact_length] = '\0';
            const tp_lua_fact_value fact = {
                .id = "boundary", .value = fact_value};
            const char *source =
                "return function(atlas, host) local w=host:document('meta'); "
                "w:write(host:fact('boundary')); w:finish() end\n";
            tp_lua_runtime_input input = fixture_input(source, NULL, 1U);
            input.facts = &fact;
            input.fact_count = 1U;
            tp_lua_runtime_result result = {0};
            tp_error error = {{0}};
            const tp_status status =
                tp_lua_runtime_serialize(&input, &result, &error);
            const child_reply reply = {
                .status = (int32_t)status,
                .diagnostic = (int32_t)first_code(result.diagnostics),
            };
            const bool wrote =
                fwrite(&reply, 1U, sizeof reply, stdout) == sizeof reply &&
                fflush(stdout) == 0;
            tp_lua_runtime_result_destroy(&result);
            return wrote ? 0 : 3;
        }
        case 'L':
            return write_runtime_reply(
                "return function(atlas, host) host:document('meta'); "
                "host:document('meta') end\n", NULL);
        case 'U':
            return write_runtime_reply(
                "return function(atlas, host) host:document('unknown') end\n",
                NULL);
        case 'X':
            return write_runtime_reply(
                "return function(atlas, host) local w=host:document('meta'); "
                "w:finish(); w:write('late') end\n", NULL);
        case 'E':
            return write_runtime_reply(
                "return function(atlas, host) end\n", NULL);
        case 'P':
            tp_lua__test_trigger_panic();
            return 4;
        default: return 2;
    }
}

static child_observation invoke(char scenario) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *process =
        tp_proc_spawn_owned_tree(self, TP_LUA_LIMIT_CHILD_ARG, NULL);
    TEST_ASSERT_NOT_NULL(process);
    TEST_ASSERT_TRUE(tp_proc_write_stdin(process, &scenario, 1U));
    child_observation observation = {0};
    unsigned char wire[sizeof observation.reply + 1U] = {0};
    bool finished = false;
    enum { WATCHDOG_SLICES = 200, WATCHDOG_SLICE_MS = 10 };
    for (unsigned int slice = 0U;
         slice < WATCHDOG_SLICES && !observation.eof; ++slice) {
        size_t received = 0U;
        TEST_ASSERT_TRUE(tp_proc_try_read_stdout(
            process, wire + observation.reply_length,
            sizeof wire - observation.reply_length, &received,
            &observation.eof));
        observation.reply_length += received;
        if (observation.eof) {
            break;
        }
        TEST_ASSERT_TRUE(tp_proc_wait_slice(
            process, WATCHDOG_SLICE_MS, &observation.process, &finished));
    }
    if (!observation.eof) {
        observation.watchdog_expired = true;
        tp_proc_kill(process);
    }
    if (observation.reply_length >= sizeof observation.reply) {
        memcpy(&observation.reply, wire, sizeof observation.reply);
    }
    TEST_ASSERT_TRUE(tp_proc_wait_slice(process, 5000, &observation.process,
                                        &finished));
    TEST_ASSERT_TRUE(finished);
    tp_proc_destroy(process);
    return observation;
}

static void assert_limit(char scenario,
                         tp_format_diagnostic_code expected) {
    const child_observation observation = invoke(scenario);
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, observation.process.how);
    TEST_ASSERT_EQUAL_INT(0, observation.process.code);
    TEST_ASSERT_TRUE(observation.eof);
    TEST_ASSERT_EQUAL_size_t(sizeof(child_reply), observation.reply_length);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          observation.reply.status);
    TEST_ASSERT_EQUAL_INT(expected, observation.reply.diagnostic);
}

static void assert_success(char scenario) {
    const child_observation observation = invoke(scenario);
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, observation.process.how);
    TEST_ASSERT_EQUAL_INT(0, observation.process.code);
    TEST_ASSERT_EQUAL_size_t(sizeof(child_reply), observation.reply_length);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, observation.reply.status);
    TEST_ASSERT_EQUAL_INT(0, observation.reply.diagnostic);
}

static void assert_host_oom(char scenario) {
    const child_observation observation = invoke(scenario);
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, observation.process.how);
    TEST_ASSERT_EQUAL_INT(0, observation.process.code);
    TEST_ASSERT_TRUE(observation.eof);
    TEST_ASSERT_EQUAL_size_t(sizeof(child_reply), observation.reply_length);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, observation.reply.status);
    TEST_ASSERT_EQUAL_INT(0, observation.reply.diagnostic);
}

static void test_api_v1_ceiling_constants_are_frozen(void) {
    TEST_ASSERT_EQUAL_size_t(134217728U, TP_LUA_LIVE_BYTES_MAX);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(250000000), TP_LUA_INSTRUCTION_MAX);
    TEST_ASSERT_EQUAL_INT(10000, TP_LUA_HOOK_INTERVAL);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(8388608), TP_LUA_HOST_CALL_MAX);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(4194304), TP_LUA_WRITER_CALL_MAX);
    TEST_ASSERT_EQUAL_size_t(1048576U,
                             TP_LUA_WRITER_ARGUMENT_MAX_BYTES);
    TEST_ASSERT_EQUAL_size_t(4095U, TP_LUA_FACT_VALUE_MAX_BYTES);
    TEST_ASSERT_EQUAL_size_t(67108864U, TP_LUA_DOCUMENT_MAX_BYTES);
    TEST_ASSERT_EQUAL_size_t(67108864U,
                             TP_LUA_DOCUMENT_TOTAL_MAX_BYTES);
    TEST_ASSERT_EQUAL_size_t(4096U, TP_LUA_NOTICE_MAX);
    TEST_ASSERT_EQUAL_size_t(1024U,
                             TP_LUA_NOTICE_MESSAGE_MAX_BYTES);
    TEST_ASSERT_EQUAL_size_t(1048576U,
                             TP_LUA_NOTICE_TOTAL_MAX_BYTES);
}

static void test_instruction_ceiling_fails_closed(void) {
    assert_limit('I', TP_FORMAT_DIAGNOSTIC_INSTRUCTION_LIMIT);
}

static void test_instruction_watchdog_kills_and_reaps_a_hook_regression(void) {
    const child_observation observation = invoke('Z');
    TEST_ASSERT_TRUE(observation.watchdog_expired);
    TEST_ASSERT_FALSE(observation.eof);
    TEST_ASSERT_EQUAL_size_t(0U, observation.reply_length);
    TEST_ASSERT_TRUE(observation.process.how == TP_PROC_END_ABNORMAL ||
                     observation.process.code != 0);
}

static void test_host_call_ceiling_fails_closed(void) {
    assert_limit('H', TP_FORMAT_DIAGNOSTIC_HOST_CALL_LIMIT);
}

static void test_writer_call_ceiling_uses_frozen_host_call_diagnostic(void) {
    assert_limit('W', TP_FORMAT_DIAGNOSTIC_HOST_CALL_LIMIT);
}

static void test_writer_argument_ceiling_fails_closed(void) {
    assert_limit('O', TP_FORMAT_DIAGNOSTIC_OUTPUT_LIMIT);
}

static void test_per_document_boundary_and_plus_one(void) {
    assert_success('d');
    assert_limit('D', TP_FORMAT_DIAGNOSTIC_OUTPUT_LIMIT);
}

static void test_all_documents_boundary_and_plus_one(void) {
    assert_success('t');
    assert_limit('T', TP_FORMAT_DIAGNOSTIC_OUTPUT_LIMIT);
}

static void test_notice_ceiling_fails_closed(void) {
    assert_limit('N', TP_FORMAT_DIAGNOSTIC_NOTICE_LIMIT);
}

static void test_notice_message_boundary_and_plus_one(void) {
    assert_success('n');
    assert_limit('Q', TP_FORMAT_DIAGNOSTIC_NOTICE_LIMIT);
}

static void test_notice_total_boundary_and_plus_one(void) {
    assert_success('q');
    assert_limit('R', TP_FORMAT_DIAGNOSTIC_NOTICE_LIMIT);
}

static void test_compile_allocator_ceiling_is_package_diagnostic(void) {
    assert_limit('M', TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT);
}

static void test_runtime_allocator_ceiling_is_package_diagnostic(void) {
    assert_limit('m', TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT);
}

static void test_allocator_fault_stays_primary_oom_without_diagnostic(void) {
    assert_host_oom('A');
    assert_host_oom('a');
}

static void test_preflight_cancellation_stays_cancelled(void) {
    const child_observation observation = invoke('C');
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, observation.process.how);
    TEST_ASSERT_EQUAL_INT(0, observation.process.code);
    TEST_ASSERT_EQUAL_size_t(sizeof(child_reply), observation.reply_length);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, observation.reply.status);
    TEST_ASSERT_EQUAL_INT(0, observation.reply.diagnostic);
}

static void test_cancellation_is_polled_after_text_compilation(void) {
    const child_observation observation = invoke('c');
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, observation.process.how);
    TEST_ASSERT_EQUAL_INT(0, observation.process.code);
    TEST_ASSERT_EQUAL_size_t(sizeof(child_reply), observation.reply_length);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, observation.reply.status);
    TEST_ASSERT_EQUAL_INT(0, observation.reply.diagnostic);
}

static void test_host_fact_boundary_and_plus_one(void) {
    assert_success('f');
    const child_observation observation = invoke('F');
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, observation.process.how);
    TEST_ASSERT_EQUAL_INT(0, observation.process.code);
    TEST_ASSERT_EQUAL_size_t(sizeof(child_reply), observation.reply_length);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          observation.reply.status);
    TEST_ASSERT_EQUAL_INT(0, observation.reply.diagnostic);
}

static void test_document_lifecycle_faults_are_structured(void) {
    assert_limit('L', TP_FORMAT_DIAGNOSTIC_DOCUMENT_DUPLICATE);
    assert_limit('U', TP_FORMAT_DIAGNOSTIC_DOCUMENT_UNKNOWN);
    assert_limit('X', TP_FORMAT_DIAGNOSTIC_DOCUMENT_WRITE_AFTER_FINISH);
    assert_limit('E', TP_FORMAT_DIAGNOSTIC_DOCUMENT_MISSING);
}

static void test_unprotected_panic_terminates_only_the_child(void) {
    const child_observation observation = invoke('P');
    TEST_ASSERT_TRUE(observation.eof);
    TEST_ASSERT_EQUAL_size_t(0U, observation.reply_length);
    TEST_ASSERT_TRUE(observation.process.how == TP_PROC_END_ABNORMAL ||
                     observation.process.code != 0);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], TP_LUA_LIMIT_CHILD_ARG) == 0)
        return run_child();
    UNITY_BEGIN();
    RUN_TEST(test_api_v1_ceiling_constants_are_frozen);
    RUN_TEST(test_instruction_ceiling_fails_closed);
    RUN_TEST(test_instruction_watchdog_kills_and_reaps_a_hook_regression);
    RUN_TEST(test_host_call_ceiling_fails_closed);
    RUN_TEST(test_writer_call_ceiling_uses_frozen_host_call_diagnostic);
    RUN_TEST(test_writer_argument_ceiling_fails_closed);
    RUN_TEST(test_per_document_boundary_and_plus_one);
    RUN_TEST(test_all_documents_boundary_and_plus_one);
    RUN_TEST(test_notice_ceiling_fails_closed);
    RUN_TEST(test_notice_message_boundary_and_plus_one);
    RUN_TEST(test_notice_total_boundary_and_plus_one);
    RUN_TEST(test_compile_allocator_ceiling_is_package_diagnostic);
    RUN_TEST(test_runtime_allocator_ceiling_is_package_diagnostic);
    RUN_TEST(test_allocator_fault_stays_primary_oom_without_diagnostic);
    RUN_TEST(test_preflight_cancellation_stays_cancelled);
    RUN_TEST(test_cancellation_is_polled_after_text_compilation);
    RUN_TEST(test_host_fact_boundary_and_plus_one);
    RUN_TEST(test_document_lifecycle_faults_are_structured);
    RUN_TEST(test_unprotected_panic_terminates_only_the_child);
    return UNITY_END();
}
