#include "gui_canvas.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "math/nt_math.h"
#include "renderers/nt_shape_renderer.h"

#include "clay.h"
#include "stb_image.h"

/* Vertex the sprite shaders consume: a_position(loc0, vec3), a_color(loc2, vec4),
 * a_texcoord(loc3, vec2). Tightly packed -> stride 36. */
typedef struct canvas_vert {
    float pos[3];
    float col[4];
    float uv[2];
} canvas_vert;

// #region D4 decode (mirror of tp_pack_read.c:25-53 tp_transform_decode / tp_transform_out_dims)
/* Maps a trim-local corner (x,y) in [0..tw]x[0..th] to its on-page-relative corner given the
 * region's D4 transform mask. Apply order: diagonal -> flipH -> flipV (corner reflection). */
static void d4_decode(int32_t x, int32_t y, uint8_t flags, int32_t tw, int32_t th, int32_t *ox, int32_t *oy) {
    int32_t rx = x;
    int32_t ry = y;
    if (flags & 4u) {
        int32_t t = rx;
        rx = ry;
        ry = t;
    }
    int32_t w = (flags & 4u) ? th : tw;
    int32_t h = (flags & 4u) ? tw : th;
    if (flags & 1u) {
        rx = w - rx;
    }
    if (flags & 2u) {
        ry = h - ry;
    }
    *ox = rx;
    *oy = ry;
}
static void d4_out_dims(uint8_t flags, int32_t tw, int32_t th, int32_t *ow, int32_t *oh) {
    if (flags & 4u) {
        *ow = th;
        *oh = tw;
    } else {
        *ow = tw;
        *oh = th;
    }
}
// #endregion

// #region lifecycle
void gui_canvas_restore_gpu(gui_canvas *c) {
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    c->ibo = nt_gfx_make_buffer(&(nt_buffer_desc_t){
        .type = NT_BUFFER_INDEX,
        .usage = NT_USAGE_IMMUTABLE,
        .data = idx,
        .size = sizeof idx,
        .index_type = NT_INDEX_UINT16,
        .label = "canvas_ibo",
    });
    c->vbo = nt_gfx_make_buffer(&(nt_buffer_desc_t){
        .type = NT_BUFFER_VERTEX,
        .usage = NT_USAGE_DYNAMIC,
        .size = 4U * sizeof(canvas_vert),
        .label = "canvas_vbo",
    });
    c->sampler = nt_gfx_make_sampler(&(nt_sampler_desc_t){
        .min_filter = NT_FILTER_NEAREST, /* nearest so packed pixels are crisp when zoomed in */
        .mag_filter = NT_FILTER_NEAREST,
        .wrap_u = NT_WRAP_CLAMP_TO_EDGE,
        .wrap_v = NT_WRAP_CLAMP_TO_EDGE,
        .label = "canvas_sampler",
    });
    c->buffers_ready = true;
    /* pipeline + textures are GPU-side: force a rebuild/redecode after a loss */
    c->pipe_ready = false;
    c->has_tex = false;
    c->loaded_path[0] = '\0';
    for (int i = 0; i < GUI_CANVAS_MAX_PAGES; i++) {
        c->page_valid[i] = false;
    }
    if (c->result) {
        c->pages_dirty = true; /* re-upload from the (still-borrowed) result */
    }
}

void gui_canvas_init(gui_canvas *c) {
    memset(c, 0, sizeof *c);
    c->mode = GUI_CANVAS_SOURCE;
    c->sel_sprite = -1;
    c->hover_sprite = -1;
    c->show_outline = true;
    c->scale = 1.0F;
    c->fit_pending = true;
    gui_canvas_restore_gpu(c);
}

void gui_canvas_shutdown(gui_canvas *c) {
    if (c->has_tex) {
        nt_gfx_destroy_texture(c->tex);
        c->has_tex = false;
    }
    for (int i = 0; i < GUI_CANVAS_MAX_PAGES; i++) {
        if (c->page_valid[i]) {
            nt_gfx_destroy_texture(c->pages[i]);
            c->page_valid[i] = false;
        }
    }
}
// #endregion

