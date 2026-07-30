#ifndef NTPACKER_GUI_SHOT_H
#define NTPACKER_GUI_SHOT_H

/* Dev seam: the `--shot` screenshot mode -- render the real UI at a requested size/scale, pack +
 * select so the panels populate, dump ONE full-frame PNG at the pre-swap point, and quit. Compiled
 * only when NTPACKER_GUI_DEV_SEAMS is on (gui_shot.c is not compiled otherwise), so the shipped
 * binary carries neither the capture code nor its argv strings -- the #else fallbacks below make
 * every call site in main()/frame() fold away. It doubles as the byte-reproducible refactor gate.
 * The prototypes below are the only entry points main()/frame() call. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NTPACKER_GUI_DEV_SEAMS

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

/* frame(), at the between-frame ingress boundary (beside gui_bench_tick) BEFORE the observation is
 * pinned: dead-stick the mouse pointers so the live cursor can't add hover/wheel state to the
 * capture, advance the total-frame liveness counter (the ONLY place it advances), and run the
 * blocking pack -- a model mutation, so it must never run inside the pinned frame. The pack is
 * gated on RENDERED frames. No-op unless --shot is active. */
void gui_shot_pre_pin_tick(void);

/* frame(), inside the can_render/sprite_info block: record that this frame actually reached the
 * render path. The pack/select/capture thresholds count RENDERED frames, not elapsed ones -- a
 * pre-bootstrap frame draws nothing, so counting it captured a blank PNG on a slow start. */
void gui_shot_note_rendered(void);

/* frame(), inside can_render after build_rows: select the first packed region and bind the dev
 * stale/packing/preview states, all of which need the built row model. No-op unless --shot. */
void gui_shot_tick(void);

/* frame(), at the pre-swap point: full-frame PNG capture + SHOT-BOUNDS logging, then quit. */
void gui_shot_post_draw(void);

#else /* !NTPACKER_GUI_DEV_SEAMS -- shipped build: every entry point is a compile-time no-op. */

static inline bool gui_shot_parse_arg(const char *arg) { (void)arg; return false; }
static inline void gui_shot_apply_window_size(void) {}
static inline void gui_shot_apply_scale(void) {}
static inline bool gui_shot_active(void) { return false; }
static inline void gui_shot_pre_pin_tick(void) {}
static inline void gui_shot_note_rendered(void) {}
static inline void gui_shot_tick(void) {}
static inline void gui_shot_post_draw(void) {}

#endif /* NTPACKER_GUI_DEV_SEAMS */

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SHOT_H */
