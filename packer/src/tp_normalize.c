#include "tp_core/tp_export.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"

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
    out->auto_group_animations = true;
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
    char *out = tp_arena_strdup(arena, start);
    if (!out) {
        return NULL;
    }
    if (o->strip_extension) {
        char *slash = strrchr(out, '/');
        char *base = slash ? slash + 1 : out;
        char *dot = strrchr(base, '.');
        if (dot && dot != base) { /* keep dotfiles like ".gitkeep" intact */
            *dot = '\0';
        }
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
/* numeric-suffix animation auto-grouping                                   */
/* ======================================================================== */

typedef struct {
    char *base;    /* arena-owned group id */
    long num;      /* numeric suffix value (ordering key) */
    int sprite_ix; /* index into prepared sprites (its final_name is the frame) */
} auto_match;

/* Splits `name` into (base, trailing-number). Returns false when there is no
 * trailing digit run or the base would be empty. `base` is arena-owned. */
static bool split_suffix(const char *name, tp_arena *arena, char **out_base, long *out_num) {
    size_t len = strlen(name);
    size_t i = len;
    while (i > 0 && isdigit((unsigned char)name[i - 1])) {
        i--;
    }
    if (i == len) {
        return false; /* no trailing digits */
    }
    size_t base_end = i;
    if (base_end > 0 && (name[base_end - 1] == '_' || name[base_end - 1] == '-')) {
        base_end--; /* strip one optional separator */
    }
    if (base_end == 0) {
        return false; /* base empty (e.g. "01", "_01") */
    }
    char *base = (char *)tp_arena_alloc(arena, base_end + 1U);
    if (!base) {
        return false;
    }
    memcpy(base, name, base_end);
    base[base_end] = '\0';
    *out_base = base;
    *out_num = strtol(name + i, NULL, 10);
    return true;
}

static int cmp_auto(const void *a, const void *b) {
    const auto_match *ma = (const auto_match *)a;
    const auto_match *mb = (const auto_match *)b;
    int c = strcmp(ma->base, mb->base);
    if (c != 0) {
        return c;
    }
    if (ma->num != mb->num) {
        return ma->num < mb->num ? -1 : 1;
    }
    return 0; /* equal base+num cannot happen: distinct final names */
}

static int cmp_anim_id(const void *a, const void *b) {
    const tp_export_anim *aa = (const tp_export_anim *)a;
    const tp_export_anim *ab = (const tp_export_anim *)b;
    return strcmp(aa->id, ab->id);
}

static bool id_is_explicit(const tp_normalize_opts *o, const char *id) {
    for (int i = 0; i < o->animation_count; i++) {
        if (o->animations[i].id && strcmp(o->animations[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

/* Builds the merged animation list: explicit (from opts) + numeric-suffix auto
 * groups whose id is not already explicit. Sorted ascending by id. */
static tp_status build_animations(const tp_export_prepared *prep, const tp_normalize_opts *o, tp_arena *arena,
                                  tp_export_anim **out_anims, int *out_count, tp_error *err) {
    int n = prep->sprite_count;

    /* auto matches */
    auto_match *matches = NULL;
    int match_count = 0;
    if (o->auto_group_animations && n > 0) {
        matches = (auto_match *)tp_arena_alloc(arena, (size_t)n * sizeof(auto_match));
        if (!matches) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (auto matches)");
        }
        for (int i = 0; i < n; i++) {
            char *base = NULL;
            long num = 0;
            if (split_suffix(prep->sprites[i].final_name, arena, &base, &num)) {
                matches[match_count].base = base;
                matches[match_count].num = num;
                matches[match_count].sprite_ix = i;
                match_count++;
            }
        }
        qsort(matches, (size_t)match_count, sizeof(auto_match), cmp_auto);
    }

    /* Count auto groups (runs of equal base with >= 2 frames, id not explicit). */
    int auto_groups = 0;
    for (int i = 0; i < match_count;) {
        int j = i + 1;
        while (j < match_count && strcmp(matches[j].base, matches[i].base) == 0) {
            j++;
        }
        if (j - i >= 2 && !id_is_explicit(o, matches[i].base)) {
            auto_groups++;
        }
        i = j;
    }

    int total = o->animation_count + auto_groups;
    tp_export_anim *anims = NULL;
    if (total > 0) {
        anims = (tp_export_anim *)tp_arena_alloc(arena, (size_t)total * sizeof(tp_export_anim));
        if (!anims) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (animations)");
        }
    }
    int w = 0;

    /* explicit animations (frames used verbatim as final names) */
    for (int i = 0; i < o->animation_count; i++) {
        const tp_export_anim_in *in = &o->animations[i];
        tp_export_anim *a = &anims[w++];
        a->id = tp_arena_strdup(arena, in->id ? in->id : "");
        a->fps = in->fps;
        a->playback = in->playback;
        a->flip_h = in->flip_h;
        a->flip_v = in->flip_v;
        a->frame_count = in->frame_count;
        a->frames = NULL;
        if (in->frame_count > 0) {
            a->frames = (const char **)tp_arena_alloc(arena, (size_t)in->frame_count * sizeof(char *));
            if (!a->id || !a->frames) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (explicit anim)");
            }
            for (int f = 0; f < in->frame_count; f++) {
                a->frames[f] = tp_arena_strdup(arena, in->frames[f] ? in->frames[f] : "");
                if (!a->frames[f]) {
                    return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (explicit frame)");
                }
            }
        } else if (!a->id) {
            return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (explicit anim id)");
        }
    }

    /* auto groups */
    for (int i = 0; i < match_count;) {
        int j = i + 1;
        while (j < match_count && strcmp(matches[j].base, matches[i].base) == 0) {
            j++;
        }
        int frames = j - i;
        if (frames >= 2 && !id_is_explicit(o, matches[i].base)) {
            tp_export_anim *a = &anims[w++];
            a->id = matches[i].base; /* arena-owned */
            a->fps = 30.0F;          /* TP_PROJECT_ANIM_FPS_DEFAULT */
            a->playback = 0;
            a->flip_h = false;
            a->flip_v = false;
            a->frame_count = frames;
            a->frames = (const char **)tp_arena_alloc(arena, (size_t)frames * sizeof(char *));
            if (!a->frames) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_normalize: OOM (auto frames)");
            }
            for (int f = 0; f < frames; f++) {
                a->frames[f] = prep->sprites[matches[i + f].sprite_ix].final_name;
            }
        }
        i = j;
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
