#include "gui_canvas_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_image.h"

static nt_texture_t upload_rgba_premul(const unsigned char *src, int w, int h,
                                       const char *label) {
    const size_t count = (size_t)w * (size_t)h;
    unsigned char *pixels = malloc(count * 4U);
    if (!pixels) {
        return (nt_texture_t){0};
    }
    for (size_t i = 0; i < count; i++) {
        const unsigned alpha = src[i * 4U + 3U];
        pixels[i * 4U + 0U] =
            (unsigned char)((src[i * 4U + 0U] * alpha) / 255U);
        pixels[i * 4U + 1U] =
            (unsigned char)((src[i * 4U + 1U] * alpha) / 255U);
        pixels[i * 4U + 2U] =
            (unsigned char)((src[i * 4U + 2U] * alpha) / 255U);
        pixels[i * 4U + 3U] = (unsigned char)alpha;
    }
    nt_texture_t texture = nt_gfx_make_texture(&(nt_texture_desc_t){
        .width = (uint16_t)w,
        .height = (uint16_t)h,
        .data = pixels,
        .format = NT_PIXEL_RGBA8,
        .min_filter = NT_FILTER_NEAREST,
        .mag_filter = NT_FILTER_NEAREST,
        .wrap_u = NT_WRAP_CLAMP_TO_EDGE,
        .wrap_v = NT_WRAP_CLAMP_TO_EDGE,
        .label = label,
    });
    free(pixels);
    return texture;
}

static void drop_pages(gui_canvas *canvas) {
    for (int i = 0; i < GUI_CANVAS_MAX_PAGES; i++) {
        if (canvas->page_valid[i]) {
            nt_gfx_destroy_texture(canvas->pages[i]);
            canvas->page_valid[i] = false;
        }
    }
    canvas->page_count = 0;
}

bool gui_canvas_resource_handles_ready(const gui_canvas *canvas) {
    return canvas && canvas->ibo.id != 0U && canvas->vbo.id != 0U &&
           canvas->sampler.id != 0U && canvas->vbo_checker.id != 0U &&
           canvas->checker_tex.id != 0U &&
           canvas->checker_sampler.id != 0U;
}

static void drop_buffer_resources(gui_canvas *canvas) {
    if (canvas->ibo.id != 0U) {
        nt_gfx_destroy_buffer(canvas->ibo);
    }
    if (canvas->vbo.id != 0U) {
        nt_gfx_destroy_buffer(canvas->vbo);
    }
    if (canvas->vbo_checker.id != 0U) {
        nt_gfx_destroy_buffer(canvas->vbo_checker);
    }
    if (canvas->checker_tex.id != 0U) {
        nt_gfx_destroy_texture(canvas->checker_tex);
    }
    canvas->ibo = (nt_buffer_t){0};
    canvas->vbo = (nt_buffer_t){0};
    canvas->sampler = (nt_sampler_t){0};
    canvas->vbo_checker = (nt_buffer_t){0};
    canvas->checker_tex = (nt_texture_t){0};
    canvas->checker_sampler = (nt_sampler_t){0};
    canvas->buffers_ready = false;
    canvas->checker_valid = false;
}

void gui_canvas_restore_gpu(gui_canvas *canvas) {
    canvas->buffers_ready = false;
    canvas->checker_valid = false;
    static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    canvas->ibo = nt_gfx_make_buffer(&(nt_buffer_desc_t){
        .type = NT_BUFFER_INDEX,
        .usage = NT_USAGE_IMMUTABLE,
        .data = indices,
        .size = sizeof indices,
        .index_type = NT_INDEX_UINT16,
        .label = "canvas_ibo",
    });
    canvas->vbo = nt_gfx_make_buffer(&(nt_buffer_desc_t){
        .type = NT_BUFFER_VERTEX,
        .usage = NT_USAGE_DYNAMIC,
        .size = 4U * sizeof(canvas_vert),
        .label = "canvas_vbo",
    });
    canvas->sampler = nt_gfx_make_sampler(&(nt_sampler_desc_t){
        .min_filter = NT_FILTER_NEAREST,
        .mag_filter = NT_FILTER_NEAREST,
        .wrap_u = NT_WRAP_CLAMP_TO_EDGE,
        .wrap_v = NT_WRAP_CLAMP_TO_EDGE,
        .label = "canvas_sampler",
    });
    canvas->vbo_checker = nt_gfx_make_buffer(&(nt_buffer_desc_t){
        .type = NT_BUFFER_VERTEX,
        .usage = NT_USAGE_DYNAMIC,
        .size = 4U * sizeof(canvas_vert),
        .label = "canvas_checker_vbo",
    });
    static const uint8_t checker_px[16] = {
        44, 44, 50, 255, 58, 58, 66, 255,
        58, 58, 66, 255, 44, 44, 50, 255,
    };
    canvas->checker_tex = nt_gfx_make_texture(&(nt_texture_desc_t){
        .width = 2,
        .height = 2,
        .data = checker_px,
        .format = NT_PIXEL_RGBA8,
        .min_filter = NT_FILTER_NEAREST,
        .mag_filter = NT_FILTER_NEAREST,
        .wrap_u = NT_WRAP_REPEAT,
        .wrap_v = NT_WRAP_REPEAT,
        .label = "canvas_checker_tex",
    });
    canvas->checker_sampler = nt_gfx_make_sampler(&(nt_sampler_desc_t){
        .min_filter = NT_FILTER_NEAREST,
        .mag_filter = NT_FILTER_NEAREST,
        .wrap_u = NT_WRAP_REPEAT,
        .wrap_v = NT_WRAP_REPEAT,
        .label = "canvas_checker_sampler",
    });
    canvas->buffers_ready = gui_canvas_resource_handles_ready(canvas);
    canvas->checker_valid = canvas->buffers_ready;
    if (!canvas->buffers_ready) {
        drop_buffer_resources(canvas);
    }
    canvas->pipe_ready = false;
    canvas->has_tex = false;
    canvas->loaded_path[0] = '\0';
    for (int i = 0; i < GUI_CANVAS_MAX_PAGES; i++) {
        canvas->page_valid[i] = false;
    }
    if (canvas->result) {
        canvas->pages_dirty = true;
    }
}

