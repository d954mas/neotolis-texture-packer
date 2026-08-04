#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "tp_lua_host_internal.h"
#include "tp_proc_internal.h"
#include "unity.h"

#define TP_LUA_SANDBOX_CHILD_ARG "--tp-test-lua-sandbox-child"

typedef struct child_reply {
    int32_t status;
    int32_t diagnostic;
    uint32_t document_length;
    uint32_t notice_count;
    uint32_t frame_count;
    uint32_t frame_lines[TP_FORMAT_DIAGNOSTIC_FRAME_MAX];
    char diagnostic_message[128];
    char notices[2][32];
    char frame_text[TP_FORMAT_DIAGNOSTIC_FRAME_MAX][128];
    char document[512];
} child_reply;

void setUp(void) {}
void tearDown(void) {}

static tp_lua_runtime_input fixture_input(const char *source, bool hidden) {
    static tp_export_page page = {
        .artifact_id = 0, .w = 32, .h = 16, .premultiplied = false};
    static tp_point vertices[] = {{.x = 0, .y = 0},
                                  {.x = 10, .y = 0},
                                  {.x = 0, .y = 6}};
    static uint16_t indices[] = {0U, 1U, 2U};
    static tp_export_sprite sprites[] = {
        {
            .final_name = "hero",
            .data = {
                .name = "raw/hero.png",
                .page = 0,
                .frame = {.x = 1, .y = 2, .w = 10, .h = 6},
                .transform = 5U,
                .trimmed = true,
                .spriteSourceSize = {.x = 3, .y = 4, .w = 10, .h = 6},
                .sourceSize = {.w = 20, .h = 30},
                .pivot = {.x = 0.25F, .y = 0.75F},
                .slice9_lrtb = {1U, 2U, 3U, 4U},
                .verts = vertices,
                .vert_count = 3,
                .indices = indices,
                .index_count = 3,
                .alias_of = -1,
            },
            .is_solid = false,
        },
        {
            .final_name = "hero_alias",
            .data = {
                .name = "raw/hero-alias.png",
                .page = 0,
                .frame = {.x = 20, .y = 2, .w = 4, .h = 4},
                .transform = 0U,
                .trimmed = false,
                .spriteSourceSize = {.x = 0, .y = 0, .w = 4, .h = 4},
                .sourceSize = {.w = 4, .h = 4},
                .pivot = {.x = 0.5F, .y = 0.5F},
                .alias_of = 0,
            },
            .is_solid = true,
        },
    };
    static const char *animation_frames[] = {"hero", "hero_alias"};
    static tp_export_anim animations[] = {{
        .id = "walk",
        .frames = animation_frames,
        .frame_count = 2,
        .fps = 12.5F,
        .playback = 3,
        .flip_h = true,
        .flip_v = false,
    }};
    static const tp_export_ir ir = {
        .version = TP_EXPORT_IR_VERSION,
        .atlas_name = "atlas",
        .pixels_per_unit = 2.0F,
        .pages = &page,
        .page_count = 1,
        .sprites = sprites,
        .sprite_count = 2,
        .animations = animations,
        .animation_count = 1,
    };
    static const tp_export_ir hidden_ir = {
        .version = TP_EXPORT_IR_VERSION,
        .atlas_name = "atlas",
        .pixels_per_unit = 2.0F,
        .pages = &page,
        .page_count = 1,
        .sprites = sprites,
        .sprite_count = 2,
    };
    static const char *const images[] = {"atlas-0.png"};
    static const tp_lua_projected_ir projected = {
        .value = &ir,
        .page_images = images,
        .page_image_count = 1U,
        .polygons_visible = true,
        .pivot_visible = true,
        .slice9_visible = true,
        .aliases_visible = true,
    };
    static const tp_lua_projected_ir hidden_projected = {
        .value = &hidden_ir,
        .page_images = images,
        .page_image_count = 1U,
    };
    static const tp_lua_document_decl documents[] = {{.id = "meta"}};
    static const tp_lua_fact_value facts[] = {
        {.id = "separator", .value = "|"}};
    return (tp_lua_runtime_input){
        .source = (const unsigned char *)source,
        .source_byte_count = strlen(source),
        .format_id = "sandbox-test",
        .package_path = "formats/sandbox-test/export.lua",
        .projected_ir = hidden ? &hidden_projected : &projected,
        .documents = documents,
        .document_count = 1U,
        .facts = facts,
        .fact_count = 1U,
    };
}

