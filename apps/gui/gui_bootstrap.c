#include "gui_bootstrap.h"

#include <stdio.h>

#include "atlas/nt_atlas.h"
#include "core/nt_assert.h"
#include "font/nt_font.h"
#include "hash/nt_hash.h"
#include "log/nt_log.h"
#include "nt_pack_format.h"
#include "renderers/nt_text_renderer.h"
#include "resource/nt_resource.h"
#include "ui/nt_ui.h"
#include "ui/nt_ui_dropdown.h"
#include "ui/nt_ui_input.h"
#include "ui/nt_ui_menu.h"
#include "ui/nt_ui_modal.h"
#include "ui/nt_ui_scroll.h"
#include "ui/nt_ui_slider.h"
#include "ui/nt_ui_tooltip.h"

#include "ntpacker_ui_assets.h"

#include "gui_defs.h"
#include "gui_state.h"
#include "gui_widgets.h"

static nt_hash32_t s_pack_id;
static nt_resource_t s_sprite_vs_handle;
static nt_resource_t s_sprite_fs_handle;
static nt_resource_t s_text_vs_handle;
static nt_resource_t s_text_fs_handle;
static nt_resource_t s_atlas_handle;
static nt_resource_t s_atlas_tex_handle;
static nt_resource_t s_font_resource;
static nt_material_t s_sprite_material;
static nt_material_t s_text_material;
static bool s_atlas_bound;
static bool s_ids_ready;
static void ensure_ids(void) {
    if (s_ids_ready) {
        return;
    }
    s_id_btn_pack = nt_ui_id("ntpacker/btn_pack");
    s_id_btn_export = nt_ui_id("ntpacker/btn_export");
    s_id_btn_refresh = nt_ui_id("ntpacker/btn_refresh");
    s_id_vlist = nt_ui_id("ntpacker/sprite_vlist");
    s_id_canvas = nt_ui_id("ntpacker/canvas");
    s_id_modal = nt_ui_id("ntpacker/confirm_modal");
    s_id_about = nt_ui_id("ntpacker/about_modal");
    s_id_rename = nt_ui_id("ntpacker/rename_input");
    s_id_right_panel = nt_ui_id("ntpacker/right_panel");
    s_id_left_panel = nt_ui_id("ntpacker/left_panel");
    s_id_strip = nt_ui_id("ntpacker/canvas_strip");
    s_id_status_pill = nt_ui_id("ntpacker/status_pill");
    s_id_right_content = nt_ui_id("ntpacker/right_content");
    s_id_export_modal = nt_ui_id("ntpacker/export_modal");
    s_id_recovery = nt_ui_id("ntpacker/recovery_modal");
    s_id_mb_file = nt_ui_id("ntpacker/mb_file");
    s_id_mb_edit = nt_ui_id("ntpacker/mb_edit");
    s_id_mb_atlas = nt_ui_id("ntpacker/mb_atlas");
    s_id_mb_view = nt_ui_id("ntpacker/mb_view");
    s_id_mb_help = nt_ui_id("ntpacker/mb_help");
    s_id_menu_file = nt_ui_id("ntpacker/menu_file");
    s_id_menu_edit = nt_ui_id("ntpacker/menu_edit");
    s_id_menu_atlas = nt_ui_id("ntpacker/menu_atlas");
    s_id_menu_view = nt_ui_id("ntpacker/menu_view");
    s_id_menu_help = nt_ui_id("ntpacker/menu_help");
    s_id_ctx_menu = nt_ui_id("ntpacker/ctx_menu");

    s_menu_style = nt_ui_menu_style_defaults();
    s_menu_style.icon_size = 0U;
    /* Floating surfaces must SEPARATE from the panels (owner: open menus blended in). The engine
     * default is a semi-transparent near-panel gray + a warm hover foreign to the palette. Elevated
     * surface = opaque header tone (lighter than panel, same as section headers, §2.1); hover = the
     * app's selection blue; separators = the border tone. */
    s_menu_style.bg_color = RGBA8(40, 45, 57);         /* C_HEADER: opaque elevated surface */
    s_menu_style.item_hover_color = RGBA8(48, 74, 120); /* C_SEL: selection blue, not the default amber */
    s_menu_style.separator_color = RGBA8(52, 58, 72);   /* C_BORDER */
    s_modal_style = nt_ui_modal_style_defaults();
    s_tip_style = nt_ui_tooltip_style_defaults();

    s_rename_input = nt_ui_input_style_defaults();
    s_rename_input.text.font_id = 0;
    s_rename_input.text.color = (Clay_Color){225.0F, 228.0F, 235.0F, 255.0F};
    s_rename_input.placeholder.font_id = 0;
    s_rename_input.placeholder.color = (Clay_Color){120.0F, 126.0F, 138.0F, 255.0F};
    /* Field well (§2.7 item 7): recessed `input` fill + `border`; focus ring = `border-strong` blue. */
    s_rename_input.skin[NT_UI_INPUT_IDLE].bg_color = RGBA8(21, 23, 30);
    s_rename_input.skin[NT_UI_INPUT_IDLE].border_color = RGBA8(52, 58, 72);
    s_rename_input.skin[NT_UI_INPUT_FOCUSED].bg_color = RGBA8(21, 23, 30);
    s_rename_input.skin[NT_UI_INPUT_FOCUSED].border_color = RGBA8(86, 132, 204);
    s_rename_input.border_width = 1.0F;

    /* Settings-panel widget styles. The atlas WHITE region (s_white_ref, bound by now
     * since can_render gates ensure_ids) is the art for checkbox/slider parts, tinted
     * per state; the dropdown is flat-color. */
    s_dd_style = nt_ui_dropdown_style_defaults();
    s_dd_style.font_id = 0;
    s_dd_style.trigger_text = RGBA8(214, 220, 230);
    s_dd_style.row_text = RGBA8(214, 220, 230);
    /* Well look (§2.7 item 7): recessed `input` fill so combos read as fields, not buttons. The engine
     * dropdown trigger has no border field, so the recessed fill (darker than panel) carries the well
     * read; the open state uses the lighter `pressed` tint as its active signal. */
    s_dd_style.trigger_idle.fill = RGBA8(21, 23, 30);
    s_dd_style.trigger_hover.fill = RGBA8(30, 34, 44);
    s_dd_style.trigger_pressed.fill = RGBA8(40, 46, 58);
    /* The OPEN list is a floating surface, not a well: elevated header tone (was RGBA8(30,33,41),
     * 2 units off the panel fill -- it blended in; owner report). Matches s_menu_style.bg_color. */
    s_dd_style.panel_fill = RGBA8(40, 45, 57);
    s_dd_style.row_idle.fill = 0U;
    s_dd_style.row_hover.fill = RGBA8(54, 60, 74);
    s_dd_style.row_pressed.fill = RGBA8(36, 40, 50);
    s_dd_style.row_selected.fill = RGBA8(52, 78, 120);
    s_dd_style.panel_corner_radius = 4U;
    s_dd_style.max_visible_rows = 8U;

    s_slider_style = nt_ui_slider_style_defaults();
    s_slider_style.states[NT_UI_SLIDER_IDLE].track = s_white_ref;
    s_slider_style.states[NT_UI_SLIDER_IDLE].track_tint = RGBA8(46, 50, 60);
    s_slider_style.states[NT_UI_SLIDER_IDLE].fill = s_white_ref;
    s_slider_style.states[NT_UI_SLIDER_IDLE].fill_tint = RGBA8(78, 126, 192);
    s_slider_style.states[NT_UI_SLIDER_IDLE].thumb = s_white_ref;
    s_slider_style.states[NT_UI_SLIDER_IDLE].thumb_tint = RGBA8(220, 228, 238);

    s_num_input = nt_ui_input_style_defaults();
    s_num_input.text.font_id = 0;
    s_num_input.text.color = (Clay_Color){225.0F, 228.0F, 235.0F, 255.0F};
    s_num_input.placeholder.font_id = 0;
    s_num_input.placeholder.color = (Clay_Color){120.0F, 126.0F, 138.0F, 255.0F};
    /* Field well (§2.7 item 7): recessed `input` fill + always-on `border`; focus/active = `border-strong`. */
    s_num_input.skin[NT_UI_INPUT_IDLE].bg_color = RGBA8(21, 23, 30);
    s_num_input.skin[NT_UI_INPUT_IDLE].border_color = RGBA8(52, 58, 72);
    s_num_input.skin[NT_UI_INPUT_HOVER].bg_color = RGBA8(21, 23, 30);
    s_num_input.skin[NT_UI_INPUT_HOVER].border_color = RGBA8(70, 78, 96);
    s_num_input.skin[NT_UI_INPUT_FOCUSED].bg_color = RGBA8(21, 23, 30);
    s_num_input.skin[NT_UI_INPUT_FOCUSED].border_color = RGBA8(86, 132, 204);
    s_num_input.skin[NT_UI_INPUT_DISABLED].bg_color = RGBA8(26, 28, 36);
    s_num_input.skin[NT_UI_INPUT_DISABLED].border_color = RGBA8(40, 44, 54);
    s_num_input.border_width = 1.0F;

    s_panel_scroll = nt_ui_scroll_style_defaults();
    s_panel_scroll.scroll_x = false;
    s_panel_scroll.scroll_y = true;
    s_panel_scroll.bar_visibility = NT_UI_SCROLLBAR_AUTO_HIDE;
    s_panel_scroll.track_ref = s_white_ref;
    s_panel_scroll.track_tint = RGBA8(30, 33, 41);
    s_panel_scroll.thumb_ref = s_white_ref;
    s_panel_scroll.thumb_tint = RGBA8(80, 86, 100);

    s_ids_ready = true;
}

