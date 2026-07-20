/* Versioned build-worker protocol codec (decision 0018, ROADMAP H0.3).
 *
 * Pins the request/response wire: encode->decode round-trips EXACTLY across a
 * rich multi-sprite atlas (a request carries one atlas by design; multi-atlas
 * orchestration is a higher layer), the zero-sprite edge, and a max-length name.
 * Then the fail-closed matrix -- truncated frame, oversized declared length,
 * wrong magic, unsupported version, and a corrupt payload -- each returns a
 * structured status and never asserts/crashes (runs under the sanitizer config).
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tp_build_proto_internal.h"
#include "tp_core/tp_error.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* The frame header and request head are fixed, _Static_assert-pinned wire sizes;
 * the fail-closed cases poke fields by their contract offset. The sprite head
 * (40 bytes) follows the request head + the sample's atlas/out names; its rgba_len
 * is the last u32. encode_sample uses atlas_name "a" (1) and out_name "a.ntpack" (8). */
enum {
    PROTO_FRAME_HDR = 12,
    PROTO_REQ_HEAD = 48,
    PROTO_SPRITE_HEAD = 40,
    SAMPLE_ATLAS_LEN = 1,
    SAMPLE_OUT_LEN = 8,
    SAMPLE_SPRITE_HEAD_OFF = PROTO_FRAME_HDR + PROTO_REQ_HEAD + SAMPLE_ATLAS_LEN + SAMPLE_OUT_LEN,
    SAMPLE_RGBA_LEN_OFF = SAMPLE_SPRITE_HEAD_OFF + (PROTO_SPRITE_HEAD - 4),
    REQ_ATLAS_NAME_LEN_OFF = PROTO_FRAME_HDR + 36, /* atlas_name_len field in request head */
    REQ_SPRITE_COUNT_OFF = PROTO_FRAME_HDR + PROTO_REQ_HEAD - 4
};

static uint8_t *make_pixels(uint32_t w, uint32_t h, uint8_t seed) {
    size_t n = (size_t)w * h * 4U;
    uint8_t *p = (uint8_t *)malloc(n);
    TEST_ASSERT_NOT_NULL(p);
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(seed + i);
    }
    return p;
}

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Round-trip is byte-exact, so floats compare bit-for-bit (this Unity build
 * disables the tolerance-based float asserts). */
static void assert_float_bits_equal(float a, float b) {
    uint32_t ua = 0, ub = 0;
    memcpy(&ua, &a, sizeof ua);
    memcpy(&ub, &b, sizeof ub);
    TEST_ASSERT_EQUAL_HEX32(ua, ub);
}

static void assert_sprite_equal(const tp_build_proto_sprite *a, const tp_build_proto_sprite *b) {
    TEST_ASSERT_EQUAL_STRING(a->name, b->name);
    TEST_ASSERT_EQUAL_UINT32(a->width, b->width);
    TEST_ASSERT_EQUAL_UINT32(a->height, b->height);
    assert_float_bits_equal(a->origin_x, b->origin_x);
    assert_float_bits_equal(a->origin_y, b->origin_y);
    for (int k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT16(a->slice9_lrtb[k], b->slice9_lrtb[k]);
    }
    TEST_ASSERT_EQUAL_UINT8(a->ov_mask, b->ov_mask);
    TEST_ASSERT_EQUAL_UINT8(a->ov_shape, b->ov_shape);
    TEST_ASSERT_EQUAL_UINT8(a->ov_allow_rotate, b->ov_allow_rotate);
    TEST_ASSERT_EQUAL_UINT8(a->ov_max_vertices, b->ov_max_vertices);
    TEST_ASSERT_EQUAL_UINT8(a->ov_margin, b->ov_margin);
    TEST_ASSERT_EQUAL_UINT8(a->ov_extrude, b->ov_extrude);
    TEST_ASSERT_EQUAL_INT(0, memcmp(a->rgba, b->rgba, (size_t)a->width * a->height * 4U));
}

