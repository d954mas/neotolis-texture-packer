#include "tp_core/tp_export.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_names.h"

/* ======================================================================== */
/* options + final-name munging                                             */
/* ======================================================================== */

void tp_export_ir_opts_defaults(tp_export_ir_opts *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->strip_extension = true;
    out->strip_folders = false;
    out->scale = 1.0F;
}

static const char *override_for(const tp_export_ir_opts *o, const char *raw) {
    for (int i = 0; i < o->override_count; i++) {
        if (o->overrides[i].raw_name && strcmp(o->overrides[i].raw_name, raw) == 0) {
            return o->overrides[i].final_name;
        }
    }
    return NULL;
}

/* Final export name for one sprite: an override is the final name VERBATIM
 * (owner requirement); otherwise apply folder-strip then ext-strip on the
 * basename only. Returns an arena-owned string, or NULL on OOM. */
static char *final_name(const char *raw, const tp_export_ir_opts *o, tp_arena *arena) {
    const char *ov = override_for(o, raw);
    if (ov) {
        return tp_arena_strdup(arena, ov);
    }
    const char *start = raw;
    if (o->strip_folders) {
        const char *slash = strrchr(raw, '/');
        if (slash) {
            start = slash + 1;
        }
    }
    size_t cap = strlen(start) + 1;
    char *out = (char *)tp_arena_alloc(arena, cap);
    if (!out) {
        return NULL;
    }
    if (o->strip_extension) {
        tp_sprite_export_key(start, out, cap); /* shared name policy (single source) */
    } else {
        memcpy(out, start, cap);
    }
    return out;
}

/* ======================================================================== */
/* sort + alias remap                                                       */
/* ======================================================================== */

typedef struct tp_ir_sprite_build {
    tp_export_sprite sprite;
    int source_index;
} tp_ir_sprite_build;

static int cmp_sprite_final(const void *a, const void *b) {
    const tp_ir_sprite_build *sa = (const tp_ir_sprite_build *)a;
    const tp_ir_sprite_build *sb = (const tp_ir_sprite_build *)b;
    return strcmp(sa->sprite.final_name, sb->sprite.final_name);
}

static tp_status validate_build_inputs(const tp_result *result,
                                       const tp_export_ir_opts *opts,
                                       tp_error *err) {
    if (!result->atlas_name || result->atlas_name[0] == '\0' ||
        !isfinite(result->pixels_per_unit) || result->pixels_per_unit <= 0.0F) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_build: invalid atlas identity or scale");
    }
    if (result->page_count <= 0 || result->page_count > TP_PACK_MAX_PAGES ||
        !result->pages) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_build: invalid source page table");
    }
    for (int p = 0; p < result->page_count; ++p) {
        if (!tp_pack_max_size_valid(result->pages[p].w) ||
            !tp_pack_max_size_valid(result->pages[p].h) ||
            !result->pages[p].rgba) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_build: invalid source page %d", p);
        }
    }
    if (result->sprite_count < 0 ||
        (unsigned)result->sprite_count > UINT16_MAX ||
        (result->sprite_count > 0 && !result->sprites)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_build: invalid source sprite table");
    }
    for (int i = 0; i < result->sprite_count; ++i) {
        const tp_sprite *sprite = &result->sprites[i];
        if (!sprite->name || sprite->name[0] == '\0' ||
            sprite->vert_count < 0 ||
            sprite->vert_count > TP_PACK_MAX_VERTICES ||
            (sprite->vert_count > 0 && !sprite->verts) ||
            sprite->index_count < 0 ||
            (sprite->index_count > 0 && !sprite->indices)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_build: invalid source sprite %d", i);
        }
        if (sprite->alias_of < -1 ||
            sprite->alias_of >= result->sprite_count ||
            sprite->alias_of == i ||
            (sprite->alias_of >= 0 &&
             result->sprites[sprite->alias_of].alias_of != -1)) {
            return tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "tp_export_ir_build: invalid source alias for sprite %d", i);
        }
    }
    if ((unsigned)opts->override_count > UINT16_MAX ||
        (unsigned)opts->animation_count > UINT16_MAX ||
        (unsigned)opts->sprite_ref_count > UINT16_MAX) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_export_ir_build: options table count exceeds %u",
                            (unsigned)UINT16_MAX);
    }
    if (opts->override_count < 0 ||
        (opts->override_count > 0 && !opts->overrides) ||
        opts->animation_count < 0 ||
        (opts->animation_count > 0 && !opts->animations) ||
        opts->sprite_ref_count < 0 ||
        (opts->sprite_ref_count > 0 && !opts->sprite_refs)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_build: invalid options table");
    }
    for (int i = 0; i < opts->override_count; ++i) {
        if (!opts->overrides[i].raw_name ||
            !opts->overrides[i].final_name ||
            opts->overrides[i].final_name[0] == '\0') {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_build: invalid rename override %d", i);
        }
    }
    for (int i = 0; i < opts->animation_count; ++i) {
        const tp_export_anim_in *animation = &opts->animations[i];
        if ((unsigned)animation->frame_count > UINT16_MAX) {
            return tp_error_set(
                err, TP_STATUS_OUT_OF_BOUNDS,
                "tp_export_ir_build: animation %d frame count exceeds %u", i,
                (unsigned)UINT16_MAX);
        }
        if (!animation->id || animation->id[0] == '\0' ||
            animation->frame_count < 0 ||
            (animation->frame_count > 0 && !animation->frames)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_build: invalid animation %d", i);
        }
    }
    return TP_STATUS_OK;
}

