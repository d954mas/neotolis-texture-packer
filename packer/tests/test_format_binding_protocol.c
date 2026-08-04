#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "tp_core/tp_format.h"
#include "tp_core/tp_id.h"
#include "tp_format_binding_proto_internal.h"
#include "tp_format_descriptor_internal.h"
#include "tp_hex.h"

static const unsigned char g_descriptor[] =
    "{\"api_version\":1,\"id\":\"fixture-wire\","
    "\"display_name\":\"Fixture Wire\",\"capabilities\":{"
    "\"transforms\":[\"identity\"],\"polygons\":false,"
    "\"pivot\":false,\"slice9\":false,\"multipage\":true,"
    "\"aliases\":false,\"animations\":false},\"outputs\":[{"
    "\"id\":\"metadata\",\"suffix\":\".txt\"}]}";
static const unsigned char g_source[] = "return function() end\n";

static void put_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned int i = 0U; i < 4U; ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
}

static void put_u64(uint8_t *bytes, uint64_t value) {
    for (unsigned int i = 0U; i < 8U; ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
}

static void fingerprint_package(const unsigned char *descriptor,
                                size_t descriptor_size,
                                const unsigned char *source,
                                size_t source_size, char out[33]) {
    static const unsigned char tag[] = "ntpacker-format-package-v1";
    uint8_t api[4] = {1U, 0U, 0U, 0U};
    uint8_t descriptor_length[8];
    uint8_t source_length[8];
    put_u64(descriptor_length, descriptor_size);
    put_u64(source_length, source_size);
    tp_hasher hasher = tp_hasher_init();
    tp_hasher_update(&hasher, tag, sizeof tag);
    tp_hasher_update(&hasher, api, sizeof api);
    tp_hasher_update(&hasher, descriptor_length, sizeof descriptor_length);
    tp_hasher_update(&hasher, descriptor, descriptor_size);
    tp_hasher_update(&hasher, source_length, sizeof source_length);
    tp_hasher_update(&hasher, source, source_size);
    const tp_id128 value = tp_hasher_final(hasher);
    tp_hex_encode_lower(value.bytes, sizeof value.bytes, out);
}

static tp_format_owned_descriptor *parse_descriptor(void) {
    tp_format_descriptor_parse_result parsed = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_descriptor_v1_parse(g_descriptor, sizeof g_descriptor - 1U,
                                      &parsed, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_DESCRIPTOR_ADMITTED, parsed.outcome);
    TEST_ASSERT_NOT_NULL(parsed.owned_descriptor);
    return parsed.owned_descriptor;
}

static const tp_format_descriptor *native_descriptor(void) {
    tp_format_catalog_row row = {0};
    TEST_ASSERT_TRUE(tp_format_catalog_row_at(tp_format_catalog_native(), 0U,
                                              &row));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_IMPLEMENTATION_NATIVE,
                          row.implementation);
    TEST_ASSERT_NOT_NULL(row.descriptor);
    return row.descriptor;
}

static tp_id128 test_id(uint8_t last) {
    tp_id128 id = {{0}};
    id.bytes[15] = last;
    return id;
}

static tp_format_binding_proto_binding lua_binding(
    const tp_format_descriptor *descriptor) {
    tp_format_binding_proto_binding binding = {
        .implementation = TP_FORMAT_IMPLEMENTATION_LUA,
        .descriptor = descriptor,
        .api_version = TP_FORMAT_API_VERSION,
        .descriptor_bytes = g_descriptor,
        .descriptor_byte_count = sizeof g_descriptor - 1U,
        .source_bytes = g_source,
        .source_byte_count = sizeof g_source - 1U,
    };
    fingerprint_package(g_descriptor, sizeof g_descriptor - 1U, g_source,
                        sizeof g_source - 1U, binding.fingerprint);
    return binding;
}

