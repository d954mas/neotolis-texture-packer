#include "gui_canvas.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "math/nt_math.h"

#include "clay.h"
#include "stb_image.h"

/* Vertex the sprite shaders consume: a_position(loc0, vec3), a_color(loc2, vec4),
 * a_texcoord(loc3, vec2). Tightly packed -> stride 36. */
typedef struct canvas_vert {
    float pos[3];
    float col[4];
    float uv[2];
} canvas_vert;

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
        .min_filter = NT_FILTER_LINEAR,
        .mag_filter = NT_FILTER_LINEAR,
        .wrap_u = NT_WRAP_CLAMP_TO_EDGE,
        .wrap_v = NT_WRAP_CLAMP_TO_EDGE,
        .label = "canvas_sampler",
    });
    c->buffers_ready = true;
    /* pipeline + decoded texture are GPU-side too: force a rebuild/redecode after a loss */
    c->pipe_ready = false;
    c->has_tex = false;
    c->loaded_path[0] = '\0';
}

void gui_canvas_init(gui_canvas *c) {
    memset(c, 0, sizeof *c);
    gui_canvas_restore_gpu(c);
}

void gui_canvas_shutdown(gui_canvas *c) {
    if (c->has_tex) {
        nt_gfx_destroy_texture(c->tex);
        c->has_tex = false;
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

    /* Reuse the already-compiled sprite shaders (resolved handle ids from the material). */
    c->pipe = nt_gfx_make_pipeline(&(nt_pipeline_desc_t){
        .vertex_shader = {.id = sprite_info->resolved_vs},
        .fragment_shader = {.id = sprite_info->resolved_fs},
        .layout = layout,
        .depth_test = false,
        .depth_write = false,
        .cull_mode = 0,
        /* Premultiplied blend: matches sprite.frag's premultiplied output; we premultiply
         * the decoded pixels below so transparent PNGs composite without halos. */
        .blend = true,
        .blend_src = NT_BLEND_ONE,
        .blend_dst = NT_BLEND_ONE_MINUS_SRC_ALPHA,
        .label = "ntpacker_canvas",
    });
    c->pipe_ready = (c->pipe.id != 0U);
}

void gui_canvas_set_frame_ubo(gui_canvas *c, nt_buffer_t ubo) { c->frame_ubo = ubo; }
// #endregion

// #region image load
bool gui_canvas_set_image(gui_canvas *c, const char *abs_path, char *err_out, size_t err_cap) {
    if (!abs_path || abs_path[0] == '\0') {
        return false;
    }
    if (c->has_tex && strcmp(c->loaded_path, abs_path) == 0) {
        return true; /* cached: already decoded + uploaded */
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *px = stbi_load(abs_path, &w, &h, &comp, 4);
    if (!px) {
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
        stbi_image_free(px);
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "image %dx%d exceeds max texture %u", w, h, maxdim);
        }
        return false;
    }

    /* Premultiply straight-alpha (stb) so the premultiplied blend is correct. */
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = px + (i * 4U);
        const unsigned a = p[3];
        p[0] = (unsigned char)((p[0] * a) / 255U);
        p[1] = (unsigned char)((p[1] * a) / 255U);
        p[2] = (unsigned char)((p[2] * a) / 255U);
    }

    if (c->has_tex) {
        nt_gfx_destroy_texture(c->tex);
        c->has_tex = false;
    }
    c->tex = nt_gfx_make_texture(&(nt_texture_desc_t){
        .width = (uint16_t)w,
        .height = (uint16_t)h,
        .data = px,
        .format = NT_PIXEL_RGBA8,
        .min_filter = NT_FILTER_LINEAR,
        .mag_filter = NT_FILTER_LINEAR,
        .wrap_u = NT_WRAP_CLAMP_TO_EDGE,
        .wrap_v = NT_WRAP_CLAMP_TO_EDGE,
        .label = "ntpacker_canvas_img",
    });
    stbi_image_free(px);

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

// #region draw handler
void gui_canvas_handler(const nt_ui_custom_frame_t *frame, void *userdata) {
    gui_canvas *c = (gui_canvas *)userdata;
    if (!c || !c->has_tex || !c->pipe_ready || !c->buffers_ready) {
        return;
    }
    const Clay_RenderCommand *cmd = (const Clay_RenderCommand *)frame->clay_cmd;
    const Clay_BoundingBox bb = cmd->boundingBox; /* LAYOUT space, Y-down */
    if (bb.width <= 1.0F || bb.height <= 1.0F) {
        return;
    }

    /* Fit-to-canvas letterbox; never upscale past 100% (Task 5). */
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
    const float x1 = x0 + dw;
    const float y1 = y0 + dh;

    /* TL, TR, BR, BL. uv(0,0) = image top-left (stb row 0). world_mat4 Y-flips LAYOUT->world. */
    const float corner[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const float uv[4][2] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}};
    const float alpha = frame->opacity;

    canvas_vert verts[4];
    for (int i = 0; i < 4; i++) {
        vec4 in = {corner[i][0], corner[i][1], 0.0F, 1.0F};
        vec4 out;
        glm_mat4_mulv((vec4 *)frame->world_mat4, in, out); /* LAYOUT -> world; shader applies view_proj */
        verts[i].pos[0] = out[0];
        verts[i].pos[1] = out[1];
        verts[i].pos[2] = out[2];
        verts[i].col[0] = 1.0F;
        verts[i].col[1] = 1.0F;
        verts[i].col[2] = 1.0F;
        verts[i].col[3] = alpha;
        verts[i].uv[0] = uv[i][0];
        verts[i].uv[1] = uv[i][1];
    }
    nt_gfx_update_buffer(c->vbo, verts, sizeof verts);

    nt_gfx_bind_pipeline(c->pipe);
    nt_gfx_bind_uniform_buffer(c->frame_ubo, 0); /* Globals: view_proj */
    nt_gfx_bind_vertex_buffer(c->vbo);
    nt_gfx_bind_index_buffer(c->ibo);
    nt_gfx_bind_texture(c->tex, 0);
    nt_gfx_bind_sampler(c->sampler, 0);
    nt_gfx_draw_indexed(0, 6, 4);
}
// #endregion