void gui_canvas_init(gui_canvas *canvas) {
    memset(canvas, 0, sizeof *canvas);
    canvas->mode = GUI_CANVAS_SOURCE;
    canvas->sel_sprite = -1;
    canvas->hover_sprite = -1;
    canvas->anim_sprite = -1;
    canvas->show_outline = true;
    canvas->show_slice9 = true;
    canvas->overlay_scale = 1.0F;
    canvas->scale = 1.0F;
    canvas->fit_pending = true;
    gui_canvas_restore_gpu(canvas);
}

void gui_canvas_shutdown(gui_canvas *canvas) {
    if (canvas->has_tex) {
        nt_gfx_destroy_texture(canvas->tex);
        canvas->has_tex = false;
    }
    for (int i = 0; i < GUI_CANVAS_MAX_PAGES; i++) {
        if (canvas->page_valid[i]) {
            nt_gfx_destroy_texture(canvas->pages[i]);
            canvas->page_valid[i] = false;
        }
    }
    drop_buffer_resources(canvas);
}

void gui_canvas_ensure_pipeline(gui_canvas *canvas,
                                const nt_material_info_t *sprite_info) {
    if (canvas->pipe_ready || !sprite_info || !sprite_info->ready) {
        return;
    }
    nt_vertex_layout_t layout = {0};
    layout.attrs[0] = (nt_vertex_attr_t){
        .location = NT_ATTR_POSITION, .format = NT_FORMAT_FLOAT3, .offset = 0};
    layout.attrs[1] = (nt_vertex_attr_t){
        .location = NT_ATTR_COLOR, .format = NT_FORMAT_FLOAT4, .offset = 12};
    layout.attrs[2] = (nt_vertex_attr_t){
        .location = NT_ATTR_TEXCOORD0, .format = NT_FORMAT_FLOAT2, .offset = 28};
    layout.attr_count = 3;
    layout.stride = (uint16_t)sizeof(canvas_vert);
    canvas->pipe = nt_gfx_make_pipeline(&(nt_pipeline_desc_t){
        .vertex_shader = {.id = sprite_info->resolved_vs},
        .fragment_shader = {.id = sprite_info->resolved_fs},
        .layout = layout,
        .depth_test = false,
        .depth_write = false,
        .cull_mode = 0,
        .blend = true,
        .blend_src = NT_BLEND_ONE,
        .blend_dst = NT_BLEND_ONE_MINUS_SRC_ALPHA,
        .label = "ntpacker_canvas",
    });
    canvas->pipe_ready = canvas->pipe.id != 0U;
}

void gui_canvas_set_frame_ubo(gui_canvas *canvas, nt_buffer_t ubo) {
    canvas->frame_ubo = ubo;
}

void gui_canvas_set_ui_scale(gui_canvas *canvas, float scale) {
    canvas->overlay_scale = scale > 0.1F ? scale : 1.0F;
}

bool gui_canvas_set_image(gui_canvas *canvas, const char *abs_path,
                          char *err_out, size_t err_cap) {
    if (!abs_path || abs_path[0] == '\0') {
        return false;
    }
    const size_t path_len = strlen(abs_path);
    if (path_len >= sizeof canvas->loaded_path) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap,
                           "image path exceeds maximum of %zu UTF-8 bytes",
                           sizeof canvas->loaded_path - 1U);
        }
        return false;
    }
    if (canvas->has_tex && strcmp(canvas->loaded_path, abs_path) == 0) {
        return true;
    }
    tp_image_rgba8 image = {0};
    tp_error error = {{0}};
    if (tp_image_load_file(abs_path, &image, &error) != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", error.msg);
        }
        return false;
    }
    const int width = image.width;
    const int height = image.height;
    uint32_t max_dimension =
        nt_gfx_gpu_caps() ? nt_gfx_gpu_caps()->max_texture_size : 4096U;
    if (max_dimension == 0U) {
        max_dimension = 4096U;
    }
    if ((uint32_t)width > max_dimension ||
        (uint32_t)height > max_dimension) {
        tp_image_free(&image);
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap,
                           "image %dx%d exceeds max texture %u", width,
                           height, max_dimension);
        }
        return false;
    }
    if (canvas->has_tex) {
        nt_gfx_destroy_texture(canvas->tex);
        canvas->has_tex = false;
    }
    canvas->tex = upload_rgba_premul(image.pixels, width, height,
                                     "ntpacker_canvas_img");
    tp_image_free(&image);
    canvas->img_w = width;
    canvas->img_h = height;
    canvas->has_tex = true;
    memcpy(canvas->loaded_path, abs_path, path_len + 1U);
    return true;
}

