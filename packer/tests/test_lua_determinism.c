#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "tp_lua_host_internal.h"
#include "tp_proc_internal.h"
#include "unity.h"

#define TP_LUA_DETERMINISM_CHILD_ARG "--tp-test-lua-determinism-child"
#define TP_LUA_DETERMINISM_REPLY_MAGIC UINT32_C(0x54504452)
#define TP_LUA_DETERMINISM_REPLY_VERSION UINT32_C(1)
#define TP_LUA_DETERMINISM_MAX_RESULTS 2U

typedef struct child_result {
    int32_t status;
    int32_t diagnostic;
    uint32_t document_length;
    unsigned char document[512];
} child_result;

typedef struct child_reply {
    uint32_t magic;
    uint32_t version;
    uint32_t result_count;
    uint32_t reserved;
    child_result results[TP_LUA_DETERMINISM_MAX_RESULTS];
} child_reply;

_Static_assert(sizeof(child_result) == 524U,
               "child result wire layout must not contain padding");
_Static_assert(sizeof(child_reply) == 1064U,
               "child reply wire layout must remain exact");

void setUp(void) {}
void tearDown(void) {}

static tp_lua_runtime_input fixture_input(const char *source) {
    static const tp_export_page page = {
        .artifact_id = 0, .w = 64, .h = 64, .premultiplied = false};
    static const tp_export_ir ir = {
        .version = TP_EXPORT_IR_VERSION,
        .atlas_name = "deterministic-atlas",
        .pixels_per_unit = 1.0F,
        .pages = (tp_export_page *)&page,
        .page_count = 1,
    };
    static const char *const images[] = {"stable.png"};
    static const tp_lua_projected_ir projected = {
        .value = &ir, .page_images = images, .page_image_count = 1U};
    static const tp_lua_document_decl documents[] = {{.id = "meta"}};
    return (tp_lua_runtime_input){
        .source = (const unsigned char *)source,
        .source_byte_count = strlen(source),
        .format_id = "determinism-test",
        .package_path = "formats/determinism-test/export.lua",
        .projected_ir = &projected,
        .documents = documents,
        .document_count = 1U,
    };
}

static void serialize_once(const tp_lua_runtime_input *input,
                           child_result *reply) {
    tp_lua_runtime_result result = {0};
    tp_error error = {{0}};
    reply->status =
        (int32_t)tp_lua_runtime_serialize(input, &result, &error);
    if (result.diagnostics &&
        tp_format_diagnostic_report_count(result.diagnostics) > 0U) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(result.diagnostics, 0U);
        reply->diagnostic = diagnostic ? (int32_t)diagnostic->code : 0;
    }
    if (result.document_count == 1U &&
        result.documents[0].byte_count <= sizeof reply->document) {
        reply->document_length = (uint32_t)result.documents[0].byte_count;
        memcpy(reply->document, result.documents[0].bytes,
               result.documents[0].byte_count);
    }
    tp_lua_runtime_result_destroy(&result);
}

static int run_child(void) {
    static const char typed_writers[] =
        "return function(atlas, host)\n"
        "  local w = host:document('meta')\n"
        "  w:write_json_string('a\\\"\\n')\n"
        "  w:write('|'); w:write_i64(-42)\n"
        "  w:write('|'); w:write_u64(42)\n"
        "  w:write('|'); w:write_f32(0.1)\n"
        "  w:write('|'); w:write_bool(true)\n"
        "  w:write('|'); w:write_null()\n"
        "  w:finish()\n"
        "end\n";
    static const char fresh_state[] =
        "counter = (counter or 0) + 1\n"
        "return function(atlas, host)\n"
        "  local w = host:document('meta')\n"
        "  w:write_i64(counter)\n"
        "  w:finish()\n"
        "end\n";
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    const int scenario = fgetc(stdin);
    if (scenario != 'T' && scenario != 'F') return 2;
    const char *source = scenario == 'T' ? typed_writers : fresh_state;
    const tp_lua_runtime_input input = fixture_input(source);
    child_reply reply = {
        .magic = TP_LUA_DETERMINISM_REPLY_MAGIC,
        .version = TP_LUA_DETERMINISM_REPLY_VERSION,
        .result_count = scenario == 'T' ? 1U : 2U,
    };
    for (uint32_t i = 0U; i < reply.result_count; ++i) {
        serialize_once(&input, &reply.results[i]);
    }
    const bool wrote = fwrite(&reply, 1U, sizeof reply, stdout) == sizeof reply &&
                       fflush(stdout) == 0;
    return wrote ? 0 : 3;
}

static child_reply invoke(char scenario) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *process =
        tp_proc_spawn(self, TP_LUA_DETERMINISM_CHILD_ARG, NULL);
    TEST_ASSERT_NOT_NULL(process);
    TEST_ASSERT_TRUE(tp_proc_write_stdin(process, &scenario, 1U));
    child_reply reply = {0};
    size_t length = 0U;
    bool eof = false;
    TEST_ASSERT_TRUE(tp_proc_read_stdout(process, &reply, sizeof reply,
                                         &length, &eof));
    TEST_ASSERT_TRUE(eof);
    TEST_ASSERT_EQUAL_size_t(sizeof reply, length);
    TEST_ASSERT_EQUAL_HEX32(TP_LUA_DETERMINISM_REPLY_MAGIC, reply.magic);
    TEST_ASSERT_EQUAL_UINT32(TP_LUA_DETERMINISM_REPLY_VERSION, reply.version);
    TEST_ASSERT_TRUE(reply.result_count > 0U);
    TEST_ASSERT_TRUE(reply.result_count <= TP_LUA_DETERMINISM_MAX_RESULTS);
    TEST_ASSERT_EQUAL_UINT32(0U, reply.reserved);
    bool finished = false;
    tp_proc_result process_result = {0};
    TEST_ASSERT_TRUE(tp_proc_wait_slice(process, 5000, &process_result,
                                        &finished));
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_EQUAL_INT(TP_PROC_END_EXITED, process_result.how);
    TEST_ASSERT_EQUAL_INT(0, process_result.code);
    tp_proc_destroy(process);
    return reply;
}

static void test_typed_writers_are_byte_deterministic_across_workers(void) {
    const child_reply first = invoke('T');
    const child_reply second = invoke('T');
    TEST_ASSERT_EQUAL_MEMORY(&first, &second, sizeof first);
    TEST_ASSERT_EQUAL_UINT32(1U, first.result_count);
    const child_result *result = &first.results[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result->status);
    static const char expected[] =
        "\"a\\\"\\n\"|-42|42|0.100000001|true|null";
    TEST_ASSERT_EQUAL_UINT32(sizeof expected - 1U, result->document_length);
    TEST_ASSERT_EQUAL_MEMORY(expected, result->document,
                             result->document_length);
}

static void test_each_execution_receives_a_fresh_global_state(void) {
    const child_reply reply = invoke('F');
    TEST_ASSERT_EQUAL_UINT32(2U, reply.result_count);
    for (uint32_t i = 0U; i < reply.result_count; ++i) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, reply.results[i].status);
        TEST_ASSERT_EQUAL_UINT32(1U, reply.results[i].document_length);
        TEST_ASSERT_EQUAL_MEMORY("1", reply.results[i].document,
                                 reply.results[i].document_length);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], TP_LUA_DETERMINISM_CHILD_ARG) == 0)
        return run_child();
    UNITY_BEGIN();
    RUN_TEST(test_typed_writers_are_byte_deterministic_across_workers);
    RUN_TEST(test_each_execution_receives_a_fresh_global_state);
    return UNITY_END();
}
