/* Procedural fixture generators (plan §3.1, task 6). See tp_fixtures.h. */

#define _CRT_SECURE_NO_WARNINGS

#include "tp_fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nt_builder.h"

#include "tp_core/tp_name_map.h"

/* Largest fixture sprite is 200x120 (sprite_eq_page); keep a headroom buffer. */
#define TP_FIX_BUF_DIM 256
static uint8_t g_pixbuf[(size_t)TP_FIX_BUF_DIM * TP_FIX_BUF_DIM * 4];

// #region authoring helpers
static void put_px(uint8_t *buf, int w, int x, int y, const uint8_t c[4]) {
    uint8_t *p = &buf[((size_t)y * (size_t)w + (size_t)x) * 4];
    p[0] = c[0];
    p[1] = c[1];
    p[2] = c[2];
    p[3] = c[3];
}

/* 5 mutually-distinct opaque colours per sprite. XOR by a per-seed constant is a
 * bijection, so the 5 base slots stay distinct after masking -> corner probes
 * uniquely identify each corner (and thus the transform orientation). */
static void sprite_palette(int seed, uint8_t base[4], uint8_t corners[4][4]) {
    static const uint8_t slot_rgb[5][3] = {
        {200, 60, 60},   /* base */
        {250, 240, 40},  /* TL */
        {40, 240, 240},  /* TR */
        {240, 40, 240},  /* BL */
        {30, 120, 250},  /* BR */
    };
    uint8_t s = (uint8_t)((seed * 37 + 11) & 0x7F);
    uint8_t out[5][4];
    for (int i = 0; i < 5; i++) {
        out[i][0] = (uint8_t)(slot_rgb[i][0] ^ s);
        out[i][1] = (uint8_t)(slot_rgb[i][1] ^ s);
        out[i][2] = (uint8_t)(slot_rgb[i][2] ^ s);
        out[i][3] = 255;
    }
    memcpy(base, out[0], 4);
    memcpy(corners[0], out[1], 4);
    memcpy(corners[1], out[2], 4);
    memcpy(corners[2], out[3], 4);
    memcpy(corners[3], out[4], 4);
}

static void add_probe(tp_fixture_sprite *s, int x, int y, const uint8_t c[4]) {
    tp_fix_probe *p = &s->probes[s->probe_count++];
    p->src_x = x;
    p->src_y = y;
    memcpy(p->rgba, c, 4);
}

static tp_fixture_sprite *new_sprite(tp_fixture_case *c, const char *name, int sw, int sh, int tx, int ty, int tw,
                                     int th, float pivx, float pivy, tp_fix_mode mode) {
    tp_fixture_sprite *s = &c->sprites[c->sprite_count++];
    memset(s, 0, sizeof *s);
    s->name = name;
    s->src_w = sw;
    s->src_h = sh;
    s->trim_x = tx;
    s->trim_y = ty;
    s->trim_w = tw;
    s->trim_h = th;
    s->pivot_x = pivx;
    s->pivot_y = pivy;
    s->mode = mode;
    s->alias_to = -1;
    s->expect_trimmed = !(tx == 0 && ty == 0 && tw == sw && th == sh);
    return s;
}

/* Filled trim rect with 4 distinct corner markers + auto corner probes. */
static tp_fixture_sprite *add_rect_sprite(tp_fixture_case *c, const char *name, int sw, int sh, int tx, int ty, int tw,
                                          int th, float pivx, float pivy, int seed) {
    tp_fixture_sprite *s = new_sprite(c, name, sw, sh, tx, ty, tw, th, pivx, pivy, TP_FIX_EXACT);
    s->gen_kind = TP_GEN_RECT;
    sprite_palette(seed, s->base, s->corners);
    if (tw >= 3 && th >= 3) {
        add_probe(s, tx, ty, s->corners[0]);
        add_probe(s, tx + tw - 1, ty, s->corners[1]);
        add_probe(s, tx, ty + th - 1, s->corners[2]);
        add_probe(s, tx + tw - 1, ty + th - 1, s->corners[3]);
    } else {
        /* Too small for distinct corners -- one flat marker. */
        for (int i = 0; i < 4; i++) {
            memcpy(s->corners[i], s->base, 4);
        }
        add_probe(s, tx, ty, s->base);
    }
    return s;
}

static tp_fixture_sprite *add_poly_sprite(tp_fixture_case *c, const char *name, int sw, int sh, int tx, int ty, int tw,
                                          int th, tp_gen_kind kind, int seed) {
    tp_fixture_sprite *s = new_sprite(c, name, sw, sh, tx, ty, tw, th, 0.5f, 0.5f, TP_FIX_TOLERANCE);
    s->gen_kind = kind;
    sprite_palette(seed, s->base, s->corners);
    return s;
}
// #endregion