static uint8_t *encode_single_lua(size_t *out_length) {
    tp_format_owned_descriptor *owned = parse_descriptor();
    tp_format_binding_proto_binding binding =
        lua_binding(tp_format_owned_descriptor_view(owned));
    tp_format_binding_proto_value value = {
        .bindings = &binding,
        .binding_count = 1U,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index = 0U},
    };
    uint8_t *bytes = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, out_length, &error),
        error.msg);
    tp_format_owned_descriptor_destroy(owned);
    return bytes;
}

void setUp(void) {}
void tearDown(void) {}

void test_round_trip_preserves_exact_bindings_refs_and_diagnostics(void) {
    tp_format_owned_descriptor *owned = parse_descriptor();
    tp_format_binding_proto_binding bindings[2] = {
        {.implementation = TP_FORMAT_IMPLEMENTATION_NATIVE,
         .descriptor = native_descriptor()},
        lua_binding(tp_format_owned_descriptor_view(owned)),
    };
    const tp_format_diagnostic_frame frames[] = {
        {.text = "@formats/fixture-wire/export.lua:handler", .line = 7U},
    };
    const tp_format_diagnostic diagnostics[] = {
        {.severity = TP_FORMAT_DIAGNOSTIC_ERROR,
         .code = TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
         .phase = TP_FORMAT_PHASE_COMPILE,
         .format_id = "fixture-wire",
         .package_path = "formats/fixture-wire/export.lua",
         .line = 7U,
         .column = 2U,
         .message = "syntax error",
         .frames = frames,
         .frame_count = 1U},
    };
    const tp_format_binding_proto_target_ref targets[] = {
        {.atlas_id = {{0}},
         .target_id = {{0}},
         .resolution = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                        .binding_index = 0U}},
        {.atlas_id = {{0}},
         .target_id = {{0}},
         .resolution = {.kind = TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE,
                        .binding_index = UINT32_MAX,
                        .diagnostic_offset = 0U,
                        .diagnostic_count = 1U}},
        {.atlas_id = {{0}},
         .target_id = {{0}},
         .resolution = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT}},
    };
    tp_format_binding_proto_target_ref mutable_targets[3];
    memcpy(mutable_targets, targets, sizeof targets);
    for (size_t i = 0U; i < 3U; ++i) {
        mutable_targets[i].atlas_id = test_id((uint8_t)(i + 1U));
        mutable_targets[i].target_id = test_id((uint8_t)(i + 11U));
    }
    tp_format_binding_proto_value input = {
        .bindings = bindings,
        .binding_count = 2U,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index = 1U},
        .targets = mutable_targets,
        .target_count = 3U,
        .diagnostics = (tp_format_diagnostic *)diagnostics,
        .diagnostic_count = 1U,
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&input, &bytes, &length, &error),
        error.msg);

    tp_format_binding_proto_value decoded = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error),
        error.msg);
    free(bytes);
    tp_format_owned_descriptor_destroy(owned);

    TEST_ASSERT_EQUAL_size_t(2U, decoded.binding_count);
    TEST_ASSERT_EQUAL_STRING(native_descriptor()->id,
                             decoded.bindings[0].descriptor->id);
    TEST_ASSERT_EQUAL_STRING("fixture-wire",
                             decoded.bindings[1].descriptor->id);
    TEST_ASSERT_EQUAL_MEMORY(g_descriptor,
                             decoded.bindings[1].descriptor_bytes,
                             sizeof g_descriptor - 1U);
    TEST_ASSERT_EQUAL_MEMORY(g_source, decoded.bindings[1].source_bytes,
                             sizeof g_source - 1U);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_BINDING_RESOLUTION_BINDING,
                          decoded.preview.kind);
    TEST_ASSERT_EQUAL_UINT32(1U, decoded.preview.binding_index);
    TEST_ASSERT_EQUAL_size_t(3U, decoded.target_count);
    TEST_ASSERT_TRUE(tp_id128_eq(test_id(2U), decoded.targets[1].atlas_id));
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE,
                          decoded.targets[1].resolution.kind);
    TEST_ASSERT_EQUAL_INT(TP_FORMAT_BINDING_RESOLUTION_ABSENT,
                          decoded.targets[2].resolution.kind);
    TEST_ASSERT_EQUAL_size_t(1U, decoded.diagnostic_count);
    TEST_ASSERT_EQUAL_STRING("syntax error", decoded.diagnostics[0].message);
    TEST_ASSERT_EQUAL_STRING("@formats/fixture-wire/export.lua:handler",
                             decoded.diagnostics[0].frames[0].text);
    TEST_ASSERT_EQUAL_UINT32(7U, decoded.diagnostics[0].frames[0].line);
    TEST_ASSERT_TRUE(decoded.diagnostic_dynamic_bytes > 0U);
    tp_format_binding_proto_value_free(&decoded);
    tp_format_binding_proto_value_free(&decoded);
}

