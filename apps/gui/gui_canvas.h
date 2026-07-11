#ifndef NTPACKER_GUI_CANVAS_H
#define NTPACKER_GUI_CANVAS_H

/* The center canvas, now DUAL-MODE (ux.md region E / §2.4):
 *   - SOURCE mode: a single stb-decoded source image, letterboxed (selecting a sprite row with no
 *     pack result). Original Task-5 behaviour.
 *   - ATLAS mode: the REAL packed page texture from a tp_result, drawn at a game-owned zoom/pan with
 *     a checkerboard behind transparency and region overlays (outline / hull / selection) via the
 *     shape renderer. This is the live artifact -- baked rotations/flips are visible in the pixels.
 *
 * Both modes share one small textured-quad pipeline built from the sprite shaders (the sprite
 * renderer always binds the atlas PAGE, so it cannot show an arbitrary texture -- we bind our own).
 * The page-draw helper (draw_region_at) is structured so the next packet's animation preview window
 * can reuse it to draw a named region into a small rect.
 *
 * Coordinate spaces: the CUSTOM handler gets a LAYOUT-space (Y-down) bounding box + a world_mat4 that
 * bakes LAYOUT->world (incl. the screen Y-flip). Page pixels (y-down) map to layout via a game-owned
 * scale + pan, then to world via world_mat4. Overlays feed the shape renderer world-space verts. */

#include <stddef.h>

#include "graphics/nt_gfx.h"
#include "material/nt_material.h"
#include "ui/nt_ui.h"

#include "tp_core/tp_model.h" /* tp_result / tp_sprite */

