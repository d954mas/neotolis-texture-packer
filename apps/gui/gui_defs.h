#ifndef NTPACKER_GUI_DEFS_H
#define NTPACKER_GUI_DEFS_H

/* Pure constants, macros and immutable style tokens for the ntpacker GUI -- include-anywhere.
 * No mutable state lives here: the one `extern float g_ui_scale` is only the DECLARATION that
 * S()/Su() read; its definition lives in gui_state.c. Split out of main.c (GUI decomposition
 * step 1) as a pure move -- no behavior change. gui_pack.h is the header-shape template. */

#include <stdint.h>

#include "clay.h"
#include "ui/nt_ui_label.h" /* nt_ui_label_style_t + Clay_Color (base label styles below) */

/* --- layout constants + render layers + global UI scale --- */
#define LAYER_IMG 1
#define LAYER_TEXT 2

/* Global UI scale (owner: "too small -- make it bigger"). Every layout metric and font size
 * flows through S()/Su(), so one knob resizes the whole chrome. Seeded from the system DPI at
 * startup (a high-DPI Windows display inflates the framebuffer to physical pixels, which makes
 * fixed 1:1 metrics physically tiny -- the scale compensates) and overridable from View > UI
 * Scale. BASE_* sizes are already bumped ~15-20% over the old shell for desktop-tool density. */
extern float g_ui_scale; /* defined in gui_state.c */
static inline float S(float px) { return px * g_ui_scale; }
static inline uint16_t Su(float px) { return (uint16_t)((px * g_ui_scale) + 0.5F); }

#define BASE_MENUBAR_H 32.0F
#define BASE_LEFT_PANEL_W 300.0F
#define BASE_RIGHT_PANEL_W 300.0F /* settings panel (regions F/G), fixed width, own scroll */
#define BASE_ROW_H 27.0F
#define PANEL_LABEL_W 116.0F /* settings-row label column (base; runtime-clamped in compute_panel_widths) */
/* Canvas-column budget (design px, pre-S()). MIN_CANVAS_W is reserved for the canvas so the strip never
 * forces the middle row wider than the window; it is >= the compact two-row strip's min-content. Below
 * STRIP_SINGLE_MIN_W the strip drops to the compact two-row layout instead of a single overflowing row. */
#define MIN_CANVAS_W 285.0F
#define MIN_PANEL_W 100.0F
/* Canvas-strip responsive stops, in design px of the canvas COLUMN width (compared as
 * s_canvas_w < S(stop): both sides scale by g_ui_scale, so the compare is in design px). Icons make
 * the buttons much narrower than the old text blobs, so the single-row floor drops well below the
 * old 545. Ladder (§4): >= LABELS Pack/Export show text; >= CHIP the stale chip shows too; below
 * SINGLE the strip falls to the overflow-safe two-row compact (icon-only).
 * CHIP must clear the FULL labeled strip min-content PLUS the preview selector (added by packet
 * EXP-PREVIEW: a fixed-width combo now always sits in the single-row strip once STRIP_PREVIEW_MIN_W is
 * met) PLUS the trailing chip PLUS the canvas card's L/R padding. Measured selector + labeled actions +
 * pages + zoom + chip min-content ~715 design px (max across the stale "outdated" chip and the bounded
 * degradation chip). The stale chip and the degradation chip share this stop and never coexist (the stale
 * chip is suppressed while a preview target is active), and the degradation chip's text is width-bounded
 * (truncate_to_width) so a multi-degradation project can't grow the row past it. Below CHIP the amber Pack
 * carries the stale signal (§4) and the chip is dropped, so a trailing chip can never push the row (a GROW
 * child can't shrink below min-content) past the canvas and shove the right panel off-screen. The selftest
 * phases 6/8 assert chip DROPPED at 1920x1080@1.5 and SHOWN at 2200x1080@1.5 (the wide stop the selector
 * pushed the "roomy" threshold up to). */
