#include "tp_core/tp_export.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_project.h"

/* nt_atlas_shape_t: 0 = RECT (see tp_pack.h shape). Kept as a literal so this
 * TU pulls in no builder header (tp_core stays builder-free, #282). */
#define TP_SHAPE_RECT 0

/* ======================================================================== */
/* capabilities                                                             */
/* ======================================================================== */

tp_export_caps tp_export_caps_full(void) {
    return (tp_export_caps){
        .rotate90 = true,
        .flips = true,
        .polygons = true,
        .pivot = true,
        .slice9 = true,
        .multipage = true,
        .aliases = true,
    };
}

/* ======================================================================== */
/* notices                                                                  */
/* ======================================================================== */

void tp_export_notices_init(tp_export_notices *n) {
    if (n) {
        n->items = NULL;
        n->count = 0;
        n->cap = 0;
    }
}

/* Grows the list if needed and returns the next zeroed slot (NOT yet counted),
 * or NULL on OOM. realloc does NOT zero -- memset here so the structured fields
 * (sprite/target/field_id/reason_id) default cleanly for the prose adder. */
static tp_export_notice *notice_reserve(tp_export_notices *n) {
    if (n->count + 1 > n->cap) {
        int new_cap = (n->cap == 0) ? 8 : n->cap * 2;
        tp_export_notice *items = (tp_export_notice *)realloc(n->items, (size_t)new_cap * sizeof(tp_export_notice));
        if (!items) {
            return NULL;
        }
        n->items = items;
        n->cap = new_cap;
    }
    tp_export_notice *slot = &n->items[n->count];
    memset(slot, 0, sizeof *slot);
    return slot;
}

tp_status tp_export_notice_addf(tp_export_notices *n, const char *fmt, ...) {
    if (!n) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_export_notice *slot = notice_reserve(n);
    if (!slot) {
        return TP_STATUS_OOM;
    }
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(slot->msg, sizeof slot->msg, fmt, args);
    va_end(args);
    n->count++;
    return TP_STATUS_OK;
}

tp_status tp_export_notice_add_ex(tp_export_notices *n, int field_id, int reason_id, const char *sprite,
                                  const char *target, const char *fmt, ...) {
    if (!n) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_export_notice *slot = notice_reserve(n);
    if (!slot) {
        return TP_STATUS_OOM;
    }
    slot->field_id = field_id;
    slot->reason_id = reason_id;
    slot->sprite = sprite;
    slot->target = target;
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(slot->msg, sizeof slot->msg, fmt, args);
    va_end(args);
    n->count++;
    return TP_STATUS_OK;
}

void tp_export_notices_free(tp_export_notices *n) {
    if (n) {
        free(n->items);
        n->items = NULL;
        n->count = 0;
        n->cap = 0;
    }
}

/* ======================================================================== */
/* capability -> pack-settings clamp (SUMMARY.md §5h)                        */
/* ======================================================================== */

tp_status tp_export_effective_settings(const tp_pack_settings *in, const tp_export_caps *caps, tp_pack_settings *out) {
    if (!in || !caps || !out) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (out != in) {
        *out = *in;
    }
    /* v1: nt_builder can only express full-D4 (all 8) or identity. Keep
     * transforms ON only when the FORMAT can hold the full D4 the builder would
     * bake -- i.e. rotate90 AND flips. Anything less packs identity-only. */
    out->allow_transform = in->allow_transform && caps->rotate90 && caps->flips;
    /* A format that cannot store polygons should pack rectangles, not tight
     * hulls it would only flatten to their AABB. */
    if (!caps->polygons) {
        out->shape = TP_SHAPE_RECT;
        out->extrude = in->extrude; /* extrude is valid for RECT; leave as-is */
    }
    return TP_STATUS_OK;
}