// #region pipeline / frame ubo
void gui_canvas_ensure_pipeline(gui_canvas *c, const nt_material_info_t *sprite_info) {
    if (c->pipe_ready || !sprite_info || !sprite_info->ready) {
        return;
    }
    nt_vertex_layout_t layout = {0};
    layout.attrs[0] = (nt_vertex_attr_t){.location = NT_ATTR_POSITION, .format = NT_FORMAT_FLOAT3, .offset = 0};
    layout.attrs[1] = (nt_vertex_attr_t){.location = NT_ATTR_COLOR, .format = NT_FORMAT_FLOAT4, .offset = 12};
    layout.attrs[2] = (nt_vertex_attr_t){.location = NT_ATTR_TEXCOORD0, .format = NT_FORMAT_FLOAT2, .offset = 28};
    layout.attr_count = 3;
    layout.stride = (uint16_t)sizeof(canvas_vert);
    c->pipe = nt_gfx_make_pipeline(&(nt_pipeline_desc_t){
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
    c->pipe_ready = (c->pipe.id != 0U);
}

void gui_canvas_set_frame_ubo(gui_canvas *c, nt_buffer_t ubo) { c->frame_ubo = ubo; }
// #endregion

// #region source image
static nt_texture_t upload_rgba_premul(const unsigned char *src, int w, int h, const char *label) {
    const size_t n = (size_t)w * (size_t)h;
    unsigned char *px = (unsigned char *)malloc(n * 4U);
    if (!px) {
        return (nt_texture_t){0};
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned a = src[i * 4U + 3U];
        px[i * 4U + 0U] = (unsigned char)((src[i * 4U + 0U] * a) / 255U);
        px[i * 4U + 1U] = (unsigned char)((src[i * 4U + 1U] * a) / 255U);
        px[i * 4U + 2U] = (unsigned char)((src[i * 4U + 2U] * a) / 255U);
        px[i * 4U + 3U] = (unsigned char)a;
    }
    nt_texture_t t = nt_gfx_make_texture(&(nt_texture_desc_t){
        .width = (uint16_t)w,
        .height = (uint16_t)h,
        .data = px,
        .format = NT_PIXEL_RGBA8,
        .min_filter = NT_FILTER_NEAREST,
        .mag_filter = NT_FILTER_NEAREST,
        .wrap_u = NT_WRAP_CLAMP_TO_EDGE,
        .wrap_v = NT_WRAP_CLAMP_TO_EDGE,
        .label = label,
    });
    free(px);
    return t;
}

bool gui_canvas_set_image(gui_canvas *c, const char *abs_path, char *err_out, size_t err_cap) {
    if (!abs_path || abs_path[0] == '\0') {
        return false;
    }
    if (c->has_tex && strcmp(c->loaded_path, abs_path) == 0) {
        return true;
    }
    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *src = stbi_load(abs_path, &w, &h, &comp, 4);
    if (!src) {
        const char *why = stbi_failure_reason();
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "decode failed: %s", why ? why : "unknown");
        }
        return false;
    }
    uint32_t maxdim = nt_gfx_gpu_caps() ? nt_gfx_gpu_caps()->max_texture_size : 4096U;
    if (maxdim == 0U) {
        maxdim = 4096U;
    }
    if ((uint32_t)w > maxdim || (uint32_t)h > maxdim) {
        stbi_image_free(src);
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "image %dx%d exceeds max texture %u", w, h, maxdim);
        }
        return false;
    }
    if (c->has_tex) {
        nt_gfx_destroy_texture(c->tex);
        c->has_tex = false;
    }
    c->tex = upload_rgba_premul(src, w, h, "ntpacker_canvas_img");
    stbi_image_free(src);
    c->img_w = w;
    c->img_h = h;
    c->has_tex = true;
    (void)snprintf(c->loaded_path, sizeof c->loaded_path, "%s", abs_path);
    return true;
}

void gui_canvas_clear(gui_canvas *c) {
    if (c->has_tex) {
        nt_gfx_destroy_texture(c->tex);
        c->has_tex = false;
    }
    c->img_w = 0;
    c->img_h = 0;
    c->loaded_path[0] = '\0';
}

void gui_canvas_invalidate(gui_canvas *c) { c->loaded_path[0] = '\0'; }
const char *gui_canvas_loaded_path(const gui_canvas *c) { return c->loaded_path; }
bool gui_canvas_has_image(const gui_canvas *c) { return c->has_tex; }
int gui_canvas_img_w(const gui_canvas *c) { return c->img_w; }
int gui_canvas_img_h(const gui_canvas *c) { return c->img_h; }
// #endregion

