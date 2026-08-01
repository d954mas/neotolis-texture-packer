#include "tp_core/tp_export.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_project.h"
#include "tp_core/tp_arena.h"
#include "tp_core/tp_utf8.h"
#include "tp_session_internal.h"

/* nt_atlas_shape_t: 0 = RECT (see tp_pack.h shape). Kept as a literal so this
 * TU pulls in no builder header (tp_core stays builder-free, #282). */
#define TP_SHAPE_RECT 0

/* ======================================================================== */
/* capabilities                                                             */
/* ======================================================================== */

tp_export_caps tp_export_caps_full(void) {
    return (tp_export_caps){
        .transform_mask = TP_EXPORT_TRANSFORMS_ALL,
        .polygons = true,
        .pivot = true,
        .slice9 = true,
        .multipage = true,
        .aliases = true,
    };
}

bool tp_export_caps_supports_rotate90(const tp_export_caps *caps) {
    const uint8_t rotate90 = (uint8_t)(TP_TRANSFORM_DIAGONAL |
                                       TP_TRANSFORM_FLIP_H);
    return caps &&
           (caps->transform_mask & TP_EXPORT_TRANSFORM_BIT(rotate90)) != 0U;
}

bool tp_export_caps_supports_flips(const tp_export_caps *caps) {
    const uint8_t reflections = (uint8_t)(
        TP_EXPORT_TRANSFORM_BIT(TP_TRANSFORM_FLIP_H) |
        TP_EXPORT_TRANSFORM_BIT(TP_TRANSFORM_FLIP_V) |
        TP_EXPORT_TRANSFORM_BIT(TP_TRANSFORM_DIAGONAL) |
        TP_EXPORT_TRANSFORM_BIT(TP_TRANSFORM_FLIP_H |
                                TP_TRANSFORM_FLIP_V |
                                TP_TRANSFORM_DIAGONAL));
    return caps && (caps->transform_mask & reflections) != 0U;
}

/* ======================================================================== */
/* notices                                                                  */
/* ======================================================================== */

#ifdef TP_ENABLE_TEST_SEAMS
static _Thread_local bool s_fail_next_notice_reserve;

void tp_export_notices__test_fail_next_reserve(void) {
    s_fail_next_notice_reserve = true;
}
#endif

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
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_fail_next_notice_reserve) {
        s_fail_next_notice_reserve = false;
        return NULL;
    }
#endif
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
/* output-file enumeration (structured report)                              */
/* ======================================================================== */

tp_status tp_export_output_path(const char *out_path_base, const char *suffix,
                                char out[TP_IDENTITY_PATH_MAX], tp_error *err) {
    if (!out_path_base || !suffix || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "export output path requires base, suffix, and output");
    }
    if (!tp_utf8_is_valid_c_string(out_path_base) || !tp_utf8_is_valid_c_string(suffix)) {
        out[0] = '\0';
        return tp_error_set(err, TP_STATUS_INVALID_UTF8,
                            "export output path must be valid UTF-8");
    }
    const int n = snprintf(out, TP_IDENTITY_PATH_MAX, "%s%s", out_path_base, suffix);
    if (n < 0 || n >= TP_IDENTITY_PATH_MAX) {
        out[0] = '\0';
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "export output path exceeds the %d-byte canonical limit",
                            TP_IDENTITY_PATH_MAX - 1);
    }
    return TP_STATUS_OK;
}

tp_status tp_export_page_path(const char *out_path_base, int page,
                              char out[TP_IDENTITY_PATH_MAX], tp_error *err) {
    if (page < 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "export page index must be non-negative");
    }
    char suffix[32];
    const int n = snprintf(suffix, sizeof suffix, "-%d.png", page);
    if (n < 0 || (size_t)n >= sizeof suffix) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "export page suffix exceeds the supported limit");
    }
    return tp_export_output_path(out_path_base, suffix, out, err);
}

/* ======================================================================== */
/* capability -> pack-settings clamp                                        */
/* ======================================================================== */