/* Resolve one baked icon region into a memoized ref, exactly like the white region. The icon MUST be
 * present (build_packs bakes it) -- a miss is a bake/codegen mismatch, so crash early. */
static nt_atlas_region_ref_t bind_icon_ref(nt_hash64_t region_id) {
    const uint32_t idx = nt_atlas_find_region(s_atlas_handle, region_id.value);
    NT_ASSERT(idx != NT_ATLAS_INVALID_REGION && "ntpacker-gui: baked UI icon region missing");
    return nt_atlas_ref_idx(s_atlas_handle, region_id.value, idx);
}

static void try_bind_resources(void) {
    if (s_atlas_bound && s_font_bound) {
        return;
    }
    if (!s_atlas_bound && nt_resource_is_ready(s_atlas_handle)) {
        const uint32_t white = nt_atlas_find_region(s_atlas_handle, ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS__WHITE.value);
        NT_ASSERT(white != NT_ATLAS_INVALID_REGION);
        nt_ui_set_atlas_white_region(s_ctx, s_atlas_handle, white);
        s_white_ref = nt_atlas_ref_idx(s_atlas_handle, ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS__WHITE.value, white);
        s_ic_layout_grid = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_LAYOUT_GRID);
        s_ic_triangle_alert = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_TRIANGLE_ALERT);
        s_ic_download = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_DOWNLOAD);
        s_ic_refresh = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_REFRESH_CW);
        s_ic_chevron_left = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_CHEVRON_LEFT);
        s_ic_chevron_right = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_CHEVRON_RIGHT);
        s_ic_minus = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_MINUS);
        s_ic_plus = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_PLUS);
        s_ic_scan = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_SCAN);
        s_ic_maximize = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_MAXIMIZE_2);
        s_ic_chevron_down = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_CHEVRON_DOWN);
        s_ic_layers = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_LAYERS);
        s_ic_folder = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FOLDER);
        s_ic_image = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_IMAGE);
        s_ic_film = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FILM);
        s_ic_file_plus = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FILE_PLUS);
        s_ic_folder_plus = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FOLDER_PLUS);
        s_ic_x = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_X);
        s_ic_info = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_INFO);
        s_ic_circle_check = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_CIRCLE_CHECK);
        s_ic_octagon_alert = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_OCTAGON_ALERT);
        s_ic_folder_plus_hero = bind_icon_ref(ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS_FOLDER_PLUS_HERO);
        s_atlas_bound = true;
        nt_log_info("ntpacker-gui: atlas white + icon regions bound");
    }
    if (!s_font_bound && nt_resource_is_ready(s_font_resource)) {
        nt_font_add(s_font, s_font_resource);
        nt_ui_set_font(s_ctx, 0U, s_font);
        s_font_bound = true;
        nt_log_info("ntpacker-gui: font bound at slot 0");
    }
}

