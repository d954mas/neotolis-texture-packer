/* Fixture-driven golden round-trip for tp_pack_read (plan §3.2, task 7).
 *
 * For every fixture case (tp_fixtures.h, §3.1): build a .ntpack via nt_builder
 * into the scratch dir (argv[1]), parse it back with tp_pack_read_file, then
 * assert the recovered canonical model:
 *   - RECT sprites: EXACT frame w/h, sourceSize, spriteSourceSize.
 *   - polygon sprites: tolerance mode (builder inflates non-RECT hulls ~1px, so
 *     exact trim is invalid -- plan R1 amendment); assert parse ok, invariants,
 *     vertex_count <= max_vertices, trim within tolerance, probes hit content.
 *   - pivot (epsilon), slice9, trimmed, alias_of, page count/dims, premultiplied.
 *   - frame.x/y + transform are packer-chosen -> validated ONLY via pixel probes
 *     mapped source -> trim -> page through the recovered placement + D4 texel
 *     transform. Every sprite carries >=1 probe.
 *   - §3.6 GUARD: at least one recovered region has the diagonal bit set.
 *
 * Preserves the interim test's negative cases (truncated, bad magic, unknown
 * region) and the pixels_per_unit round-trip. */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_core/tp_pack_read.h"
#include "unity.h"

#include "tp_fixtures.h"

/* Recovered polygon trim may exceed the known trim by the clipper2 inflation
 * (~1-2px per touched side); RECT stays exact. */
#define POLY_TRIM_TOL 4

typedef struct {
    const tp_fixture_case *cs;
    char path[1024];
    tp_result *res; /* the single atlas parsed from this case's pack */
} case_ctx;

static case_ctx g_cc[16];
static int g_ncc;
static tp_arena *g_arena;
static tp_name_map *g_names;

/* bytes of the "plain" pack, kept for the negative tests. */
static uint8_t *g_plain_bytes;
static size_t g_plain_size;

void setUp(void) {}
void tearDown(void) {}

// #region helpers
static const tp_sprite *find_sprite(const tp_result *res, const char *name) {
    for (int i = 0; i < res->sprite_count; i++) {
        if (strcmp(res->sprites[i].name, name) == 0) {
            return &res->sprites[i];
        }
    }
    return NULL;
}