#define STRIP_SINGLE_MIN_W 440.0F
#define STRIP_LABELS_MIN_W 560.0F
/* Export-target preview selector (packet EXP-PREVIEW): the selector is a fixed-width combo added to the
 * single-row strip, so it raises that row's min-content. STRIP_SINGLE_MIN_W was calibrated WITHOUT it, so
 * a bare selector overflows the narrow single-row band just above that stop (measured: the labeled
 * single-row + selector min-content is ~560 design px). Gate the selector on its own higher stop -- below
 * it the selector folds away exactly like the compact strip drops controls, and the canvas binds the
 * native session pack (preview_target_result mirrors this stop). 620 clears the ~560 min-content with
 * margin and sits below STRIP_CHIP_MIN_W so the degradation chip only ever adds width where there is room. */
#define STRIP_PREVIEW_MIN_W 620.0F
#define STRIP_CHIP_MIN_W 760.0F

/* Cap on export targets shown per atlas (settings panel target rows + the Export dialog loop). */
#define GUI_MAX_TARGETS 16

/* Playback mode labels, order == the Defold-pinned enum (0 once_forward .. 6 none). Printed by the
 * animation editor (settings panel) and the canvas preview caption. */
static const char *const k_playback_names[7] = {"Once forward",  "Loop forward",  "Once backward", "Loop backward",
                                                "Once pingpong", "Loop pingpong", "None"};

/* Pack an sRGB triple into the engine's 0xAABBGGRR (opaque) -- clearer than hand-swizzling. */
#define RGBA8(r, g, b) ((uint32_t)0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

/* Base font px (scaled per frame into the g_* styles below). Redesign §2.3: widen the ladder so
 * hierarchy reads from size+color+case (one vector font). title 16 strong / section 13 dim UPPERCASE /
 * body+row 15 / caption 13 dim / hint 20 faint / tag 13. The old bunched 15-19 set gave no tiers. */
#define FS_TITLE 16.0F
#define FS_SECTION 13.0F
#define FS_BODY 15.0F
#define FS_ROW 15.0F
#define FS_CAPTION 13.0F
#define FS_HINT 20.0F
#define FS_TAG 13.0F

/* --- palette --- */
/* Redesign §2.1: widen the value gaps between surfaces so structure reads. bg darkest, canvas
 * darkest surface, panel mid; borders/hover/selection nudged for contrast. Section-header + input +
 * severity tokens (header/input/success/danger) are Packet B/C -- deferred so this packet does not
 * restyle headers, wells, or the status bar; only surfaces + selection + hover move here. */
static const Clay_Color C_BG = {17.0F, 18.0F, 23.0F, 255.0F};
static const Clay_Color C_PANEL = {28.0F, 31.0F, 40.0F, 255.0F};
static const Clay_Color C_CANVAS = {11.0F, 12.0F, 15.0F, 255.0F};
static const Clay_Color C_BORDER = {52.0F, 58.0F, 72.0F, 255.0F};
static const Clay_Color C_STATUS = {24.0F, 26.0F, 34.0F, 255.0F};     /* status-bar fill (Packet C owns severity) */
static const Clay_Color C_HEADER = {40.0F, 45.0F, 57.0F, 255.0F};     /* section-header fill: LIGHTER than panel so headers advance (§2.1) */
static const Clay_Color C_INPUT = {21.0F, 23.0F, 30.0F, 255.0F};      /* field well: recessed, darker than panel (§2.1) */
static const Clay_Color C_BORDER_STRONG = {86.0F, 132.0F, 204.0F, 255.0F}; /* focus ring / active input (§2.1) */
static const Clay_Color C_ACCENT = {64.0F, 140.0F, 214.0F, 255.0F};   /* primary accent: section left-rule (§2.1) */
/* Severity + stale hues (§2.1/§2.8/§2.9) as Clay_Colors: status-bar tint, the amber "outdated" canvas
 * tag fill, and the semi-transparent dim laid over a stale packed page. */
static const Clay_Color C_WARN = {228.0F, 158.0F, 92.0F, 255.0F};     /* amber: warning + stale */
static const Clay_Color C_SUCCESS = {104.0F, 186.0F, 124.0F, 255.0F}; /* green: pack/export success */
static const Clay_Color C_DANGER = {214.0F, 96.0F, 96.0F, 255.0F};    /* red: errors + destructive */
static const Clay_Color C_STALE_DIM = {0.0F, 0.0F, 0.0F, 31.0F};      /* ~12% black over a stale page (§2.9) */
static const Clay_Color C_SEL = {48.0F, 74.0F, 120.0F, 255.0F};       /* selected-row desaturated blue FILL */
static const Clay_Color C_HOVER = {46.0F, 52.0F, 66.0F, 255.0F};      /* row/btn hover */
static const Clay_Color C_TRANSPARENT = {0.0F, 0.0F, 0.0F, 0.0F};