static void assert_request_equal(const tp_build_proto_request *a, const tp_build_proto_request *b) {
    TEST_ASSERT_EQUAL_INT32(a->max_size, b->max_size);
    TEST_ASSERT_EQUAL_INT32(a->padding, b->padding);
    TEST_ASSERT_EQUAL_INT32(a->margin, b->margin);
    TEST_ASSERT_EQUAL_INT32(a->extrude, b->extrude);
    TEST_ASSERT_EQUAL_INT32(a->alpha_threshold, b->alpha_threshold);
    TEST_ASSERT_EQUAL_INT32(a->max_vertices, b->max_vertices);
    TEST_ASSERT_EQUAL_INT32(a->shape, b->shape);
    TEST_ASSERT_EQUAL_UINT8(a->allow_transform, b->allow_transform);
    TEST_ASSERT_EQUAL_UINT8(a->power_of_two, b->power_of_two);
    assert_float_bits_equal(a->pixels_per_unit, b->pixels_per_unit);
    TEST_ASSERT_EQUAL_STRING(a->atlas_name, b->atlas_name);
    TEST_ASSERT_EQUAL_STRING(a->out_name, b->out_name);
    TEST_ASSERT_EQUAL_UINT32(a->sprite_count, b->sprite_count);
    for (uint32_t i = 0; i < a->sprite_count; i++) {
        assert_sprite_equal(&a->sprites[i], &b->sprites[i]);
    }
}

/* Encode `req`, decode it back, assert structural equality, free everything. */
static void roundtrip_request(const tp_build_proto_request *req) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_build_proto_encode_request(req, &bytes, &len, &err), err.msg);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_TRUE(len >= PROTO_FRAME_HDR + PROTO_REQ_HEAD);

    tp_build_proto_request decoded;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_build_proto_decode_request(bytes, len, &decoded, &err), err.msg);
    assert_request_equal(req, &decoded);
    tp_build_proto_request_free(&decoded);
    free(bytes);
}

void test_request_roundtrip_multi_sprite(void) {
    uint8_t *px0 = make_pixels(4, 4, 10);
    uint8_t *px1 = make_pixels(96, 48, 40);
    uint8_t *px2 = make_pixels(1, 1, 200);
    tp_build_proto_sprite sprites[3] = {
        {.name = "disc_red", .width = 4, .height = 4, .origin_x = 0.5F, .origin_y = 0.5F, .rgba = px0},
        {.name = "banner",
         .width = 96,
         .height = 48,
         .origin_x = 0.25F,
         .origin_y = 0.75F,
         .slice9_lrtb = {2, 3, 4, 5},
         .ov_mask = 0x1F,
         .ov_shape = 1,
         .ov_allow_rotate = 1,
         .ov_max_vertices = 8,
         .ov_margin = 2,
         .ov_extrude = 1,
         .rgba = px1},
        {.name = "dot", .width = 1, .height = 1, .origin_x = 0.0F, .origin_y = 1.0F, .rgba = px2},
    };
    tp_build_proto_request req = {
        .max_size = 2048,
        .padding = 2,
        .margin = 1,
        .extrude = 0,
        .alpha_threshold = 128,
        .max_vertices = 8,
        .shape = 2,
        .allow_transform = 1,
        .power_of_two = 1,
        .pixels_per_unit = 16.0F,
        .atlas_name = "sheet",
        .out_name = "sheet.ntpack",
        .sprites = sprites,
        .sprite_count = 3,
    };
    roundtrip_request(&req);
    free(px0);
    free(px1);
    free(px2);
}

void test_request_roundtrip_zero_sprites(void) {
    tp_build_proto_request req = {
        .max_size = 512,
        .padding = 0,
        .margin = 0,
        .extrude = 0,
        .alpha_threshold = 1,
        .max_vertices = 16,
        .shape = 0,
        .allow_transform = 0,
        .power_of_two = 0,
        .pixels_per_unit = 1.0F,
        .atlas_name = "empty",
        .out_name = "empty.ntpack",
        .sprites = NULL,
        .sprite_count = 0,
    };
    roundtrip_request(&req);
}

