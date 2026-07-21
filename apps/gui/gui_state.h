#ifndef NTPACKER_GUI_STATE_H
#define NTPACKER_GUI_STATE_H

/* Shared mutable editor/UI state for the ntpacker GUI: selection, multi-select set, inline-edit,
 * disclosure bits, animation preview, runtime panel widths, the nt_ui context + canvas, executable
 * dir, UI ids, per-frame row tooltips, the status line, and the ~30 per-frame-scaled style objects
 * (written by apply_ui_scale in gui_widgets + seeded once in ensure_ids; read everywhere).
 *
 * Include discipline: this header pulls in ENGINE ui/font/atlas headers (for the style + context
 * types) plus the one MODEL header gui_canvas.h (for the gui_canvas type). It must NEVER include a
 * sibling view/actions/rows/widgets header. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_scan.h"

#include "font/nt_font.h"        /* nt_font_t (s_font) */
#include "ui/nt_ui.h"            /* nt_ui_context_t (s_ctx) */
#include "ui/nt_ui_button.h"     /* nt_ui_button_style_t */
#include "ui/nt_ui_dropdown.h"   /* nt_ui_dropdown_style_t */
#include "ui/nt_ui_input.h"      /* nt_ui_input_style_t */
#include "ui/nt_ui_label.h"      /* nt_ui_label_style_t */
#include "ui/nt_ui_menu.h"       /* nt_ui_menu_style_t */
#include "ui/nt_ui_modal.h"      /* nt_ui_modal_style_t */
#include "ui/nt_ui_scroll.h"     /* nt_ui_scroll_style_t */
#include "ui/nt_ui_slider.h"     /* nt_ui_slider_style_t */
#include "ui/nt_ui_tooltip.h"    /* nt_ui_tooltip_style_t */

#include "gui_canvas.h" /* gui_canvas (s_canvas) */