static char *large_result_source(void) {
    static const char prefix[] =
        "return function(atlas, host)\n"
        " local w=host:document('meta'); w:finish()\n"
        " return string.byte('";
    static const char suffix[] = "',1,4096)\nend\n";
    enum { RESULT_COUNT = 4096 };
    const size_t capacity = sizeof prefix + sizeof suffix + RESULT_COUNT;
    char *source = (char *)malloc(capacity);
    if (!source) return NULL;
    size_t length = sizeof prefix - 1U;
    memcpy(source, prefix, length);
    memset(source + length, 'a', RESULT_COUNT);
    length += RESULT_COUNT;
    memcpy(source + length, suffix, sizeof suffix);
    return source;
}

static int run_child(void) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    const int scenario = fgetc(stdin);
    static const char allowed[] =
        "return function(atlas, host, ...)\n"
        "  assert(select('#', ...) == 0 and type(host) == 'userdata')\n"
        "  assert(type(atlas) == 'userdata')\n"
        "  assert(atlas:name() == 'atlas')\n"
        "  assert(atlas:pixels_per_unit() == 2)\n"
        "  assert(atlas:page_count() == 1)\n"
        "  local page = atlas:page(1)\n"
        "  assert(page:index() == 1 and page:width() == 32)\n"
        "  assert(page:height() == 16 and page:image() == 'atlas-0.png')\n"
        "  assert(atlas:sprite_count() == 2)\n"
        "  local sprite = atlas:sprite(1)\n"
        "  assert(sprite:name() == 'hero' and sprite:page() == 1)\n"
        "  local x,y,w,h = sprite:frame()\n"
        "  assert(x == 1 and y == 2 and w == 10 and h == 6)\n"
        "  local fw,fh = sprite:footprint()\n"
        "  assert(fw == 6 and fh == 10)\n"
        "  local tx,ty,tw,th = sprite:trim_rect()\n"
        "  assert(tx == 3 and ty == 4 and tw == 10 and th == 6)\n"
        "  local sw,sh = sprite:source_size()\n"
        "  assert(sw == 20 and sh == 30)\n"
        "  assert(sprite:transform() == 'rotate_90_cw')\n"
        "  assert(sprite:trimmed() and not sprite:is_solid())\n"
        "  local px,py = sprite:pivot()\n"
        "  assert(px == 0.25 and py == 0.75)\n"
        "  local l,r,t,b = sprite:slice9()\n"
        "  assert(l == 1 and r == 2 and t == 3 and b == 4)\n"
        "  assert(sprite:alias_of() == nil)\n"
        "  assert(sprite:vertex_count() == 3 and sprite:index_count() == 3)\n"
        "  local vx,vy = sprite:vertex(2)\n"
        "  assert(vx == 10 and vy == 0 and sprite:index(3) == 2)\n"
        "  local alias = atlas:sprite(2)\n"
        "  assert(alias:name() == 'hero_alias' and alias:alias_of() == 1)\n"
        "  assert(alias:is_solid() and not alias:trimmed())\n"
        "  assert(atlas:animation_count() == 1)\n"
        "  local animation = atlas:animation(1)\n"
        "  assert(animation:id() == 'walk' and animation:frame_count() == 2)\n"
        "  assert(animation:frame(1) == 'hero')\n"
        "  assert(animation:frame(2) == 'hero_alias')\n"
        "  assert(animation:fps() == 12.5 and animation:playback() == 3)\n"
        "  assert(animation:flip_h() and not animation:flip_v())\n"
        "  assert(math.floor(1.9) == 1 and string.sub('abc', 2) == 'bc')\n"
        "  assert(utf8.len('\xc3\xa9') == 1)\n"
        "  local writer = host:document('meta')\n"
        "  writer:write(atlas:name())\n"
        "  writer:write(host:fact('separator'))\n"
        "  writer:write(page:image())\n"
        "  writer:finish()\n"
        "end\n";
    static const char hidden[] =
        "return function(atlas, host)\n"
        "  assert(atlas:animation_count() == 0)\n"
        "  local sprite = atlas:sprite(1)\n"
        "  local a,b = sprite:pivot(); assert(a == nil and b == nil)\n"
        "  local l,r,t,b = sprite:slice9()\n"
        "  assert(l == nil and r == nil and t == nil and b == nil)\n"
        "  assert(sprite:vertex_count() == 0 and sprite:index_count() == 0)\n"
        "  assert(atlas:sprite(2):alias_of() == nil)\n"
        "  local writer = host:document('meta'); writer:finish()\n"
        "end\n";
    static const char denied[] =
        "return function(atlas, host)\n"
        "  assert(_ENV._G == nil and _ENV._VERSION == nil)\n"
        "  assert(_ENV.os == nil and _ENV.io == nil)\n"
        "  assert(_ENV.package == nil and _ENV.debug == nil)\n"
        "  assert(_ENV.coroutine == nil and _ENV.table == nil)\n"
        "  assert(_ENV.load == nil and _ENV.loadfile == nil)\n"
        "  assert(_ENV.dofile == nil and _ENV.require == nil)\n"
        "  assert(_ENV.collectgarbage == nil and _ENV.getmetatable == nil)\n"
        "  assert(_ENV.setmetatable == nil and _ENV.rawget == nil)\n"
        "  assert(_ENV.rawset == nil and _ENV.rawequal == nil)\n"
        "  assert(_ENV.rawlen == nil and _ENV.next == nil and _ENV.pairs == nil)\n"
        "  assert(_ENV.ipairs == nil and _ENV.print == nil and _ENV.warn == nil)\n"
        "  assert(_ENV.tonumber == nil and _ENV.tostring == nil)\n"
        "  assert(_ENV.pcall == nil and _ENV.xpcall == nil)\n"
        "  assert(string.dump == nil and string.find == nil)\n"
        "  assert(string.format == nil and string.gmatch == nil)\n"
        "  assert(string.gsub == nil and string.lower == nil)\n"
        "  assert(string.match == nil and string.pack == nil)\n"
        "  assert(string.packsize == nil and string.rep == nil)\n"
        "  assert(string.reverse == nil and string.unpack == nil)\n"
        "  assert(string.upper == nil)\n"
        "  assert(('x').dump == nil and ('x').find == nil)\n"
        "  assert(('x').format == nil and ('x').gmatch == nil)\n"
        "  assert(('x').gsub == nil and ('x').lower == nil)\n"
        "  assert(('x').match == nil and ('x').pack == nil)\n"
        "  assert(('x').packsize == nil and ('x').rep == nil)\n"
        "  assert(('x').reverse == nil and ('x').unpack == nil)\n"
        "  assert(('x').upper == nil)\n"
        "  assert(math.acos == nil and math.asin == nil and math.atan == nil)\n"
        "  assert(math.cos == nil and math.deg == nil and math.exp == nil)\n"
        "  assert(math.fmod == nil and math.huge == nil and math.log == nil)\n"
        "  assert(math.modf == nil and math.pi == nil and math.rad == nil)\n"
        "  assert(math.random == nil and math.randomseed == nil)\n"
        "  assert(math.sin == nil and math.sqrt == nil and math.tan == nil)\n"
        "  assert(utf8.charpattern == nil)\n"
        "  local writer = host:document('meta')\n"
        "  writer:write('sealed')\n"
        "  writer:finish()\n"
        "end\n";
    static const char unfinished[] =
        "return function(atlas, host)\n"
        "  local writer = host:document('meta')\n"
        "  writer:write('partial')\n"
        "end\n";
    static const char bad_index[] =
        "return function(atlas, host) atlas:sprite(3) end\n";
    static const char bad_type[] =
        "return function(atlas, host) atlas:page('1') end\n";
    static const char chunk_extra[] =
        "return function(atlas, host) end, 7\n";
    static const char chunk_not_handler[] = "return 7\n";
    static const char handler_return[] =
        "return function(atlas, host) "
        "local w=host:document('meta'); w:finish(); return 7 end\n";
    static const char retained_writer[] =
        "local retained\n"
        "return function(atlas, host) retained=host:document('meta'); "
        "retained:finish(); retained:write('late') end\n";
    static const char notices[] =
        "return function(atlas, host) host:notice('first'); "
        "host:notice('second'); local w=host:document('meta'); "
        "w:finish() end\n";
    static const char invalid_error[] =
        "return function(atlas, host) error(string.char(255)) end\n";
    static const char framed_error[] =
        "local function inner()\n"
        "  error('boom')\n"
        "end\n"
        "return function(atlas, host)\n"
        "  inner()\n"
        "end\n";
    char *owned_source = NULL;
    const char *source = unfinished;
    bool use_hidden = false;
    switch (scenario) {
        case 'A': source = allowed; break;
        case 'D': source = denied; break;
        case 'H': source = hidden; use_hidden = true; break;
        case 'I': source = bad_index; break;
        case 'B': source = bad_type; break;
        case 'C': source = chunk_extra; break;
        case 'N': source = chunk_not_handler; break;
        case 'R': source = handler_return; break;
        case 'W': source = retained_writer; break;
        case 'O': source = notices; break;
        case 'X': source = invalid_error; break;
        case 'F': source = framed_error; break;
        case 'G':
            owned_source = large_result_source();
            if (!owned_source) return 4;
            source = owned_source;
            break;
        default: break;
    }
    const tp_lua_runtime_input input = fixture_input(source, use_hidden);
    tp_lua_runtime_result result = {0};
    tp_error error = {{0}};
    const tp_status status = tp_lua_runtime_serialize(&input, &result, &error);
    child_reply reply = {.status = (int32_t)status};
    if (result.diagnostics &&
        tp_format_diagnostic_report_count(result.diagnostics) > 0U) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(result.diagnostics, 0U);
        reply.diagnostic = diagnostic ? (int32_t)diagnostic->code : 0;
        if (diagnostic) {
            if (diagnostic->message) {
                (void)snprintf(reply.diagnostic_message,
                               sizeof reply.diagnostic_message, "%s",
                               diagnostic->message);
            }
            reply.frame_count =
                diagnostic->frame_count <= TP_FORMAT_DIAGNOSTIC_FRAME_MAX
                    ? (uint32_t)diagnostic->frame_count
                    : (uint32_t)TP_FORMAT_DIAGNOSTIC_FRAME_MAX;
            for (uint32_t i = 0U; i < reply.frame_count; ++i) {
                reply.frame_lines[i] = diagnostic->frames[i].line;
                if (diagnostic->frames[i].text) {
                    (void)snprintf(reply.frame_text[i],
                                   sizeof reply.frame_text[i], "%s",
                                   diagnostic->frames[i].text);
                }
            }
        }
    }
    if (result.document_count == 1U && result.documents[0].byte_count <=
                                                sizeof reply.document) {
        reply.document_length = (uint32_t)result.documents[0].byte_count;
        memcpy(reply.document, result.documents[0].bytes,
               result.documents[0].byte_count);
    }
    reply.notice_count = result.notice_count <= UINT32_MAX
                             ? (uint32_t)result.notice_count
                             : UINT32_MAX;
    for (size_t i = 0U; i < result.notice_count && i < 2U; ++i) {
        (void)snprintf(reply.notices[i], sizeof reply.notices[i], "%s",
                       result.notices[i].message);
    }
    const bool wrote = fwrite(&reply, 1U, sizeof reply, stdout) == sizeof reply &&
                       fflush(stdout) == 0;
    tp_lua_runtime_result_destroy(&result);
    free(owned_source);
    return wrote ? 0 : 3;
}

