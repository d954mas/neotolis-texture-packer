#include "tp_build_driver_internal.h"

#include <stdint.h>
#include <stdlib.h>

#include "nt_builder.h"

/* The desc override values (tp_pack.h) mirror the engine encoding 1:1; these
 * guard the tp_pack_sprite_desc -> nt_atlas_sprite_opts_t mapping below. */
_Static_assert(TP_PACK_SPRITE_SHAPE_RECT == NT_ATLAS_SPRITE_SHAPE_RECT, "sprite shape RECT encoding");
_Static_assert(TP_PACK_SPRITE_SHAPE_CONVEX == NT_ATLAS_SPRITE_SHAPE_CONVEX, "sprite shape CONVEX encoding");
_Static_assert(TP_PACK_SPRITE_SHAPE_CONCAVE == NT_ATLAS_SPRITE_SHAPE_CONCAVE, "sprite shape CONCAVE encoding");
_Static_assert(TP_PACK_SPRITE_ROTATE_NO == NT_ATLAS_TRANSFORMS_IDENTITY,
               "sprite no-transform encoding");
_Static_assert(TP_PACK_TRANSFORMS_IDENTITY == NT_ATLAS_TRANSFORMS_IDENTITY,
               "atlas identity-transform encoding");
_Static_assert(TP_PACK_TRANSFORMS_ALL == NT_ATLAS_TRANSFORMS_ALL,
               "atlas all-transforms encoding");
_Static_assert(TP_PACK_MAX_PAGES == NT_ATLAS_MAX_PAGES,
               "packer and engine atlas page limits must match");

static void free_loaded_images(tp_image_rgba8 *images, int count) {
    if (!images) {
        return;
    }
    for (int i = 0; i < count; i++) {
        tp_image_free(&images[i]);
    }
    free(images);
}

/* Drive nt_builder for one atlas into `out_path`. Threaded encode, sequential
 * assembly (nt_builder_set_threads_auto): determinism is covered by the engine's
 * own threaded tests plus our byte-identical determinism/oracle suites. */
tp_status tp_build_driver_run(const tp_pack_settings *s,
                              tp_image_rgba8 *loaded_images,
                              const char *out_path, tp_error *err) {
    NtBuilderContext *ctx = nt_builder_start_pack(out_path);
    if (!ctx) {
        free_loaded_images(loaded_images, s->sprite_count);
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "tp_pack: nt_builder_start_pack('%s') failed (bad work_dir?)", out_path);
    }
    nt_builder_set_threads_auto(ctx); /* parallel encode; assembly stays sequential */

    /* §5 export-friendly profile + caller knobs. */
    nt_atlas_opts_t o = nt_atlas_opts_defaults();
    o.premultiplied = false; /* straight alpha (R3: expected NT_LOG_WARN, non-fatal) */
    o.compress = NULL;       /* RAW RGBA8 -- reader requires it (§2.3) */
    o.gen_mipmaps = false;
    o.filter_min = NT_TEXTURE_DEFAULT_FILTER_LINEAR;
    o.filter_mag = NT_TEXTURE_DEFAULT_FILTER_LINEAR;
    o.format = NT_TEXTURE_FORMAT_RGBA8;
    o.debug_png = false;
    o.max_size = (uint32_t)s->max_size;
    o.padding = (uint32_t)s->padding;
    o.margin = (uint32_t)s->margin;
    o.extrude = (uint32_t)s->extrude;
    o.alpha_threshold = (uint8_t)s->alpha_threshold;
    o.max_vertices = (uint8_t)s->max_vertices;
    o.shape = (nt_atlas_shape_t)s->shape;
    o.allowed_transforms = s->allowed_transforms;
    o.power_of_two = s->power_of_two;
    o.pixels_per_unit = s->pixels_per_unit;

    NtAtlasBuild *atlas = nt_atlas_begin(ctx, s->atlas_name, &o);
    for (int i = 0; i < s->sprite_count; i++) {
        const tp_pack_sprite_desc *sp = &s->sprites[i];
        nt_atlas_sprite_opts_t so = nt_atlas_sprite_opts_defaults();
        so.name = sp->name;
        so.origin_x = sp->origin_x;
        so.origin_y = sp->origin_y;
        so.slice9_left = sp->slice9_lrtb[0];
        so.slice9_right = sp->slice9_lrtb[1];
        so.slice9_top = sp->slice9_lrtb[2];
        so.slice9_bottom = sp->slice9_lrtb[3];
        /* Per-sprite overrides: 0 stays "inherit atlas" (engine encoding); we only
         * write a field when its mask bit is set (validated in tp_pack). */
        if (sp->ov_mask & TP_PACK_OV_SHAPE) {
            so.shape = sp->ov_shape;
        }
        if (sp->ov_mask & TP_PACK_OV_ROTATE) {
            so.allowed_transforms = sp->ov_allow_rotate;
        }
        if (sp->ov_mask & TP_PACK_OV_MAXVERT) {
            so.max_vertices = sp->ov_max_vertices;
        }
        if (sp->ov_mask & TP_PACK_OV_MARGIN) {
            so.margin = sp->ov_margin;
        }
        if (sp->ov_mask & TP_PACK_OV_EXTRUDE) {
            so.extrude = sp->ov_extrude;
        }
        if (sp->path) {
            nt_atlas_add_raw(atlas, loaded_images[i].pixels,
                             (uint32_t)loaded_images[i].width,
                             (uint32_t)loaded_images[i].height, &so);
        } else {
            nt_atlas_add_raw(atlas, sp->rgba, (uint32_t)sp->w,
                             (uint32_t)sp->h, &so);
        }
    }
    const nt_build_result_t atlas_result = nt_atlas_commit(atlas);

    char atlas_error[256] = {0};
    if (atlas_result != NT_BUILD_OK) {
        uint32_t error_count = 0;
        const nt_build_error_t *errors = nt_builder_get_errors(ctx, &error_count);
        if (errors && error_count > 0U) {
            nt_build_error_format(&errors[0], atlas_error, sizeof atlas_error);
        }
    }

    /* nt_atlas_add_raw deep-copies every valid image before returning, so the
     * pack-job-owned decode buffers can be released before encode/assembly. */
    free_loaded_images(loaded_images, s->sprite_count);

    nt_build_result_t br = nt_builder_finish_pack(ctx);
    nt_builder_free_pack(ctx); /* always, per nt_builder.h lifecycle contract */
    if (atlas_result != NT_BUILD_OK) {
        if (atlas_error[0] != '\0') {
            return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                                "tp_pack: atlas '%s' failed: %s",
                                s->atlas_name, atlas_error);
        }
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "tp_pack: atlas '%s' commit failed (code %d)",
                            s->atlas_name, (int)atlas_result);
    }
    if (br != NT_BUILD_OK) {
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED, "tp_pack: nt_builder_finish_pack failed (code %d) for '%s'",
                            (int)br, s->atlas_name);
    }
    return TP_STATUS_OK;
}