// #region atlas pages
static void drop_pages(gui_canvas *c) {
    for (int i = 0; i < GUI_CANVAS_MAX_PAGES; i++) {
        if (c->page_valid[i]) {
            nt_gfx_destroy_texture(c->pages[i]);
            c->page_valid[i] = false;
        }
    }
    c->page_count = 0;
}

void gui_canvas_set_result(gui_canvas *c, const tp_result *result) {
    c->result = result;
    c->sel_sprite = -1;
    c->hover_sprite = -1;
    if (!result) {
        drop_pages(c);
        c->pages_dirty = false;
        c->mode = GUI_CANVAS_SOURCE;
        return;
    }
    c->mode = GUI_CANVAS_ATLAS;
    c->cur_page = 0;
    c->fit_pending = true;
    c->pages_dirty = true; /* GL upload deferred to gui_canvas_upload_pages (render pass) */
}

void gui_canvas_upload_pages(gui_canvas *c) {
    if (!c->pages_dirty || !c->result) {
        return;
    }
    drop_pages(c);
    int n = c->result->page_count;
    if (n > GUI_CANVAS_MAX_PAGES) {
        n = GUI_CANVAS_MAX_PAGES;
    }
    for (int i = 0; i < n; i++) {
        const tp_page *pg = &c->result->pages[i];
        if (!pg->rgba || pg->w <= 0 || pg->h <= 0) {
            continue;
        }
        char label[32];
        (void)snprintf(label, sizeof label, "ntpacker_page_%d", i);
        /* tp_page.rgba is straight-alpha (export profile premultiplied=false) -> premultiply here. */
        c->pages[i] = pg->premultiplied ? nt_gfx_make_texture(&(nt_texture_desc_t){
                                              .width = (uint16_t)pg->w,
                                              .height = (uint16_t)pg->h,
                                              .data = pg->rgba,
                                              .format = NT_PIXEL_RGBA8,
                                              .min_filter = NT_FILTER_NEAREST,
                                              .mag_filter = NT_FILTER_NEAREST,
                                              .wrap_u = NT_WRAP_CLAMP_TO_EDGE,
                                              .wrap_v = NT_WRAP_CLAMP_TO_EDGE,
                                              .label = label,
                                          })
                                        : upload_rgba_premul(pg->rgba, pg->w, pg->h, label);
        c->page_w[i] = pg->w;
        c->page_h[i] = pg->h;
        c->page_valid[i] = true;
    }
    c->page_count = n;
    if (c->cur_page >= n) {
        c->cur_page = 0;
    }
    c->pages_dirty = false;
}

bool gui_canvas_has_atlas(const gui_canvas *c) { return c->result != NULL && c->page_count > 0; }
int gui_canvas_page_count(const gui_canvas *c) { return c->page_count; }
int gui_canvas_cur_page(const gui_canvas *c) { return c->cur_page; }
void gui_canvas_set_page(gui_canvas *c, int page) {
    if (page >= 0 && page < c->page_count) {
        c->cur_page = page;
        c->fit_pending = true;
    }
}
void gui_canvas_set_mode(gui_canvas *c, gui_canvas_mode mode) { c->mode = mode; }
gui_canvas_mode gui_canvas_get_mode(const gui_canvas *c) { return c->mode; }
// #endregion

