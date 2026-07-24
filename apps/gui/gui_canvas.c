#include "gui_canvas_internal.h"

#include <math.h>

#include "math/nt_math.h"
#include "renderers/nt_shape_renderer.h"

#include "tp_core/tp_transform.h"

#include "clay.h"

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

bool gui_canvas_zoom_to_sprite(gui_canvas *c, const float bb[4],
                               int sprite_index) {
    if (!c || !bb || !c->result || sprite_index < 0 ||
        sprite_index >= c->result->sprite_count || bb[2] <= 0.0F ||
        bb[3] <= 0.0F) {
        return false;
    }
    const tp_sprite *sprite = &c->result->sprites[sprite_index];
    const int page = sprite->page;
    if (page < 0 || page >= c->page_count || c->page_w[page] <= 0 ||
        c->page_h[page] <= 0) {
        return false;
    }
    int32_t out_w = 0;
    int32_t out_h = 0;
    tp_transform_out_dims(sprite->transform, sprite->frame.w, sprite->frame.h,
                          &out_w, &out_h);
    if (out_w <= 0 || out_h <= 0) {
        return false;
    }

    const float sx = bb[2] / (float)out_w;
    const float sy = bb[3] / (float)out_h;
    const float scale = clampf((sx < sy ? sx : sy) * 0.96F, 0.05F, 16.0F);
    const float region_cx =
        (float)sprite->frame.x + (float)out_w * 0.5F;
    const float region_cy =
        (float)sprite->frame.y + (float)out_h * 0.5F;

    c->cur_page = page;
    c->scale = scale;
    c->cam_x =
        ((float)c->page_w[page] * 0.5F - region_cx) * scale;
    c->cam_y =
        ((float)c->page_h[page] * 0.5F - region_cy) * scale;
    c->fit_pending = false;
    clamp_cam(c, bb);
    return true;
}

void gui_canvas_double_click_reset(gui_canvas_double_click_ref *ref) {
    if (ref) {
        *ref = (gui_canvas_double_click_ref){0};
    }
}