void test_lua_fingerprint_descriptor_and_source_tampering_is_rejected(void) {
    size_t length = 0U;
    uint8_t *bytes = encode_single_lua(&length);
    const size_t descriptor_offset = 108U;
    TEST_ASSERT_TRUE(length > descriptor_offset);
    bytes[descriptor_offset] ^= 1U;
    tp_format_binding_proto_value decoded = {0};
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    tp_format_binding_proto_value_free(&decoded);
    free(bytes);

    bytes = encode_single_lua(&length);
    const size_t source_offset = descriptor_offset + sizeof g_descriptor - 1U;
    TEST_ASSERT_TRUE(length > source_offset);
    bytes[source_offset] ^= 1U;
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    free(bytes);

    bytes = encode_single_lua(&length);
    bytes[60U] = bytes[60U] == (uint8_t)'0' ? (uint8_t)'1' : (uint8_t)'0';
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    free(bytes);
}

void test_native_binding_must_match_compiled_in_table(void) {
    tp_format_descriptor fake = *native_descriptor();
    tp_format_binding_proto_binding binding = {
        .implementation = TP_FORMAT_IMPLEMENTATION_NATIVE,
        .descriptor = &fake,
    };
    tp_format_binding_proto_value value = {
        .bindings = &binding,
        .binding_count = 1U,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index = 0U},
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);
}

void test_lua_encode_rejects_same_id_semantic_mismatch_and_unterminated_id(
    void) {
    tp_format_owned_descriptor *owned = parse_descriptor();
    tp_format_descriptor mismatched =
        *tp_format_owned_descriptor_view(owned);
    mismatched.display_name = "Different caller view";
    tp_format_binding_proto_binding binding = lua_binding(&mismatched);
    tp_format_binding_proto_value value = {
        .bindings = &binding,
        .binding_count = 1U,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index = 0U},
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);

    char unterminated[TP_FORMAT_ID_MAX_BYTES + 1U];
    memset(unterminated, 'a', sizeof unterminated);
    mismatched = *tp_format_owned_descriptor_view(owned);
    mismatched.id = unterminated;
    binding = lua_binding(&mismatched);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);
    tp_format_owned_descriptor_destroy(owned);
}

static void assert_bad_source_rejected(const unsigned char *source,
                                       size_t source_size) {
    tp_format_owned_descriptor *owned = parse_descriptor();
    tp_format_binding_proto_binding binding =
        lua_binding(tp_format_owned_descriptor_view(owned));
    binding.source_bytes = source;
    binding.source_byte_count = source_size;
    if (source_size <= TP_FORMAT_SOURCE_MAX_BYTES) {
        fingerprint_package(g_descriptor, sizeof g_descriptor - 1U, source,
                            source_size, binding.fingerprint);
    }
    tp_format_binding_proto_value value = {
        .bindings = &binding,
        .binding_count = 1U,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index = 0U},
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);
    tp_format_owned_descriptor_destroy(owned);
}

void test_source_admission_rejects_utf8_nul_bom_bytecode_and_size_max(void) {
    static const unsigned char invalid_utf8[] = {0xffU};
    static const unsigned char embedded_nul[] = {'a', 0U, 'b'};
    static const unsigned char bom[] = {0xefU, 0xbbU, 0xbfU, 'x'};
    static const unsigned char bytecode[] = {0x1bU, 'L', 'u', 'a'};
    assert_bad_source_rejected(invalid_utf8, sizeof invalid_utf8);
    assert_bad_source_rejected(embedded_nul, sizeof embedded_nul);
    assert_bad_source_rejected(bom, sizeof bom);
    assert_bad_source_rejected(bytecode, sizeof bytecode);
    assert_bad_source_rejected(g_source, SIZE_MAX);
}

