/* UV recovery property test (plan §3.3, task 8). Pure functions, no builder.
 *
 * For every page dim W in the spec set and EVERY px in [0,W], the exact
 * round-trip tp_uv_to_px(tp_px_to_uv(px,W),W) == px must hold (idealized double
 * encode, §2.5). Additionally the REAL builder encodes in float32
 * (nt_builder_atlas.c:1765-1780); replicate that math and assert decode still
 * recovers px exactly -- this closes the §3.3 caveat that §2.5's proof is over
 * idealized arithmetic while the builder uses float. */

#include "tp_pack_read_internal.h"
#include "unity.h"

#include <stdint.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static const int32_t g_dims[] = {2, 3, 7, 16, 100, 127, 128, 255, 256, 1000, 2048, 4095, 4096};
#define NDIMS ((int)(sizeof g_dims / sizeof g_dims[0]))

/* Builder's actual float32 encode + clamp (nt_builder_atlas.c:1765-1780). */
static uint16_t encode_f32(int32_t px, int32_t page_dim) {
    float t = (((float)px * 65535.0f) / (float)page_dim) + 0.5f;
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 65535.0f) {
        t = 65535.0f;
    }
    return (uint16_t)t;
}

void test_uv_roundtrip_idealized(void) {
    for (int d = 0; d < NDIMS; d++) {
        int32_t W = g_dims[d];
        for (int32_t px = 0; px <= W; px++) {
            uint16_t u = tp_px_to_uv(px, W);
            int32_t back = tp_uv_to_px(u, W);
            if (back != px) {
                char msg[96];
                (void)snprintf(msg, sizeof msg, "idealized W=%d px=%d u=%u back=%d", W, px, (unsigned)u, back);
                TEST_FAIL_MESSAGE(msg);
            }
        }
    }
}

void test_uv_roundtrip_float32(void) {
    for (int d = 0; d < NDIMS; d++) {
        int32_t W = g_dims[d];
        for (int32_t px = 0; px <= W; px++) {
            uint16_t u = encode_f32(px, W);
            int32_t back = tp_uv_to_px(u, W);
            if (back != px) {
                char msg[96];
                (void)snprintf(msg, sizeof msg, "float32 W=%d px=%d u=%u back=%d", W, px, (unsigned)u, back);
                TEST_FAIL_MESSAGE(msg);
            }
        }
    }
}

/* Edge pins: px=0 -> u=0, px=W -> u=65535, both encoders. */
void test_uv_edges(void) {
    for (int d = 0; d < NDIMS; d++) {
        int32_t W = g_dims[d];
        TEST_ASSERT_EQUAL_UINT16(0, tp_px_to_uv(0, W));
        TEST_ASSERT_EQUAL_UINT16(65535, tp_px_to_uv(W, W));
        TEST_ASSERT_EQUAL_UINT16(0, encode_f32(0, W));
        TEST_ASSERT_EQUAL_UINT16(65535, encode_f32(W, W));
        TEST_ASSERT_EQUAL_INT32(0, tp_uv_to_px(0, W));
        TEST_ASSERT_EQUAL_INT32(W, tp_uv_to_px(65535, W));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_uv_roundtrip_idealized);
    RUN_TEST(test_uv_roundtrip_float32);
    RUN_TEST(test_uv_edges);
    return UNITY_END();
}
