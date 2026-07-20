/* ntpacker-gui dev seam: the `--shot` screenshot mode (compiled into every build). Renders the real
 * UI at a requested size/scale, packs + selects so the panels populate, dumps ONE full-frame PNG at
 * the pre-swap point, and quits -- also the byte-reproducible refactor gate. See gui_shot.h. */

#include "gui_shot.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nt_utf8_fs.h"

#include "app/nt_app.h"       /* nt_app_quit */
#include "fpng/nt_fpng.h"     /* nt_fpng_encode_rgb (PNG capture) */
#include "graphics/nt_gfx.h"  /* nt_gfx_read_pixels */
#include "input/nt_input.h"   /* g_nt_input (neutralized each shot frame) */
#include "log/nt_log.h"       /* nt_log_info / nt_log_error */
#include "ui/nt_ui.h"         /* nt_ui_get_bbox / nt_ui_bbox_t */
#include "window/nt_window.h" /* g_nt_window (framebuffer dims) */

#include "tp_core/tp_export.h" /* tp_exporter_count/at -> map --shot-preview id to a selector index */
#include "tp_core/tp_identity.h"
#include "tp_core/tp_utf8.h"

#include "gui_actions.h" /* do_pack_blocking */
#include "gui_canvas.h"  /* s_canvas ops + GUI_CANVAS_ATLAS */
#include "gui_pack.h"    /* gui_pack_result / gui_pack_debug_force_busy / gui_pack_preview_blocking */
#include "gui_project.h" /* gui_project_mark_stale / snapshot generation */
#include "gui_rows.h"    /* select_row_for_region */
#include "gui_state.h"   /* s_ctx / s_id_* / s_sel_atlas / g_ui_scale / s_status_fixed_time / s_preview_* */

/* `--shot=out.png [--size=WxH] [--scale=F] [project]` renders the real UI at the requested window
 * size, packs the selected atlas, selects the first packed region (so the Region panel populates),
 * dumps ONE full-frame PNG at the pre-swap point, and quits. Lets tooling/agents SEE the actual UI
 * at arbitrary resolutions without a human taking screenshots. Not documented in --help/README on
 * purpose -- it is a dev seam, same spirit as NTPACKER_GUI_SELFTEST but available in every build. */
static bool s_shot_active;
static char s_shot_path[TP_IDENTITY_PATH_MAX];
static int s_shot_w = 1280;
static int s_shot_h = 800;
static float s_shot_scale;  /* 0 = keep the DPI-detected scale */
static int s_shot_frame;    /* counts only frames the UI actually rendered (can_render) */
static bool s_shot_written; /* capture happened; quit on the next frame boundary */
static bool s_shot_stale;   /* --shot-stale: pack, then re-mark stale so the shot shows the amber Pack + chip */
static bool s_shot_packing; /* --shot-packing: pack (blocking), then force the busy strip for the shot */
static char s_shot_preview[TP_EXPORTER_ID_MAX]; /* exact canonical exporter id */

/* main() arg loop: handle one dev screenshot flag; returns true if `arg` was consumed. Mirrors the
 * original inline parsing (order + validation unchanged). */