bool gui_canvas_double_click_press(gui_canvas_double_click_ref *ref,
                                   const tp_result *result, int sprite_index,
                                   bool engine_double_clicked) {
    if (!ref) {
        return false;
    }
    const bool same_ref =
        ref->valid && ref->result == result &&
        ref->sprite_index == sprite_index;
    ref->result = result;
    ref->sprite_index = sprite_index;
    ref->valid = result != NULL && sprite_index >= 0;
    return engine_double_clicked && same_ref && ref->valid;
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
        tp_transform_out_dims(s->transform, s->frame.w, s->frame.h, &ow, &oh);
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

static void draw_quad(gui_canvas *c, const float world[16], nt_buffer_t vbo, nt_texture_t tex, nt_sampler_t samp,
                      float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float alpha) {
    const float uv[4] = {u0, v0, u1, v1};
    canvas_vert v[4];
    quad_to_world(world, x0, y0, x1, y1, uv, alpha, v);
    nt_gfx_update_buffer(vbo, v, sizeof v);
    nt_gfx_bind_pipeline(c->pipe);
    nt_gfx_bind_uniform_buffer(c->frame_ubo, 0);
    nt_gfx_bind_vertex_buffer(vbo);
    nt_gfx_bind_index_buffer(c->ibo);
    nt_gfx_bind_texture(tex, 0);
    nt_gfx_bind_sampler(samp, 0);
    nt_gfx_draw_indexed(0, 6, 4);
}

static void draw_tex_quad(gui_canvas *c, const float world[16], nt_texture_t tex, float x0, float y0, float x1,
                          float y1, float alpha) {
    draw_quad(c, world, c->vbo, tex, c->sampler, x0, y0, x1, y1, 0.0F, 0.0F, 1.0F, 1.0F, alpha);
}

/* Textured quad with independent per-corner layout positions + UVs (order TL,TR,BR,BL). Unlike
 * draw_tex_quad this maps each corner's UV explicitly, so a region baked rotated/flipped into the page
 * can be re-drawn upright (the animation preview). */
static void draw_quad_corners(gui_canvas *c, const float world[16], nt_texture_t tex, const float pos[4][2],
                              const float uv[4][2], float alpha) {
    canvas_vert v[4];
    for (int i = 0; i < 4; i++) {
        vec4 in = {pos[i][0], pos[i][1], 0.0F, 1.0F};
        vec4 o;
        glm_mat4_mulv((vec4 *)world, in, o);
        v[i].pos[0] = o[0];
        v[i].pos[1] = o[1];
        v[i].pos[2] = o[2];
        v[i].col[0] = 1.0F;
        v[i].col[1] = 1.0F;
        v[i].col[2] = 1.0F;
        v[i].col[3] = alpha;
        v[i].uv[0] = uv[i][0];
        v[i].uv[1] = uv[i][1];
    }
    nt_gfx_update_buffer(c->vbo, v, sizeof v);
    nt_gfx_bind_pipeline(c->pipe);
    nt_gfx_bind_uniform_buffer(c->frame_ubo, 0);
    nt_gfx_bind_vertex_buffer(c->vbo);
    nt_gfx_bind_index_buffer(c->ibo);
    nt_gfx_bind_texture(tex, 0);
    nt_gfx_bind_sampler(c->sampler, 0);
    nt_gfx_draw_indexed(0, 6, 4);
}

/* Draws the current animation-preview frame: region `c->anim_sprite` placed in untrimmed source space
 * (frame's source box centered on the canvas, trimmed content offset by spriteSourceSize -> no jitter),
 * with a checkerboard behind. `flip_h/flip_v` mirror the quad about the source-box centre. */
static void draw_anim_frame(gui_canvas *c, const float world[16], const float box[4], float alpha) {
    const int rw = (c->anim_ref_w > 0) ? c->anim_ref_w : 1;
    const int rh = (c->anim_ref_h > 0) ? c->anim_ref_h : 1;
    const float sx = box[2] / (float)rw;
    const float sy = box[3] / (float)rh;
    float pscale = (sx < sy ? sx : sy) * 0.9F;
    pscale = clampf(pscale, 0.001F, 4096.0F);
    const float cx = box[0] + box[2] * 0.5F;
    const float cy = box[1] + box[3] * 0.5F;

    if (c->checker_valid) {
        const float rbw = (float)rw * pscale;
        const float rbh = (float)rh * pscale;
        const float x0 = cx - rbw * 0.5F;
        const float y0 = cy - rbh * 0.5F;
        draw_quad(c, world, c->vbo_checker, c->checker_tex, c->checker_sampler, x0, y0, x0 + rbw, y0 + rbh, 0.0F, 0.0F,
                  rbw / 16.0F, rbh / 16.0F, alpha);
    }

    const int si = c->anim_sprite;
    if (si < 0 || !c->result || si >= c->result->sprite_count) {
        return;
    }
    const tp_sprite *s = &c->result->sprites[si];
    const int pg = s->page;
    if (pg < 0 || pg >= c->page_count || !c->page_valid[pg]) {
        return;
    }
    const float pw = (float)c->page_w[pg];
    const float ph = (float)c->page_h[pg];
    const float srcW = (float)s->sourceSize.w;
    const float srcH = (float)s->sourceSize.h;
    const float sboxX = cx - srcW * pscale * 0.5F;
    const float sboxY = cy - srcH * pscale * 0.5F;
    const float tw = (float)s->frame.w;
    const float th = (float)s->frame.h;
    const float dx0 = sboxX + (float)s->spriteSourceSize.x * pscale;
    const float dy0 = sboxY + (float)s->spriteSourceSize.y * pscale;
    const float dx1 = dx0 + tw * pscale;
    const float dy1 = dy0 + th * pscale;
    float pos[4][2] = {{dx0, dy0}, {dx1, dy0}, {dx1, dy1}, {dx0, dy1}};
    for (int i = 0; i < 4; i++) {
        if (c->anim_flip_h) {
            pos[i][0] = 2.0F * cx - pos[i][0];
        }
        if (c->anim_flip_v) {
            pos[i][1] = 2.0F * cy - pos[i][1];
        }
    }
    /* Each display corner maps to a trim-local corner; the shared D4 decode bakes the page rotation/flip into UV. */
    const int32_t lc[4][2] = {{0, 0}, {s->frame.w, 0}, {s->frame.w, s->frame.h}, {0, s->frame.h}};
    float uv[4][2];
    for (int i = 0; i < 4; i++) {
        int32_t px = 0;
        int32_t py = 0;
        tp_transform_decode(lc[i][0], lc[i][1], s->transform,
                            s->frame.w, s->frame.h, &px, &py);
        uv[i][0] = ((float)s->frame.x + (float)px) / pw;
        uv[i][1] = ((float)s->frame.y + (float)py) / ph;
    }
    draw_quad_corners(c, world, c->pages[pg], pos, uv, alpha);
}

int gui_canvas_anim_frame_at(double elapsed, float fps, int playback, int frame_count, bool *finished) {
    bool fin_local = false;
    if (!finished) {
        finished = &fin_local;
    }
    *finished = false;
    if (frame_count <= 1 || playback == 6 /* none */) {
        *finished = true;
        return 0;
    }
    if (!(fps > 0.0F)) {
        return 0;
    }
    if (elapsed < 0.0) {
        elapsed = 0.0;
    }
    const long step = (long)floor(elapsed * (double)fps);
    const long n = frame_count;
    switch (playback) {
        case 0: /* once forward */
            if (step >= n - 1) {
                *finished = true;
                return (int)(n - 1);
            }
            return (int)step;
        case 1: /* loop forward */
            return (int)(step % n);
        case 2: /* once backward */
            if (step >= n - 1) {
                *finished = true;
                return 0;
            }
            return (int)(n - 1 - step);
        case 3: /* loop backward */
            return (int)(n - 1 - (step % n));
        case 4: { /* once pingpong: 0..n-1..0 then hold at 0 */
            const long period = 2 * n - 2;
            if (step >= period) {
                *finished = true;
                return 0;
            }
            return (int)((step < n) ? step : (period - step));
        }
        case 5: { /* loop pingpong */
            const long period = 2 * n - 2;
            const long k = step % period;
            return (int)((k < n) ? k : (period - k));
        }
        default:
            return (int)(step % n);
    }
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

/* Max hull vertices we render. Engine per-region cap is 8 (hard 16), so 256 is comfortably generous;
 * a hull larger than this is SKIPPED (return 0), never silently truncated + closed across the sprite. */
#define GUI_CANVAS_MAX_HULL 256

/* Region placement polygon in page px -> `pts` (cap points); returns the point count, or 0 if the
 * hull exceeds `cap` (caller skips it rather than drawing a wrong closed boundary). Hull for polygon
 * sprites, placed AABB for rects. */
static int region_polygon(const tp_sprite *s, float ox, float oy, float scale, float pts[][2], int cap) {
    const int32_t fx = s->frame.x;
    const int32_t fy = s->frame.y;
    const int32_t tw = s->frame.w;
    const int32_t th = s->frame.h;
    int n = 0;
    if (s->vert_count >= 3 && s->verts) {
        if (s->vert_count > cap) {
            return 0; /* never close a truncated polygon */
        }
        for (int i = 0; i < s->vert_count; i++) {
            int32_t px = 0;
            int32_t py = 0;
            tp_transform_decode(s->verts[i].x, s->verts[i].y,
                                s->transform, tw, th, &px, &py);
            pts[n][0] = ox + (float)(fx + px) * scale;
            pts[n][1] = oy + (float)(fy + py) * scale;
            n++;
        }
    } else {
        int32_t ow = 0;
        int32_t oh = 0;
        tp_transform_out_dims(s->transform, tw, th, &ow, &oh);
        const int32_t cx[4] = {fx, fx + ow, fx + ow, fx};
        const int32_t cy[4] = {fy, fy, fy + oh, fy + oh};
        for (int i = 0; i < 4; i++) {
            pts[n][0] = ox + (float)cx[i] * scale;
            pts[n][1] = oy + (float)cy[i] * scale;
            n++;
        }
    }
    return n;
}

/* Placed AABB (from the shared D4 dimensions) in page-layout coords -- the rectangular bounds incl. the
 * padding gap between regions. Always 4 points. */
static void region_aabb(const tp_sprite *s, float ox, float oy, float scale, float pts[4][2]) {
    int32_t ow = 0;
    int32_t oh = 0;
    tp_transform_out_dims(s->transform, s->frame.w, s->frame.h, &ow, &oh);
    const int32_t fx = s->frame.x;
    const int32_t fy = s->frame.y;
    const int32_t cx[4] = {fx, fx + ow, fx + ow, fx};
    const int32_t cy[4] = {fy, fy, fy + oh, fy + oh};
    for (int i = 0; i < 4; i++) {
        pts[i][0] = ox + (float)cx[i] * scale;
        pts[i][1] = oy + (float)cy[i] * scale;
    }
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

/* Original (untrimmed) source bounds in page-layout coords: the trim box is [0..tw]x[0..th] in
 * trim-local space; the source box is [-ssx..-ssx+srcW] x [-ssy..-ssy+srcH], mapped through the same
 * (affine) D4 transform + frame offset. Shows how much was trimmed away vs the packed content. */
static void region_source_rect(const tp_sprite *s, float ox, float oy, float scale, float pts[4][2]) {
    const int32_t tw = s->frame.w;
    const int32_t th = s->frame.h;
    const int32_t fx = s->frame.x;
    const int32_t fy = s->frame.y;
    const int32_t sx0 = -s->spriteSourceSize.x;
    const int32_t sy0 = -s->spriteSourceSize.y;
    const int32_t sx1 = sx0 + s->sourceSize.w;
    const int32_t sy1 = sy0 + s->sourceSize.h;
    const int32_t cx[4] = {sx0, sx1, sx1, sx0};
    const int32_t cy[4] = {sy0, sy0, sy1, sy1};
    for (int i = 0; i < 4; i++) {
        int32_t px = 0;
        int32_t py = 0;
        tp_transform_decode(cx[i], cy[i], s->transform, tw, th, &px, &py);
        pts[i][0] = ox + (float)(fx + px) * scale;
        pts[i][1] = oy + (float)(fy + py) * scale;
    }
}

/* One slice9 guide segment: trim-local endpoints -> D4 -> page -> world line (same mapping as the
 * trim ghost corners, so guides stay glued to the sprite under any packed transform). */
static void slice9_line(const float world[16], const tp_sprite *s, float ox, float oy, float scale,
                        float x0, float y0, float x1, float y1, const float color[4]) {
    float ax = 0.0F;
    float ay = 0.0F;
    float bx = 0.0F;
    float by = 0.0F;
    tp_transform_decode_f(x0, y0, s->transform, (float)s->frame.w,
                          (float)s->frame.h, &ax, &ay);
    tp_transform_decode_f(x1, y1, s->transform, (float)s->frame.w,
                          (float)s->frame.h, &bx, &by);
    float wa[3];
    float wb[3];
    layout_to_world(world, ox + ((float)s->frame.x + ax) * scale, oy + ((float)s->frame.y + ay) * scale, wa);
    layout_to_world(world, ox + ((float)s->frame.x + bx) * scale, oy + ((float)s->frame.y + by) * scale, wb);
    nt_shape_renderer_line(wa, wb, color);
}

/* Pivot in page-layout coords: pivot is normalized over sourceSize (y-down) -> source px -> trim-local
 * -> D4 -> page. May sit outside the frame. */
static void pivot_point(const tp_sprite *s, float ox, float oy, float scale, float out[2]) {
    const float pvx = s->pivot.x * (float)s->sourceSize.w - (float)s->spriteSourceSize.x;
    const float pvy = s->pivot.y * (float)s->sourceSize.h - (float)s->spriteSourceSize.y;
    float dx = 0.0F;
    float dy = 0.0F;
    tp_transform_decode_f(pvx, pvy, s->transform, (float)s->frame.w,
                          (float)s->frame.h, &dx, &dy);
    out[0] = ox + ((float)s->frame.x + dx) * scale;
    out[1] = oy + ((float)s->frame.y + dy) * scale;
}

static void stroke_crosshair(const float world[16], float cx, float cy, float hl, const float color[4]) {
    float a[3];
    float b[3];
    layout_to_world(world, cx - hl, cy, a);
    layout_to_world(world, cx + hl, cy, b);
    nt_shape_renderer_line(a, b, color);
    layout_to_world(world, cx, cy - hl, a);
    layout_to_world(world, cx, cy + hl, b);
    nt_shape_renderer_line(a, b, color);
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

    if (c->mode == GUI_CANVAS_ANIM) {
        const float box[4] = {bb.x, bb.y, bb.width, bb.height};
        draw_anim_frame(c, world, box, alpha);
        return;
    }

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

        /* checkerboard behind transparency (tiled ~16px screen cells), then the packed page */
        if (c->checker_valid) {
            const float cell = 16.0F;
            draw_quad(c, world, c->vbo_checker, c->checker_tex, c->checker_sampler, ox, oy, ox + pw, oy + ph, 0.0F,
                      0.0F, pw / cell, ph / cell, alpha);
        }
        draw_tex_quad(c, world, c->pages[c->cur_page], ox, oy, ox + pw, oy + ph, alpha);

        /* overlays (shape renderer, world-space lines). Line widths scale with the host UI scale so
         * outlines read on high-DPI. Draw order: trim ghost, frame AABB, hull outline, pivot; the
         * hovered/selected hull last so it sits on top. */
        if ((c->show_outline || c->show_trim || c->show_pivot || c->show_frame || c->show_slice9) && c->result) {
            const float w = c->overlay_scale;
            const float col_out[4] = {0.30F, 0.72F, 1.0F, 0.80F * alpha};
            const float col_hov[4] = {0.95F, 0.97F, 1.0F, 0.95F * alpha};
            const float col_sel[4] = {1.0F, 0.72F, 0.20F, 1.0F * alpha};
            const float col_trim[4] = {0.45F, 0.9F, 0.5F, 0.5F * alpha};
            const float col_frame[4] = {0.55F, 0.58F, 0.66F, 0.55F * alpha};
            const float col_piv[4] = {1.0F, 0.4F, 0.85F, 0.95F * alpha};
            for (int i = 0; i < c->result->sprite_count; i++) {
                const tp_sprite *s = &c->result->sprites[i];
                if (s->page != c->cur_page) {
                    continue;
                }
                if (c->show_trim && s->trimmed) {
                    float sp[4][2];
                    region_source_rect(s, ox, oy, c->scale, sp);
                    nt_shape_renderer_set_line_width(1.0F * w);
                    stroke_polygon(world, sp, 4, col_trim);
                }
                if (c->show_frame) {
                    float fr[4][2];
                    region_aabb(s, ox, oy, c->scale, fr);
                    nt_shape_renderer_set_line_width(1.0F * w);
                    stroke_polygon(world, fr, 4, col_frame);
                }
                if (c->show_outline && i != c->sel_sprite && i != c->hover_sprite) {
                    float pts[GUI_CANVAS_MAX_HULL][2];
                    const int np = region_polygon(s, ox, oy, c->scale, pts, GUI_CANVAS_MAX_HULL);
                    nt_shape_renderer_set_line_width(2.0F * w);
                    stroke_polygon(world, pts, np, col_out);
                }
                if (c->show_pivot) {
                    float pv[2];
                    pivot_point(s, ox, oy, c->scale, pv);
                    nt_shape_renderer_set_line_width(1.5F * w);
                    stroke_crosshair(world, pv[0], pv[1], 6.0F * w, col_piv);
                }
            }
            if (c->show_outline && c->hover_sprite >= 0 && c->hover_sprite < c->result->sprite_count &&
                c->result->sprites[c->hover_sprite].page == c->cur_page) {
                float pts[GUI_CANVAS_MAX_HULL][2];
                const int np = region_polygon(&c->result->sprites[c->hover_sprite], ox, oy, c->scale, pts, GUI_CANVAS_MAX_HULL);
                nt_shape_renderer_set_line_width(2.5F * w);
                stroke_polygon(world, pts, np, col_hov);
            }
            if (c->show_outline && c->sel_sprite >= 0 && c->sel_sprite < c->result->sprite_count &&
                c->result->sprites[c->sel_sprite].page == c->cur_page) {
                float pts[GUI_CANVAS_MAX_HULL][2];
                const int np = region_polygon(&c->result->sprites[c->sel_sprite], ox, oy, c->scale, pts, GUI_CANVAS_MAX_HULL);
                nt_shape_renderer_set_line_width(3.0F * w);
                stroke_polygon(world, pts, np, col_sel);
            }
            /* Slice9 guides (selected region only, ux: make the 9-patch cuts visible on the sprite):
             * two vertical + two horizontal cut lines in untrimmed source space, mapped through the
             * same D4 path as the trim ghost -- correct over hull shapes and packed transforms alike.
             * Insets are LIVE (host feeds the project override each frame), so typing in the Region
             * panel moves the lines immediately; the region geometry itself is from the last pack. */
            if (c->show_slice9 && c->sel_sprite >= 0 && c->sel_sprite < c->result->sprite_count &&
                c->result->sprites[c->sel_sprite].page == c->cur_page &&
                (c->sel_slice9[0] || c->sel_slice9[1] || c->sel_slice9[2] || c->sel_slice9[3])) {
                const tp_sprite *s = &c->result->sprites[c->sel_sprite];
                const float col_s9[4] = {0.25F, 0.92F, 0.95F, 0.95F * alpha};
                const float sw = (float)s->sourceSize.w;
                const float sh = (float)s->sourceSize.h;
                /* source-image (0,0) sits at (-spriteSourceSize.x, -spriteSourceSize.y) trim-local */
                const float sx0 = -(float)s->spriteSourceSize.x;
                const float sy0 = -(float)s->spriteSourceSize.y;
                nt_shape_renderer_set_line_width(1.5F * w);
                if (c->sel_slice9[0] > 0) {
                    const float x = sx0 + (float)c->sel_slice9[0];
                    slice9_line(world, s, ox, oy, c->scale, x, sy0, x, sy0 + sh, col_s9);
                }
                if (c->sel_slice9[1] > 0) {
                    const float x = sx0 + sw - (float)c->sel_slice9[1];
                    slice9_line(world, s, ox, oy, c->scale, x, sy0, x, sy0 + sh, col_s9);
                }
                if (c->sel_slice9[2] > 0) {
                    const float y = sy0 + (float)c->sel_slice9[2];
                    slice9_line(world, s, ox, oy, c->scale, sx0, y, sx0 + sw, y, col_s9);
                }
                if (c->sel_slice9[3] > 0) {
                    const float y = sy0 + sh - (float)c->sel_slice9[3];
                    slice9_line(world, s, ox, oy, c->scale, sx0, y, sx0 + sw, y, col_s9);
                }
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
