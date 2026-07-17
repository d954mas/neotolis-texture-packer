#ifndef NTPACKER_GUI_SELFTEST_H
#define NTPACKER_GUI_SELFTEST_H

/* Dev seam: a headless project-ops smoke test (create/add/decode/save/reload/pack/export/animations)
 * plus an auto-quit phase driver that renders real frames, pixel-verifies the region overlay + no
 * layout overflow, then quits. Entirely compiled out unless NTPACKER_GUI_SELFTEST is defined
 * (gui_selftest.c is then an empty TU). The three hooks below are the ONLY entry points main()/frame()
 * call. */

#ifdef NTPACKER_GUI_SELFTEST

#ifdef __cplusplus
extern "C" {
#endif

/* main(), before the frame loop: the headless project-ops smoke (asserts + SELFTEST-* logging). */
void run_selftest(void);

/* frame(), top: sets up each auto-quit phase's scene BEFORE the layout/walk; quits when done. */
void selftest_pre_frame(void);

/* frame(), end: pixel readbacks after nt_ui_walk drew the overlay, before the buffers swap. */
void selftest_post_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SELFTEST */

#endif /* NTPACKER_GUI_SELFTEST_H */