tp_status tp_export_effective_settings(const tp_pack_settings *in, const tp_export_caps *caps, tp_pack_settings *out) {
    if (!in || !caps || !out) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (out != in) {
        *out = *in;
    }
    if ((in->allowed_transforms & TP_PACK_TRANSFORMS_IDENTITY) == 0U ||
        (caps->transform_mask & TP_EXPORT_TRANSFORMS_IDENTITY) == 0U) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    out->allowed_transforms =
        (uint8_t)(in->allowed_transforms & caps->transform_mask);
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
           a->max_vertices == b->max_vertices && a->shape == b->shape &&
           a->allowed_transforms == b->allowed_transforms &&
           a->power_of_two == b->power_of_two && a->pixels_per_unit == b->pixels_per_unit;
}

/* ======================================================================== */
/* exporter registry                                                        */
/* ======================================================================== */

static const tp_format_artifact_decl g_json_artifacts[] = {
    {.id = "metadata", .suffix = ".json"},
};

static const tp_format_descriptor g_json_neotolis_format = {
    .id = TP_EXPORTER_ID_JSON_NEOTOLIS,
    .display_name = "JSON (neotolis, full fidelity)",
    .caps = {.transform_mask = TP_EXPORT_TRANSFORMS_ALL,
             .polygons = true,
             .pivot = true,
             .slice9 = true,
             .multipage = true,
             .aliases = true},
    .artifacts = g_json_artifacts,
    .artifact_count = 1,
};

static const tp_exporter g_json_neotolis = {
    .format = &g_json_neotolis_format,
    .serialize = tp_export_json_neotolis_serialize,
};

/* Defold (extension-texturepacker .tpinfo). Caps = the FORMAT's real abilities
 * (docs/formats/defold-tpinfo.md): identity and clockwise 90-degree rotation,
 * trim, polygons, pivots (v2.0), multipage and aliases. Region-level flips and
 * 9-slice are not representable. The exact transform mask is intersected with
 * the requested pack mask before the engine is called. */
static const tp_format_artifact_decl g_defold_artifacts[] = {
    {.id = "tpinfo", .suffix = ".tpinfo"},
    {.id = "tpatlas", .suffix = ".tpatlas"},
};

static const tp_format_descriptor g_defold_format = {
    .id = "defold",
    .display_name = "Defold (.tpinfo + .tpatlas)",
    .caps = {.transform_mask = (uint8_t)(TP_EXPORT_TRANSFORMS_IDENTITY |
                                         TP_EXPORT_TRANSFORM_BIT(TP_TRANSFORM_DIAGONAL |
                                                                 TP_TRANSFORM_FLIP_H)),
             .polygons = true,
             .pivot = true,
             .slice9 = false,
             .multipage = true,
             .aliases = true},
    .artifacts = g_defold_artifacts,
    .artifact_count = 2,
};

static const tp_exporter g_defold = {
    .format = &g_defold_format,
    .serialize = tp_export_defold_serialize,
};

/* Built-in table: the current user-facing exporters. */
static const tp_exporter *const g_builtins[] = {&g_json_neotolis, &g_defold};
#define TP_BUILTIN_COUNT ((int)(sizeof g_builtins / sizeof g_builtins[0]))

#ifdef TP_ENABLE_TEST_SEAMS
/* Test-only injection. Production formats are the fixed built-in table; future
 * templates/Lua bind through their own package runtime, not this native table. */
#define TP_REGISTERED_MAX 32
static const tp_exporter *g_registered[TP_REGISTERED_MAX];
static int g_registered_count;
#endif

tp_status tp_exporter_id_validate(const char *id, tp_error *err) {
    if (!id || id[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "exporter id must be non-empty");
    }
    size_t length = 0U;
    while (length < TP_EXPORTER_ID_MAX && id[length] != '\0') {
        length++;
    }
    if (length == TP_EXPORTER_ID_MAX) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "exporter id exceeds the %d-byte canonical limit",
            TP_EXPORTER_ID_MAX - 1);
    }
    if (!tp_utf8_is_valid_c_string(id)) {
        return tp_error_set(err, TP_STATUS_INVALID_UTF8,
                            "exporter id must be valid UTF-8");
    }
    return TP_STATUS_OK;
}