static int sprite_index(const tp_result *res, const char *name) {
    for (int i = 0; i < res->sprite_count; i++) {
        if (strcmp(res->sprites[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Texel-space D4 transform -- byte-for-byte mirror of the engine's
 * transform_point_texel (nt_builder_atlas_geometry.c): pixel index -> pixel
 * index. Corner reflection is w-1-x (texel), not w-x. */
static void tp_texel(int sx, int sy, uint8_t flags, int tw, int th, int *ox, int *oy) {
    int x = sx;
    int y = sy;
    if (flags & 4u) {
        int t = x;
        x = y;
        y = t;
    }
    int w = (flags & 4u) ? th : tw;
    int h = (flags & 4u) ? tw : th;
    if (flags & 1u) {
        x = w - 1 - x;
    }
    if (flags & 2u) {
        y = h - 1 - y;
    }
    *ox = x;
    *oy = y;
}

static bool is_pot(int v) {
    return v > 0 && (v & (v - 1)) == 0;
}

static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        (void)fclose(f);
        return NULL;
    }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) {
        (void)fclose(f);
        return NULL;
    }
    size_t rd = fread(b, 1, (size_t)sz, f);
    (void)fclose(f);
    if (rd != (size_t)sz) {
        free(b);
        return NULL;
    }
    *out_size = (size_t)sz;
    return b;
}

static case_ctx *find_case(const char *name) {
    for (int i = 0; i < g_ncc; i++) {
        if (strcmp(g_cc[i].cs->name, name) == 0) {
            return &g_cc[i];
        }
    }
    return NULL;
}

/* Map a probe's source pixel through the recovered placement and match the page
 * colour. Only independent check of frame.x/y and atlas_v orientation. */
static void check_probe(const tp_result *res, const tp_sprite *s, const tp_fix_probe *pr) {
    int sx = pr->src_x - s->spriteSourceSize.x; /* trim-local, y-down */
    int sy = pr->src_y - s->spriteSourceSize.y;
    int tx = 0;
    int ty = 0;
    tp_texel(sx, sy, s->transform, s->frame.w, s->frame.h, &tx, &ty);
    int px = s->frame.x + tx;
    int py = s->frame.y + ty;
    TEST_ASSERT_TRUE_MESSAGE(s->page >= 0 && s->page < res->page_count, "probe: page index out of range");
    const tp_page *pg = &res->pages[s->page];
    char msg[256];
    (void)snprintf(msg, sizeof msg, "sprite '%s' probe src(%d,%d)->page(%d,%d) transform=%u frame(%d,%d,%d,%d)", s->name,
                   pr->src_x, pr->src_y, px, py, (unsigned)s->transform, s->frame.x, s->frame.y, s->frame.w, s->frame.h);
    TEST_ASSERT_TRUE_MESSAGE(px >= 0 && py >= 0 && px < pg->w && py < pg->h, msg);
    const uint8_t *p = &pg->rgba[((size_t)py * (size_t)pg->w + (size_t)px) * 4];
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(pr->rgba[0], p[0], msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(pr->rgba[1], p[1], msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(pr->rgba[2], p[2], msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(pr->rgba[3], p[3], msg);
}

static void check_sprite(const case_ctx *cc, const tp_fixture_sprite *fs) {
    const tp_result *res = cc->res;
    const tp_sprite *s = find_sprite(res, fs->name);
    char msg[128];
    (void)snprintf(msg, sizeof msg, "case '%s' sprite '%s'", cc->cs->name, fs->name);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, msg);

    /* Always exact: source size, pivot, slice9, trimmed flag. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(fs->src_w, s->sourceSize.w, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(fs->src_h, s->sourceSize.h, msg);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(s->pivot.x - fs->pivot_x) < 1e-4f, msg);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(s->pivot.y - fs->pivot_y) < 1e-4f, msg);
    for (int k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(fs->slice9_lrtb[k], s->slice9_lrtb[k], msg);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(fs->expect_trimmed ? 1 : 0, s->trimmed ? 1 : 0, msg);

    if (fs->mode == TP_FIX_EXACT) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(fs->trim_w, s->frame.w, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(fs->trim_h, s->frame.h, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(fs->trim_x, s->spriteSourceSize.x, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(fs->trim_y, s->spriteSourceSize.y, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(fs->trim_w, s->spriteSourceSize.w, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(fs->trim_h, s->spriteSourceSize.h, msg);
    } else {
        /* Tolerance mode (polygon): trim within a few px; frame == ssrc dims;
         * vertex_count within the atlas cap. */
        TEST_ASSERT_TRUE_MESSAGE(abs(s->frame.w - fs->trim_w) <= POLY_TRIM_TOL, msg);
        TEST_ASSERT_TRUE_MESSAGE(abs(s->frame.h - fs->trim_h) <= POLY_TRIM_TOL, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(s->frame.w, s->spriteSourceSize.w, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(s->frame.h, s->spriteSourceSize.h, msg);
        TEST_ASSERT_TRUE_MESSAGE(s->vert_count >= 3 && s->vert_count <= (int)cc->cs->max_vertices, msg);
        TEST_ASSERT_TRUE_MESSAGE(s->index_count >= 3, msg);
    }

    /* frame within its page (independent of the reader's own invariant guard). */
    const tp_page *pg = &res->pages[s->page];
    int aabb_w = (s->transform & 4u) ? s->frame.h : s->frame.w;
    int aabb_h = (s->transform & 4u) ? s->frame.w : s->frame.h;
    TEST_ASSERT_TRUE_MESSAGE(s->frame.x >= 0 && s->frame.y >= 0, msg);
    TEST_ASSERT_TRUE_MESSAGE(s->frame.x + aabb_w <= pg->w && s->frame.y + aabb_h <= pg->h, msg);

    /* pixel probes (required per fixture). */
    TEST_ASSERT_TRUE_MESSAGE(fs->probe_count >= 1, msg);
    for (int i = 0; i < fs->probe_count; i++) {
        check_probe(res, s, &fs->probes[i]);
    }
}
// #endregion

// #region tests
void test_all_cases_recovered(void) {
    for (int i = 0; i < g_ncc; i++) {
        const case_ctx *cc = &g_cc[i];
        TEST_ASSERT_NOT_NULL(cc->res);
        /* atlas display name resolves back through the reverse map. */
        TEST_ASSERT_EQUAL_STRING(cc->cs->name, cc->res->atlas_name);
        /* every page straight-alpha (§5 profile). */
        for (int p = 0; p < cc->res->page_count; p++) {
            char m[96];
            (void)snprintf(m, sizeof m, "case '%s' page %d premultiplied", cc->cs->name, p);
            TEST_ASSERT_FALSE_MESSAGE(cc->res->pages[p].premultiplied, m);
            TEST_ASSERT_TRUE(cc->res->pages[p].w > 0 && cc->res->pages[p].h > 0);
            TEST_ASSERT_NOT_NULL(cc->res->pages[p].rgba);
        }
        for (int j = 0; j < cc->cs->sprite_count; j++) {
            check_sprite(cc, &cc->cs->sprites[j]);
        }
    }
}

void test_page_expectations(void) {
    for (int i = 0; i < g_ncc; i++) {
        const case_ctx *cc = &g_cc[i];
        const tp_fixture_case *c = cc->cs;
        char m[96];
        (void)snprintf(m, sizeof m, "case '%s'", c->name);
        int min_pages = c->expect_min_pages > 0 ? c->expect_min_pages : 1;
        TEST_ASSERT_TRUE_MESSAGE(cc->res->page_count >= min_pages, m);

        if (c->expect_page_eq_sprite) {
            const tp_fixture_sprite *s0 = &c->sprites[0];
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, cc->res->page_count, m);
            TEST_ASSERT_EQUAL_INT_MESSAGE(s0->src_w, cc->res->pages[0].w, m);
            TEST_ASSERT_EQUAL_INT_MESSAGE(s0->src_h, cc->res->pages[0].h, m);
        }
        if (c->expect_page_non_pot) {
            bool any_non_pot = false;
            for (int p = 0; p < cc->res->page_count; p++) {
                if (!is_pot(cc->res->pages[p].w) || !is_pot(cc->res->pages[p].h)) {
                    any_non_pot = true;
                }
            }
            TEST_ASSERT_TRUE_MESSAGE(any_non_pot, m);
        }
    }
}

void test_alias_relationship(void) {
    case_ctx *cc = find_case("alias");
    TEST_ASSERT_NOT_NULL(cc);
    const tp_result *res = cc->res;
    const tp_sprite *a = find_sprite(res, "alias_a");
    const tp_sprite *b = find_sprite(res, "alias_b");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    int ia = sprite_index(res, "alias_a");
    /* sorted-by-name: alias_a is the original. */
    TEST_ASSERT_EQUAL_INT(-1, a->alias_of);
    TEST_ASSERT_EQUAL_INT(ia, b->alias_of);
    TEST_ASSERT_EQUAL_INT(a->page, b->page);
    TEST_ASSERT_EQUAL_INT(a->frame.x, b->frame.x);
    TEST_ASSERT_EQUAL_INT(a->frame.y, b->frame.y);
    TEST_ASSERT_EQUAL_INT(a->transform, b->transform);
}

void test_multipage_pages(void) {
    case_ctx *cc = find_case("multipage");
    TEST_ASSERT_NOT_NULL(cc);
    TEST_ASSERT_TRUE(cc->res->page_count >= 2);
    /* sprites land on more than one distinct page. */
    bool seen[64] = {false};
    int distinct = 0;
    for (int i = 0; i < cc->res->sprite_count; i++) {
        int pg = cc->res->sprites[i].page;
        if (pg >= 0 && pg < 64 && !seen[pg]) {
            seen[pg] = true;
            distinct++;
        }
    }
    TEST_ASSERT_TRUE(distinct >= 2);
}

void test_pixels_per_unit(void) {
    case_ctx *cc = find_case("plain");
    TEST_ASSERT_NOT_NULL(cc);
    TEST_ASSERT_TRUE(fabsf(cc->res->pixels_per_unit - 32.0f) < 1e-5f);
    /* a default case must report 1.0 */
    case_ctx *tc = find_case("trimmed");
    TEST_ASSERT_NOT_NULL(tc);
    TEST_ASSERT_TRUE(fabsf(tc->res->pixels_per_unit - 1.0f) < 1e-5f);
}

/* §3.6 guard: the packer, not the test, chooses transforms -- prove the diagonal
 * (dim-swap) path is actually exercised across the fixture set. */
void test_diagonal_transform_present(void) {
    bool found = false;
    for (int i = 0; i < g_ncc && !found; i++) {
        for (int j = 0; j < g_cc[i].res->sprite_count; j++) {
            if (g_cc[i].res->sprites[j].transform & 4u) {
                found = true;
                break;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(
        found, "no recovered region has a diagonal transform bit: adjust rotated/flipped fixture sizes until the "
               "packer picks a diagonal transform");
}

void test_determinism_byte_identical(void) {
    /* Same fixture built twice must be byte-identical (single-threaded builder,
     * no timestamps in the pack header). Rebuild into the same scratch dir
     * (overwrites) and compare the two snapshots. */
    const tp_fixture_case *c = find_case("multipage")->cs;
    char dir[1024];
    (void)snprintf(dir, sizeof dir, "%s", g_cc[0].path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
    } else {
        (void)snprintf(dir, sizeof dir, ".");
    }
    char path[1024];
    TEST_ASSERT_TRUE(tp_fixture_build(c, dir, path, sizeof path));
    size_t n1 = 0;
    uint8_t *b1 = read_whole_file(path, &n1);
    TEST_ASSERT_TRUE(tp_fixture_build(c, dir, path, sizeof path));
    size_t n2 = 0;
    uint8_t *b2 = read_whole_file(path, &n2);
    TEST_ASSERT_NOT_NULL(b1);
    TEST_ASSERT_NOT_NULL(b2);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n1, (uint32_t)n2, "determinism: pack sizes differ");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(b1, b2, n1), "determinism: pack bytes differ across two builds");
    free(b1);
    free(b2);
}

void test_neg_truncated(void) {
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_result **r = NULL;
    int c = -1;
    tp_error err;
    err.msg[0] = '\0';
    tp_status st = tp_pack_read_memory(g_plain_bytes, g_plain_size / 2, g_names, ar, &r, &c, &err);
    TEST_ASSERT_TRUE(st != TP_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(0, c);
    TEST_ASSERT_NULL(r);
    tp_arena_destroy(ar);
}

void test_neg_bad_magic(void) {
    uint8_t *copy = (uint8_t *)malloc(g_plain_size);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, g_plain_bytes, g_plain_size);
    copy[0] ^= 0xFFu;
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_result **r = NULL;
    int c = -1;
    tp_error err;
    err.msg[0] = '\0';
    tp_status st = tp_pack_read_memory(copy, g_plain_size, g_names, ar, &r, &c, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_MAGIC, st);
    tp_arena_destroy(ar);
    free(copy);
}

void test_neg_unknown_region(void) {
    tp_name_map *empty = tp_name_map_create();
    TEST_ASSERT_NOT_NULL(empty);
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_result **r = NULL;
    int c = -1;
    tp_error err;
    err.msg[0] = '\0';
    tp_status st = tp_pack_read_memory(g_plain_bytes, g_plain_size, empty, ar, &r, &c, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNKNOWN_REGION, st);
    tp_arena_destroy(ar);
    tp_name_map_destroy(empty);
}
// #endregion

static bool setup_all(const char *dir) {
    int n = 0;
    const tp_fixture_case *cases = tp_fixtures_all(&n);
    if (n <= 0 || n > (int)(sizeof g_cc / sizeof g_cc[0])) {
        (void)fprintf(stderr, "test_pack_read: bad case count %d\n", n);
        return false;
    }
    g_arena = tp_arena_create(0);
    g_names = tp_name_map_create();
    if (!g_arena || !g_names) {
        (void)fprintf(stderr, "test_pack_read: alloc failed\n");
        return false;
    }
    if (!tp_fixtures_register_names(g_names)) {
        (void)fprintf(stderr, "test_pack_read: name registration failed (collision?)\n");
        return false;
    }

    g_ncc = n;
    for (int i = 0; i < n; i++) {
        g_cc[i].cs = &cases[i];
        if (!tp_fixture_build(&cases[i], dir, g_cc[i].path, sizeof g_cc[i].path)) {
            (void)fprintf(stderr, "test_pack_read: build failed for case '%s'\n", cases[i].name);
            return false;
        }
        tp_result **results = NULL;
        int count = 0;
        tp_error err;
        err.msg[0] = '\0';
        tp_status st = tp_pack_read_file(g_cc[i].path, g_names, g_arena, &results, &count, &err);
        if (st != TP_STATUS_OK) {
            (void)fprintf(stderr, "test_pack_read: parse failed for '%s': %s\n", cases[i].name, err.msg);
            return false;
        }
        if (count != 1 || !results || !results[0]) {
            (void)fprintf(stderr, "test_pack_read: case '%s' expected 1 atlas, got %d\n", cases[i].name, count);
            return false;
        }
        g_cc[i].res = results[0];
        if (strcmp(cases[i].name, "plain") == 0) {
            g_plain_bytes = read_whole_file(g_cc[i].path, &g_plain_size);
        }
    }
    if (!g_plain_bytes) {
        (void)fprintf(stderr, "test_pack_read: failed to snapshot plain pack bytes\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    if (!setup_all(dir)) {
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_all_cases_recovered);
    RUN_TEST(test_page_expectations);
    RUN_TEST(test_alias_relationship);
    RUN_TEST(test_multipage_pages);
    RUN_TEST(test_pixels_per_unit);
    RUN_TEST(test_diagonal_transform_present);
    RUN_TEST(test_determinism_byte_identical);
    RUN_TEST(test_neg_truncated);
    RUN_TEST(test_neg_bad_magic);
    RUN_TEST(test_neg_unknown_region);
    int rc = UNITY_END();

    tp_arena_destroy(g_arena);
    tp_name_map_destroy(g_names);
    free(g_plain_bytes);
    return rc;
}
