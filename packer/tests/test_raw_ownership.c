/* C0-03 task 6: raw-RGBA ownership regression.
 *
 * The engine's nt_builder_atlas_add_raw() DEEP-COPIES the caller's RGBA buffer
 * (external/neotolis-engine/tools/builder/nt_builder_atlas.c:771-775: malloc +
 * memcpy, then sprite->rgba = the copy). The PUBLIC header (nt_builder.h:424)
 * does NOT promise this lifetime -- so this test pins the observed behaviour as
 * an executable regression, and packer/spike/c0/upstream-issue-raw-lifetime.md
 * asks the engine to document the contract in the public header.
 *
 * Because the copy is CONFIRMED in the source we read, exercising it is safe (no
 * UB): we fill an RGBA buffer, add_raw it, then MUTATE AND FREE that caller
 * buffer immediately -- before end_atlas/finish_pack, where the real blitting
 * happens -- and assert the packed page still holds the ORIGINAL pixels. If the
 * builder had adopted the pointer instead of copying, the page would show the
 * corrupted bytes (or ASan would trap the use-after-free).
 *
 * Drives nt_builder directly (mirroring tp_pack.c run_builder) so the mutate/free
 * can be interleaved between add_raw and finish_pack, which tp_pack() cannot
 * expose. Reads the pack back with the packer's own reader and probes pixels. */

/* nt_builder.h pulls the engine's CRT setup and must precede the ucrt/unity
 * headers (mirrors apps/smoke/main.c and tp_fixtures.c). */
#include "nt_builder.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_core/tp_pack_read.h"

static char s_work_dir[1024];

void setUp(void) {}
void tearDown(void) {}

#define SPR_W 8
#define SPR_H 8
static const uint8_t ORIG[4] = {11, 22, 33, 255}; /* solid opaque -> not trimmed */
#define CORRUPT_BYTE 0xEE

/* Drive one atlas with a single raw sprite; mutate+free the caller buffer right
 * after add_raw if `sabotage` is set. Returns true if the pack succeeded. */
static bool pack_one_raw(const char *path, const char *atlas, const char *sprite, bool sabotage) {
    size_t n = (size_t)SPR_W * SPR_H * 4u;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) {
        return false;
    }
    for (size_t p = 0; p < n; p += 4) {
        memcpy(buf + p, ORIG, 4);
    }

    NtBuilderContext *ctx = nt_builder_start_pack(path);
    if (!ctx) {
        free(buf);
        return false;
    }
    /* Single-threaded (default): deterministic byte-identical output. */
    nt_atlas_opts_t o = nt_atlas_opts_defaults();
    o.premultiplied = false; /* straight alpha: stored pixels == originals */
    o.compress = NULL;       /* RAW RGBA8 -- the reader requires it */
    o.gen_mipmaps = false;
    o.format = NT_TEXTURE_FORMAT_RGBA8;
    o.debug_png = false;
    o.max_size = 256;
    o.padding = 0;
    o.margin = 0;
    o.extrude = 0;
    o.alpha_threshold = 0;
    o.max_vertices = 8;
    o.shape = NT_ATLAS_SHAPE_RECT;
    o.allow_transform = false;
    o.power_of_two = false;
    o.pixels_per_unit = 100.0f;

    nt_builder_begin_atlas(ctx, atlas, &o);
    nt_atlas_sprite_opts_t so = nt_atlas_sprite_opts_defaults();
    so.name = sprite;
    so.origin_x = 0.5f;
    so.origin_y = 0.5f;
    nt_builder_atlas_add_raw(ctx, buf, SPR_W, SPR_H, &so);

    if (sabotage) {
        /* The load-bearing step: destroy the caller buffer BEFORE the pack blits. */
        memset(buf, CORRUPT_BYTE, n);
        free(buf);
        buf = NULL;
    }

    nt_builder_end_atlas(ctx);
    nt_build_result_t br = nt_builder_finish_pack(ctx);
    nt_builder_free_pack(ctx);
    free(buf); /* no-op if already freed/NULL */
    return br == NT_BUILD_OK;
}

/* Read the pack back and return the single result's first page + sprite. */
static tp_result *read_one(const char *path, const char *atlas, const char *sprite, tp_arena *arena) {
    tp_name_map *names = tp_name_map_create();
    TEST_ASSERT_NOT_NULL(names);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_name_map_insert(names, atlas));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_name_map_insert(names, sprite));
    tp_result **results = NULL;
    int count = 0;
    tp_error err;
    tp_status st = tp_pack_read_file(path, names, arena, &results, &count, &err);
    tp_name_map_destroy(names);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, err.msg);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_NOT_NULL(results);
    return results[0];
}

/* Every pixel in the sprite's placed frame equals the ORIGINAL color. */
static void assert_frame_is_original(const tp_result *r) {
    TEST_ASSERT_EQUAL_INT(1, r->sprite_count);
    const tp_sprite *sp = &r->sprites[0];
    TEST_ASSERT_FALSE(sp->trimmed); /* solid opaque: full 8x8 frame */
    TEST_ASSERT_EQUAL_INT(SPR_W, sp->frame.w);
    TEST_ASSERT_EQUAL_INT(SPR_H, sp->frame.h);
    const tp_page *pg = &r->pages[sp->page];
    for (int y = 0; y < sp->frame.h; y++) {
        for (int x = 0; x < sp->frame.w; x++) {
            size_t off = ((size_t)(sp->frame.y + y) * (size_t)pg->w + (size_t)(sp->frame.x + x)) * 4u;
            const uint8_t *px = pg->rgba + off;
            TEST_ASSERT_EQUAL_UINT8_ARRAY(ORIG, px, 4);
        }
    }
}

/* The mutate/free-immediately-after-add_raw case: the packed result is
 * unaffected -> the builder took its own copy. */
void test_add_raw_copies_caller_buffer(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/ownraw_sabotage.ntpack", s_work_dir);
    TEST_ASSERT_TRUE(pack_one_raw(path, "ownraw", "spr", true));

    tp_arena *arena = tp_arena_create(1 << 16);
    tp_result *r = read_one(path, "ownraw", "spr", arena);
    assert_frame_is_original(r);
    tp_arena_destroy(arena);
}

/* Control: an unsabotaged pack yields the same pixels (the mutation had no
 * effect above because the copy already happened at add_raw time). */
void test_add_raw_control_matches(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/ownraw_control.ntpack", s_work_dir);
    TEST_ASSERT_TRUE(pack_one_raw(path, "ownraw", "spr", false));

    tp_arena *arena = tp_arena_create(1 << 16);
    tp_result *r = read_one(path, "ownraw", "spr", arena);
    assert_frame_is_original(r);
    tp_arena_destroy(arena);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        snprintf(s_work_dir, sizeof s_work_dir, "%s", argv[1]);
    } else {
        snprintf(s_work_dir, sizeof s_work_dir, ".");
    }
    UNITY_BEGIN();
    RUN_TEST(test_add_raw_copies_caller_buffer);
    RUN_TEST(test_add_raw_control_matches);
    return UNITY_END();
}