static bool packed_region_is_solid(const tp_result *result,
                                   const tp_sprite *sprite) {
    if (sprite->page < 0 || sprite->page >= result->page_count) {
        return false;
    }
    const tp_page *page = &result->pages[sprite->page];
    const int fw = (sprite->transform & 4U) ? sprite->frame.h : sprite->frame.w;
    const int fh = (sprite->transform & 4U) ? sprite->frame.w : sprite->frame.h;
    if (!page->rgba || sprite->frame.x < 0 || sprite->frame.y < 0 || fw <= 0 ||
        fh <= 0 || sprite->frame.x > page->w - fw ||
        sprite->frame.y > page->h - fh) {
        return false;
    }
    for (int y = sprite->frame.y; y < sprite->frame.y + fh; ++y) {
        for (int x = sprite->frame.x; x < sprite->frame.x + fw; ++x) {
            size_t offset = (((size_t)y * (size_t)page->w) + (size_t)x) * 4U;
            if (page->rgba[offset + 3U] != 255U) {
                return false;
            }
        }
    }
    return true;
}

/* ======================================================================== */
/* animation assembly (EXPLICIT project animations only)                    */
/* ======================================================================== */

/* Product contract (docs/formats/json-neotolis.md): animations are assembled
 * EXPLICITLY from the project -- there is NO numeric-suffix auto-grouping. bob
 * still auto-promotes every atlas sprite to a 1-frame animation on the engine
 * side; that is independent of this list. */

static int cmp_anim_id(const void *a, const void *b) {
    const tp_export_anim *aa = (const tp_export_anim *)a;
    const tp_export_anim *ab = (const tp_export_anim *)b;
    return strcmp(aa->id, ab->id);
}

/* Copies the explicit animations from opts into arena-owned prepared anims,
 * sorted ascending by id (determinism). Frames are stored in override-KEY space
 * (ext-stripped, folder-kept -- the GUI-row identity); each is resolved to its
 * FINAL export name through the packed sprite set, so a rename flows into the
 * frames automatically (the prepared sprite's final_name already reflects it).
 * A frame that matches no packed sprite is a dangling frame -> hard error naming
 * the animation + frame. */
