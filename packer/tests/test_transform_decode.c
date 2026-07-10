/* Synthetic D4 transform-decode unit test (plan §3.6). Exercises tp_transform_decode
 * (the pure mirror of the engine serializer's transform_point) across all 8 dihedral
 * masks -- independent of whatever transform the packer happens to emit, so it is the
 * only place the diagonal / dim-swap path is guaranteed to be covered deterministically.
 *
 * Corner space, tw=7 th=3 (non-square catches axis swaps). Expected corner mappings
 * are hand-derived from the reference decode: swap(diagonal) -> flipH -> flipV. */

#include "tp_pack_read_internal.h"
#include "unity.h"

#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

#define TW 7
#define TH 3

/* corner order: (0,0) (tw,0) (0,th) (tw,th) */
static const int32_t g_corners[4][2] = {{0, 0}, {TW, 0}, {0, TH}, {TW, TH}};

static void check_mask(uint8_t flags, const int32_t expect[4][2]) {
    int32_t minx = 0, miny = 0, maxx = 0, maxy = 0;
    for (int i = 0; i < 4; i++) {
        int32_t ox = 0, oy = 0;
        tp_transform_decode(g_corners[i][0], g_corners[i][1], flags, TW, TH, &ox, &oy);
        TEST_ASSERT_EQUAL_INT32(expect[i][0], ox);
        TEST_ASSERT_EQUAL_INT32(expect[i][1], oy);
        if (i == 0) {
            minx = maxx = ox;
            miny = maxy = oy;
        } else {
            if (ox < minx) minx = ox;
            if (ox > maxx) maxx = ox;
            if (oy < miny) miny = oy;
            if (oy > maxy) maxy = oy;
        }
    }

    /* Footprint dims from tp_transform_out_dims must match the transformed-corner AABB. */
    int32_t ow = 0, oh = 0;
    tp_transform_out_dims(flags, TW, TH, &ow, &oh);
    TEST_ASSERT_EQUAL_INT32(ow, maxx - minx);
    TEST_ASSERT_EQUAL_INT32(oh, maxy - miny);

    /* bit2 (diagonal) swaps the reported dims. */
    if (flags & 4u) {
        TEST_ASSERT_EQUAL_INT32(TH, ow);
        TEST_ASSERT_EQUAL_INT32(TW, oh);
    } else {
        TEST_ASSERT_EQUAL_INT32(TW, ow);
        TEST_ASSERT_EQUAL_INT32(TH, oh);
    }
}

void test_identity(void) {
    const int32_t e[4][2] = {{0, 0}, {7, 0}, {0, 3}, {7, 3}};
    check_mask(0, e);
}

void test_flip_h(void) {
    const int32_t e[4][2] = {{7, 0}, {0, 0}, {7, 3}, {0, 3}};
    check_mask(1, e);
}

void test_flip_v(void) {
    const int32_t e[4][2] = {{0, 3}, {7, 3}, {0, 0}, {7, 0}};
    check_mask(2, e);
}

void test_flip_hv(void) {
    const int32_t e[4][2] = {{7, 3}, {0, 3}, {7, 0}, {0, 0}};
    check_mask(3, e);
}

void test_diagonal(void) {
    const int32_t e[4][2] = {{0, 0}, {0, 7}, {3, 0}, {3, 7}};
    check_mask(4, e);
}

void test_diagonal_flip_h(void) {
    const int32_t e[4][2] = {{3, 0}, {3, 7}, {0, 0}, {0, 7}};
    check_mask(5, e);
}

void test_diagonal_flip_v(void) {
    const int32_t e[4][2] = {{0, 7}, {0, 0}, {3, 7}, {3, 0}};
    check_mask(6, e);
}

void test_diagonal_flip_hv(void) {
    const int32_t e[4][2] = {{3, 7}, {3, 0}, {0, 7}, {0, 0}};
    check_mask(7, e);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_identity);
    RUN_TEST(test_flip_h);
    RUN_TEST(test_flip_v);
    RUN_TEST(test_flip_hv);
    RUN_TEST(test_diagonal);
    RUN_TEST(test_diagonal_flip_h);
    RUN_TEST(test_diagonal_flip_v);
    RUN_TEST(test_diagonal_flip_hv);
    return UNITY_END();
}