// #region view math
float gui_canvas_zoom_pct(const gui_canvas *c) { return c->scale * 100.0F; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Page origin (layout coords of page pixel (0,0)) for the current page under (scale, cam). */
static void page_origin(const gui_canvas *c, const float bb[4], float scale, float *ox, float *oy) {
    const int pw = c->page_w[c->cur_page];
    const int ph = c->page_h[c->cur_page];
    const float cx = bb[0] + bb[2] * 0.5F + c->cam_x;
    const float cy = bb[1] + bb[3] * 0.5F + c->cam_y;
    *ox = cx - (float)pw * scale * 0.5F;
    *oy = cy - (float)ph * scale * 0.5F;
}

/* Keep the page always partly on-screen (>= 48 px of it inside the canvas box). */
static void clamp_cam(gui_canvas *c, const float bb[4]) {
    if (c->cur_page < 0 || c->cur_page >= c->page_count) {
        return;
    }
    const float pw = (float)c->page_w[c->cur_page] * c->scale;
    const float ph = (float)c->page_h[c->cur_page] * c->scale;
    const float margin = 48.0F;
    const float lim_x = pw * 0.5F + bb[2] * 0.5F - margin;
    const float lim_y = ph * 0.5F + bb[3] * 0.5F - margin;
    if (lim_x > 0.0F) {
        c->cam_x = clampf(c->cam_x, -lim_x, lim_x);
    } else {
        c->cam_x = 0.0F;
    }
    if (lim_y > 0.0F) {
        c->cam_y = clampf(c->cam_y, -lim_y, lim_y);
    } else {
        c->cam_y = 0.0F;
    }
}

void gui_canvas_fit(gui_canvas *c) { c->fit_pending = true; }

static void do_fit(gui_canvas *c, const float bb[4]) {
    if (c->cur_page < 0 || c->cur_page >= c->page_count) {
        return;
    }
    const float sx = bb[2] / (float)c->page_w[c->cur_page];
    const float sy = bb[3] / (float)c->page_h[c->cur_page];
    float s = (sx < sy ? sx : sy) * 0.96F;
    c->scale = clampf(s, 0.05F, 16.0F);
    c->cam_x = 0.0F;
    c->cam_y = 0.0F;
    c->fit_pending = false;
}

void gui_canvas_zoom_at(gui_canvas *c, const float bb[4], float cursor_x, float cursor_y, float wheel_notches) {
    if (c->cur_page < 0 || c->cur_page >= c->page_count) {
        return;
    }
    const float s0 = c->scale;
    const float s1 = clampf(s0 * powf(1.1F, -wheel_notches), 0.05F, 16.0F);
    if (s1 == s0) {
        return;
    }
    float ox0 = 0.0F;
    float oy0 = 0.0F;
    page_origin(c, bb, s0, &ox0, &oy0);
    const float u = (cursor_x - ox0) / s0; /* page px under cursor */
    const float v = (cursor_y - oy0) / s0;
    /* want ox1 + u*s1 = cursor -> derive cam from the requested origin */
    const float pw = (float)c->page_w[c->cur_page];
    const float ph = (float)c->page_h[c->cur_page];
    const float ox1 = cursor_x - u * s1;
    const float oy1 = cursor_y - v * s1;
    c->cam_x = ox1 + pw * s1 * 0.5F - (bb[0] + bb[2] * 0.5F);
    c->cam_y = oy1 + ph * s1 * 0.5F - (bb[1] + bb[3] * 0.5F);
    c->scale = s1;
    clamp_cam(c, bb);
}

void gui_canvas_set_zoom_pct(gui_canvas *c, const float bb[4], float pct) {
    c->scale = clampf(pct / 100.0F, 0.05F, 16.0F);
    clamp_cam(c, bb);
}

void gui_canvas_pan(gui_canvas *c, const float bb[4], float dx, float dy) {
    c->cam_x += dx;
    c->cam_y += dy;
    clamp_cam(c, bb);
}

int gui_canvas_hit(const gui_canvas *c, float lx, float ly) {
    if (!c->result || c->last_scale <= 0.0F) {
        return -1;
    }
    const float px = (lx - c->last_ox) / c->last_scale; /* page px */
    const float py = (ly - c->last_oy) / c->last_scale;
    int best = -1;
    for (int i = 0; i < c->result->sprite_count; i++) {
        const tp_sprite *s = &c->result->sprites[i];
        if (s->page != c->cur_page) {
            continue;
        }
        int32_t ow = 0;
        int32_t oh = 0;
        d4_out_dims(s->transform, s->frame.w, s->frame.h, &ow, &oh);
        if (px >= (float)s->frame.x && px < (float)(s->frame.x + ow) && py >= (float)s->frame.y &&
            py < (float)(s->frame.y + oh)) {
            best = i; /* last match wins -> topmost in draw order */
        }
    }
    return best;
}

void gui_canvas_select(gui_canvas *c, int sprite_index) {
    c->sel_sprite = sprite_index;
    if (c->result && sprite_index >= 0 && sprite_index < c->result->sprite_count) {
        const int pg = c->result->sprites[sprite_index].page;
        if (pg != c->cur_page && pg >= 0 && pg < c->page_count) {
            c->cur_page = pg;
        }
    }
}
int gui_canvas_selected(const gui_canvas *c) { return c->sel_sprite; }
// #endregion

// #region draw helpers
static void quad_to_world(const float world[16], float x0, float y0, float x1, float y1, const float uv[4],
                          float alpha, canvas_vert out[4]) {
    const float corner[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; i++) {
        vec4 in = {corner[i][0], corner[i][1], 0.0F, 1.0F};
        vec4 o;
        glm_mat4_mulv((vec4 *)world, in, o);
        out[i].pos[0] = o[0];
        out[i].pos[1] = o[1];
        out[i].pos[2] = o[2];
        out[i].col[0] = 1.0F;
        out[i].col[1] = 1.0F;
        out[i].col[2] = 1.0F;
        out[i].col[3] = alpha;
        out[i].uv[0] = (i == 0 || i == 3) ? uv[0] : uv[2];
        out[i].uv[1] = (i < 2) ? uv[1] : uv[3];
    }
}

static void draw_tex_quad(gui_canvas *c, const float world[16], nt_texture_t tex, float x0, float y0, float x1,
                          float y1, float alpha) {
    const float uv[4] = {0.0F, 0.0F, 1.0F, 1.0F};
    canvas_vert v[4];
    quad_to_world(world, x0, y0, x1, y1, uv, alpha, v);
    nt_gfx_update_buffer(c->vbo, v, sizeof v);
    nt_gfx_bind_pipeline(c->pipe);
    nt_gfx_bind_uniform_buffer(c->frame_ubo, 0);
    nt_gfx_bind_vertex_buffer(c->vbo);
    nt_gfx_bind_index_buffer(c->ibo);
    nt_gfx_bind_texture(tex, 0);
    nt_gfx_bind_sampler(c->sampler, 0);
    nt_gfx_draw_indexed(0, 6, 4);
}

/* layout (x,y) -> world 3-vec for the shape renderer. */
static void layout_to_world(const float world[16], float lx, float ly, float out[3]) {
    vec4 in = {lx, ly, 0.0F, 1.0F};
    vec4 o;
    glm_mat4_mulv((vec4 *)world, in, o);
    out[0] = o[0];
    out[1] = o[1];
    out[2] = o[2];
}

/* Region placement polygon in page px -> up to 32 points; returns count. Hull for polygon sprites,
 * placed AABB for rects. */
static int region_polygon(const tp_sprite *s, float ox, float oy, float scale, float pts[][2], int cap) {
    const int32_t fx = s->frame.x;
    const int32_t fy = s->frame.y;
    const int32_t tw = s->frame.w;
    const int32_t th = s->frame.h;
    int n = 0;
    if (s->vert_count >= 3 && s->verts) {
        for (int i = 0; i < s->vert_count && n < cap; i++) {
            int32_t px = 0;
            int32_t py = 0;
            d4_decode(s->verts[i].x, s->verts[i].y, s->transform, tw, th, &px, &py);
            pts[n][0] = ox + (float)(fx + px) * scale;
            pts[n][1] = oy + (float)(fy + py) * scale;
            n++;
        }
    } else {
        int32_t ow = 0;
        int32_t oh = 0;
        d4_out_dims(s->transform, tw, th, &ow, &oh);
        const int32_t cx[4] = {fx, fx + ow, fx + ow, fx};
        const int32_t cy[4] = {fy, fy, fy + oh, fy + oh};
        for (int i = 0; i < 4 && n < cap; i++) {
            pts[n][0] = ox + (float)cx[i] * scale;
            pts[n][1] = oy + (float)cy[i] * scale;
            n++;
        }
    }
    return n;
}

static void stroke_polygon(const float world[16], const float pts[][2], int n, const float color[4]) {
    for (int i = 0; i < n; i++) {
        float a[3];
        float b[3];
        layout_to_world(world, pts[i][0], pts[i][1], a);
        layout_to_world(world, pts[(i + 1) % n][0], pts[(i + 1) % n][1], b);
        nt_shape_renderer_line(a, b, color);
    }
}
// #endregion

// #region draw handler
void gui_canvas_handler(const nt_ui_custom_frame_t *frame, void *userdata) {
    gui_canvas *c = (gui_canvas *)userdata;
    if (!c || !c->pipe_ready || !c->buffers_ready) {
        return;
    }
    const Clay_RenderCommand *cmd = (const Clay_RenderCommand *)frame->clay_cmd;
    const Clay_BoundingBox bb = cmd->boundingBox;
    if (bb.width <= 1.0F || bb.height <= 1.0F) {
        return;
    }
    const float *world = frame->world_mat4;
    const float alpha = frame->opacity;

    if (c->mode == GUI_CANVAS_ATLAS && c->page_count > 0 && c->cur_page < c->page_count &&
        c->page_valid[c->cur_page]) {
        const float box[4] = {bb.x, bb.y, bb.width, bb.height};
        if (c->fit_pending) {
            do_fit(c, box);
        }
        clamp_cam(c, box);
        float ox = 0.0F;
        float oy = 0.0F;
        page_origin(c, box, c->scale, &ox, &oy);
        const float pw = (float)c->page_w[c->cur_page] * c->scale;
        const float ph = (float)c->page_h[c->cur_page] * c->scale;
        c->last_bb[0] = bb.x;
        c->last_bb[1] = bb.y;
        c->last_bb[2] = bb.width;
        c->last_bb[3] = bb.height;
        c->last_scale = c->scale;
        c->last_ox = ox;
        c->last_oy = oy;

        /* the packed page */
        draw_tex_quad(c, world, c->pages[c->cur_page], ox, oy, ox + pw, oy + ph, alpha);

        /* overlays: region outlines / hull + hover + selection (shape renderer, world-space lines) */
        if (c->show_outline && c->result) {
            const float col_out[4] = {0.35F, 0.75F, 1.0F, 0.55F * alpha};
            const float col_hov[4] = {0.85F, 0.9F, 1.0F, 0.85F * alpha};
            const float col_sel[4] = {1.0F, 0.78F, 0.28F, 0.95F * alpha};
            nt_shape_renderer_set_line_width(1.5F);
            for (int i = 0; i < c->result->sprite_count; i++) {
                const tp_sprite *s = &c->result->sprites[i];
                if (s->page != c->cur_page || i == c->sel_sprite || i == c->hover_sprite) {
                    continue; /* draw selected/hover last, on top */
                }
                float pts[32][2];
                const int np = region_polygon(s, ox, oy, c->scale, pts, 32);
                stroke_polygon(world, pts, np, col_out);
            }
            if (c->hover_sprite >= 0 && c->hover_sprite < c->result->sprite_count &&
                c->result->sprites[c->hover_sprite].page == c->cur_page) {
                float pts[32][2];
                const int np = region_polygon(&c->result->sprites[c->hover_sprite], ox, oy, c->scale, pts, 32);
                nt_shape_renderer_set_line_width(2.0F);
                stroke_polygon(world, pts, np, col_hov);
            }
            if (c->sel_sprite >= 0 && c->sel_sprite < c->result->sprite_count &&
                c->result->sprites[c->sel_sprite].page == c->cur_page) {
                float pts[32][2];
                const int np = region_polygon(&c->result->sprites[c->sel_sprite], ox, oy, c->scale, pts, 32);
                nt_shape_renderer_set_line_width(2.5F);
                stroke_polygon(world, pts, np, col_sel);
            }
            nt_shape_renderer_flush();
        }
        return;
    }

    /* SOURCE mode: fit-to-canvas letterbox, never upscale past 100% */
    if (!c->has_tex) {
        return;
    }
    const float sx = bb.width / (float)c->img_w;
    const float sy = bb.height / (float)c->img_h;
    float scale = (sx < sy) ? sx : sy;
    if (scale > 1.0F) {
        scale = 1.0F;
    }
    const float dw = (float)c->img_w * scale;
    const float dh = (float)c->img_h * scale;
    const float x0 = bb.x + ((bb.width - dw) * 0.5F);
    const float y0 = bb.y + ((bb.height - dh) * 0.5F);
    draw_tex_quad(c, world, c->tex, x0, y0, x0 + dw, y0 + dh, alpha);
}
// #endregion