static tp_status build_animations(const tp_export_ir *prep, const tp_export_ir_opts *o, tp_arena *arena,
                                  tp_export_anim **out_anims, int *out_count, tp_error *err) {
    int total = o->animation_count;
    tp_export_anim *anims = NULL;
    if (total > 0) {
        anims = (tp_export_anim *)tp_arena_alloc(arena, (size_t)total * sizeof(tp_export_anim));
        if (!anims) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (animations)");
        }
    }

    int n = prep->sprite_count;

    for (int i = 0; i < o->animation_count; i++) {
        const tp_export_anim_in *in = &o->animations[i];
        tp_export_anim *a = &anims[i];
        a->id = tp_arena_strdup(arena, in->id ? in->id : "");
        a->fps = in->fps;
        a->playback = in->playback;
        a->flip_h = in->flip_h;
        a->flip_v = in->flip_v;
        a->frame_count = in->frame_count;
        a->frames = NULL;
        if (!a->id) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (explicit anim id)");
        }
        if (in->frame_count > 0) {
            a->frames = (const char **)tp_arena_alloc(arena, (size_t)in->frame_count * sizeof(char *));
            if (!a->frames) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (explicit anim)");
            }
            for (int f = 0; f < in->frame_count; f++) {
                const tp_export_frame_ref *frame = &in->frames[f];
                const char *raw_name = NULL;
                for (int r = 0; r < o->sprite_ref_count; r++) {
                    const tp_export_sprite_ref_in *ref = &o->sprite_refs[r];
                    if (tp_id128_eq(ref->source_id, frame->source_id) &&
                        ref->source_key && frame->source_key &&
                        strcmp(ref->source_key, frame->source_key) == 0) {
                        raw_name = ref->raw_name;
                        break;
                    }
                }
                const char *fin = NULL;
                for (int s = 0; raw_name && s < n; s++) {
                    if (prep->sprites[s].data.name &&
                        strcmp(prep->sprites[s].data.name, raw_name) == 0) {
                        fin = prep->sprites[s].final_name;
                        break;
                    }
                }
                if (!fin) {
                    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                        "tp_export_ir_build: animation '%s' references frame '%s' which matches no "
                                        "packed sprite (dangling frame -- the sprite was removed or never packed)",
                                        in->id ? in->id : "",
                                        frame->source_key ? frame->source_key : "");
                }
                a->frames[f] = tp_arena_strdup(arena, fin);
                if (!a->frames[f]) {
                    return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (explicit frame)");
                }
            }
        }
    }

    if (total > 1) {
        qsort(anims, (size_t)total, sizeof(tp_export_anim), cmp_anim_id);
    }
    *out_anims = anims;
    *out_count = total;
    return TP_STATUS_OK;
}

/* ======================================================================== */
/* entry                                                                    */
/* ======================================================================== */

