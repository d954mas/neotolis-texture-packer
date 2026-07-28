#ifndef TP_CORE_SRC_TP_EXPORT_GEOM_H
#define TP_CORE_SRC_TP_EXPORT_GEOM_H

/* Shared hull-shape predicate for the format writers. Header-only static inline
 * (like tp_core/tp_sb.h) so every exporter answers "is this hull just the trim
 * rect?" with ONE definition -- the json-neotolis and Defold writers used to
 * carry byte-identical private copies, which is exactly how the two formats would
 * drift on the polygon/rect decision. A private packer/src header, not public API. */

#include <stdbool.h>

#include "tp_core/tp_model.h"

/* True when the sprite's hull is exactly the axis-aligned trim quad (a plain
 * RECT) -- then the canonical frame/source rect already describes it and no
 * polygon mesh is emitted. Verts are trim-local (0..frame.w/h), each of the four
 * corners hit exactly once, in any winding. */
static inline bool tp_export_is_rect_quad(const tp_sprite *s) {
    if (s->vert_count != 4 || !s->verts) {
        return false;
    }
    int w = s->frame.w;
    int h = s->frame.h;
    bool seen[4] = {false, false, false, false};
    for (int i = 0; i < 4; i++) {
        int x = s->verts[i].x;
        int y = s->verts[i].y;
        int ci;
        if (x == 0 && y == 0) {
            ci = 0;
        } else if (x == w && y == 0) {
            ci = 1;
        } else if (x == 0 && y == h) {
            ci = 2;
        } else if (x == w && y == h) {
            ci = 3;
        } else {
            return false;
        }
        if (seen[ci]) {
            return false;
        }
        seen[ci] = true;
    }
    return true;
}

#endif /* TP_CORE_SRC_TP_EXPORT_GEOM_H */