bool tp_export_settings_equal(const tp_pack_settings *a, const tp_pack_settings *b) {
    if (!a || !b) {
        return a == b;
    }
    /* Compare every knob that changes the pack. atlas_name/work_dir/sprites are
     * identical across an atlas's targets (same source), so pointer compare is
     * sufficient and cheap for those borrowed fields. */
    return a->sprites == b->sprites && a->sprite_count == b->sprite_count && a->atlas_name == b->atlas_name &&
           a->work_dir == b->work_dir && a->max_size == b->max_size && a->padding == b->padding &&
           a->margin == b->margin && a->extrude == b->extrude && a->alpha_threshold == b->alpha_threshold &&
           a->max_vertices == b->max_vertices && a->shape == b->shape && a->allow_transform == b->allow_transform &&
           a->power_of_two == b->power_of_two && a->pixels_per_unit == b->pixels_per_unit;
}

/* ======================================================================== */
/* exporter registry                                                        */
/* ======================================================================== */

static const tp_exporter g_json_neotolis = {
    .id = TP_EXPORTER_ID_JSON_NEOTOLIS,
    .display_name = "JSON (neotolis, full fidelity)",
    .extension = "json",
    .caps = {.rotate90 = true,
             .flips = true,
             .polygons = true,
             .pivot = true,
             .slice9 = true,
             .multipage = true,
             .aliases = true},
    .write = tp_export_json_neotolis_write,
};

/* Defold (extension-texturepacker .tpinfo). Caps = the FORMAT's real abilities
 * (docs/research/defold.md): 90-degree rotation, trim, polygons, pivots (v2.0),
 * multipage and aliases YES; region-level flips NO (flips are per-animation only)
 * and 9-slice NO (dropped with a notice). rotate90 && !flips means the per-target
 * clamp packs Defold identity-only in v1 (the anticipated "rotate90-only target"
 * in tp_export_effective_settings) -- correct output, rotation density deferred to
 * the future engine transform-policy PR. */
static const tp_exporter g_defold = {
    .id = "defold",
    .display_name = "Defold (.tpinfo + .tpatlas)",
    .extension = "tpinfo",
    .caps = {.rotate90 = true,
             .flips = false,
             .polygons = true,
             .pivot = true,
             .slice9 = false,
             .multipage = true,
             .aliases = true},
    .write = tp_export_defold_write,
};

/* Built-in table: the v1 user-facing exporters (SUMMARY.md §6 Q5). */
static const tp_exporter *const g_builtins[] = {&g_json_neotolis, &g_defold};
#define TP_BUILTIN_COUNT ((int)(sizeof g_builtins / sizeof g_builtins[0]))

/* Runtime overflow table (Phase 7 templates; tests inject descriptors). */
#define TP_REGISTERED_MAX 32
static const tp_exporter *g_registered[TP_REGISTERED_MAX];
static int g_registered_count;

const tp_exporter *tp_exporter_find(const char *id) {
    if (!id) {
        return NULL;
    }
    for (int i = 0; i < TP_BUILTIN_COUNT; i++) {
        if (strcmp(g_builtins[i]->id, id) == 0) {
            return g_builtins[i];
        }
    }
    for (int i = 0; i < g_registered_count; i++) {
        if (strcmp(g_registered[i]->id, id) == 0) {
            return g_registered[i];
        }
    }
    return NULL;
}

int tp_exporter_count(void) { return TP_BUILTIN_COUNT + g_registered_count; }

const tp_exporter *tp_exporter_at(int index) {
    if (index < 0 || index >= tp_exporter_count()) {
        return NULL;
    }
    if (index < TP_BUILTIN_COUNT) {
        return g_builtins[index];
    }
    return g_registered[index - TP_BUILTIN_COUNT];
}

tp_status tp_exporter_register(const tp_exporter *e) {
    if (!e || !e->id || !e->write) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (tp_exporter_find(e->id)) {
        return TP_STATUS_INVALID_ARGUMENT; /* duplicate id */
    }
    if (g_registered_count >= TP_REGISTERED_MAX) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    g_registered[g_registered_count++] = e;
    return TP_STATUS_OK;
}

/* ======================================================================== */
/* degradation prediction (review §3.4)                                     */
/* ======================================================================== */