const tp_exporter *tp_exporter_find(const char *id) {
    if (tp_exporter_id_validate(id, NULL) != TP_STATUS_OK) {
        return NULL;
    }
    for (int i = 0; i < TP_BUILTIN_COUNT; i++) {
        if (strcmp(g_builtins[i]->format->id, id) == 0) {
            return g_builtins[i];
        }
    }
#ifdef TP_ENABLE_TEST_SEAMS
    for (int i = 0; i < g_registered_count; i++) {
        if (strcmp(g_registered[i]->format->id, id) == 0) {
            return g_registered[i];
        }
    }
#endif
    return NULL;
}

tp_status tp_export_artifact_plan_build(const tp_exporter *exp,
                                        const tp_export_ir *ir,
                                        const char *out_path_base,
                                        tp_arena *arena,
                                        tp_export_artifact_plan *out,
                                        tp_error *err) {
    if (!exp || !exp->format || !exp->serialize || !ir || !out_path_base ||
        !arena || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "artifact plan requires exporter, IR, base, arena, and output");
    }
    tp_status st = tp_export_ir_validate(ir, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    const tp_format_descriptor *format = exp->format;
    if (tp_exporter_id_validate(format->id, err) != TP_STATUS_OK ||
        !format->display_name || format->artifact_count <= 0 ||
        !format->artifacts) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "format descriptor is incomplete");
    }
    if (strcmp(ir->target_id, format->id) != 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "Export IR target '%s' does not match format '%s'",
                            ir->target_id, format->id);
    }
    for (int i = 0; i < ir->sprite_count; ++i) {
        const uint8_t transform = ir->sprites[i].data.transform;
        if ((format->caps.transform_mask &
             TP_EXPORT_TRANSFORM_BIT(transform)) == 0U) {
            return tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "format '%s' cannot represent transform %u for sprite '%s'",
                format->id, (unsigned)transform,
                ir->sprites[i].final_name);
        }
    }
    const int total = format->artifact_count + ir->page_count;
    tp_export_artifact *items = (tp_export_artifact *)tp_arena_alloc(
        arena, (size_t)total * sizeof *items);
    if (!items) {
        return tp_error_set(err, TP_STATUS_OOM, "artifact plan: OOM");
    }
    memset(items, 0, (size_t)total * sizeof *items);
    for (int i = 0; i < format->artifact_count; ++i) {
        const tp_format_artifact_decl *decl = &format->artifacts[i];
        if (!decl->id || decl->id[0] == '\0' || !decl->suffix ||
            decl->suffix[0] != '.' || strchr(decl->suffix, '/') ||
            strchr(decl->suffix, '\\')) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "format '%s' has invalid artifact declaration %d",
                                format->id, i);
        }
        for (int j = 0; j < i; ++j) {
            if (strcmp(decl->id, format->artifacts[j].id) == 0 ||
                strcmp(decl->suffix, format->artifacts[j].suffix) == 0) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                    "format '%s' has duplicate artifact id or suffix",
                                    format->id);
            }
        }
        char path[TP_IDENTITY_PATH_MAX];
        st = tp_export_output_path(out_path_base, decl->suffix, path, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
        items[i] = (tp_export_artifact){
            .kind = TP_EXPORT_ARTIFACT_DOCUMENT,
            .logical_id = i,
            .id = decl->id,
            .path = tp_arena_strdup(arena, path),
        };
        if (!items[i].path) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "artifact plan: OOM copying document path");
        }
    }
    for (int p = 0; p < ir->page_count; ++p) {
        char path[TP_IDENTITY_PATH_MAX];
        st = tp_export_page_path(out_path_base, ir->pages[p].artifact_id, path, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
        char id[32];
        const int n = snprintf(id, sizeof id, "page-%d", ir->pages[p].artifact_id);
        if (n < 0 || (size_t)n >= sizeof id) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "artifact plan page id overflow");
        }
        const int index = format->artifact_count + p;
        items[index] = (tp_export_artifact){
            .kind = TP_EXPORT_ARTIFACT_PAGE,
            .logical_id = ir->pages[p].artifact_id,
            .id = tp_arena_strdup(arena, id),
            .path = tp_arena_strdup(arena, path),
        };
        if (!items[index].id || !items[index].path) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "artifact plan: OOM copying page artifact");
        }
    }
    out->out_path_base = tp_arena_strdup(arena, out_path_base);
    out->artifacts = items;
    out->artifact_count = total;
    out->document_count = format->artifact_count;
    if (!out->out_path_base) {
        return tp_error_set(err, TP_STATUS_OOM, "artifact plan: OOM copying base");
    }
    return TP_STATUS_OK;
}