// #region case table
#define TP_FIX_MAX_CASES 12
static tp_fixture_case g_cases[TP_FIX_MAX_CASES];
static int g_case_count;
static bool g_built;

static tp_fixture_case *new_case(const char *name, int shape, bool allow_transform, bool power_of_two, uint32_t max_size,
                                 uint32_t padding, uint32_t margin, uint8_t max_vertices, float ppu) {
    tp_fixture_case *c = &g_cases[g_case_count++];
    memset(c, 0, sizeof *c);
    c->name = name;
    c->shape = shape;
    c->allow_transform = allow_transform;
    c->power_of_two = power_of_two;
    c->max_size = max_size;
    c->padding = padding;
    c->margin = margin;
    c->max_vertices = max_vertices;
    c->pixels_per_unit = ppu;
    return c;
}

static void build_cases(void) {
    if (g_built) {
        return;
    }
    g_case_count = 0;

    /* 1. plain opaque rect, untrimmed. Carries a non-default pixels_per_unit so
     * the golden test can assert the meta round-trip (supersedes the interim). */
    {
        tp_fixture_case *c = new_case("plain", NT_ATLAS_SHAPE_RECT, false, true, 512, 2, 0, 8, 32.0f);
        add_rect_sprite(c, "plain_rect", 24, 16, 0, 0, 24, 16, 0.5f, 0.5f, 1);
    }

    /* 2. trimmed asymmetric: transparent margins, trim_x != trim_y, non-zero TOP
     * strip in y-down space -> exercises source_h - trim_offset_y - trim_h. */
    {
        tp_fixture_case *c = new_case("trimmed", NT_ATLAS_SHAPE_RECT, false, true, 512, 2, 0, 8, 1.0f);
        add_rect_sprite(c, "trimmed_blob", 40, 30, 5, 8, 25, 17, 0.5f, 0.5f, 2);
    }

    /* 3. rotated/flipped: wide + tall RECT mix with allow_transform; sized so the
     * packer must rotate the tall sprite (diagonal bit) to minimise area.
     * power_of_two=false + padding=0 makes the area win unambiguous. */
    {
        tp_fixture_case *c = new_case("rotated", NT_ATLAS_SHAPE_RECT, true, false, 1024, 0, 0, 8, 1.0f);
        add_rect_sprite(c, "rot_wide", 120, 24, 0, 0, 120, 24, 0.5f, 0.5f, 3);
        add_rect_sprite(c, "rot_tall", 24, 100, 0, 0, 24, 100, 0.5f, 0.5f, 4);
    }

    /* 4. polygon L-shape + diamond, CONCAVE_CONTOUR -> tolerance mode. */
    {
        tp_fixture_case *c = new_case("polygon", NT_ATLAS_SHAPE_CONCAVE_CONTOUR, false, true, 512, 2, 0, 8, 1.0f);
        tp_fixture_sprite *l = add_poly_sprite(c, "poly_l", 48, 48, 4, 4, 40, 40, TP_GEN_L, 5);
        add_probe(l, 4 + 6, 4 + 20, l->base);  /* vertical bar */
        add_probe(l, 4 + 24, 4 + 34, l->base); /* horizontal bar */
        tp_fixture_sprite *d = add_poly_sprite(c, "poly_diamond", 48, 48, 4, 4, 40, 40, TP_GEN_DIAMOND, 6);
        add_probe(d, 4 + 20, 4 + 20, d->base); /* centre */
        add_probe(d, 4 + 20, 4 + 13, d->base);
        add_probe(d, 4 + 13, 4 + 20, d->base);
    }

    /* 5. plus/cross (R1 hull-touching extremes) -> tolerance mode. max_vertices
     * high so the 12 corners survive RDP and tips reach all four trim edges. */
    {
        tp_fixture_case *c = new_case("plus", NT_ATLAS_SHAPE_CONCAVE_CONTOUR, false, true, 512, 2, 0, 16, 1.0f);
        tp_fixture_sprite *p = add_poly_sprite(c, "plus_cross", 48, 48, 4, 4, 40, 40, TP_GEN_PLUS, 7);
        add_probe(p, 4 + 20, 4 + 20, p->base); /* centre */
        add_probe(p, 4 + 6, 4 + 20, p->base);  /* left arm */
        add_probe(p, 4 + 33, 4 + 20, p->base); /* right arm */
        add_probe(p, 4 + 20, 4 + 6, p->base);  /* top arm */
        add_probe(p, 4 + 20, 4 + 33, p->base); /* bottom arm */
    }

    /* 6. slice9: non-zero borders force RECT + no-rotate and full (untrimmed)
     * source rect. Fully opaque -> trimmed=false. */
    {
        tp_fixture_case *c = new_case("slice9", NT_ATLAS_SHAPE_RECT, false, true, 512, 2, 0, 8, 1.0f);
        tp_fixture_sprite *s = add_rect_sprite(c, "slice9_panel", 40, 40, 0, 0, 40, 40, 0.5f, 0.5f, 8);
        s->slice9_lrtb[0] = 6;
        s->slice9_lrtb[1] = 7;
        s->slice9_lrtb[2] = 8;
        s->slice9_lrtb[3] = 9;
    }

    /* 7. off-frame pivot: origin outside [0,1] on both axes and directions. */
    {
        tp_fixture_case *c = new_case("off_pivot", NT_ATLAS_SHAPE_RECT, false, true, 512, 2, 0, 8, 1.0f);
        add_rect_sprite(c, "off_pivot_rect", 30, 20, 0, 0, 30, 20, 1.5f, -0.25f, 9);
    }

    /* 8. alias: two byte-identical sprites dedup to one placement. */
    {
        tp_fixture_case *c = new_case("alias", NT_ATLAS_SHAPE_RECT, false, true, 512, 2, 0, 8, 1.0f);
        add_rect_sprite(c, "alias_a", 20, 20, 0, 0, 20, 20, 0.5f, 0.5f, 10);
        tp_fixture_sprite *b = add_rect_sprite(c, "alias_b", 20, 20, 0, 0, 20, 20, 0.5f, 0.5f, 10);
        b->alias_to = 0; /* dedups to alias_a (sorted-first = original) */
    }

    /* 9. multipage: small max_size forces >= 2 pages (page_index varies). */
    {
        tp_fixture_case *c = new_case("multipage", NT_ATLAS_SHAPE_RECT, false, true, 128, 2, 0, 8, 1.0f);
        c->expect_min_pages = 2;
        add_rect_sprite(c, "mp_a", 100, 90, 0, 0, 100, 90, 0.5f, 0.5f, 11);
        add_rect_sprite(c, "mp_b", 90, 100, 0, 0, 90, 100, 0.5f, 0.5f, 12);
        add_rect_sprite(c, "mp_c", 110, 80, 0, 0, 110, 80, 0.5f, 0.5f, 13);
    }

    /* 10. 1x1 sprite: degenerate trim/AABB at the floor. */
    {
        tp_fixture_case *c = new_case("one_by_one", NT_ATLAS_SHAPE_RECT, false, true, 512, 2, 0, 8, 1.0f);
        add_rect_sprite(c, "one_px", 1, 1, 0, 0, 1, 1, 0.5f, 0.5f, 14);
    }

    /* 11. sprite == page: single sprite fills the whole page (u hits 0 and 65535
     * on both axes). power_of_two=false + padding/margin=0 so page == sprite. */
    {
        tp_fixture_case *c = new_case("sprite_eq_page", NT_ATLAS_SHAPE_RECT, false, false, 256, 0, 0, 8, 1.0f);
        c->expect_page_eq_sprite = true;
        add_rect_sprite(c, "full_page", 200, 120, 0, 0, 200, 120, 0.5f, 0.5f, 15);
    }

    /* 12. non-POT page: power_of_two=false so page dims are non-power-of-two,
     * exercising the §2.5 UV math against non-POT W/H (multiple placements). */
    {
        tp_fixture_case *c = new_case("nonpot", NT_ATLAS_SHAPE_RECT, false, false, 256, 0, 0, 8, 1.0f);
        c->expect_page_non_pot = true;
        add_rect_sprite(c, "np_a", 60, 40, 0, 0, 60, 40, 0.5f, 0.5f, 16);
        add_rect_sprite(c, "np_b", 50, 40, 0, 0, 50, 40, 0.5f, 0.5f, 17);
    }

    g_built = true;
}
// #endregion

