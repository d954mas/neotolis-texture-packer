/* Build the ntpacker-gui UI shell pack:
 *   ntpacker_ui.ntpack -- sprite + text shaders, a 1x1 white pixel atlas (UI rect
 *   fill), and one Latin font. This is the minimal set nt_ui needs to draw the
 *   skeleton shell (panels/labels/buttons). The live atlas preview is a SEPARATE
 *   session pack written by tp_core at runtime, not baked here.
 * Usage: build_ntpacker_gui_packs <pack_dir> <header_dir>
 * Run from the engine root (WORKING_DIRECTORY) so shader/font paths stay short
 * and the generated macro names match the engine examples. */

/* clang-format off */
#include "nt_builder.h"
/* clang-format on */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* Latin-only: the shell shows English chrome; no Cyrillic/rich family needed. */
#define FONT_PATH "examples/ui_showcase/raw/font.ttf"

static char s_path_buf[512];

static const char *pack_path(const char *dir, const char *name) {
    (void)snprintf(s_path_buf, sizeof(s_path_buf), "%s/%s", dir, name);
    return s_path_buf;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        (void)fprintf(stderr, "Usage: build_ntpacker_gui_packs <pack_dir> <header_dir>\n");
        return 1;
    }
    const char *out_dir = argv[1];
    const char *header_dir = argv[2];

    (void)printf("=== Build ntpacker-gui UI pack -> %s ===\n\n", out_dir);

    MKDIR(out_dir);
    MKDIR(header_dir);

    char cache_dir[512];
    (void)snprintf(cache_dir, sizeof(cache_dir), "%s/_cache", out_dir);
    MKDIR(cache_dir);

    // #region pack: ntpacker_ui.ntpack
    NtBuilderContext *ctx = nt_builder_start_pack(pack_path(out_dir, "ntpacker_ui.ntpack"));
    if (!ctx) {
        (void)fprintf(stderr, "Failed to start ntpacker_ui.ntpack\n");
        return 1;
    }
    nt_builder_set_header_dir(ctx, header_dir);
    nt_builder_set_cache_dir(ctx, cache_dir);
    nt_builder_set_threads_auto(ctx);
    // #endregion

    // #region shaders (sprite for UI rects/images + Slug for text)
    nt_builder_add_shader(ctx, "assets/shaders/sprite.vert", NT_BUILD_SHADER_VERTEX);
    nt_builder_add_shader(ctx, "assets/shaders/sprite.frag", NT_BUILD_SHADER_FRAGMENT);
    nt_builder_add_shader(ctx, "assets/shaders/slug_text.vert", NT_BUILD_SHADER_VERTEX);
    nt_builder_add_shader(ctx, "assets/shaders/slug_text.frag", NT_BUILD_SHADER_FRAGMENT);
    (void)printf("  Shaders added: 4 (sprite + slug_text)\n");
    // #endregion

    // #region atlas: white pixel only (UI panel/rect fill)
    nt_atlas_opts_t atlas_opts = nt_atlas_opts_defaults();
    atlas_opts.shape = NT_ATLAS_SHAPE_RECT;
    atlas_opts.allow_transform = false;
    atlas_opts.pixels_per_unit = 1.0F;
    atlas_opts.padding = 2;
    atlas_opts.margin = 2;
    atlas_opts.extrude = 1;
    atlas_opts.premultiplied = true;
    atlas_opts.compress = NULL;
    atlas_opts.filter_min = NT_TEXTURE_DEFAULT_FILTER_LINEAR;
    atlas_opts.filter_mag = NT_TEXTURE_DEFAULT_FILTER_LINEAR;
    atlas_opts.wrap_u = NT_TEXTURE_DEFAULT_WRAP_CLAMP_TO_EDGE;
    atlas_opts.wrap_v = NT_TEXTURE_DEFAULT_WRAP_CLAMP_TO_EDGE;
    atlas_opts.gen_mipmaps = false;

    nt_builder_begin_atlas(ctx, "ntpacker_ui_atlas", &atlas_opts);

    static const uint8_t white_pixel[4] = {255, 255, 255, 255};
    nt_atlas_sprite_opts_t white_opts = nt_atlas_sprite_opts_defaults();
    white_opts.name = "_white";
    nt_builder_atlas_add_raw(ctx, white_pixel, 1, 1, &white_opts);
    (void)printf("  Atlas 'ntpacker_ui_atlas' region '_white': 1x1\n");

    nt_builder_end_atlas(ctx);
    // #endregion

    // #region font: ASCII Latin
    nt_builder_add_font(ctx, FONT_PATH,
                        &(nt_font_opts_t){
                            .charset = NT_CHARSET_ASCII,
                            .resource_name = "ntpacker_ui/font",
                        });
    (void)printf("  Font (ASCII) added: ntpacker_ui/font\n");
    // #endregion

    // #region finish + codegen
    nt_build_result_t r = nt_builder_finish_pack(ctx);
    nt_builder_free_pack(ctx);
    if (r != NT_BUILD_OK) {
        (void)fprintf(stderr, "ntpacker_ui.ntpack failed: %d\n", r);
        return 1;
    }

    char base_hdr[512];
    (void)snprintf(base_hdr, sizeof(base_hdr), "%s/ntpacker_ui.h", header_dir);
    const char *headers[] = {base_hdr};
    char combined[512];
    (void)snprintf(combined, sizeof(combined), "%s/ntpacker_ui_assets.h", header_dir);
    nt_builder_merge_headers(headers, 1, combined);
    (void)printf("Generated: %s\n", combined);
    // #endregion

    (void)printf("\n=== Done ===\n");
    return 0;
}