#ifdef __cplusplus
extern "C" {
#endif

/* ANIM = the animation preview player: draws a single packed region per frame in untrimmed source
 * space (so trim never shifts the pivot -> no jitter between frames), honoring per-animation flips. */
typedef enum { GUI_CANVAS_SOURCE = 0, GUI_CANVAS_ATLAS, GUI_CANVAS_ANIM } gui_canvas_mode;

#define GUI_CANVAS_MAX_PAGES 16

typedef struct gui_canvas {
    gui_canvas_mode mode;

    /* --- SOURCE image --- */
    nt_texture_t tex;
    bool has_tex;
    int img_w, img_h;
    char loaded_path[512]; /* decode cache key */

    /* --- ATLAS pages --- */
    nt_texture_t pages[GUI_CANVAS_MAX_PAGES];
    bool page_valid[GUI_CANVAS_MAX_PAGES];
    int page_w[GUI_CANVAS_MAX_PAGES], page_h[GUI_CANVAS_MAX_PAGES];
    int page_count;
    int cur_page;
    const tp_result *result; /* borrowed (arena-owned by gui_pack); NULL = no atlas */
    bool pages_dirty;        /* result set but textures not yet uploaded (GL deferred to the pass) */
    bool upload_failed;      /* a page exceeded the GPU max texture size / OOM'd -> skipped (16K pages) */
    int sel_sprite;          /* accent-outlined region index, -1 none */
    int hover_sprite;        /* subtle-outline region index, -1 none (set by the handler each frame) */
    bool show_outline, show_trim, show_pivot, show_frame; /* overlay toggles (frame = placed AABB) */
    bool show_slice9;        /* slice9 guide lines over the SELECTED region (silent unless its result
                                slice9 is nonzero; values come from the packed result, like pivots) */
    float overlay_scale;     /* DPI/UI scale for overlay line widths (set by the host each frame) */

    /* --- ANIM preview (host sets these each frame; the handler draws one region) --- */
    int anim_sprite;                  /* result sprite index of the current frame, -1 = none */
    int anim_ref_w, anim_ref_h;       /* untrimmed reference box (source px) for stable scale/centering */
    bool anim_flip_h, anim_flip_v;    /* per-animation flips, applied on top of the baked D4 transform */

    /* --- ATLAS view: scale (screen px per page px; 1.0 == 100%) + pan of page centre from canvas
     * centre (layout px). fit_pending refits on the next draw. --- */
    float scale;
    float cam_x, cam_y;
    bool fit_pending;
    /* last drawn geometry (captured in the handler) so main.c input maps layout<->page consistently */
    float last_bb[4];    /* canvas box x,y,w,h (layout) */
    float last_scale;    /* actual scale used */
    float last_ox, last_oy; /* layout coords of current page's pixel (0,0) */

    /* --- shared draw resources --- */
    nt_pipeline_t pipe;
    bool pipe_ready;
    nt_buffer_t vbo; /* dynamic, 4 verts (source / atlas page) */
    nt_buffer_t ibo; /* static, 6 indices */
    nt_sampler_t sampler;
    bool buffers_ready;
    nt_buffer_t frame_ubo; /* view_proj (Globals) */
    /* checkerboard behind atlas transparency: 2x2 texture + REPEAT sampler + its own vbo (avoids
     * aliasing the page vbo with two updates/frame) */
    nt_buffer_t vbo_checker;
    nt_texture_t checker_tex;
    nt_sampler_t checker_sampler;
    bool checker_valid;
} gui_canvas;

void gui_canvas_init(gui_canvas *c);
void gui_canvas_shutdown(gui_canvas *c);
void gui_canvas_restore_gpu(gui_canvas *c);
void gui_canvas_ensure_pipeline(gui_canvas *c, const nt_material_info_t *sprite_info);
void gui_canvas_set_frame_ubo(gui_canvas *c, nt_buffer_t ubo);
/* Host UI scale (drives overlay line widths so outlines read on high-DPI). */
void gui_canvas_set_ui_scale(gui_canvas *c, float scale);

/* --- SOURCE mode --- */
bool gui_canvas_set_image(gui_canvas *c, const char *abs_path, char *err_out, size_t err_cap);
void gui_canvas_clear(gui_canvas *c); /* drops source image; leaves atlas state */
void gui_canvas_invalidate(gui_canvas *c);
const char *gui_canvas_loaded_path(const gui_canvas *c);
bool gui_canvas_has_image(const gui_canvas *c);
int gui_canvas_img_w(const gui_canvas *c);
int gui_canvas_img_h(const gui_canvas *c);

/* --- ATLAS mode --- */
/* Borrows `result` (NULL clears the atlas view). Marks pages dirty; the actual GPU upload happens in
 * gui_canvas_upload_pages (call inside the render pass). Switches mode to ATLAS and refits. */
void gui_canvas_set_result(gui_canvas *c, const tp_result *result);
/* Uploads any pending page textures (GL; call once per frame inside the pass). No-op when clean. */
void gui_canvas_upload_pages(gui_canvas *c);
bool gui_canvas_has_atlas(const gui_canvas *c);
int gui_canvas_page_count(const gui_canvas *c);
int gui_canvas_cur_page(const gui_canvas *c);
void gui_canvas_set_page(gui_canvas *c, int page);
void gui_canvas_set_mode(gui_canvas *c, gui_canvas_mode mode);
gui_canvas_mode gui_canvas_get_mode(const gui_canvas *c);

/* zoom/pan (bb = canvas box in layout px, from nt_ui_get_bbox). zoom_pct: 100 == 1:1. */
float gui_canvas_zoom_pct(const gui_canvas *c);
void gui_canvas_fit(gui_canvas *c);
void gui_canvas_set_zoom_pct(gui_canvas *c, const float bb[4], float pct);
void gui_canvas_zoom_at(gui_canvas *c, const float bb[4], float cursor_x, float cursor_y, float wheel_notches);
void gui_canvas_pan(gui_canvas *c, const float bb[4], float dx, float dy);

/* Region hit-test at layout point (lx,ly): the sprite index on the current page whose placed AABB
 * contains the point, or -1. Uses the last drawn geometry. */
int gui_canvas_hit(const gui_canvas *c, float lx, float ly);
/* Selects a region (accent outline); -1 clears. If the region is on another page, switches to it. */
void gui_canvas_select(gui_canvas *c, int sprite_index);
int gui_canvas_selected(const gui_canvas *c);

/* The CUSTOM element draw handler. Register via nt_ui_set_custom_handler(ctx, fn, c). */
void gui_canvas_handler(const nt_ui_custom_frame_t *frame, void *userdata);

/* --- animation playback (pure; no GL) --- */
/* Frame index to display at `elapsed` seconds for a `frame_count`-frame flipbook at `fps`, under the
 * Defold-pinned playback id (0 once_forward, 1 loop_forward, 2 once_backward, 3 loop_backward,
 * 4 once_pingpong, 5 loop_pingpong, 6 none). `finished` (nullable) is set true once a non-looping mode
 * has reached its final logical frame (and for `none`). Bounds-safe for count<=1 / fps<=0. */
int gui_canvas_anim_frame_at(double elapsed, float fps, int playback, int frame_count, bool *finished);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_CANVAS_H */