bool gui_shot_parse_arg(const char *arg) {
    if (strncmp(arg, "--shot=", 7) == 0) {
        const char *path = arg + 7;
        const size_t length = strlen(path);
        if (path[0] == '\0' || length >= sizeof s_shot_path ||
            !tp_utf8_is_valid_c_string(path)) {
            s_shot_path[0] = '\0';
            s_shot_active = false;
            s_status_fixed_time = false;
            (void)fprintf(stderr,
                          "ntpacker-gui: --shot path is empty, invalid UTF-8, or too long\n");
            return true;
        }
        memcpy(s_shot_path, path, length + 1U);
        s_shot_active = true;
        s_status_fixed_time = s_shot_active; /* shots must be byte-reproducible (refactor gate) */
        return true;
    }
    if (strncmp(arg, "--size=", 7) == 0) {
        int w = 0;
        int h = 0;
        if (sscanf(arg + 7, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 16384 && h <= 16384) {
            s_shot_w = w;
            s_shot_h = h;
        }
        return true;
    }
    if (strncmp(arg, "--scale=", 8) == 0) {
        const double f = atof(arg + 8);
        if (f >= 0.5 && f <= 4.0) {
            s_shot_scale = (float)f;
        }
        return true;
    }
    if (strcmp(arg, "--shot-stale") == 0) {
        s_shot_stale = true; /* dev: capture the amber stale state (Pack + chip) */
        return true;
    }
    if (strcmp(arg, "--shot-packing") == 0) {
        s_shot_packing = true; /* dev: capture the busy Packing... strip state */
        return true;
    }
    if (strncmp(arg, "--shot-preview=", 15) == 0) {
        const char *exporter_id = arg + 15;
        tp_error error = {0};
        if (tp_exporter_id_validate(exporter_id, &error) != TP_STATUS_OK) {
            s_shot_preview[0] = '\0';
            (void)fprintf(stderr, "ntpacker-gui: --shot-preview: %s\n",
                          error.msg);
            return true;
        }
        const size_t length = strlen(exporter_id);
        memcpy(s_shot_preview, exporter_id, length + 1U); /* dev: bind an export-target preview */
        return true;
    }
    return false;
}

/* main(), before nt_window_init(): pick the initial window size (shot size or the 1280x800 default). */
void gui_shot_apply_window_size(void) {
    g_nt_window.width = s_shot_active ? (uint32_t)s_shot_w : 1280;
    g_nt_window.height = s_shot_active ? (uint32_t)s_shot_h : 800;
}

/* main(), after DPI detection: pin the UI scale for reproducible captures (--scale). */
void gui_shot_apply_scale(void) {
    if (s_shot_scale > 0.0F) {
        g_ui_scale = s_shot_scale; /* screenshot mode: pin the scale for reproducible captures */
    }
}

/* True while a --shot capture is in progress (the shell gates hotkeys on it). */
bool gui_shot_active(void) { return s_shot_active; }

/* Runs inside the can_render block, after build_rows (selection needs the row model). */
void gui_shot_tick(void) {
    if (!s_shot_active) {
        return;
    }
    /* Byte-reproducibility: the shot window opens under the user's LIVE cursor, so real mouse
     * state leaks into the capture (hover outlines, wheel zoom on the canvas -- both observed as
     * flaky gate hashes while the owner used the machine). Dead-stick the pointers every frame
     * BEFORE the input pre-pass + nt_ui_begin consume them; the shot drives selection itself. */
    memset(g_nt_input.pointers, 0, sizeof g_nt_input.pointers);
    for (size_t i = 0; i < NT_INPUT_MAX_POINTERS; i++) {
        g_nt_input.pointers[i].x = -100000.0F;
        g_nt_input.pointers[i].y = -100000.0F;
    }
    s_shot_frame++;
    if (s_shot_frame == 6) { /* resources are bound; pack the selected atlas like Ctrl+P would (blocking) */
        if (s_sel_atlas < 0) {
            s_sel_atlas = 0;
        }
        if (!gui_pack_result(s_sel_atlas)) {
            do_pack_blocking();
        }
    } else if (s_shot_frame == 10) { /* pages uploaded; mimic a canvas click on region 0 */
        const tp_result *r = gui_pack_result(s_sel_atlas);
        if (r && r->sprite_count > 0) {
            s_canvas.mode = GUI_CANVAS_ATLAS;
            gui_canvas_select(&s_canvas, 0);
            select_row_for_region(0);
        }
        if (s_shot_stale) { /* dev: keep the packed preview but re-mark stale so the amber Pack + chip show */
            gui_project_mark_stale();
        }
        if (s_shot_packing) { /* dev: force the busy strip (Packing... + Cancel) for the screenshot */
            gui_pack_debug_force_busy(GUI_PACK_ASYNC_PACK);
        }
        if (s_shot_preview[0] != '\0') { /* dev: bind the named export-target preview (selector + degradation chip) */
            char perr[256] = {0};
            if (gui_pack_preview_blocking(s_sel_atlas, s_shot_preview, perr, sizeof perr)) {
                int idx = -1;
                for (int i = 0; i < tp_exporter_count(); i++) {
                    const tp_exporter *e = tp_exporter_at(i);
                    if (e && strcmp(e->id, s_shot_preview) == 0) {
                        idx = i;
                        break;
                    }
                }
                if (idx >= 0) {
                    s_preview_target = idx + 1; /* 0 = Native; k = exporter k-1 */
                }
            } else {
                nt_log_error("SHOT: preview '%s' failed: %s", s_shot_preview, perr);
            }
        }
    }
}

/* DEV: log the key container bboxes vs the window; flags any that spill past the edges. bbox comes from
 * the previous completed layout (stable by the capture frame). Mirrors the selftest overflow assert. */
static void shot_log_bounds(float win_w, float win_h) {
    const struct { const char *name; uint32_t id; } items[] = {
        {"left", s_id_left_panel}, {"strip", s_id_strip}, {"canvas", s_id_canvas},
        {"right", s_id_right_panel}, {"rcontent", s_id_right_content}, {"pill", s_id_status_pill}};
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        const nt_ui_bbox_t b = nt_ui_get_bbox(s_ctx, items[i].id);
        const float over_r = (b.x + b.width) - win_w;
        const float over_b = (b.y + b.height) - win_h;
        nt_log_info("SHOT-BOUNDS %-9s found=%d x=%.1f y=%.1f w=%.1f h=%.1f  right=%.1f/%.1f%s bottom=%.1f/%.1f%s",
                    items[i].name, (int)b.found, (double)b.x, (double)b.y, (double)b.width, (double)b.height,
                    (double)(b.x + b.width), (double)win_w, (over_r > 0.5F) ? " OVERFLOW-R" : "",
                    (double)(b.y + b.height), (double)win_h, (over_b > 0.5F) ? " OVERFLOW-B" : "");
    }
}

