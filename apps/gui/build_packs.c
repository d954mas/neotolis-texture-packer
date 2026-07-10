/* Build the ntpacker-gui UI shell pack:
 *   ntpacker_ui.ntpack -- sprite + text shaders, a 1x1 white pixel atlas (UI rect
 *   fill), and one Latin font. This is the minimal set nt_ui needs to draw the
 *   skeleton shell (panels/labels/buttons). The live atlas preview is a SEPARATE
 *   session pack written by tp_core at runtime, not baked here.
 * Usage: build_ntpacker_gui_packs <pack_dir> <header_dir> <icons_dir>
 * Run from the engine root (WORKING_DIRECTORY) so shader/font paths stay short
 * and the generated macro names match the engine examples. <icons_dir> is the
 * ntpacker GUI icon dir; CMake passes it absolute (its WORKING_DIRECTORY is the
 * engine root, not apps/gui), so we join it verbatim. */

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

/* DejaVu Sans regular: full Cyrillic + symbol coverage (font.ttf lacks 8 UI symbols). GUI chrome,
 * sprite names, and rename fields render real UTF-8 (Cyrillic filenames, and the UI symbol set). */
#define FONT_PATH "examples/ui_showcase/raw/font_dejavu_r.ttf"

/* Baked charset = printable ASCII + Latin-1 supplement (U+00A1-U+00FF incl. B1 D7 AB BB) + full
 * Russian Cyrillic incl. Yo + the UI symbol set. All glyphs verified present in DejaVu Sans;
 * the four blocks are disjoint (no duplicate codepoints). */
#define CHARSET_LATIN1 "¡¢£¤¥¦§¨©ª«¬­®¯°±²³´µ¶·¸¹º»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞßàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ"
#define CHARSET_CYRILLIC "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдеёжзийклмнопрстуфхцчшщъыьэюя"
/* warning check ellipsis, black/white circle, rotate x2, left/right triangle, small right/down triangle
 * (disclosure chevrons), right arrow, true-minus, en/em dash, up/down arrow (anim frame reorder),
 * medium vertical bar (preview pause -- rendered doubled "❚❚"; DejaVu Sans covers U+275A). */
#define CHARSET_SYMBOLS "⚠✓…●○↻⟳◀▶▸▾→−–—↑↓❚"

static char s_path_buf[512];

static const char *pack_path(const char *dir, const char *name) {
    (void)snprintf(s_path_buf, sizeof(s_path_buf), "%s/%s", dir, name);
    return s_path_buf;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        (void)fprintf(stderr, "Usage: build_ntpacker_gui_packs <pack_dir> <header_dir> <icons_dir>\n");
        return 1;
    }
    const char *out_dir = argv[1];
    const char *header_dir = argv[2];
    const char *icons_dir = argv[3]; /* absolute (CMake joins CMAKE_CURRENT_SOURCE_DIR/assets/icons) */

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

    /* Lucide icon masks (48px white-on-alpha; the hero is 96px). Explicit list, not a glob: it
     * documents the region contract (each name -> ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_<NAME>) and a
     * glob would derive names from basenames INCLUDING strays. opts.name = basename (no extension)
     * gives clean macros; sources are straight-alpha and the atlas premultiplies at bake so the UI
     * shader's color_packed tint composites correctly (same path as _white). The hero is baked here
     * (used by the Packet C empty state) so no pack change is needed later. */
    static const char *const icon_names[] = {
        "check",        "chevron-down", "chevron-left",   "chevron-right",   "circle-check",
        "crop",         "crosshair",    "download",       "external-link",   "file-plus",
        "film",         "folder",       "folder-open",    "folder-plus",     "folder-plus-hero",
        "image",        "info",         "layers",         "layout-grid",     "link",
        "maximize-2",   "minus",        "octagon-alert",  "plus",            "redo-2",
        "refresh-cw",   "save",         "scan",           "search",          "square-dashed",
        "triangle-alert", "undo-2",     "x",
    };
    char icon_path[512];
    for (size_t i = 0; i < sizeof icon_names / sizeof icon_names[0]; i++) {
        (void)snprintf(icon_path, sizeof icon_path, "%s/%s.png", icons_dir, icon_names[i]);
        nt_atlas_sprite_opts_t iopts = nt_atlas_sprite_opts_defaults();
        iopts.name = icon_names[i];
        nt_builder_atlas_add(ctx, icon_path, &iopts);
    }
    (void)printf("  Atlas 'ntpacker_ui_atlas' icons added: %zu (from %s)\n",
                 sizeof icon_names / sizeof icon_names[0], icons_dir);

    nt_builder_end_atlas(ctx);
    // #endregion

    // #region font: ASCII + Latin-1 + Cyrillic + UI symbols (DejaVu Sans)
    nt_builder_add_font(ctx, FONT_PATH,
                        &(nt_font_opts_t){
                            .charset = NT_CHARSET_ASCII CHARSET_LATIN1 CHARSET_CYRILLIC CHARSET_SYMBOLS,
                            .resource_name = "ntpacker_ui/font",
                        });
    (void)printf("  Font (ASCII+Latin1+Cyrillic+symbols) added: ntpacker_ui/font\n");
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