/* Base label styles (font_size = base px); rescale_styles() copies these into the g_* below
 * with font_size *= g_ui_scale every frame, so scaled text stays crisp (Slug vector font). */
/* Colors are §2.1 text tiers (text-strong/text/text-dim/text-faint); sizes stay on the current
 * scale (the §2.3 type-scale re-assignment is Packet B). The old dim title (recessed) is retired --
 * titles now take text-strong. */
static const nt_ui_label_style_t g_title_base = {.font_id = 0, .font_size = FS_TITLE, .color = {230.0F, 234.0F, 242.0F, 255.0F}};
/* Section caption (§2.3/§2.4): 13px text-dim, UPPERCASE (caller feeds uppercase strings) + slight tracking.
 * The accent left-rule (not size/weight) is the "you are in a section" signal, so this stays quiet. */
static const nt_ui_label_style_t g_section_base = {.font_id = 0, .font_size = FS_SECTION, .color = {140.0F, 148.0F, 164.0F, 255.0F}, .letter_tracking = 1};
static const nt_ui_label_style_t g_body_base = {.font_id = 0, .font_size = FS_BODY, .color = {196.0F, 204.0F, 216.0F, 255.0F}};
static const nt_ui_label_style_t g_row_base = {.font_id = 0, .font_size = FS_ROW, .color = {196.0F, 204.0F, 216.0F, 255.0F}};
/* Selected-row label: text-strong (the selection fill carries the highlight; the label brightens too). */
static const nt_ui_label_style_t g_row_strong_base = {.font_id = 0, .font_size = FS_ROW, .color = {230.0F, 234.0F, 242.0F, 255.0F}};
/* Destructive affordance (remove-x hover tint): danger red (§2.1). */
static const nt_ui_label_style_t g_danger_base = {.font_id = 0, .font_size = FS_CAPTION, .color = {214.0F, 96.0F, 96.0F, 255.0F}};
static const nt_ui_label_style_t g_caption_base = {.font_id = 0, .font_size = FS_CAPTION, .color = {140.0F, 148.0F, 164.0F, 255.0F}};
static const nt_ui_label_style_t g_canvas_hint_base = {.font_id = 0, .font_size = FS_HINT, .color = {98.0F, 104.0F, 120.0F, 255.0F}, .align = CLAY_TEXT_ALIGN_CENTER};
static const nt_ui_label_style_t g_tag_base = {.font_id = 0, .font_size = FS_TAG, .color = {245.0F, 244.0F, 243.0F, 255.0F}};
/* Missing-file rows / placeholder (ux.md §3.7): amber warning accent (§2.1 warn). */
static const nt_ui_label_style_t g_warn_base = {.font_id = 0, .font_size = FS_ROW, .color = {228.0F, 158.0F, 92.0F, 255.0F}};
/* Hyperlink label (About repo link): link-blue so it reads as clickable; hover tint on the button
 * behind it is the extra affordance (no cursor-shape API). */
static const nt_ui_label_style_t g_link_base = {.font_id = 0, .font_size = FS_CAPTION, .color = {110.0F, 170.0F, 245.0F, 255.0F}};
/* Dimmed caption for stale stats (they describe the LAST pack, not current settings): text-faint. */
static const nt_ui_label_style_t g_dim_base = {.font_id = 0, .font_size = FS_CAPTION, .color = {98.0F, 104.0F, 120.0F, 255.0F}};
/* Content tiers for the colored button fills: bright on the blue accent (primary), dark on the amber
 * warn (stale). Icon tint on those buttons derives from these (ui_icon_btn packs the label color). */
static const nt_ui_label_style_t g_onaccent_base = {.font_id = 0, .font_size = FS_BODY, .color = {234.0F, 240.0F, 248.0F, 255.0F}};
static const nt_ui_label_style_t g_onwarn_base = {.font_id = 0, .font_size = FS_BODY, .color = {40.0F, 28.0F, 12.0F, 255.0F}};

#endif /* NTPACKER_GUI_DEFS_H */