tp_status tp_export_ir_build(const tp_result *result, const tp_export_ir_opts *opts,
                             const char *target_id, tp_arena *arena,
                             tp_export_ir *out, tp_error *err) {
    if (!result || !target_id || target_id[0] == '\0' || !arena || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_build: invalid result/target/arena/out");
    }
    tp_export_ir_opts defaults;
    if (!opts) {
        tp_export_ir_opts_defaults(&defaults);
        opts = &defaults;
    }
    tp_status input_status = validate_build_inputs(result, opts, err);
    if (input_status != TP_STATUS_OK) {
        return input_status;
    }

    memset(out, 0, sizeof *out);
    out->version = TP_EXPORT_IR_VERSION;
    out->target_id = tp_arena_strdup(arena, target_id);
    out->atlas_name = tp_arena_strdup(arena, result->atlas_name ? result->atlas_name : "");
    out->pixels_per_unit = result->pixels_per_unit;
    out->scale = (opts->scale != 0.0F) ? opts->scale : 1.0F;
    if (!out->target_id || !out->atlas_name) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (identity)");
    }

    out->page_count = result->page_count;
    if (out->page_count > 0) {
        out->pages = (tp_export_page *)tp_arena_alloc(
            arena, (size_t)out->page_count * sizeof *out->pages);
        if (!out->pages) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (pages)");
        }
        for (int i = 0; i < out->page_count; ++i) {
            out->pages[i] = (tp_export_page){
                .artifact_id = i,
                .w = result->pages[i].w,
                .h = result->pages[i].h,
                .premultiplied = result->pages[i].premultiplied,
            };
        }
    }

    int n = result->sprite_count;
    tp_ir_sprite_build *build = NULL;
    if (n > 0) {
        build = (tp_ir_sprite_build *)tp_arena_alloc(
            arena, (size_t)n * sizeof *build);
        if (!build) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (sprites)");
        }
    }
    for (int i = 0; i < n; i++) {
        char *fn = final_name(result->sprites[i].name, opts, arena);
        if (!fn) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (final name)");
        }
        build[i].sprite.final_name = fn;
        build[i].sprite.data = result->sprites[i];
        build[i].sprite.data.verts = NULL;
        build[i].sprite.data.indices = NULL;
        build[i].sprite.is_solid = packed_region_is_solid(
            result, &result->sprites[i]);
        build[i].source_index = i;
        build[i].sprite.data.name = tp_arena_strdup(
            arena, result->sprites[i].name ? result->sprites[i].name : "");
        if (!build[i].sprite.data.name) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_export_ir_build: OOM (raw name)");
        }
        if (result->sprites[i].vert_count > 0) {
            size_t bytes = (size_t)result->sprites[i].vert_count * sizeof(tp_point);
            build[i].sprite.data.verts = (tp_point *)tp_arena_alloc(arena, bytes);
            if (!build[i].sprite.data.verts) {
                return tp_error_set(err, TP_STATUS_OOM,
                                    "tp_export_ir_build: OOM (vertices)");
            }
            memcpy(build[i].sprite.data.verts, result->sprites[i].verts, bytes);
        }
        if (result->sprites[i].index_count > 0) {
            size_t bytes = (size_t)result->sprites[i].index_count * sizeof(uint16_t);
            build[i].sprite.data.indices = (uint16_t *)tp_arena_alloc(arena, bytes);
            if (!build[i].sprite.data.indices) {
                return tp_error_set(err, TP_STATUS_OOM,
                                    "tp_export_ir_build: OOM (indices)");
            }
            memcpy(build[i].sprite.data.indices, result->sprites[i].indices, bytes);
        }
    }

    /* Determinism: sort by final export name. */
    qsort(build, (size_t)n, sizeof *build, cmp_sprite_final);

    tp_export_sprite *sprites = NULL;
    if (n > 0) {
        sprites = (tp_export_sprite *)tp_arena_alloc(
            arena, (size_t)n * sizeof *sprites);
        if (!sprites) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_export_ir_build: OOM (sorted sprites)");
        }
        for (int i = 0; i < n; ++i) {
            sprites[i] = build[i].sprite;
        }
    }

    /* Collision: two entries munged/renamed to the same final name cannot both
     * be emitted. Aliases are DISTINCT names by design and never collide here. */
    for (int i = 1; i < n; i++) {
        if (strcmp(sprites[i - 1].final_name, sprites[i].final_name) == 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_build: two sprites resolve to the same export name '%s' "
                                "(raw '%s' and '%s') -- rename one",
                                sprites[i].final_name, sprites[i - 1].data.name,
                                sprites[i].data.name);
        }
    }

    /* Remap alias_of (result index) -> prepared (final-name-sorted) index. */
    if (n > 0) {
        int *pos = (int *)tp_arena_alloc(arena, (size_t)n * sizeof(int));
        if (!pos) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_export_ir_build: OOM (alias map)");
        }
        for (int p = 0; p < n; p++) {
            pos[build[p].source_index] = p;
        }
        for (int p = 0; p < n; p++) {
            int a = sprites[p].data.alias_of;
            sprites[p].data.alias_of = (a >= 0 && a < n) ? pos[a] : -1;
        }
    }

    out->sprites = sprites;
    out->sprite_count = n;

    tp_status status = build_animations(
        out, opts, arena, &out->animations, &out->animation_count, err);
    return status == TP_STATUS_OK ? tp_export_ir_validate(out, err) : status;
}

