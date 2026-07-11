#ifndef NTPACKER_GUI_SHOT_H
#define NTPACKER_GUI_SHOT_H

/* Dev seam: the `--shot` screenshot mode -- render the real UI at a requested size/scale, pack +
 * select so the panels populate, dump ONE full-frame PNG at the pre-swap point, and quit. Compiled
 * into EVERY build (unlike the selftest) -- it doubles as the byte-reproducible refactor gate. Split
 * out of main.c as GUI decomposition step 3 -- pure move, no behavior change. The prototypes below are
 * the only entry points main()/frame() call. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main() arg loop: consume one dev screenshot flag (--shot=/--size=/--scale=/--shot-stale/
 * --shot-packing). Returns true if `arg` was a screenshot flag (so main() skips its project-arg
 * fallback). Other dev flags (e.g. --auto-pack) and the project path stay in main(). */
bool gui_shot_parse_arg(const char *arg);

/* main(), BEFORE nt_window_init(): set the initial window dims -- the shot size when --shot is active,
 * else the default 1280x800. Kept a call from main() so the init order is unchanged. */
void gui_shot_apply_window_size(void);

/* main(), after the DPI scale is detected: pin g_ui_scale to --scale for reproducible captures. */
void gui_shot_apply_scale(void);

/* True while a --shot capture is in progress. The shell gates the global hotkeys on it (the user
 * may be typing/clicking elsewhere while a headless capture runs -- real input must not leak in). */
bool gui_shot_active(void);

/* frame(), inside can_render after build_rows: pack + select the first region, then (on the capture
 * frame) drive the PNG dump. No-op unless --shot is active. Also dead-sticks the mouse pointers
 * each frame so the live cursor can't add hover/wheel state to the capture. */
void gui_shot_tick(void);

/* frame(), at the pre-swap point: full-frame PNG capture + SHOT-BOUNDS logging, then quit. */
void gui_shot_post_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SHOT_H */
