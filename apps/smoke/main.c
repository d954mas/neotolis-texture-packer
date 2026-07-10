/* Environment smoke test: packs a few generated RGBA sprites into an atlas
 * inside an .ntpack via nt_builder, with debug page PNGs enabled. Proves the
 * toolchain + engine submodule + builder pipeline work in this checkout. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "nt_builder.h"

#define SPRITE_MAX_DIM 96U

static void fill_disc(uint8_t *rgba, uint32_t w, uint32_t h, const uint8_t color[4]) {
    const float cx = (float)w * 0.5F;
    const float cy = (float)h * 0.5F;
    const float r = ((float)(w < h ? w : h)) * 0.5F - 1.0F;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const float dx = ((float)x + 0.5F) - cx;
            const float dy = ((float)y + 0.5F) - cy;
            uint8_t *px = &rgba[(y * w + x) * 4];
            if (dx * dx + dy * dy <= r * r) {
                px[0] = color[0];
                px[1] = color[1];
                px[2] = color[2];
                px[3] = color[3];
            } else {
                px[0] = px[1] = px[2] = px[3] = 0;
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *pack_path = (argc > 1) ? argv[1] : "smoke.ntpack";

    NtBuilderContext *ctx = nt_builder_start_pack(pack_path);
    if (!ctx) {
        (void)fprintf(stderr, "smoke: nt_builder_start_pack(%s) failed\n", pack_path);
        return 1;
    }

    nt_atlas_opts_t atlas_opts = nt_atlas_opts_defaults();
    atlas_opts.max_size = 256;
    atlas_opts.debug_png = true;

    nt_builder_begin_atlas(ctx, "smoke_atlas", &atlas_opts);

    static uint8_t pixels[SPRITE_MAX_DIM * SPRITE_MAX_DIM * 4];
    static const uint8_t colors[3][4] = {
        {230, 80, 60, 255},
        {70, 180, 90, 255},
        {60, 110, 220, 255},
    };
    static const uint32_t sizes[3][2] = {{64, 64}, {96, 48}, {33, 90}};
    static const char *names[3] = {"disc_red", "disc_green", "disc_blue"};

    for (int i = 0; i < 3; i++) {
        const uint32_t w = sizes[i][0];
        const uint32_t h = sizes[i][1];
        fill_disc(pixels, w, h, colors[i]);
        nt_atlas_sprite_opts_t sprite_opts = nt_atlas_sprite_opts_defaults();
        sprite_opts.name = names[i];
        nt_builder_atlas_add_raw(ctx, pixels, w, h, &sprite_opts);
    }

    nt_builder_end_atlas(ctx);

    const nt_build_result_t result = nt_builder_finish_pack(ctx);
    nt_builder_free_pack(ctx);

    if (result != NT_BUILD_OK) {
        (void)fprintf(stderr, "smoke: finish_pack failed with %d\n", (int)result);
        return 1;
    }
    (void)printf("smoke: OK, wrote %s\n", pack_path);
    return 0;
}