#ifdef __cplusplus
extern "C" {
#endif

/* --- global UI scale (definition; declared in gui_defs.h for S()/Su()) --- */
extern float g_ui_scale;

/* --- status line + severity (§2.8): leading icon + text tint. Info default; errors keep their tint
 * until the next status write (no timed behavior). --- */
typedef enum { STATUS_INFO, STATUS_SUCCESS, STATUS_WARNING, STATUS_ERROR } status_sev_t;
extern char s_status[256];
extern status_sev_t s_status_sev;
extern bool s_status_dismissed; /* the floating message pill was clicked away; cleared by the next set_status* */
/* DEV (--shot): wall-clock values in status text (pack ms) vary run-to-run and break the
 * byte-identical screenshot comparison used as a refactor gate; the shot seam sets this so
 * timed status messages print a fixed 0 instead. */
extern bool s_status_fixed_time;

#if defined(__GNUC__) || defined(__clang__)
#define GUI_PRINTF(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define GUI_PRINTF(fmt_idx, args_idx)
#endif
void set_status(const char *msg);
void set_status_ex(status_sev_t sev, const char *msg);
void set_statusf(const char *fmt, ...) GUI_PRINTF(1, 2);
void set_statusf_ex(status_sev_t sev, const char *fmt, ...) GUI_PRINTF(2, 3);

/* Pack-button state cached for the tooltip pass. Written each frame by the canvas strip
 * (gui_view_canvas's declare_canvas_strip); s_pack_stale is also read by gui_view_chrome's
 * declare_tooltips to word the Pack tooltip, so it can never be canvas-view-local -- same
 * class of shared UI state as the disclosure/modal flags above. */
extern bool s_pack_has_sources, s_pack_stale;

/* --- executable directory (resolved once at startup; selftest + pack-session paths hang off it) --- */
extern char s_exe_dir[TP_IDENTITY_PATH_MAX];

/* --- nt_ui context + the dual-mode center canvas --- */
extern nt_ui_context_t *s_ctx;
extern gui_canvas s_canvas;

/* --- font (bound lazily in try_bind_resources; read by the shared text-measure kit) --- */
extern nt_font_t s_font;
extern bool s_font_bound;

/* --- UI ids (resolved once in ensure_ids) --- */
extern uint32_t s_id_btn_pack, s_id_btn_export, s_id_btn_refresh, s_id_vlist, s_id_modal, s_id_about;
extern uint32_t s_id_canvas;      /* the atlas-page custom element (bbox drives zoom/pan/hit input) */
extern uint32_t s_id_rename;      /* the single inline rename input (one edit active at a time) */
extern uint32_t s_id_right_panel; /* settings-panel container (bbox: press-outside blurs focused inputs) */
extern uint32_t s_id_left_panel;  /* left panel container (bbox: overflow regression check) */
extern uint32_t s_id_strip;       /* canvas action strip (bbox: the overflow-prone middle-row term) */
extern uint32_t s_id_status_pill;  /* floating message pill over the canvas (replaces the status bar row) */
extern uint32_t s_id_right_content; /* right-panel scroll content (bbox: detect rows wider than the panel) */
extern uint32_t s_id_export_modal; /* the Export dialog */
extern uint32_t s_id_recovery;     /* R6b: the startup crash-recovery modal */
/* Menubar buttons + their drop-down panels. Same class as s_id_ctx_menu below (an id seeded once in
 * ensure_ids / the shell, read only by the declare machinery that moved to gui_view_chrome.c in step
 * 6b) -- moved here alongside it rather than left main.c-local, so it can never be view-local either. */
extern uint32_t s_id_mb_file, s_id_mb_edit, s_id_mb_view, s_id_mb_help;
extern uint32_t s_id_menu_file, s_id_menu_edit, s_id_menu_view, s_id_menu_help;

/* --- primary selection (s_sel_src/child stays the last-clicked selection for the region panel + canvas sync) --- */
extern int s_sel_atlas;      /* selected atlas index */
extern int s_sel_src;        /* selected source index within the atlas */
extern int s_sel_child;      /* selected folder-child index (-1 = the source row / a file) */
extern char s_sel_abs[TP_IDENTITY_PATH_MAX]; /* authoritative resolved image path */
extern bool s_sel_missing;   /* selection is a missing file -> canvas shows a placeholder (§3.7) */

/* Multi-select set over canonical leaf sprite identities (rows rebuild each frame). Drives
 * "Create animation from selection" + the editor's "Add frames" (ux.md §3.7b). Growable storage:
 * the old fixed 4096 cap silently ignored selections past it. Grows
 * geometrically in multi_sel_add_ref (gui_rows.c); see the growth-policy note there. */
typedef struct gui_selected_sprite {
    tp_id128 source_id;
    char *source_key; /* malloc-owned exact canonical key */
} gui_selected_sprite;
extern gui_selected_sprite *s_multi_sel;
extern int s_multi_sel_count;
extern int s_multi_sel_cap;  /* allocated slots in s_multi_sel (grow-only; 0 == unallocated) */
extern int s_sel_anchor_row; /* VIEW index anchor for Shift-range selection (s_view space) */
extern int s_focus_view;     /* keyboard-focused row (index into s_view; -1 none) — U-02 list focus model */
extern bool s_filter_active; /* Ctrl+F speed-search armed: typed chars edit the sprite-tree filter (U-02 T1) */

/* Animation selection + editor state (ux.md §3.7b). */
extern int s_sel_anim;       /* selected animation index in the current atlas, -1 none */
extern int s_sel_anim_frame; /* selected frame row in the editor (for the Del hotkey), -1 none */

/* --- export-target preview (packet EXP-PREVIEW): a view-only overlay that shows what a chosen exporter
 * would produce from the CURRENT settings, without touching the native session pack. `s_preview_target`
 * is the strip selector's choice: 0 = Native (session pack, default); k >= 1 selects registered exporter
 * (k-1) via tp_exporter_at. The actual clamped result lives in gui_pack's preview slot; these fields track
 * intent + when to drop it. Shared with the canvas view (selector + chip), actions (start/reset/bind),
 * the selftest and the shot seam, so they can never be view-local. --- */
extern int s_preview_target;       /* 0 = Native; else 1 + tp_exporter_at index */

/* Animation preview player (canvas ANIM mode). s_preview_time is the master clock; the frame index is
 * a pure function of it (gui_canvas_anim_frame_at), so play/pause/step all reduce to moving the clock. */
extern bool s_preview_active;
extern bool s_preview_playing;
extern bool s_preview_finished;
extern double s_preview_time;
extern int s_preview_cur;         /* resolved current frame index (0-based) this frame */
extern int s_preview_frame_count; /* resolved (missing-frame-skipped) frame count this frame */

/* Inline rename edit (F1): one active at a time. kind 0 none / 1 atlas / 2 sprite / 3 animation. */
enum { EDIT_NONE = 0, EDIT_ATLAS, EDIT_SPRITE, EDIT_ANIM };
extern int s_edit_kind;
extern int s_edit_atlas;        /* atlas being renamed (EDIT_ATLAS) */
extern int s_edit_anim;         /* animation index being renamed (EDIT_ANIM) */
extern char s_edit_sprite[TP_SRCKEY_MAX]; /* atlas-relative sprite name being renamed */
extern char s_edit_buf[TP_SRCKEY_MAX];    /* the input buffer */

/* Runtime (already SCALED) column widths. Clamped narrow when the window can't fit both side panels +
 * a minimal canvas, so the panels never get pushed off-screen (recomputed each frame). */
extern float s_content_w;     /* logical content width, for caption/status truncation */
extern float s_left_panel_w;
extern float s_right_panel_w;
extern float s_canvas_w;      /* logical width the canvas column actually gets (drives the strip's compact mode) */
extern float s_panel_label_w; /* settings-row label column; shrinks so the widget cell never hits 0 */

/* Right settings panel: session-remembered disclosure state (s_sec_*_open are shared with the
 * selftest, so they can never be view-local). */
extern bool s_sec_atlas_open, s_sec_region_open, s_sec_export_open;
extern bool s_atlas_adv_open; /* Basic/Advanced disclosure (region F) */
extern bool s_sec_anim_open;  /* the "Animation" section disclosure */

/* Modal open flags: the About box + the Export dialog. Shared with the selftest (it opens both so the
 * auto-quit frames render them, and closes them before the pixel probe), so they can never be
 * view-local. gui_view_chrome owns their declares. */
extern bool s_about_open;
extern bool s_export_open;

/* Right-click context menu: one cursor-anchored menu whose items depend on the row a right-click armed
 * it over (§3.3e mouse-complete access). This is the shared TRIGGER/PAYLOAD state written by three
 * different views (left panel / canvas / settings) and read by the DECLARE machinery (gui_view_chrome's
 * declare_context_menu) -- "menu/modal open flags", same class as
 * s_about_open/s_export_open, so it can never be view-local. s_id_ctx_menu is seeded once in
 * ensure_ids (shell); only the storage moved here. */
extern uint32_t s_id_ctx_menu;
extern nt_ui_menu_state_t s_ctx_state;
enum { CTX_NONE = 0, CTX_ATLAS, CTX_SPRITE, CTX_CANVAS, CTX_TARGET, CTX_ANIM };
extern int s_ctx_kind;
extern tp_id128 s_ctx_atlas_id;
extern int64_t s_ctx_atlas_revision;
extern tp_id128 s_ctx_anim_atlas_id;
extern tp_id128 s_ctx_anim_id;
extern int64_t s_ctx_anim_revision;
extern tp_id128 s_ctx_target_atlas_id;
extern tp_id128 s_ctx_target_id;
extern int64_t s_ctx_target_revision;
extern tp_id128 s_ctx_sprite_atlas_id;
extern tp_id128 s_ctx_sprite_source_id;
extern int64_t s_ctx_sprite_revision;
extern char s_ctx_sprite_source_key[TP_SRCKEY_MAX];
extern char s_ctx_sprite_display_name[TP_SRCKEY_MAX];
extern bool s_ctx_leaf;        /* a renamable leaf sprite (file source or folder child) */
extern bool s_ctx_removable;   /* a removable source row (has an [x] today) */

/* One frame: a press landed outside the panels -> declare the settings-panel numeric/text fields
 * disabled so the engine drops keyboard focus (the engine exposes no programmatic blur). Set by the
 * shell input pre-pass (frame()), read by view field widgets (gui_view_settings' ui_int/float/
 * text_field). Same family as the published pending flags -- a shell-set, view-read bit. */
extern bool s_blur_inputs;

/* Per-frame collected row tooltips: TRUNCATED-label full text AND icon-only remove-x "Remove" hints.
 * Bounded by visible (virtualized) rows + the right panel's target/frame lists, not project size. */
#define MAX_ROW_TIPS 192
typedef struct row_tip {
    uint32_t id;
    char full[224];
} row_tip;
extern row_tip s_row_tips[MAX_ROW_TIPS];
extern int s_row_tip_count;

/* --- the ~30 per-frame-scaled style objects (apply_ui_scale writes font/metric fields each frame;
 * ensure_ids seeds the non-scaled fields once). Read by the widget kit and every view. --- */
extern nt_ui_label_style_t g_title, g_section, g_body, g_row, g_row_strong, g_caption, g_canvas_hint, g_tag, g_warn, g_link, g_dim, g_danger; /* scaled each frame */
extern nt_ui_label_style_t g_onaccent, g_onwarn;                                                           /* colored-button content (scaled each frame) */
extern nt_ui_label_style_t g_check;                                                                        /* checkbox tick glyph (scaled each frame) */

extern nt_ui_button_style_t g_btn;
extern nt_ui_button_style_t g_btn_primary;
extern nt_ui_button_style_t g_btn_accent;
extern nt_ui_button_style_t g_btn_stale;
extern nt_ui_button_style_t g_btn_ghost;
extern nt_ui_button_style_t g_btn_link;
extern nt_ui_button_style_t g_menubtn;

extern nt_ui_menu_style_t s_menu_style;
extern nt_ui_modal_style_t s_modal_style;
extern nt_ui_tooltip_style_t s_tip_style;
extern nt_ui_input_style_t s_rename_input; /* inline rename field (atlas + sprite) */
extern nt_ui_dropdown_style_t s_dd_style;
extern nt_ui_slider_style_t s_slider_style;
extern nt_ui_input_style_t s_num_input;    /* numeric + short text fields */
extern nt_ui_scroll_style_t s_panel_scroll;

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_STATE_H */
