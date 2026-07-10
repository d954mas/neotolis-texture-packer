#ifndef NTPACKER_GUI_CANVAS_H
#define NTPACKER_GUI_CANVAS_H

/* The center-canvas source-image preview (ux.md region E, Task 5): a self-contained
 * textured-quad draw inside an nt_ui CUSTOM element. The selected sprite/source row's
 * image is stb-decoded to RGBA8, premultiplied, uploaded to a GPU texture, and drawn
 * letterboxed into the canvas box through the CUSTOM handler's world_mat4.
 *
 * Why a dedicated pipeline and not the sprite renderer: the sprite renderer always binds
 * the atlas PAGE texture to slot 0, so it cannot show an arbitrary decoded image. Instead
 * we build one small pipeline from the already-packed sprite shaders (per-vertex, not
 * instanced) and bind our own texture to slot 0 -- the walker flushes the renderers before
 * the handler and the handler owns its GL state, so this composes cleanly.
 *
 * This struct is the SEED of the future atlas-page canvas (zoom/pan/overlays); the fit math
 * and draw are structured so zoom/pan slot in later. */

#include <stddef.h>

#include "graphics/nt_gfx.h"
#include "material/nt_material.h"
#include "ui/nt_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gui_canvas {
    /* decoded source image */
    nt_texture_t tex;
    bool has_tex;
    int img_w, img_h;
    char loaded_path[512]; /* cache key: skip re-decode when unchanged */

    /* draw resources (lazy: pipeline waits for the sprite shaders to resolve) */
    nt_pipeline_t pipe;
    bool pipe_ready;
    nt_buffer_t vbo; /* dynamic, 4 verts */
    nt_buffer_t ibo; /* static, 6 indices */
    nt_sampler_t sampler;
    bool buffers_ready;
    nt_buffer_t frame_ubo; /* view_proj (Globals) -- refreshed each frame by the host */
} gui_canvas;

/* Creates the static index buffer, dynamic vertex buffer, and sampler. */
void gui_canvas_init(gui_canvas *c);
void gui_canvas_shutdown(gui_canvas *c);

/* Rebuilds GPU-side buffers/sampler after a context loss (call from the restore path). */
void gui_canvas_restore_gpu(gui_canvas *c);

/* Builds the pipeline from the resolved sprite shaders once they are ready. No-op after. */
void gui_canvas_ensure_pipeline(gui_canvas *c, const nt_material_info_t *sprite_info);

/* Host binds the per-frame view_proj UBO here before nt_ui_walk (handle changes on restore). */
void gui_canvas_set_frame_ubo(gui_canvas *c, nt_buffer_t ubo);

/* Decodes+uploads abs_path (cached by path). On failure fills err_out and clears the image.
 * Returns true on success (including the cached no-op). */
bool gui_canvas_set_image(gui_canvas *c, const char *abs_path, char *err_out, size_t err_cap);

/* Drops the current image (no selection). */
void gui_canvas_clear(gui_canvas *c);

bool gui_canvas_has_image(const gui_canvas *c);
int gui_canvas_img_w(const gui_canvas *c);
int gui_canvas_img_h(const gui_canvas *c);

/* The CUSTOM element draw handler. Register via nt_ui_set_custom_handler(ctx, fn, c). */
void gui_canvas_handler(const nt_ui_custom_frame_t *frame, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_CANVAS_H */