/* True if any sprite override in the atlas carries a non-zero 9-slice border. */
static bool atlas_uses_slice9(const tp_project_atlas *a) {
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *s = &a->sprites[i];
        if (s->slice9_lrtb[0] || s->slice9_lrtb[1] || s->slice9_lrtb[2] || s->slice9_lrtb[3]) {
            return true;
        }
    }
    return false;
}

/* True if any sprite override carries a non-default pivot. */
static bool atlas_uses_pivot(const tp_project_atlas *a) {
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *s = &a->sprites[i];
        if (s->origin_x != TP_PROJECT_ORIGIN_DEFAULT || s->origin_y != TP_PROJECT_ORIGIN_DEFAULT) {
            return true;
        }
    }
    return false;
}

tp_status tp_export_predict_loss(const struct tp_project *project, int atlas_index, const tp_export_caps *caps,
                                 const char *target_id, const tp_export_prepared *opt_prep, tp_export_notices *out,
                                 tp_error *err) {
    if (!project || !caps || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_predict_loss: NULL project/caps/out");
    }
    if (atlas_index < 0 || atlas_index >= project->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_export_predict_loss: atlas index %d out of range",
                            atlas_index);
    }
    const tp_project_atlas *a = &project->atlases[atlas_index];

    /* Project-knowable axes: native vs capability-clamped pack settings -- the
     * exact enumeration the GUI chip used to own (review §3.1). */
    tp_pack_settings native;
    tp_status st = tp_project_atlas_to_settings(project, atlas_index, &native, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    tp_pack_settings eff;
    st = tp_export_effective_settings(&native, caps, &eff);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_export_predict_loss: effective-settings failed");
    }

#define PREDICT_ADD(field, sprite_name, ...)                                                                       \
    do {                                                                                                           \
        if (tp_export_notice_add_ex(out, (field), TP_NOTICE_REASON_CAPS_UNSUPPORTED, (sprite_name), target_id,    \
                                    __VA_ARGS__) != TP_STATUS_OK) {                                               \
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_predict_loss: OOM appending notice");              \
        }                                                                                                          \
    } while (0)

    if (native.allow_transform && !eff.allow_transform) {
        PREDICT_ADD(TP_NOTICE_FIELD_TRANSFORM, NULL,
                    "rotations/flips off -- this format cannot encode the full D4 orientation set");
    }
    if (native.shape != eff.shape) {
        PREDICT_ADD(TP_NOTICE_FIELD_POLYGON, NULL,
                    "polygon hulls flattened to rectangles -- this format stores quads only");
    }
    if (!caps->slice9 && atlas_uses_slice9(a)) {
        PREDICT_ADD(TP_NOTICE_FIELD_SLICE9, NULL, "9-slice borders dropped -- this format does not store them");
    }
    if (!caps->pivot && atlas_uses_pivot(a)) {
        PREDICT_ADD(TP_NOTICE_FIELD_PIVOT, NULL, "per-sprite pivots dropped -- this format does not store them");
    }

    /* Pack-dependent axes exist only once packed -- the CLI dry-run supplies the
     * prep; the GUI chip passes NULL (project-only preview). */
    if (opt_prep) {
        const tp_result *r = opt_prep->result;
        if (!caps->multipage && r && r->page_count > 1) {
            PREDICT_ADD(TP_NOTICE_FIELD_MULTIPAGE, NULL, "atlas has %d pages but the target is single-page",
                        r->page_count);
        }
        if (!caps->aliases) {
            for (int i = 0; i < opt_prep->sprite_count; i++) {
                if (opt_prep->sprites[i].alias_of >= 0) {
                    PREDICT_ADD(TP_NOTICE_FIELD_ALIAS, opt_prep->sprites[i].final_name,
                                "alias link dropped for '%s' (target has no alias support)",
                                opt_prep->sprites[i].final_name);
                }
            }
        }
    }
#undef PREDICT_ADD
    return TP_STATUS_OK;
}