static child_reply invoke(char scenario) {
    char self[4096];
    TEST_ASSERT_TRUE(tp_proc_self_path(self, sizeof self));
    tp_proc *process =
        tp_proc_spawn(self, TP_LUA_SANDBOX_CHILD_ARG, NULL);
    TEST_ASSERT_NOT_NULL(process);
    TEST_ASSERT_TRUE(tp_proc_write_stdin(process, &scenario, 1U));
    child_reply reply = {0};
    unsigned char wire[sizeof reply + 1U] = {0};
    size_t length = 0U;
    bool eof = false;
    TEST_ASSERT_TRUE(tp_proc_read_stdout(process, wire, sizeof wire,
                                         &length, &eof));
    TEST_ASSERT_TRUE(eof);
    TEST_ASSERT_EQUAL_size_t(sizeof reply, length);
    memcpy(&reply, wire, sizeof reply);
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

static void test_allowlist_and_opaque_views_execute_in_child_only(void) {
    const child_reply reply = invoke('A');
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, reply.status);
    TEST_ASSERT_EQUAL_INT(0, reply.diagnostic);
    TEST_ASSERT_EQUAL_UINT32(17U, reply.document_length);
    TEST_ASSERT_EQUAL_MEMORY("atlas|atlas-0.png", reply.document,
                             reply.document_length);
}