const tp_fixture_case *tp_fixtures_all(int *out_count) {
    build_cases();
    if (out_count) {
        *out_count = g_case_count;
    }
    return g_cases;
}

// #region pixel generation
static void gen_pixels(const tp_fixture_sprite *s, uint8_t *buf) {
    int sw = s->src_w;
    int sh = s->src_h;
    memset(buf, 0, (size_t)sw * (size_t)sh * 4); /* transparent field */
    int tx = s->trim_x;
    int ty = s->trim_y;
    int tw = s->trim_w;
    int th = s->trim_h;

    switch (s->gen_kind) {
        case TP_GEN_RECT: {
            for (int y = ty; y < ty + th; y++) {
                for (int x = tx; x < tx + tw; x++) {
                    put_px(buf, sw, x, y, s->base);
                }
            }
            if (tw >= 3 && th >= 3) {
                put_px(buf, sw, tx, ty, s->corners[0]);
                put_px(buf, sw, tx + tw - 1, ty, s->corners[1]);
                put_px(buf, sw, tx, ty + th - 1, s->corners[2]);
                put_px(buf, sw, tx + tw - 1, ty + th - 1, s->corners[3]);
            }
            break;
        }
        case TP_GEN_L: {
            int vbar = (tw * 2) / 5;
            int hbar = (th * 3) / 5;
            for (int ly = 0; ly < th; ly++) {
                for (int lx = 0; lx < tw; lx++) {
                    if (lx < vbar || ly >= hbar) {
                        put_px(buf, sw, tx + lx, ty + ly, s->base);
                    }
                }
            }
            break;
        }
        case TP_GEN_DIAMOND: {
            int dw = tw - 1;
            int dh = th - 1;
            for (int ly = 0; ly < th; ly++) {
                for (int lx = 0; lx < tw; lx++) {
                    int mx = abs(2 * lx - dw);
                    int my = abs(2 * ly - dh);
                    if (mx * dh + my * dw <= dw * dh) {
                        put_px(buf, sw, tx + lx, ty + ly, s->base);
                    }
                }
            }
            break;
        }
        case TP_GEN_PLUS: {
            int lo_x = (tw * 3) / 8;
            int hi_x = (tw * 5) / 8;
            int lo_y = (th * 3) / 8;
            int hi_y = (th * 5) / 8;
            for (int ly = 0; ly < th; ly++) {
                for (int lx = 0; lx < tw; lx++) {
                    if ((lx >= lo_x && lx < hi_x) || (ly >= lo_y && ly < hi_y)) {
                        put_px(buf, sw, tx + lx, ty + ly, s->base);
                    }
                }
            }
            break;
        }
    }
}
// #endregion