void test_api_and_native_id_wire_tampering_is_rejected(void) {
    size_t length = 0U;
    uint8_t *bytes = encode_single_lua(&length);
    put_u32(bytes + 56U, TP_FORMAT_API_VERSION + 1U);
    tp_format_binding_proto_value decoded = {0};
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    free(bytes);

    tp_format_binding_proto_binding binding = {
        .implementation = TP_FORMAT_IMPLEMENTATION_NATIVE,
        .descriptor = native_descriptor(),
    };
    tp_format_binding_proto_value value = {
        .bindings = &binding,
        .binding_count = 1U,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index = 0U},
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error),
        error.msg);
    /* One native binding begins at 52: kind, length, then exact ID bytes. */
    const uint32_t id_length = (uint32_t)strlen(native_descriptor()->id);
    TEST_ASSERT_TRUE(id_length > 0U);
    memset(bytes + 60U, 'x', id_length);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    free(bytes);
}

void test_native_and_lua_duplicate_identities_are_rejected(void) {
    tp_format_binding_proto_binding native[2] = {
        {.implementation = TP_FORMAT_IMPLEMENTATION_NATIVE,
         .descriptor = native_descriptor()},
        {.implementation = TP_FORMAT_IMPLEMENTATION_NATIVE,
         .descriptor = native_descriptor()},
    };
    tp_format_binding_proto_value value = {
        .bindings = native,
        .binding_count = 2U,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index = 0U},
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));

    tp_format_owned_descriptor *owned = parse_descriptor();
    tp_format_binding_proto_binding lua[2] = {
        lua_binding(tp_format_owned_descriptor_view(owned)),
        lua_binding(tp_format_owned_descriptor_view(owned)),
    };
    value.bindings = lua;
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));

    static const unsigned char second_source[] =
        "-- distinct\nreturn function() end\n";
    lua[1].source_bytes = second_source;
    lua[1].source_byte_count = sizeof second_source - 1U;
    fingerprint_package(g_descriptor, sizeof g_descriptor - 1U, second_source,
                        sizeof second_source - 1U, lua[1].fingerprint);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error),
        error.msg);
    free(bytes);
    tp_format_owned_descriptor_destroy(owned);
}

void test_malformed_resolution_slices_ids_and_trailing_bytes_are_rejected(void) {
    size_t length = 0U;
    uint8_t *bytes = encode_single_lua(&length);
    put_u32(bytes + 36U, TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE);
    put_u32(bytes + 40U, UINT32_MAX);
    put_u32(bytes + 44U, 0U);
    put_u32(bytes + 48U, 1U);
    tp_format_binding_proto_value decoded = {0};
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    free(bytes);

    bytes = encode_single_lua(&length);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length - 1U, &decoded, &error));
    free(bytes);

    tp_format_binding_proto_target_ref target = {
        .atlas_id = {{0}},
        .target_id = {{0}},
        .resolution = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT},
    };
    tp_format_binding_proto_value value = {
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT},
        .targets = &target,
        .target_count = 1U,
    };
    uint8_t *encoded = NULL;
    size_t encoded_length = 0U;
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK, tp_format_binding_proto_encode(
                          &value, &encoded, &encoded_length, &error));

    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
        .phase = TP_FORMAT_PHASE_COMPILE,
        .message = "endpoint",
    };
    value = (tp_format_binding_proto_value){
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE,
                    .binding_index = UINT32_MAX,
                    .diagnostic_offset = 0U,
                    .diagnostic_count = 1U},
        .diagnostics = (tp_format_diagnostic *)&diagnostic,
        .diagnostic_count = 1U,
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &encoded, &encoded_length,
                                       &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(encoded, encoded_length, &decoded,
                                       &error),
        error.msg);
    tp_format_binding_proto_value_free(&decoded);
    put_u32(encoded + 44U, UINT32_MAX);
    put_u32(encoded + 48U, 2U);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(encoded, encoded_length, &decoded,
                                       &error));
    free(encoded);
}