/* Pre-swap capture (same GL-valid point as selftest_post_draw). */
void gui_shot_post_draw(void) {
    if (!s_shot_active || s_shot_frame < 16) {
        return;
    }
    if (s_shot_written) { /* captured last frame -> quit at a clean frame boundary */
        s_shot_active = false;
        nt_app_quit();
        return;
    }
    const uint32_t w = g_nt_window.fb_width;
    const uint32_t h = g_nt_window.fb_height;
    if (w == 0 || h == 0) {
        return;
    }
    if (!s_shot_written) {
        shot_log_bounds((float)w, (float)h);
    }
    const uint32_t rgba_n = w * h * 4u;
    uint8_t *rgba = (uint8_t *)malloc(rgba_n);
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3u);
    uint8_t *png = (uint8_t *)malloc(rgba_n + 65536u);
    bool ok = rgba && rgb && png && nt_gfx_read_pixels(0, 0, (int)w, (int)h, rgba, rgba_n);
    if (ok) {
        for (uint32_t i = 0, j = 0; i < rgba_n; i += 4u, j += 3u) {
            rgb[j] = rgba[i];
            rgb[j + 1] = rgba[i + 1];
            rgb[j + 2] = rgba[i + 2];
        }
        nt_fpng_init();
        const uint32_t n = nt_fpng_encode_rgb(rgb, w, h, png, rgba_n + 65536u);
        FILE *f = (n > 0) ? nt_utf8_fopen(s_shot_path, "wb") : NULL;
        ok = f && fwrite(png, 1, n, f) == n;
        if (f) {
            (void)fclose(f);
        }
        if (ok) {
            nt_log_info("SHOT: wrote %s (%ux%u, scale %.2f)", s_shot_path, w, h, (double)g_ui_scale);
        }
    }
    if (!ok) {
        nt_log_error("SHOT: capture failed (%s)", s_shot_path);
    }
    free(png);
    free(rgb);
    free(rgba);
    s_shot_written = true;
}