int tp_exporter_count(void) {
#ifdef TP_ENABLE_TEST_SEAMS
    return TP_BUILTIN_COUNT + g_registered_count;
#else
    return TP_BUILTIN_COUNT;
#endif
}

const tp_exporter *tp_exporter_at(int index) {
    if (index < 0 || index >= tp_exporter_count()) {
        return NULL;
    }
    if (index < TP_BUILTIN_COUNT) {
        return g_builtins[index];
    }
#ifdef TP_ENABLE_TEST_SEAMS
    return g_registered[index - TP_BUILTIN_COUNT];
#else
    return NULL;
#endif
}

#ifdef TP_ENABLE_TEST_SEAMS
tp_status tp_exporter_register(const tp_exporter *e) {
    if (!e || !e->format || !e->format->id || !e->serialize) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    const tp_status id_status = tp_exporter_id_validate(e->format->id, NULL);
    if (id_status != TP_STATUS_OK) {
        return id_status;
    }
    if (tp_exporter_find(e->format->id)) {
        return TP_STATUS_INVALID_ARGUMENT; /* duplicate id */
    }
    if (g_registered_count >= TP_REGISTERED_MAX) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    g_registered[g_registered_count++] = e;
    return TP_STATUS_OK;
}
#endif

/* ======================================================================== */
/* degradation prediction                                                    */
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
                                 const char *target_id, const tp_export_ir *opt_ir, tp_export_notices *out,
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
     * exact enumeration shared by CLI and GUI. */
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

    if (native.allowed_transforms != eff.allowed_transforms) {
        PREDICT_ADD(TP_NOTICE_FIELD_TRANSFORM, NULL,
                    "unsupported orientations disabled -- this format accepts transform mask 0x%02x of requested 0x%02x",
                    (unsigned)eff.allowed_transforms, (unsigned)native.allowed_transforms);
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
    if (opt_ir) {
        if (!caps->multipage && opt_ir->page_count > 1) {
            PREDICT_ADD(TP_NOTICE_FIELD_MULTIPAGE, NULL, "atlas has %d pages but the target is single-page",
                        opt_ir->page_count);
        }
        if (!caps->aliases) {
            for (int i = 0; i < opt_ir->sprite_count; i++) {
                if (opt_ir->sprites[i].data.alias_of >= 0) {
                    PREDICT_ADD(TP_NOTICE_FIELD_ALIAS, opt_ir->sprites[i].final_name,
                                "alias link dropped for '%s' (target has no alias support)",
                                opt_ir->sprites[i].final_name);
                }
            }
        }
    }
#undef PREDICT_ADD
    return TP_STATUS_OK;
}

tp_status tp_export_predict_loss_snapshot(const tp_session_snapshot *snapshot,
                                          tp_id128 atlas_id,
                                          const tp_export_caps *caps,
                                          const char *target_id,
                                          const tp_export_ir *opt_ir,
                                          tp_export_notices *out,
                                          tp_error *err) {
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "export loss snapshot requires snapshot");
    }
    const tp_project *project = tp_session_snapshot_project_internal(snapshot);
    const int atlas_index = tp_project_find_atlas_by_id(project, atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "export loss atlas id was not found");
    }
    return tp_export_predict_loss(project, atlas_index, caps, target_id,
                                  opt_ir, out, err);
}