void test_declared_caps_and_version_are_rejected_before_materialization(void) {
    size_t length = 0U;
    uint8_t *bytes = encode_single_lua(&length);
    tp_format_binding_proto_value decoded = {0};
    tp_error error = {{0}};

    put_u16(bytes + 4U, TP_FORMAT_BINDING_PROTO_VERSION + 1U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_BAD_VERSION,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    put_u16(bytes + 4U, TP_FORMAT_BINDING_PROTO_VERSION);

    put_u32(bytes + 12U, TP_FORMAT_BINDING_PROTO_MAX_BINDINGS + 1U);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    put_u32(bytes + 12U, 1U);
    put_u32(bytes + 16U, TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS + 1U);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    put_u32(bytes + 16U, 0U);
    put_u32(bytes + 20U, TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS + 1U);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    put_u32(bytes + 20U, 0U);
    put_u32(bytes + 24U, 0U);
    put_u64(bytes + 28U, TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    TEST_ASSERT_EQUAL_STRING("tp_format_binding_proto: package byte total mismatch",
                             error.msg);
    put_u32(bytes + 24U,
            TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTIC_BYTES + 1U);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    put_u32(bytes + 24U, 0U);
    put_u64(bytes + 28U, TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES + 1ULL);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    put_u64(bytes + 28U,
            (sizeof g_descriptor - 1U) + (sizeof g_source - 1U));
    put_u32(bytes + 8U,
            (uint32_t)TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES + 1U);
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error));
    free(bytes);
}

void test_encode_rejects_count_plus_one_and_noncanonical_resolution_fields(void) {
    tp_format_binding_proto_value value = {
        .binding_count = TP_FORMAT_BINDING_PROTO_MAX_BINDINGS + 1U,
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));

    value = (tp_format_binding_proto_value){
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT,
                    .binding_index = 1U},
    };
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));

    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
        .phase = TP_FORMAT_PHASE_COMPILE,
        .message = "x",
    };
    value = (tp_format_binding_proto_value){
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE,
                    .binding_index = UINT32_MAX,
                    .diagnostic_offset = UINT32_MAX,
                    .diagnostic_count = 2U},
        .diagnostics = (tp_format_diagnostic *)&diagnostic,
        .diagnostic_count = 1U,
    };
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));
}

void test_duplicate_stable_target_pair_is_rejected(void) {
    tp_format_binding_proto_target_ref targets[2] = {
        {.atlas_id = {{0}},
         .target_id = {{0}},
         .resolution = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT}},
        {.atlas_id = {{0}},
         .target_id = {{0}},
         .resolution = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT}},
    };
    targets[0].atlas_id = targets[1].atlas_id = test_id(1U);
    targets[0].target_id = targets[1].target_id = test_id(2U);
    tp_format_binding_proto_value value = {
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT},
        .targets = targets,
        .target_count = 2U,
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);
}

