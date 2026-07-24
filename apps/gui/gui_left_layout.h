#ifndef NTPACKER_GUI_LEFT_LAYOUT_H
#define NTPACKER_GUI_LEFT_LAYOUT_H

/* Pure left-panel layout contract shared by the Clay declaration, the
 * responsive cap calculation, and headless tests. Keep direct-child additions
 * here so the gap count and reserved sprite-list budget change together. */

#include <stdbool.h>

#define GUI_LEFT_ROW_H 27.0F
#define GUI_LEFT_SECTION_MAX_H 180.0F
#define GUI_LEFT_PANEL_PAD_Y 8.0F
#define GUI_LEFT_CHILD_GAP 6.0F
#define GUI_LEFT_ATLAS_HEADER_H 20.0F
#define GUI_LEFT_ADD_ATLAS_H 26.0F
#define GUI_LEFT_DIVIDER_H 1.0F
#define GUI_LEFT_SPRITE_HEADER_H 28.0F
#define GUI_LEFT_SORT_H 24.0F
#define GUI_LEFT_FILTER_H 24.0F
#define GUI_LEFT_ANIM_HEADER_H 28.0F
#define GUI_LEFT_SPRITE_MIN_ROWS 2
#define GUI_LEFT_DIRECT_CHILDREN 10

typedef struct gui_left_layout_budget {
    float padding;
    float fixed_chrome;
    float gaps;
    float sprite_min;
} gui_left_layout_budget;

static inline gui_left_layout_budget
gui_left_layout_budget_make(float ui_scale, bool filter_visible) {
    const int child_count =
        GUI_LEFT_DIRECT_CHILDREN + (filter_visible ? 1 : 0);
    const float fixed_chrome =
        GUI_LEFT_ATLAS_HEADER_H + GUI_LEFT_ADD_ATLAS_H +
        2.0F * GUI_LEFT_DIVIDER_H + GUI_LEFT_SPRITE_HEADER_H +
        GUI_LEFT_SORT_H + GUI_LEFT_ANIM_HEADER_H +
        (filter_visible ? GUI_LEFT_FILTER_H : 0.0F);
    return (gui_left_layout_budget){
        .padding = 2.0F * GUI_LEFT_PANEL_PAD_Y * ui_scale,
        .fixed_chrome = fixed_chrome * ui_scale,
        .gaps = (float)(child_count - 1) * GUI_LEFT_CHILD_GAP * ui_scale,
        .sprite_min =
            (float)GUI_LEFT_SPRITE_MIN_ROWS * GUI_LEFT_ROW_H * ui_scale,
    };
}

#endif /* NTPACKER_GUI_LEFT_LAYOUT_H */