void test_request_roundtrip_max_name_length(void) {
    char *big = (char *)malloc(TP_BUILD_PROTO_MAX_NAME_BYTES + 1U);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'a', TP_BUILD_PROTO_MAX_NAME_BYTES);
    big[TP_BUILD_PROTO_MAX_NAME_BYTES] = '\0';
    uint8_t *px = make_pixels(2, 2, 7);
    tp_build_proto_sprite sprite = {.name = big, .width = 2, .height = 2, .origin_x = 0.5F, .origin_y = 0.5F, .rgba = px};
    tp_build_proto_request req = {
        .max_size = 1024,
        .pixels_per_unit = 1.0F,
        .atlas_name = big,
        .out_name = big,
        .sprites = &sprite,
        .sprite_count = 1,
    };
    roundtrip_request(&req);

    /* One byte over the cap fails closed at encode. */
    char *over = (char *)malloc(TP_BUILD_PROTO_MAX_NAME_BYTES + 2U);
    TEST_ASSERT_NOT_NULL(over);
    memset(over, 'b', TP_BUILD_PROTO_MAX_NAME_BYTES + 1U);
    over[TP_BUILD_PROTO_MAX_NAME_BYTES + 1U] = '\0';
    tp_build_proto_request bad = req;
    bad.atlas_name = over;
    uint8_t *bytes = NULL;
    size_t len = 0;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_build_proto_encode_request(&bad, &bytes, &len, &err));
    TEST_ASSERT_NULL(bytes);

    free(over);
    free(px);
    free(big);
}

void test_response_roundtrip(void) {
    const tp_build_proto_response cases[] = {
        {.status = TP_STATUS_OK, .builder_code = 0, .artifact_name = "sheet.ntpack", .message = ""},
        {.status = TP_STATUS_BUILDER_FAILED, .builder_code = 3, .artifact_name = "",
         .message = "nt_builder_finish_pack failed"},
        {.status = TP_STATUS_BUILDER_CRASHED, .builder_code = 0, .artifact_name = NULL, .message = NULL},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        tp_error err = {{0}};
        TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_build_proto_encode_response(&cases[c], &bytes, &len, &err),
                                      err.msg);
        tp_build_proto_response decoded;
        TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_build_proto_decode_response(bytes, len, &decoded, &err), err.msg);
        TEST_ASSERT_EQUAL_INT32(cases[c].status, decoded.status);
        TEST_ASSERT_EQUAL_INT32(cases[c].builder_code, decoded.builder_code);
        TEST_ASSERT_EQUAL_STRING(cases[c].artifact_name ? cases[c].artifact_name : "", decoded.artifact_name);
        TEST_ASSERT_EQUAL_STRING(cases[c].message ? cases[c].message : "", decoded.message);
        tp_build_proto_response_free(&decoded);
        free(bytes);
    }
}

/* ---- fail-closed matrix: a corrupt frame is a structured error, never UB ---- */

/* Encode a fixed single-sprite request into a freshly malloc'd buffer. */
static uint8_t *encode_sample(size_t *out_len) {
    static uint8_t px[2 * 2 * 4];
    for (size_t i = 0; i < sizeof px; i++) {
        px[i] = (uint8_t)i;
    }
    tp_build_proto_sprite sprite = {.name = "s", .width = 2, .height = 2, .origin_x = 0.5F, .origin_y = 0.5F,
                                    .rgba = px};
    tp_build_proto_request req = {
        .max_size = 256,
        .pixels_per_unit = 1.0F,
        .atlas_name = "a",
        .out_name = "a.ntpack",
        .sprites = &sprite,
        .sprite_count = 1,
    };
    uint8_t *bytes = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_build_proto_encode_request(&req, &bytes, out_len, &err), err.msg);
    TEST_ASSERT_NOT_NULL(bytes);
    return bytes;
}

void test_fail_closed_truncated_frame(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    tp_build_proto_request out;
    tp_error err = {{0}};
    /* Fewer bytes than declared: the payload-length check fails closed. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_build_proto_decode_request(bytes, len - 4, &out, &err));
    /* Below even the frame header. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_build_proto_decode_request(bytes, 5, &out, &err));
    free(bytes);
}

void test_fail_closed_oversized_declared_len(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    /* Inflate the declared payload length past the buffer (offset 8 = payload_len). */
    put_u32_le(bytes + 8, (uint32_t)(len - PROTO_FRAME_HDR + 4096U));
    tp_build_proto_request out;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_build_proto_decode_request(bytes, len, &out, &err));
    free(bytes);
}

void test_fail_closed_wrong_magic(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    bytes[0] ^= 0xFFu; /* corrupt the magic */
    tp_build_proto_request out;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_MAGIC, tp_build_proto_decode_request(bytes, len, &out, &err));
    /* A response magic is not a request magic either. */
    uint8_t *rbytes = NULL;
    size_t rlen = 0;
    tp_build_proto_response resp = {.status = TP_STATUS_OK, .artifact_name = "a.ntpack", .message = ""};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_build_proto_encode_response(&resp, &rbytes, &rlen, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_MAGIC, tp_build_proto_decode_request(rbytes, rlen, &out, &err));
    free(rbytes);
    free(bytes);
}