void test_exact_binding_diagnostic_and_target_count_boundaries_round_trip(void) {
    tp_format_owned_descriptor *owned = parse_descriptor();
    tp_format_binding_proto_binding *bindings =
        (tp_format_binding_proto_binding *)calloc(
            TP_FORMAT_BINDING_PROTO_MAX_BINDINGS, sizeof *bindings);
    char(*sources)[40] =
        (char(*)[40])calloc(TP_FORMAT_BINDING_PROTO_MAX_BINDINGS,
                            sizeof *sources);
    TEST_ASSERT_NOT_NULL(bindings);
    TEST_ASSERT_NOT_NULL(sources);
    for (size_t i = 0U; i < TP_FORMAT_BINDING_PROTO_MAX_BINDINGS; ++i) {
        (void)snprintf(sources[i], sizeof sources[i],
                       "--%u\nreturn function() end\n", (unsigned int)i);
        bindings[i] = lua_binding(tp_format_owned_descriptor_view(owned));
        bindings[i].source_bytes = (const unsigned char *)sources[i];
        bindings[i].source_byte_count = strlen(sources[i]);
        fingerprint_package(g_descriptor, sizeof g_descriptor - 1U,
                            bindings[i].source_bytes,
                            bindings[i].source_byte_count,
                            bindings[i].fingerprint);
    }
    tp_format_binding_proto_value value = {
        .bindings = bindings,
        .binding_count = TP_FORMAT_BINDING_PROTO_MAX_BINDINGS,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index =
                        TP_FORMAT_BINDING_PROTO_MAX_BINDINGS - 1U},
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error),
        error.msg);
    tp_format_binding_proto_value decoded = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(TP_FORMAT_BINDING_PROTO_MAX_BINDINGS,
                             decoded.binding_count);
    tp_format_binding_proto_value_free(&decoded);
    free(bytes);
    free(sources);
    free(bindings);
    tp_format_owned_descriptor_destroy(owned);

    tp_format_diagnostic *diagnostics = (tp_format_diagnostic *)calloc(
        TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS, sizeof *diagnostics);
    char maximum_message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
    memset(maximum_message, 'm', sizeof maximum_message - 1U);
    maximum_message[sizeof maximum_message - 1U] = '\0';
    TEST_ASSERT_NOT_NULL(diagnostics);
    for (size_t i = 0U; i < TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS; ++i) {
        diagnostics[i] = (tp_format_diagnostic){
            .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
            .code = TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
            .phase = TP_FORMAT_PHASE_COMPILE,
            .message = maximum_message,
        };
    }
    value = (tp_format_binding_proto_value){
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE,
                    .binding_index = UINT32_MAX,
                    .diagnostic_offset = 0U,
                    .diagnostic_count =
                        TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS},
        .diagnostics = diagnostics,
        .diagnostic_count = TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS,
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS,
                             decoded.diagnostic_count);
    TEST_ASSERT_EQUAL_size_t(TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTIC_BYTES,
                             decoded.diagnostic_dynamic_bytes);
    tp_format_binding_proto_value_free(&decoded);
    free(bytes);

    const tp_format_diagnostic_frame plus_one = {.text = "y", .line = 1U};
    diagnostics[0].frames = &plus_one;
    diagnostics[0].frame_count = 1U;
    bytes = NULL;
    length = 0U;
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error));
    TEST_ASSERT_NULL(bytes);
    free(diagnostics);

    tp_format_binding_proto_target_ref *targets =
        (tp_format_binding_proto_target_ref *)calloc(
            TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS, sizeof *targets);
    TEST_ASSERT_NOT_NULL(targets);
    for (size_t i = 0U; i < TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS; ++i) {
        targets[i].atlas_id = test_id(1U);
        const uint32_t ordinal = (uint32_t)i + 1U;
        targets[i].target_id.bytes[12] = (uint8_t)(ordinal >> 24U);
        targets[i].target_id.bytes[13] = (uint8_t)(ordinal >> 16U);
        targets[i].target_id.bytes[14] = (uint8_t)(ordinal >> 8U);
        targets[i].target_id.bytes[15] = (uint8_t)ordinal;
        targets[i].resolution.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT;
    }
    value = (tp_format_binding_proto_value){
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT},
        .targets = targets,
        .target_count = TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS,
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&value, &bytes, &length, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_decode(bytes, length, &decoded, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS,
                             decoded.target_count);
    tp_format_binding_proto_value_free(&decoded);
    free(bytes);
    free(targets);
}

