#ifndef NTPACKER_GUI_RECOVERY_INDICATOR_H
#define NTPACKER_GUI_RECOVERY_INDICATOR_H

#include "gui_project.h"

/* Pure presentation value shared by the chrome render path and its headless
 * contract test. It borrows the persistent project's notice for one frame. */
typedef struct gui_recovery_indicator {
    bool visible;
    const char *glyph;
    const char *tooltip;
} gui_recovery_indicator;

static inline gui_recovery_indicator
gui_recovery_indicator_present(bool active,
                               const gui_recovery_notice *notice) {
    const bool visible = active && notice && notice->message[0] != '\0';
    return (gui_recovery_indicator){
        .visible = visible,
        .glyph = "!",
        .tooltip = visible ? notice->message : NULL,
    };
}

#endif /* NTPACKER_GUI_RECOVERY_INDICATOR_H */