bool tp_fixture_build(const tp_fixture_case *c, const char *out_dir, char *out_path, size_t cap) {
    (void)snprintf(out_path, cap, "%s/%s.ntpack", out_dir, c->name);

    NtBuilderContext *ctx = nt_builder_start_pack(out_path);
    if (!ctx) {
        return false;
    }

    /* §5 export-friendly profile + per-case overrides. */
    nt_atlas_opts_t o = nt_atlas_opts_defaults();
    o.premultiplied = false; /* straight alpha -> exact opaque probes (R3 warn expected) */
    o.compress = NULL;       /* RAW RGBA8 */
    o.gen_mipmaps = false;
    o.format = NT_TEXTURE_FORMAT_RGBA8;
    o.debug_png = false;
    o.extrude = 0;
    o.alpha_threshold = 1;
    o.max_size = c->max_size;
    o.padding = c->padding;
    o.margin = c->margin;
    o.max_vertices = c->max_vertices;
    o.shape = (nt_atlas_shape_t)c->shape;
    o.allow_transform = c->allow_transform;
    o.power_of_two = c->power_of_two;
    o.pixels_per_unit = c->pixels_per_unit;

    nt_builder_begin_atlas(ctx, c->name, &o);

    for (int i = 0; i < c->sprite_count; i++) {
        const tp_fixture_sprite *s = &c->sprites[i];
        if (s->src_w > TP_FIX_BUF_DIM || s->src_h > TP_FIX_BUF_DIM) {
            nt_builder_free_pack(ctx);
            return false;
        }
        gen_pixels(s, g_pixbuf);
        nt_atlas_sprite_opts_t so = nt_atlas_sprite_opts_defaults();
        so.name = s->name;
        so.origin_x = s->pivot_x;
        so.origin_y = s->pivot_y;
        so.slice9_left = s->slice9_lrtb[0];
        so.slice9_right = s->slice9_lrtb[1];
        so.slice9_top = s->slice9_lrtb[2];
        so.slice9_bottom = s->slice9_lrtb[3];
        nt_builder_atlas_add_raw(ctx, g_pixbuf, (uint32_t)s->src_w, (uint32_t)s->src_h, &so);
    }

    nt_builder_end_atlas(ctx);
    nt_build_result_t r = nt_builder_finish_pack(ctx);
    nt_builder_free_pack(ctx);
    return r == NT_BUILD_OK;
}

bool tp_fixtures_register_names(struct tp_name_map *map) {
    build_cases();
    for (int i = 0; i < g_case_count; i++) {
        const tp_fixture_case *c = &g_cases[i];
        if (tp_name_map_insert(map, c->name) != TP_STATUS_OK) {
            return false;
        }
        for (int j = 0; j < c->sprite_count; j++) {
            if (tp_name_map_insert(map, c->sprites[j].name) != TP_STATUS_OK) {
                return false;
            }
        }
    }
    return true;
}