void test_frame_cap_is_the_checked_frozen_component_derivation(void) {
    const uint64_t expected =
        TP_FORMAT_BINDING_PROTO_FIXED_PAYLOAD_BYTES +
        TP_FORMAT_BINDING_PROTO_BINDING_TABLE_MAX_BYTES +
        TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS *
            TP_FORMAT_BINDING_PROTO_TARGET_REF_BYTES +
        TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS *
            TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FIXED_BYTES +
        TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS *
            TP_FORMAT_DIAGNOSTIC_FRAME_MAX *
            TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FRAME_FIXED_BYTES +
        TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTIC_BYTES;
    TEST_ASSERT_EQUAL_UINT64(expected,
                             TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES);
    TEST_ASSERT_EQUAL_UINT64(
        61U, TP_FORMAT_BINDING_PROTO_LUA_BINDINGS_FOR_PACKAGE_MAX);
    TEST_ASSERT_EQUAL_UINT64(
        TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES +
            61U * TP_FORMAT_BINDING_PROTO_LUA_BINDING_FIXED_BYTES +
            (TP_FORMAT_BINDING_PROTO_MAX_BINDINGS - 61U) *
                TP_FORMAT_BINDING_PROTO_NATIVE_BINDING_MAX_BYTES,
        TP_FORMAT_BINDING_PROTO_BINDING_TABLE_MAX_BYTES);
    TEST_ASSERT_TRUE(TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES <= UINT32_MAX);
}

void test_reachable_frames_match_the_fixed_wire_derivation(void) {
    tp_error error = {{0}};
    uint8_t *bytes = NULL;
    size_t length = 0U;
    tp_format_binding_proto_value empty = {
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_ABSENT},
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&empty, &bytes, &length, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(12U + TP_FORMAT_BINDING_PROTO_FIXED_PAYLOAD_BYTES,
                             length);
    free(bytes);

    tp_format_binding_proto_binding native = {
        .implementation = TP_FORMAT_IMPLEMENTATION_NATIVE,
        .descriptor = native_descriptor(),
    };
    empty.bindings = &native;
    empty.binding_count = 1U;
    empty.preview.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING;
    empty.preview.binding_index = 0U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&empty, &bytes, &length, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(
        12U + TP_FORMAT_BINDING_PROTO_FIXED_PAYLOAD_BYTES + 8U +
            strlen(native.descriptor->id),
        length);
    free(bytes);

    tp_format_owned_descriptor *owned = parse_descriptor();
    tp_format_binding_proto_binding lua =
        lua_binding(tp_format_owned_descriptor_view(owned));
    empty.bindings = &lua;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_binding_proto_encode(&empty, &bytes, &length, &error),
        error.msg);
    TEST_ASSERT_EQUAL_size_t(
        12U + TP_FORMAT_BINDING_PROTO_FIXED_PAYLOAD_BYTES +
            TP_FORMAT_BINDING_PROTO_LUA_BINDING_FIXED_BYTES +
            sizeof g_descriptor - 1U + sizeof g_source - 1U,
        length);
    free(bytes);
    tp_format_owned_descriptor_destroy(owned);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_preserves_exact_bindings_refs_and_diagnostics);
    RUN_TEST(
        test_lua_fingerprint_descriptor_and_source_tampering_is_rejected);
    RUN_TEST(test_native_binding_must_match_compiled_in_table);
    RUN_TEST(
        test_lua_encode_rejects_same_id_semantic_mismatch_and_unterminated_id);
    RUN_TEST(
        test_source_admission_rejects_utf8_nul_bom_bytecode_and_size_max);
    RUN_TEST(test_api_and_native_id_wire_tampering_is_rejected);
    RUN_TEST(test_native_and_lua_duplicate_identities_are_rejected);
    RUN_TEST(
        test_malformed_resolution_slices_ids_and_trailing_bytes_are_rejected);
    RUN_TEST(
        test_declared_caps_and_version_are_rejected_before_materialization);
    RUN_TEST(
        test_encode_rejects_count_plus_one_and_noncanonical_resolution_fields);
    RUN_TEST(test_duplicate_stable_target_pair_is_rejected);
    RUN_TEST(
        test_exact_binding_diagnostic_and_target_count_boundaries_round_trip);
    RUN_TEST(test_frame_cap_is_the_checked_frozen_component_derivation);
    RUN_TEST(test_reachable_frames_match_the_fixed_wire_derivation);
    return UNITY_END();
}
