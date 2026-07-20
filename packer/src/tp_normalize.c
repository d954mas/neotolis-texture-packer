#include "tp_core/tp_export.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_names.h"

/* ======================================================================== */
/* options + final-name munging                                             */
/* ======================================================================== */

void tp_normalize_opts_defaults(tp_normalize_opts *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->strip_extension = true;
    out->strip_folders = false;
    out->scale = 1.0F;
}

static const char *override_for(const tp_normalize_opts *o, const char *raw) {
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
static char *final_name(const char *raw, const tp_normalize_opts *o, tp_arena *arena) {
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

static int cmp_sprite_final(const void *a, const void *b) {
    const tp_export_sprite *sa = (const tp_export_sprite *)a;
    const tp_export_sprite *sb = (const tp_export_sprite *)b;
    return strcmp(sa->final_name, sb->final_name);
}

/* ======================================================================== */
/* animation assembly (EXPLICIT project animations only)                    */
/* ======================================================================== */

/* Owner's standing ruling (docs/design/ux.md 3.7b): animations are assembled
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
 * the animation + frame (arch review 3.2 / plan L-4). */
static tp_status build_animations(const tp_export_prepared *prep, const tp_normalize_opts *o, tp_arena *arena,
                                  tp_export_anim **out_anims, int *out_count, tp_error *err) {
    int total = o->animation_count;
    tp_export_anim *anims = NULL;
    if (total > 0) {
        anims = (tp_export_anim *)tp_arena_alloc(arena, (size_t)total * sizeof(tp_export_anim));
        if (!anims) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (animations)");
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
            return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (explicit anim id)");
        }
        if (in->frame_count > 0) {
            a->frames = (const char **)tp_arena_alloc(arena, (size_t)in->frame_count * sizeof(char *));
            if (!a->frames) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (explicit anim)");
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
                    if (prep->sprites[s].src && prep->sprites[s].src->name &&
                        strcmp(prep->sprites[s].src->name, raw_name) == 0) {
                        fin = prep->sprites[s].final_name;
                        break;
                    }
                }
                if (!fin) {
                    return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                        "tp_normalize: animation '%s' references frame '%s' which matches no "
                                        "packed sprite (dangling frame -- the sprite was removed or never packed)",
                                        in->id ? in->id : "",
                                        frame->source_key ? frame->source_key : "");
                }
                a->frames[f] = tp_arena_strdup(arena, fin);
                if (!a->frames[f]) {
                    return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (explicit frame)");
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

tp_status tp_normalize(const tp_result *result, const tp_normalize_opts *opts, tp_arena *arena, tp_export_prepared *out,
                       tp_error *err) {
    if (!result || !arena || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_normalize: NULL result/arena/out");
    }
    tp_normalize_opts defaults;
    if (!opts) {
        tp_normalize_opts_defaults(&defaults);
        opts = &defaults;
    }

    memset(out, 0, sizeof *out);
    out->result = result;
    out->scale = (opts->scale != 0.0F) ? opts->scale : 1.0F;

    int n = result->sprite_count;
    tp_export_sprite *sprites = NULL;
    if (n > 0) {
        sprites = (tp_export_sprite *)tp_arena_alloc(arena, (size_t)n * sizeof(tp_export_sprite));
        if (!sprites) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (sprites)");
        }
    }
    for (int i = 0; i < n; i++) {
        char *fn = final_name(result->sprites[i].name, opts, arena);
        if (!fn) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (final name)");
        }
        sprites[i].final_name = fn;
        sprites[i].src = &result->sprites[i];
        sprites[i].alias_of = -1;
    }

    /* Determinism: sort by final export name (ROADMAP sort key). */
    qsort(sprites, (size_t)n, sizeof(tp_export_sprite), cmp_sprite_final);

    /* Collision: two entries munged/renamed to the same final name cannot both
     * be emitted. Aliases are DISTINCT names by design and never collide here. */
    for (int i = 1; i < n; i++) {
        if (strcmp(sprites[i - 1].final_name, sprites[i].final_name) == 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                "tp_normalize: two sprites resolve to the same export name '%s' "
                                "(raw '%s' and '%s') -- rename one",
                                sprites[i].final_name, sprites[i - 1].src->name, sprites[i].src->name);
        }
    }

    /* Remap alias_of (result index) -> prepared (final-name-sorted) index. */
    if (n > 0) {
        int *pos = (int *)tp_arena_alloc(arena, (size_t)n * sizeof(int));
        if (!pos) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (alias map)");
        }
        for (int p = 0; p < n; p++) {
            pos[(int)(sprites[p].src - result->sprites)] = p;
        }
        for (int p = 0; p < n; p++) {
            int a = sprites[p].src->alias_of;
            sprites[p].alias_of = (a >= 0 && a < n) ? pos[a] : -1;
        }
    }

    out->sprites = sprites;
    out->sprite_count = n;

    return build_animations(out, opts, arena, &out->animations, &out->animation_count, err);
}
