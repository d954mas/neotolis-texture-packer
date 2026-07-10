/* tp_pack_settings_defaults lives in tp_core (not tp_build/tp_pack.c) because the
 * project model -- tp_project_atlas_set_defaults, the sparse-save comparison, and
 * tp_project_atlas_to_settings -- needs the canonical packing defaults, and the GUI
 * links tp_core WITHOUT the builder (engine issue #282). It stays the single source
 * of truth by deriving from nt_atlas_opts_defaults(), a header-only inline: including
 * nt_builder.h pulls only headers (no link, no nt_basisu_encoder, no static-CRT pin),
 * so tp_core carries zero builder code into the render runtime. */

#include "tp_core/tp_pack.h"

#include <string.h>

#include "nt_builder.h"

tp_status tp_pack_settings_defaults(tp_pack_settings *out) {
    if (!out) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    nt_atlas_opts_t d = nt_atlas_opts_defaults();
    memset(out, 0, sizeof *out);
    out->max_size = (int)d.max_size;
    out->padding = (int)d.padding;
    out->margin = (int)d.margin;
    out->extrude = (int)d.extrude;
    out->alpha_threshold = (int)d.alpha_threshold;
    out->max_vertices = (int)d.max_vertices;
    out->shape = (int)d.shape;
    out->allow_transform = d.allow_transform;
    out->power_of_two = d.power_of_two;
    out->pixels_per_unit = d.pixels_per_unit;
    return TP_STATUS_OK;
}