void test_fail_closed_unsupported_version(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    bytes[4] = (uint8_t)(TP_BUILD_PROTO_VERSION + 1U); /* version @ offset 4 (u16 LE) */
    tp_build_proto_request out;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_VERSION, tp_build_proto_decode_request(bytes, len, &out, &err));
    free(bytes);
}

void test_fail_closed_corrupt_payload(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    /* sprite_count is the last u32 of the request head; claim one extra sprite so
     * the decoder reads a sprite head past the payload and fails closed. */
    put_u32_le(bytes + PROTO_FRAME_HDR + PROTO_REQ_HEAD - 4, 2U);
    tp_build_proto_request out;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_build_proto_decode_request(bytes, len, &out, &err));
    free(bytes);
}

/* ---- decode-side integrity guards: a well-framed frame with an internally
 * inconsistent field must fail closed at DECODE (the encoder never emits these). */

/* rgba_len that disagrees with width*height*4 is rejected before any pixel copy. */
void test_fail_closed_rgba_len_mismatch(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    /* Sample sprite is 2x2 -> rgba_len 16; claim 15 so decode fails the equality. */
    put_u32_le(bytes + SAMPLE_RGBA_LEN_OFF, 15U);
    tp_build_proto_request out;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_build_proto_decode_request(bytes, len, &out, &err));
    free(bytes);
}

/* Extra payload bytes the decode does not consume (r.off != r.len) fail closed. */
void test_fail_closed_trailing_payload_bytes(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    uint8_t *bigger = (uint8_t *)realloc(bytes, len + 4U);
    TEST_ASSERT_NOT_NULL(bigger);
    bytes = bigger;
    memset(bytes + len, 0, 4U);
    /* Declare the 4 extra bytes so the frame length check passes; they then remain
     * unconsumed after the request decodes -> trailing-bytes fail-closed. */
    put_u32_le(bytes + 8, (uint32_t)(len - PROTO_FRAME_HDR + 4U));
    tp_build_proto_request out;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_build_proto_decode_request(bytes, len + 4U, &out, &err));
    free(bytes);
}

/* A sprite name_len over the cap is rejected at decode (encode also caps it). */
void test_fail_closed_decode_name_len_over_cap(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    put_u32_le(bytes + SAMPLE_SPRITE_HEAD_OFF, TP_BUILD_PROTO_MAX_NAME_BYTES + 1U); /* sprite name_len */
    tp_build_proto_request out;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_build_proto_decode_request(bytes, len, &out, &err));
    /* Same guard on the atlas name_len in the request head. */
    uint8_t *b2 = encode_sample(&len);
    put_u32_le(b2 + REQ_ATLAS_NAME_LEN_OFF, TP_BUILD_PROTO_MAX_NAME_BYTES + 1U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_build_proto_decode_request(b2, len, &out, &err));
    free(b2);
    free(bytes);
}

/* A sprite_count over the cap is rejected at decode before any table allocation. */
void test_fail_closed_decode_sprite_count_over_cap(void) {
    size_t len = 0;
    uint8_t *bytes = encode_sample(&len);
    put_u32_le(bytes + REQ_SPRITE_COUNT_OFF, TP_BUILD_PROTO_MAX_SPRITES + 1U);
    tp_build_proto_request out;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_build_proto_decode_request(bytes, len, &out, &err));
    free(bytes);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_request_roundtrip_multi_sprite);
    RUN_TEST(test_request_roundtrip_zero_sprites);
    RUN_TEST(test_request_roundtrip_max_name_length);
    RUN_TEST(test_response_roundtrip);
    RUN_TEST(test_fail_closed_truncated_frame);
    RUN_TEST(test_fail_closed_oversized_declared_len);
    RUN_TEST(test_fail_closed_wrong_magic);
    RUN_TEST(test_fail_closed_unsupported_version);
    RUN_TEST(test_fail_closed_corrupt_payload);
    RUN_TEST(test_fail_closed_rgba_len_mismatch);
    RUN_TEST(test_fail_closed_trailing_payload_bytes);
    RUN_TEST(test_fail_closed_decode_name_len_over_cap);
    RUN_TEST(test_fail_closed_decode_sprite_count_over_cap);
    return UNITY_END();
}
