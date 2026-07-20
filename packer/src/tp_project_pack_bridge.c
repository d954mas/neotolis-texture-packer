#include "tp_core/tp_project.h"

#include "tp_core/tp_pack.h"

int tp_project_sprite_effective_shape(int atlas_shape, bool has_slice9, int ov_shape) {
    if (has_slice9) {
        return 0; /* RECT: the engine auto-forces it for slice9 sprites */
    }
    if (ov_shape != TP_PROJECT_OV_INHERIT) {
        return ov_shape;
    }
    return atlas_shape;
}

tp_status tp_project_atlas_to_settings(const tp_project *p, int atlas_index, struct tp_pack_settings *out,
                                       tp_error *err) {
    if (!p || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_atlas_to_settings: NULL project or out");
    }
    if (atlas_index < 0 || atlas_index >= p->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_atlas_to_settings: atlas index %d out of range",
                            atlas_index);
    }
    const tp_project_atlas *a = &p->atlases[atlas_index];
    tp_pack_settings_defaults(out); /* clears sprites/work_dir; the call site fills them */
    out->atlas_name = a->name;
    out->max_size = a->max_size;
    out->padding = a->padding;
    out->margin = a->margin;
    out->extrude = a->extrude;
    out->alpha_threshold = a->alpha_threshold;
    out->max_vertices = a->max_vertices;
    out->shape = a->shape;
    out->allow_transform = a->allow_transform;
    out->power_of_two = a->power_of_two;
    out->pixels_per_unit = a->pixels_per_unit;
    /* Non-RECT shapes cannot extrude (engine + tp_pack invariant). Clamp here so
     * pack, preview, AND export (tp_export_run) all see settings tp_pack accepts;
     * a saved CONCAVE+extrude project no longer hard-rejects on the export path. */
    if (out->shape != 0 /* RECT */) {
        out->extrude = 0;
    }
    return TP_STATUS_OK;
}
