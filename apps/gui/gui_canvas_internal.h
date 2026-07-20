#ifndef NTPACKER_GUI_CANVAS_INTERNAL_H
#define NTPACKER_GUI_CANVAS_INTERNAL_H

#include "gui_canvas.h"

typedef struct canvas_vert {
    float pos[3];
    float col[4];
    float uv[2];
} canvas_vert;

bool gui_canvas_resource_handles_ready(const gui_canvas *canvas);

#endif /* NTPACKER_GUI_CANVAS_INTERNAL_H */