static bool ir_has_final_name(const tp_export_ir *ir, const char *name) {
    int lo = 0;
    int hi = ir->sprite_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(ir->sprites[mid].final_name, name);
        if (cmp == 0) {
            return true;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

tp_status tp_export_ir_validate(const tp_export_ir *ir, tp_error *err) {
    if (!ir) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_validate: NULL IR");
    }
    if (ir->version != TP_EXPORT_IR_VERSION) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_validate: unsupported version %u",
                            (unsigned)ir->version);
    }
    if (tp_exporter_id_validate(ir->target_id, err) != TP_STATUS_OK ||
        !ir->atlas_name || ir->atlas_name[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_validate: invalid target or atlas identity");
    }
    if (!isfinite(ir->pixels_per_unit) || ir->pixels_per_unit <= 0.0F ||
        !isfinite(ir->scale) || ir->scale <= 0.0F) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_validate: invalid scale metadata");
    }
    if (ir->page_count <= 0 || ir->page_count > TP_PACK_MAX_PAGES ||
        !ir->pages) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_export_ir_validate: page count %d is out of range [1..%d]",
                            ir->page_count, TP_PACK_MAX_PAGES);
    }
    for (int p = 0; p < ir->page_count; ++p) {
        if (ir->pages[p].artifact_id != p ||
            !tp_pack_max_size_valid(ir->pages[p].w) ||
            !tp_pack_max_size_valid(ir->pages[p].h)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_validate: invalid page %d", p);
        }
    }
    if (ir->sprite_count < 0 || (unsigned)ir->sprite_count > UINT16_MAX ||
        (ir->sprite_count > 0 && !ir->sprites)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_export_ir_validate: invalid sprite count %d",
                            ir->sprite_count);
    }
    for (int i = 0; i < ir->sprite_count; ++i) {
        const tp_export_sprite *entry = &ir->sprites[i];
        const tp_sprite *sprite = &entry->data;
        if (!entry->final_name || entry->final_name[0] == '\0' ||
            !sprite->name || sprite->name[0] == '\0') {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_validate: sprite %d has an empty name", i);
        }
        if (i > 0 && strcmp(ir->sprites[i - 1].final_name,
                            entry->final_name) >= 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_validate: sprite names are not unique and sorted");
        }
        if (sprite->page < 0 || sprite->page >= ir->page_count ||
            sprite->transform >= 8U || sprite->frame.w <= 0 ||
            sprite->frame.h <= 0 || sprite->sourceSize.w <= 0 ||
            sprite->sourceSize.h <= 0 || sprite->spriteSourceSize.w <= 0 ||
            sprite->spriteSourceSize.h <= 0 ||
            !isfinite(sprite->pivot.x) || !isfinite(sprite->pivot.y)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_validate: invalid sprite geometry for '%s'",
                                entry->final_name);
        }
        const tp_export_page *page = &ir->pages[sprite->page];
        const int stored_w = (sprite->transform & TP_TRANSFORM_DIAGONAL)
                                 ? sprite->frame.h
                                 : sprite->frame.w;
        const int stored_h = (sprite->transform & TP_TRANSFORM_DIAGONAL)
                                 ? sprite->frame.w
                                 : sprite->frame.h;
        if (sprite->frame.x < 0 || sprite->frame.y < 0 ||
            stored_w > page->w || stored_h > page->h ||
            sprite->frame.x > page->w - stored_w ||
            sprite->frame.y > page->h - stored_h) {
            return tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "tp_export_ir_validate: sprite '%s' frame is outside page %d",
                entry->final_name, sprite->page);
        }
        if (sprite->alias_of < -1 || sprite->alias_of >= ir->sprite_count ||
            sprite->alias_of == i ||
            (sprite->alias_of >= 0 &&
             ir->sprites[sprite->alias_of].data.alias_of != -1)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_validate: invalid alias for '%s'",
                                entry->final_name);
        }
        if (sprite->vert_count < 0 || sprite->vert_count > TP_PACK_MAX_VERTICES ||
            (sprite->vert_count > 0 && !sprite->verts) ||
            sprite->index_count < 0 ||
            (sprite->index_count > 0 && !sprite->indices) ||
            (sprite->index_count % 3) != 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_validate: invalid mesh for '%s'",
                                entry->final_name);
        }
        for (int j = 0; j < sprite->index_count; ++j) {
            if (sprite->indices[j] >= (uint16_t)sprite->vert_count) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                    "tp_export_ir_validate: mesh index out of range for '%s'",
                                    entry->final_name);
            }
        }
    }
    if ((unsigned)ir->animation_count > UINT16_MAX) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_export_ir_validate: animation count %d exceeds %u",
                            ir->animation_count, (unsigned)UINT16_MAX);
    }
    if (ir->animation_count < 0 ||
        (ir->animation_count > 0 && !ir->animations)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_export_ir_validate: invalid animation count");
    }
    for (int i = 0; i < ir->animation_count; ++i) {
        const tp_export_anim *anim = &ir->animations[i];
        if ((unsigned)anim->frame_count > UINT16_MAX) {
            return tp_error_set(
                err, TP_STATUS_OUT_OF_BOUNDS,
                "tp_export_ir_validate: animation '%s' frame count exceeds %u",
                anim->id ? anim->id : "", (unsigned)UINT16_MAX);
        }
        if (!anim->id || anim->id[0] == '\0' || anim->frame_count < 0 ||
            (anim->frame_count > 0 && !anim->frames) ||
            !isfinite(anim->fps)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_validate: invalid animation %d", i);
        }
        if (i > 0 && strcmp(ir->animations[i - 1].id, anim->id) >= 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_export_ir_validate: animation ids are not unique and sorted");
        }
        for (int f = 0; f < anim->frame_count; ++f) {
            if (!anim->frames[f] || !ir_has_final_name(ir, anim->frames[f])) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                    "tp_export_ir_validate: animation '%s' has a dangling frame",
                                    anim->id);
            }
        }
    }
    return TP_STATUS_OK;
}