void gui_canvas_clear(gui_canvas *canvas) {
    if (canvas->has_tex) {
        nt_gfx_destroy_texture(canvas->tex);
        canvas->has_tex = false;
    }
    canvas->img_w = 0;
    canvas->img_h = 0;
    canvas->loaded_path[0] = '\0';
}

void gui_canvas_invalidate(gui_canvas *canvas) {
    canvas->loaded_path[0] = '\0';
}

const char *gui_canvas_loaded_path(const gui_canvas *canvas) {
    return canvas->loaded_path;
}

bool gui_canvas_has_image(const gui_canvas *canvas) {
    return canvas->has_tex;
}

int gui_canvas_img_w(const gui_canvas *canvas) {
    return canvas->img_w;
}

int gui_canvas_img_h(const gui_canvas *canvas) {
    return canvas->img_h;
}

void gui_canvas_set_result(gui_canvas *canvas, const tp_result *result) {
    canvas->result = result;
    canvas->sel_sprite = -1;
    canvas->hover_sprite = -1;
    if (!result) {
        drop_pages(canvas);
        canvas->pages_dirty = false;
        canvas->mode = GUI_CANVAS_SOURCE;
        return;
    }
    canvas->mode = GUI_CANVAS_ATLAS;
    canvas->cur_page = 0;
    canvas->fit_pending = true;
    canvas->pages_dirty = true;
}

void gui_canvas_upload_pages(gui_canvas *canvas) {
    if (!canvas->pages_dirty || !canvas->result) {
        return;
    }
    drop_pages(canvas);
    canvas->upload_failed = false;
    const uint32_t gpu_max = g_nt_gfx.gpu_caps.max_texture_size;
    int page_count = canvas->result->page_count;
    if (page_count > GUI_CANVAS_MAX_PAGES) {
        page_count = GUI_CANVAS_MAX_PAGES;
    }
    for (int i = 0; i < page_count; i++) {
        const tp_page *page = &canvas->result->pages[i];
        if (!page->rgba || page->w <= 0 || page->h <= 0) {
            continue;
        }
        if ((uint32_t)page->w > gpu_max || (uint32_t)page->h > gpu_max) {
            canvas->upload_failed = true;
            canvas->page_w[i] = page->w;
            canvas->page_h[i] = page->h;
            continue;
        }
        char label[32];
        (void)snprintf(label, sizeof label, "ntpacker_page_%d", i);
        canvas->pages[i] = page->premultiplied
                               ? nt_gfx_make_texture(&(nt_texture_desc_t){
                                     .width = (uint16_t)page->w,
                                     .height = (uint16_t)page->h,
                                     .data = page->rgba,
                                     .format = NT_PIXEL_RGBA8,
                                     .min_filter = NT_FILTER_NEAREST,
                                     .mag_filter = NT_FILTER_NEAREST,
                                     .wrap_u = NT_WRAP_CLAMP_TO_EDGE,
                                     .wrap_v = NT_WRAP_CLAMP_TO_EDGE,
                                     .label = label,
                                 })
                               : upload_rgba_premul(page->rgba, page->w,
                                                    page->h, label);
        canvas->page_w[i] = page->w;
        canvas->page_h[i] = page->h;
        if (canvas->pages[i].id == 0) {
            canvas->upload_failed = true;
            continue;
        }
        canvas->page_valid[i] = true;
    }
    canvas->page_count = page_count;
    if (canvas->cur_page >= page_count) {
        canvas->cur_page = 0;
    }
    canvas->pages_dirty = false;
}

bool gui_canvas_has_atlas(const gui_canvas *canvas) {
    return canvas->result != NULL && canvas->page_count > 0;
}

int gui_canvas_page_count(const gui_canvas *canvas) {
    return canvas->page_count;
}

int gui_canvas_cur_page(const gui_canvas *canvas) {
    return canvas->cur_page;
}

void gui_canvas_set_page(gui_canvas *canvas, int page) {
    if (page >= 0 && page < canvas->page_count) {
        canvas->cur_page = page;
        canvas->fit_pending = true;
    }
}

void gui_canvas_set_mode(gui_canvas *canvas, gui_canvas_mode mode) {
    canvas->mode = mode;
}

gui_canvas_mode gui_canvas_get_mode(const gui_canvas *canvas) {
    return canvas->mode;
}