void gui_bootstrap_init(const char *exe_dir) {
    char ui_pack_path[TP_IDENTITY_PATH_MAX + 256];
    (void)snprintf(ui_pack_path, sizeof ui_pack_path,
                   "%s/assets/ntpacker_ui.ntpack", exe_dir);
    s_pack_id = nt_hash32_str("ntpacker_ui");
    nt_resource_mount(s_pack_id, 100);
    nt_resource_load_auto(s_pack_id, ui_pack_path);

    s_sprite_vs_handle = nt_resource_request(
        ASSET_SHADER_ASSETS_SHADERS_SPRITE_VERT, NT_ASSET_SHADER_CODE);
    s_sprite_fs_handle = nt_resource_request(
        ASSET_SHADER_ASSETS_SHADERS_SPRITE_FRAG, NT_ASSET_SHADER_CODE);
    s_text_vs_handle = nt_resource_request(
        ASSET_SHADER_ASSETS_SHADERS_SLUG_TEXT_VERT, NT_ASSET_SHADER_CODE);
    s_text_fs_handle = nt_resource_request(
        ASSET_SHADER_ASSETS_SHADERS_SLUG_TEXT_FRAG, NT_ASSET_SHADER_CODE);
    s_atlas_handle = nt_resource_request(
        ASSET_ATLAS_NTPACKER_UI_ATLAS, NT_ASSET_ATLAS);
    s_atlas_tex_handle = nt_resource_request(
        ASSET_TEXTURE_NTPACKER_UI_ATLAS_TEX0, NT_ASSET_TEXTURE);
    s_font_resource = nt_resource_request(
        ASSET_FONT_NTPACKER_UI_FONT, NT_ASSET_FONT);

    s_sprite_material = nt_material_create(&(nt_material_create_desc_t){
        .vs = s_sprite_vs_handle,
        .fs = s_sprite_fs_handle,
        .textures = {{.name = "u_texture", .resource = s_atlas_tex_handle}},
        .texture_count = 1,
        .blend_mode = NT_BLEND_MODE_ALPHA,
        .depth_test = false,
        .depth_write = false,
        .cull_mode = NT_CULL_NONE,
        .label = "ntpacker_sprite",
    });
    s_text_material = nt_material_create(&(nt_material_create_desc_t){
        .vs = s_text_vs_handle,
        .fs = s_text_fs_handle,
        .blend_mode = NT_BLEND_MODE_ALPHA,
        .depth_test = false,
        .depth_write = false,
        .cull_mode = NT_CULL_NONE,
        .params[0] = {.name = "u_alpha_cutoff",
                      .value = {NT_TEXT_ALPHA_CUTOFF_DEFAULT}},
        .param_count = 1,
        .label = "ntpacker_text",
    });
    nt_ui_set_sprite_material(s_ctx, s_sprite_material);
    nt_ui_set_text_material(s_ctx, s_text_material);

    s_font = nt_font_create(&(nt_font_create_desc_t){
        .curve_texture_width = 1024,
        .curve_texture_height = 512,
        .band_texture_height = 256,
        .band_count = 8,
        .measure_cache_size = 256,
    });
    nt_resource_set_activate_time_budget(0);
}

const nt_material_info_t *gui_bootstrap_step(void) {
    nt_resource_step();
    nt_material_step();
    try_bind_resources();
    const nt_material_info_t *sprite_info =
        nt_material_get_info(s_sprite_material);
    const nt_material_info_t *text_info = nt_material_get_info(s_text_material);
    const bool ready = s_atlas_bound && s_font_bound && sprite_info &&
                       sprite_info->ready && text_info && text_info->ready;
    if (!ready) {
        return NULL;
    }
    ensure_ids();
    return sprite_info;
}

void gui_bootstrap_restore(void) {
    nt_resource_invalidate(NT_ASSET_SHADER_CODE);
    nt_resource_invalidate(NT_ASSET_TEXTURE);
    nt_resource_invalidate(NT_ASSET_FONT);
    s_atlas_bound = false;
    s_font_bound = false;
}

void gui_bootstrap_shutdown(void) {
    nt_font_destroy(s_font);
    nt_font_shutdown();
    nt_material_destroy(s_sprite_material);
    nt_material_destroy(s_text_material);
    nt_material_shutdown();
}