static void test_forbidden_standard_library_surface_is_absent(void) {
    const child_reply reply = invoke('D');
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, reply.status);
    TEST_ASSERT_EQUAL_UINT32(6U, reply.document_length);
    TEST_ASSERT_EQUAL_MEMORY("sealed", reply.document, reply.document_length);
}

static void test_unfinished_document_is_structured_failure(void) {
    const child_reply reply = invoke('U');
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, reply.status);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_DOCUMENT_UNFINISHED,
                          reply.diagnostic);
}

static void assert_diagnostic(char scenario,
                              tp_format_diagnostic_code expected) {
    const child_reply reply = invoke(scenario);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, reply.status);
    TEST_ASSERT_EQUAL_INT(expected, reply.diagnostic);
}

static void test_projected_visibility_hides_unsupported_fields(void) {
    const child_reply reply = invoke('H');
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, reply.status);
    TEST_ASSERT_EQUAL_UINT32(0U, reply.document_length);
}

static void test_bad_indices_and_types_are_handler_contract(void) {
    assert_diagnostic('I', TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT);
    assert_diagnostic('B', TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT);
}

static void test_chunk_shape_and_handler_return_arity_are_exact(void) {
    assert_diagnostic('C', TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT);
    assert_diagnostic('N', TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT);
    assert_diagnostic('R', TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT);
}

