#include "tp_core/tp_export.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

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

tp_status tp_export_notice_addf(tp_export_notices *n, const char *fmt, ...) {
    if (!n) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (n->count + 1 > n->cap) {
        int new_cap = (n->cap == 0) ? 8 : n->cap * 2;
        tp_export_notice *items = (tp_export_notice *)realloc(n->items, (size_t)new_cap * sizeof(tp_export_notice));
        if (!items) {
            return TP_STATUS_OOM;
        }
        n->items = items;
        n->cap = new_cap;
    }
    tp_export_notice *slot = &n->items[n->count];
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
    .id = "json-neotolis",
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

/* Built-in table: the v1 user-facing exporter (SUMMARY.md §6 Q5). */
static const tp_exporter *const g_builtins[] = {&g_json_neotolis};
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