static void test_retained_writer_cannot_write_after_finish(void) {
    assert_diagnostic('W',
                      TP_FORMAT_DIAGNOSTIC_DOCUMENT_WRITE_AFTER_FINISH);
}

static void test_notices_preserve_deterministic_order(void) {
    const child_reply reply = invoke('O');
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, reply.status);
    TEST_ASSERT_EQUAL_UINT32(2U, reply.notice_count);
    TEST_ASSERT_EQUAL_STRING("first", reply.notices[0]);
    TEST_ASSERT_EQUAL_STRING("second", reply.notices[1]);
}

static void test_invalid_lua_error_text_uses_fixed_fallback(void) {
    const child_reply reply = invoke('X');
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, reply.status);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                          reply.diagnostic);
    TEST_ASSERT_EQUAL_STRING("Lua handler raised a non-text error",
                             reply.diagnostic_message);
}

static void test_frames_contain_only_target_lua_positive_lines(void) {
    const child_reply reply = invoke('F');
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, reply.status);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                          reply.diagnostic);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, reply.frame_count);
    for (uint32_t i = 0U; i < reply.frame_count; ++i) {
        TEST_ASSERT_GREATER_THAN_UINT32(0U, reply.frame_lines[i]);
        TEST_ASSERT_EQUAL_INT(
            0, strncmp(reply.frame_text[i],
                       "@formats/sandbox-test/export.lua:",
                       strlen("@formats/sandbox-test/export.lua:")));
    }
    TEST_ASSERT_NOT_NULL(strstr(reply.frame_text[0], "inner"));
}

static void test_large_handler_result_is_caught_inside_protected_boundary(void) {
    assert_diagnostic('G', TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], TP_LUA_SANDBOX_CHILD_ARG) == 0)
        return run_child();
    UNITY_BEGIN();
    RUN_TEST(test_allowlist_and_opaque_views_execute_in_child_only);
    RUN_TEST(test_forbidden_standard_library_surface_is_absent);
    RUN_TEST(test_unfinished_document_is_structured_failure);
    RUN_TEST(test_projected_visibility_hides_unsupported_fields);
    RUN_TEST(test_bad_indices_and_types_are_handler_contract);
    RUN_TEST(test_chunk_shape_and_handler_return_arity_are_exact);
    RUN_TEST(test_retained_writer_cannot_write_after_finish);
    RUN_TEST(test_notices_preserve_deterministic_order);
    RUN_TEST(test_invalid_lua_error_text_uses_fixed_fallback);
    RUN_TEST(test_frames_contain_only_target_lua_positive_lines);
    RUN_TEST(test_large_handler_result_is_caught_inside_protected_boundary);
    return UNITY_END();
}
