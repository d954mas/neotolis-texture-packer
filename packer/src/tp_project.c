#include "tp_core/tp_project.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define tp_getcwd _getcwd
#else
#include <unistd.h>
#define tp_getcwd getcwd
#endif

#include "cJSON.h"

#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS (default-target seeding) */
#include "tp_core/tp_id.h"     /* shape-ID parse/format for schema-v2 structural ids */
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project_migrate.h" /* legacy synthesis + duplicate validation on load */

#define TP_PATH_MAX 4096

/* ======================================================================== */
/* small owned-string + dynamic-array primitives                            */
/* ======================================================================== */

static char *tp_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    const size_t len = strlen(s) + 1U;
    char *copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

/* Grows *arr to hold at least `needed` elements of `elem` bytes (doubling).
 * New tail slots are uninitialised -- the caller inits the appended element. */
static bool tp_grow(void **arr, int *cap, int needed, size_t elem) {
    if (needed <= *cap) {
        return true;
    }
    int new_cap = (*cap == 0) ? 4 : *cap * 2;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    void *n = realloc(*arr, (size_t)new_cap * elem);
    if (!n) {
        return false;
    }
    *arr = n;
    *cap = new_cap;
    return true;
}

/* ======================================================================== */
/* path helpers                                                             */
/* ======================================================================== */

static void tp_normalize_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

static bool tp_path_is_absolute(const char *p) {
    if (!p || !p[0]) {
        return false;
    }
    if (p[0] == '/' || p[0] == '\\') {
        return true; /* POSIX root or Windows UNC / drive-relative root */
    }
    if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
        return true; /* Windows drive path */
    }
    return false;
}

static bool tp_component_has_drive(const char *c) {
    return c[0] != '\0' && c[1] == ':' && c[2] == '\0';
}

/* Case-insensitive on Windows (paths are), case-sensitive elsewhere. */
static bool tp_component_eq(const char *a, const char *b) {
#ifdef _WIN32
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

/* Absolute, '/'-normalized directory that contains `path`. */
static tp_status tp_abs_dir_of(const char *path, char *out, size_t cap) {
    char abs[TP_PATH_MAX];
    if (tp_path_is_absolute(path)) {
        if ((size_t)snprintf(abs, sizeof abs, "%s", path) >= sizeof abs) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
    } else {
        char cwd[TP_PATH_MAX];
        if (!tp_getcwd(cwd, (int)sizeof cwd)) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        if ((size_t)snprintf(abs, sizeof abs, "%s/%s", cwd, path) >= sizeof abs) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
    }
    tp_normalize_slashes(abs);

    char *slash = strrchr(abs, '/');
    if (slash) {
        *slash = '\0';
    } else {
        abs[0] = '.';
        abs[1] = '\0';
    }
    if ((size_t)snprintf(out, cap, "%s", abs) >= cap) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    return TP_STATUS_OK;
}

/* Rewrites `abs` (an absolute, '/'-normalized path) relative to `base_dir`
 * (likewise). Falls back to copying `abs` when the two live on different roots
 * (different Windows drives / drive-vs-rootless) -- the caller's notice concern
 * (ux.md §3.6.3). Result is '/'-normalized. */
static tp_status tp_relativize(const char *abs, const char *base_dir, char *out, size_t cap) {
    char abuf[TP_PATH_MAX];
    char bbuf[TP_PATH_MAX];
    if ((size_t)snprintf(abuf, sizeof abuf, "%s", abs) >= sizeof abuf ||
        (size_t)snprintf(bbuf, sizeof bbuf, "%s", base_dir) >= sizeof bbuf) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_normalize_slashes(abuf);
    tp_normalize_slashes(bbuf);

    char *acomp[256];
    char *bcomp[256];
    int an = 0;
    int bn = 0;
    for (char *tok = strtok(abuf, "/"); tok && an < 256; tok = strtok(NULL, "/")) {
        acomp[an++] = tok;
    }
    for (char *tok = strtok(bbuf, "/"); tok && bn < 256; tok = strtok(NULL, "/")) {
        bcomp[bn++] = tok;
    }

    const bool a_drive = an > 0 && tp_component_has_drive(acomp[0]);
    const bool b_drive = bn > 0 && tp_component_has_drive(bcomp[0]);
    if (a_drive != b_drive || (a_drive && b_drive && !tp_component_eq(acomp[0], bcomp[0]))) {
        if ((size_t)snprintf(out, cap, "%s", abs) >= cap) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        return TP_STATUS_OK; /* different root -> keep absolute */
    }

    int common = 0;
    while (common < an && common < bn && tp_component_eq(acomp[common], bcomp[common])) {
        common++;
    }

    out[0] = '\0';
    size_t used = 0;
    for (int i = common; i < bn; i++) {
        const int n = snprintf(out + used, cap - used, "../");
        if (n < 0 || (size_t)n >= cap - used) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        used += (size_t)n;
    }
    for (int i = common; i < an; i++) {
        const int n = snprintf(out + used, cap - used, "%s%s", acomp[i], (i + 1 < an) ? "/" : "");
        if (n < 0 || (size_t)n >= cap - used) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        used += (size_t)n;
    }
    if (used == 0) {
        if (cap < 2) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        out[0] = '.';
        out[1] = '\0';
    }
    return TP_STATUS_OK;
}

/* ======================================================================== */
/* model lifecycle + mutation                                               */
/* ======================================================================== */

static tp_project *tp_project_alloc_empty(void) {
    tp_project *p = (tp_project *)calloc(1, sizeof(tp_project));
    if (p) {
        p->schema_version = TP_PROJECT_SCHEMA_VERSION;
    }
    return p;
}

void tp_project_atlas_set_defaults(tp_project_atlas *a) {
    if (!a) {
        return;
    }
    tp_pack_settings s;
    tp_pack_settings_defaults(&s);
    a->max_size = s.max_size;
    a->padding = s.padding;
    a->margin = s.margin;
    a->extrude = s.extrude;
    a->alpha_threshold = s.alpha_threshold;
    a->max_vertices = s.max_vertices;
    a->shape = s.shape;
    a->allow_transform = s.allow_transform;
    a->power_of_two = s.power_of_two;
    a->pixels_per_unit = s.pixels_per_unit;
}

tp_status tp_project_add_atlas(tp_project *p, const char *name, int *out_index) {
    if (!p || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (!tp_grow((void **)&p->atlases, &p->atlas_cap, p->atlas_count + 1, sizeof(tp_project_atlas))) {
        return TP_STATUS_OOM;
    }
    tp_project_atlas *a = &p->atlases[p->atlas_count];
    memset(a, 0, sizeof *a);
    a->name = tp_strdup(name);
    if (!a->name) {
        return TP_STATUS_OOM;
    }
    tp_project_atlas_set_defaults(a);
    if (out_index) {
        *out_index = p->atlas_count;
    }
    p->atlas_count++;
    return TP_STATUS_OK;
}

tp_status tp_project_set_atlas_name(tp_project_atlas *a, const char *name) {
    if (!a || !name || name[0] == '\0') {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    char *copy = tp_strdup(name);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    free(a->name);
    a->name = copy;
    return TP_STATUS_OK;
}

static void tp_free_anim(tp_project_anim *an) {
    for (int i = 0; i < an->frame_count; i++) {
        free(an->frames[i]);
    }
    free(an->frames);
    free(an->name); /* `id` is a value (tp_id128), only `name` is owned */
}

static void tp_free_atlas(tp_project_atlas *a) {
    for (int i = 0; i < a->source_count; i++) {
        free(a->sources[i]);
    }
    free(a->sources);
    for (int i = 0; i < a->sprite_count; i++) {
        free(a->sprites[i].name);
        free(a->sprites[i].rename);
    }
    free(a->sprites);
    for (int i = 0; i < a->animation_count; i++) {
        tp_free_anim(&a->animations[i]);
    }
    free(a->animations);
    for (int i = 0; i < a->target_count; i++) {
        free(a->targets[i].exporter_id);
        free(a->targets[i].out_path);
    }
    free(a->targets);
    free(a->name);
}

tp_status tp_project_remove_atlas(tp_project *p, int index) {
    if (!p) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= p->atlas_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_free_atlas(&p->atlases[index]);
    for (int i = index; i < p->atlas_count - 1; i++) {
        p->atlases[i] = p->atlases[i + 1];
    }
    p->atlas_count--;
    return TP_STATUS_OK;
}

tp_project_atlas *tp_project_get_atlas(tp_project *p, int index) {
    if (!p || index < 0 || index >= p->atlas_count) {
        return NULL;
    }
    return &p->atlases[index];
}

tp_project *tp_project_create(void) {
    tp_project *p = tp_project_alloc_empty();
    if (!p) {
        return NULL;
    }
    if (tp_project_add_atlas(p, "atlas1", NULL) != TP_STATUS_OK) {
        tp_project_destroy(p);
        return NULL;
    }
    return p;
}

void tp_project_destroy(tp_project *p) {
    if (!p) {
        return;
    }
    for (int i = 0; i < p->atlas_count; i++) {
        tp_free_atlas(&p->atlases[i]);
    }
    free(p->atlases);
    free(p->project_dir);
    free(p);
}

tp_status tp_project_atlas_add_source(tp_project_atlas *a, const char *path) {
    if (!a || !path) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    /* Dedupe: skip the append when the same '/'-normalized path already exists. */
    char norm[TP_PATH_MAX];
    if ((size_t)snprintf(norm, sizeof norm, "%s", path) >= sizeof norm) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_normalize_slashes(norm);
    for (int i = 0; i < a->source_count; i++) {
        char existing[TP_PATH_MAX];
        (void)snprintf(existing, sizeof existing, "%s", a->sources[i]);
        tp_normalize_slashes(existing);
        if (strcmp(existing, norm) == 0) {
            return TP_STATUS_OK; /* already added -- no-op */
        }
    }
    if (!tp_grow((void **)&a->sources, &a->source_cap, a->source_count + 1, sizeof(char *))) {
        return TP_STATUS_OOM;
    }
    char *copy = tp_strdup(path);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    a->sources[a->source_count++] = copy;
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_remove_source(tp_project_atlas *a, int index) {
    if (!a) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= a->source_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    free(a->sources[index]);
    for (int i = index; i < a->source_count - 1; i++) {
        a->sources[i] = a->sources[i + 1];
    }
    a->source_count--;
    return TP_STATUS_OK;
}

tp_project_sprite *tp_project_atlas_find_sprite(tp_project_atlas *a, const char *name) {
    if (!a || !name) {
        return NULL;
    }
    for (int i = 0; i < a->sprite_count; i++) {
        if (strcmp(a->sprites[i].name, name) == 0) {
            return &a->sprites[i];
        }
    }
    return NULL;
}

tp_status tp_project_atlas_add_sprite(tp_project_atlas *a, const char *name, tp_project_sprite **out) {
    if (!a || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_project_sprite *existing = tp_project_atlas_find_sprite(a, name);
    if (existing) {
        if (out) {
            *out = existing;
        }
        return TP_STATUS_OK;
    }
    if (!tp_grow((void **)&a->sprites, &a->sprite_cap, a->sprite_count + 1, sizeof(tp_project_sprite))) {
        return TP_STATUS_OOM;
    }
    tp_project_sprite *s = &a->sprites[a->sprite_count];
    memset(s, 0, sizeof *s);
    s->name = tp_strdup(name);
    if (!s->name) {
        return TP_STATUS_OOM;
    }
    s->origin_x = TP_PROJECT_ORIGIN_DEFAULT;
    s->origin_y = TP_PROJECT_ORIGIN_DEFAULT;
    s->ov_shape = TP_PROJECT_OV_INHERIT;
    s->ov_allow_rotate = TP_PROJECT_OV_INHERIT;
    s->ov_max_vertices = TP_PROJECT_OV_INHERIT;
    s->ov_margin = TP_PROJECT_OV_INHERIT;
    s->ov_extrude = TP_PROJECT_OV_INHERIT;
    a->sprite_count++;
    if (out) {
        *out = s;
    }
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_remove_sprite(tp_project_atlas *a, const char *name) {
    if (!a || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    for (int i = 0; i < a->sprite_count; i++) {
        if (strcmp(a->sprites[i].name, name) == 0) {
            free(a->sprites[i].name);
            free(a->sprites[i].rename);
            for (int j = i; j < a->sprite_count - 1; j++) {
                a->sprites[j] = a->sprites[j + 1];
            }
            a->sprite_count--;
            return TP_STATUS_OK;
        }
    }
    return TP_STATUS_OUT_OF_BOUNDS;
}

/* An override entry that would serialize to just its name (safe to drop). */
static bool tp_sprite_is_default(const tp_project_sprite *s) {
    return s->origin_x == TP_PROJECT_ORIGIN_DEFAULT && s->origin_y == TP_PROJECT_ORIGIN_DEFAULT &&
           s->slice9_lrtb[0] == 0 && s->slice9_lrtb[1] == 0 && s->slice9_lrtb[2] == 0 && s->slice9_lrtb[3] == 0 &&
           s->rename == NULL && s->ov_shape == TP_PROJECT_OV_INHERIT && s->ov_allow_rotate == TP_PROJECT_OV_INHERIT &&
           s->ov_max_vertices == TP_PROJECT_OV_INHERIT && s->ov_margin == TP_PROJECT_OV_INHERIT &&
           s->ov_extrude == TP_PROJECT_OV_INHERIT;
}

tp_status tp_project_atlas_set_sprite_rename(tp_project_atlas *a, const char *sprite_name, const char *rename) {
    if (!a || !sprite_name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (rename == NULL || rename[0] == '\0') { /* clear back to the file-derived name */
        tp_project_sprite *s = tp_project_atlas_find_sprite(a, sprite_name);
        if (!s) {
            return TP_STATUS_OK;
        }
        free(s->rename);
        s->rename = NULL;
        if (tp_sprite_is_default(s)) {
            return tp_project_atlas_remove_sprite(a, sprite_name);
        }
        return TP_STATUS_OK;
    }
    tp_project_sprite *s = NULL;
    tp_status st = tp_project_atlas_add_sprite(a, sprite_name, &s);
    if (st != TP_STATUS_OK) {
        return st;
    }
    char *copy = tp_strdup(rename);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    free(s->rename);
    s->rename = copy;
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_prune_sprite(tp_project_atlas *a, const char *name) {
    if (!a || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_project_sprite *s = tp_project_atlas_find_sprite(a, name);
    if (s && tp_sprite_is_default(s)) {
        return tp_project_atlas_remove_sprite(a, name);
    }
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_add_animation(tp_project_atlas *a, const char *name, tp_project_anim **out) {
    if (!a || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (!tp_grow((void **)&a->animations, &a->animation_cap, a->animation_count + 1, sizeof(tp_project_anim))) {
        return TP_STATUS_OOM;
    }
    tp_project_anim *an = &a->animations[a->animation_count];
    memset(an, 0, sizeof *an); /* id starts nil (a writable session promotes it) */
    an->name = tp_strdup(name);
    if (!an->name) {
        return TP_STATUS_OOM;
    }
    an->fps = TP_PROJECT_ANIM_FPS_DEFAULT;
    an->playback = TP_PROJECT_ANIM_PLAYBACK_DEFAULT;
    a->animation_count++;
    if (out) {
        *out = an;
    }
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_remove_animation(tp_project_atlas *a, const char *name) {
    if (!a || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    for (int i = 0; i < a->animation_count; i++) {
        if (a->animations[i].name && strcmp(a->animations[i].name, name) == 0) {
            tp_free_anim(&a->animations[i]);
            for (int j = i; j < a->animation_count - 1; j++) {
                a->animations[j] = a->animations[j + 1];
            }
            a->animation_count--;
            return TP_STATUS_OK;
        }
    }
    return TP_STATUS_OUT_OF_BOUNDS;
}

tp_status tp_project_anim_add_frame(tp_project_anim *anim, const char *frame_name) {
    if (!anim || !frame_name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (!tp_grow((void **)&anim->frames, &anim->frame_cap, anim->frame_count + 1, sizeof(char *))) {
        return TP_STATUS_OOM;
    }
    char *copy = tp_strdup(frame_name);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    anim->frames[anim->frame_count++] = copy;
    return TP_STATUS_OK;
}

tp_status tp_project_anim_remove_frame(tp_project_anim *anim, int index) {
    if (!anim) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= anim->frame_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    free(anim->frames[index]);
    for (int j = index; j < anim->frame_count - 1; j++) {
        anim->frames[j] = anim->frames[j + 1];
    }
    anim->frame_count--;
    return TP_STATUS_OK;
}

tp_status tp_project_anim_move_frame(tp_project_anim *anim, int index, int delta) {
    if (!anim) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= anim->frame_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    int dst = index + delta;
    if (dst < 0) {
        dst = 0;
    }
    if (dst > anim->frame_count - 1) {
        dst = anim->frame_count - 1;
    }
    if (dst == index) {
        return TP_STATUS_OK; /* no-op */
    }
    char *moved = anim->frames[index];
    if (dst > index) {
        for (int j = index; j < dst; j++) {
            anim->frames[j] = anim->frames[j + 1];
        }
    } else {
        for (int j = index; j > dst; j--) {
            anim->frames[j] = anim->frames[j - 1];
        }
    }
    anim->frames[dst] = moved;
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_add_target(tp_project_atlas *a, const char *exporter_id, const char *out_path,
                                      tp_project_target **out) {
    if (!a || !exporter_id || !out_path) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (!tp_grow((void **)&a->targets, &a->target_cap, a->target_count + 1, sizeof(tp_project_target))) {
        return TP_STATUS_OOM;
    }
    tp_project_target *t = &a->targets[a->target_count];
    memset(t, 0, sizeof *t);
    t->exporter_id = tp_strdup(exporter_id);
    t->out_path = tp_strdup(out_path);
    if (!t->exporter_id || !t->out_path) {
        free(t->exporter_id);
        free(t->out_path);
        return TP_STATUS_OOM;
    }
    t->enabled = true;
    a->target_count++;
    if (out) {
        *out = t;
    }
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_seed_default_target(tp_project *p, int atlas_index) {
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    char path[512];
    (void)snprintf(path, sizeof path, "out/%s", a->name);
    return tp_project_atlas_add_target(a, TP_EXPORTER_ID_JSON_NEOTOLIS, path, NULL);
}

tp_status tp_project_atlas_remove_target(tp_project_atlas *a, int index) {
    if (!a) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= a->target_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    free(a->targets[index].exporter_id);
    free(a->targets[index].out_path);
    for (int i = index; i < a->target_count - 1; i++) {
        a->targets[i] = a->targets[i + 1];
    }
    a->target_count--;
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_set_target(tp_project_atlas *a, int index, const char *exporter_id, const char *out_path,
                                      bool enabled) {
    if (!a || !exporter_id || exporter_id[0] == '\0' || !out_path || out_path[0] == '\0') {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= a->target_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    char *eid = tp_strdup(exporter_id);
    char *op = tp_strdup(out_path);
    if (!eid || !op) {
        free(eid);
        free(op);
        return TP_STATUS_OOM;
    }
    tp_project_target *t = &a->targets[index];
    free(t->exporter_id);
    free(t->out_path);
    t->exporter_id = eid;
    t->out_path = op;
    t->enabled = enabled;
    return TP_STATUS_OK;
}

/* ======================================================================== */
/* deterministic writer (ux.md principle 7)                                 */
/* ======================================================================== */

typedef struct tp_sb {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} tp_sb;

static void tp_sb_write(tp_sb *sb, const char *s, size_t n) {
    if (sb->oom) {
        return;
    }
    if (sb->len + n + 1U > sb->cap) {
        size_t new_cap = (sb->cap == 0) ? 1024U : sb->cap;
        while (sb->len + n + 1U > new_cap) {
            new_cap *= 2U;
        }
        char *nb = (char *)realloc(sb->buf, new_cap);
        if (!nb) {
            sb->oom = true;
            return;
        }
        sb->buf = nb;
        sb->cap = new_cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void tp_sb_str(tp_sb *sb, const char *s) { tp_sb_write(sb, s, strlen(s)); }

static void tp_sb_char(tp_sb *sb, char c) { tp_sb_write(sb, &c, 1U); }

static void tp_sb_indent(tp_sb *sb, int depth) {
    for (int i = 0; i < depth; i++) {
        tp_sb_str(sb, "  ");
    }
}

static void tp_sb_int(tp_sb *sb, long v) {
    char tmp[32];
    (void)snprintf(tmp, sizeof tmp, "%ld", v);
    tp_sb_str(sb, tmp);
}

static void tp_sb_uint(tp_sb *sb, unsigned long v) {
    char tmp[32];
    (void)snprintf(tmp, sizeof tmp, "%lu", v);
    tp_sb_str(sb, tmp);
}

/* "%.9g" round-trips a float exactly (unlike "%g"); locale is "C" in tp_core. */
static void tp_sb_num(tp_sb *sb, double v) {
    char tmp[64];
    (void)snprintf(tmp, sizeof tmp, "%.9g", v);
    tp_sb_str(sb, tmp);
}

static void tp_sb_json_string(tp_sb *sb, const char *s) {
    tp_sb_char(sb, '"');
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        switch (*c) {
            case '"': tp_sb_str(sb, "\\\""); break;
            case '\\': tp_sb_str(sb, "\\\\"); break;
            case '\b': tp_sb_str(sb, "\\b"); break;
            case '\f': tp_sb_str(sb, "\\f"); break;
            case '\n': tp_sb_str(sb, "\\n"); break;
            case '\r': tp_sb_str(sb, "\\r"); break;
            case '\t': tp_sb_str(sb, "\\t"); break;
            default:
                if (*c < 0x20U) {
                    char esc[8];
                    (void)snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*c);
                    tp_sb_str(sb, esc);
                } else {
                    tp_sb_char(sb, (char)*c);
                }
                break;
        }
    }
    tp_sb_char(sb, '"');
}

/* Opens the next "key": slot at `keydepth`, handling the leading comma. */
static void tp_obj_key(tp_sb *sb, int keydepth, bool *first, const char *key) {
    tp_sb_str(sb, *first ? "\n" : ",\n");
    *first = false;
    tp_sb_indent(sb, keydepth);
    tp_sb_json_string(sb, key);
    tp_sb_str(sb, ": ");
}

static void tp_emit_string_array(tp_sb *sb, int depth, char *const *arr, int count) {
    if (count == 0) {
        tp_sb_str(sb, "[]");
        return;
    }
    tp_sb_char(sb, '[');
    for (int i = 0; i < count; i++) {
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_json_string(sb, arr[i]);
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, ']');
}

/* Emit a structural shape-ID ("<kind>_<32hex>") as a JSON string value. Always
 * written for a v2 entity (structural, not sparse) and deterministic. A caller
 * that has not promoted a fresh entity would emit a nil id here -- such a file is
 * rejected on reload (nil id), surfacing the missing promote as a loud error. */
static void tp_emit_id(tp_sb *sb, tp_id_kind kind, tp_id128 id) {
    char text[TP_ID_TEXT_CAP];
    if (tp_id_format(kind, id, text, sizeof text, NULL) != TP_STATUS_OK) {
        text[0] = '\0'; /* unreachable: kind is valid and the buffer is TP_ID_TEXT_CAP */
    }
    tp_sb_json_string(sb, text);
}

static void tp_emit_sprite(tp_sb *sb, int depth, const tp_project_sprite *s) {
    tp_sb_char(sb, '{');
    bool first = true;
    /* Keys ASCII-ascending: allow_rotate < extrude < margin < max_vertices < name
     * < origin < rename < shape < slice9. Overrides are sparse (INHERIT skipped). */
    if (s->ov_allow_rotate != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "allow_rotate");
        tp_sb_int(sb, (long)s->ov_allow_rotate);
    }
    if (s->ov_extrude != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "extrude");
        tp_sb_int(sb, (long)s->ov_extrude);
    }
    if (s->ov_margin != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "margin");
        tp_sb_int(sb, (long)s->ov_margin);
    }
    if (s->ov_max_vertices != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "max_vertices");
        tp_sb_int(sb, (long)s->ov_max_vertices);
    }
    tp_obj_key(sb, depth + 1, &first, "name");
    tp_sb_json_string(sb, s->name);
    if (s->origin_x != TP_PROJECT_ORIGIN_DEFAULT || s->origin_y != TP_PROJECT_ORIGIN_DEFAULT) {
        tp_obj_key(sb, depth + 1, &first, "origin");
        tp_sb_char(sb, '[');
        tp_sb_num(sb, (double)s->origin_x);
        tp_sb_str(sb, ", ");
        tp_sb_num(sb, (double)s->origin_y);
        tp_sb_char(sb, ']');
    }
    if (s->rename) {
        tp_obj_key(sb, depth + 1, &first, "rename");
        tp_sb_json_string(sb, s->rename);
    }
    if (s->ov_shape != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "shape");
        tp_sb_int(sb, (long)s->ov_shape);
    }
    if (s->slice9_lrtb[0] || s->slice9_lrtb[1] || s->slice9_lrtb[2] || s->slice9_lrtb[3]) {
        tp_obj_key(sb, depth + 1, &first, "slice9");
        tp_sb_char(sb, '[');
        for (int k = 0; k < 4; k++) {
            if (k) {
                tp_sb_str(sb, ", ");
            }
            tp_sb_uint(sb, (unsigned long)s->slice9_lrtb[k]);
        }
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void tp_emit_anim(tp_sb *sb, int depth, const tp_project_anim *an) {
    tp_sb_char(sb, '{');
    bool first = true;
    if (an->flip_h) {
        tp_obj_key(sb, depth + 1, &first, "flip_h");
        tp_sb_str(sb, "true");
    }
    if (an->flip_v) {
        tp_obj_key(sb, depth + 1, &first, "flip_v");
        tp_sb_str(sb, "true");
    }
    if (an->fps != TP_PROJECT_ANIM_FPS_DEFAULT) {
        tp_obj_key(sb, depth + 1, &first, "fps");
        tp_sb_num(sb, (double)an->fps);
    }
    tp_obj_key(sb, depth + 1, &first, "frames");
    tp_emit_string_array(sb, depth + 1, an->frames, an->frame_count);
    tp_obj_key(sb, depth + 1, &first, "id"); /* structural shape-ID (persistent) */
    tp_emit_id(sb, TP_ID_KIND_ANIM, an->id);
    tp_obj_key(sb, depth + 1, &first, "name"); /* logical/display name (was v1 `id`) */
    tp_sb_json_string(sb, an->name);
    if (an->playback != TP_PROJECT_ANIM_PLAYBACK_DEFAULT) {
        tp_obj_key(sb, depth + 1, &first, "playback");
        tp_sb_int(sb, (long)an->playback);
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void tp_emit_target(tp_sb *sb, int depth, const tp_project_target *t) {
    tp_sb_char(sb, '{');
    bool first = true;
    if (!t->enabled) {
        tp_obj_key(sb, depth + 1, &first, "enabled");
        tp_sb_str(sb, "false");
    }
    tp_obj_key(sb, depth + 1, &first, "exporter_id");
    tp_sb_json_string(sb, t->exporter_id);
    tp_obj_key(sb, depth + 1, &first, "id"); /* structural shape-ID (persistent) */
    tp_emit_id(sb, TP_ID_KIND_TARGET, t->id);
    tp_obj_key(sb, depth + 1, &first, "out_path");
    tp_sb_json_string(sb, t->out_path);
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void tp_emit_atlas(tp_sb *sb, int depth, const tp_project_atlas *a, const tp_pack_settings *d) {
    tp_sb_char(sb, '{');
    bool first = true;
    if (a->allow_transform != d->allow_transform) {
        tp_obj_key(sb, depth + 1, &first, "allow_transform");
        tp_sb_str(sb, a->allow_transform ? "true" : "false");
    }
    if (a->alpha_threshold != d->alpha_threshold) {
        tp_obj_key(sb, depth + 1, &first, "alpha_threshold");
        tp_sb_int(sb, (long)a->alpha_threshold);
    }
    if (a->animation_count > 0) {
        tp_obj_key(sb, depth + 1, &first, "animations");
        tp_sb_char(sb, '[');
        for (int i = 0; i < a->animation_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            tp_emit_anim(sb, depth + 2, &a->animations[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    if (a->extrude != d->extrude) {
        tp_obj_key(sb, depth + 1, &first, "extrude");
        tp_sb_int(sb, (long)a->extrude);
    }
    tp_obj_key(sb, depth + 1, &first, "id"); /* structural shape-ID (persistent, always written) */
    tp_emit_id(sb, TP_ID_KIND_ATLAS, a->id);
    if (a->margin != d->margin) {
        tp_obj_key(sb, depth + 1, &first, "margin");
        tp_sb_int(sb, (long)a->margin);
    }
    if (a->max_size != d->max_size) {
        tp_obj_key(sb, depth + 1, &first, "max_size");
        tp_sb_int(sb, (long)a->max_size);
    }
    if (a->max_vertices != d->max_vertices) {
        tp_obj_key(sb, depth + 1, &first, "max_vertices");
        tp_sb_int(sb, (long)a->max_vertices);
    }
    tp_obj_key(sb, depth + 1, &first, "name");
    tp_sb_json_string(sb, a->name);
    if (a->padding != d->padding) {
        tp_obj_key(sb, depth + 1, &first, "padding");
        tp_sb_int(sb, (long)a->padding);
    }
    if (a->pixels_per_unit != d->pixels_per_unit) {
        tp_obj_key(sb, depth + 1, &first, "pixels_per_unit");
        tp_sb_num(sb, (double)a->pixels_per_unit);
    }
    if (a->power_of_two != d->power_of_two) {
        tp_obj_key(sb, depth + 1, &first, "power_of_two");
        tp_sb_str(sb, a->power_of_two ? "true" : "false");
    }
    if (a->shape != d->shape) {
        tp_obj_key(sb, depth + 1, &first, "shape");
        tp_sb_int(sb, (long)a->shape);
    }
    if (a->source_count > 0) {
        tp_obj_key(sb, depth + 1, &first, "sources");
        tp_emit_string_array(sb, depth + 1, a->sources, a->source_count);
    }
    if (a->sprite_count > 0) {
        tp_obj_key(sb, depth + 1, &first, "sprites");
        tp_sb_char(sb, '[');
        for (int i = 0; i < a->sprite_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            tp_emit_sprite(sb, depth + 2, &a->sprites[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    if (a->target_count > 0) {
        tp_obj_key(sb, depth + 1, &first, "targets");
        tp_sb_char(sb, '[');
        for (int i = 0; i < a->target_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            tp_emit_target(sb, depth + 2, &a->targets[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void tp_emit_root(tp_sb *sb, const tp_project *p, const tp_pack_settings *d) {
    tp_sb_char(sb, '{');
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, 1);
    tp_sb_str(sb, "\"version\": ");
    tp_sb_int(sb, (long)p->schema_version); /* "version" is always emitted first */
    tp_sb_str(sb, ",\n");
    tp_sb_indent(sb, 1);
    tp_sb_str(sb, "\"atlases\": ");
    if (p->atlas_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int i = 0; i < p->atlas_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            tp_emit_atlas(sb, 2, &p->atlases[i], d);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n");
    tp_sb_char(sb, '}');
    tp_sb_char(sb, '\n'); /* trailing newline */
}

/* ======================================================================== */
/* save                                                                     */
/* ======================================================================== */

tp_status tp_project_save_buffer(const tp_project *p, char **out, size_t *out_len, tp_error *err) {
    if (!p || !out || !out_len) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_save_buffer: NULL argument");
    }
    *out = NULL;
    *out_len = 0;
    tp_pack_settings defaults;
    tp_pack_settings_defaults(&defaults);
    tp_sb sb = {0};
    tp_emit_root(&sb, p, &defaults);
    if (sb.oom) {
        free(sb.buf);
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_save_buffer: out of memory building JSON");
    }
    *out = sb.buf;
    *out_len = sb.len;
    return TP_STATUS_OK;
}

tp_status tp_project_save(tp_project *p, const char *path, tp_error *err) {
    if (!p || !path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_save: NULL project or path");
    }

    char new_dir[TP_PATH_MAX];
    tp_status st = tp_abs_dir_of(path, new_dir, sizeof new_dir);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "tp_project_save: path too long: %s", path);
    }

    /* Relativize absolute sources against the (new) project dir; normalize the
     * rest to '/'. Absolute paths not relatable to new_dir are kept as-is. */
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        for (int si = 0; si < a->source_count; si++) {
            char norm[TP_PATH_MAX];
            if ((size_t)snprintf(norm, sizeof norm, "%s", a->sources[si]) >= sizeof norm) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_save: source path too long");
            }
            tp_normalize_slashes(norm);
            char rel[TP_PATH_MAX];
            if (tp_path_is_absolute(norm)) {
                st = tp_relativize(norm, new_dir, rel, sizeof rel);
                if (st != TP_STATUS_OK) {
                    return tp_error_set(err, st, "tp_project_save: cannot relativize %s", norm);
                }
            } else if ((size_t)snprintf(rel, sizeof rel, "%s", norm) >= sizeof rel) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_save: source path too long");
            }
            char *copy = tp_strdup(rel);
            if (!copy) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_project_save: out of memory");
            }
            free(a->sources[si]);
            a->sources[si] = copy;
        }
    }

    /* Update the in-memory project dir (Save / Save-As). */
    char *dir_copy = tp_strdup(new_dir);
    if (!dir_copy) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_save: out of memory");
    }
    free(p->project_dir);
    p->project_dir = dir_copy;

    /* file-save = relativize (above) + buffer-write + fwrite. */
    char *buf = NULL;
    size_t len = 0;
    tp_status bst = tp_project_save_buffer(p, &buf, &len, err);
    if (bst != TP_STATUS_OK) {
        return bst;
    }

    FILE *f = fopen(path, "wb"); /* binary: keep LF, no CRLF translation */
    if (!f) {
        free(buf);
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_save: cannot open %s for writing", path);
    }
    const size_t wrote = fwrite(buf, 1U, len, f);
    (void)fclose(f);
    free(buf);
    if (wrote != len) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_save: short write to %s", path);
    }
    return TP_STATUS_OK;
}

/* ======================================================================== */
/* load                                                                     */
/* ======================================================================== */

static tp_status tp_opt_int(const cJSON *o, const char *k, int *dst, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!it) {
        return TP_STATUS_OK;
    }
    if (!cJSON_IsNumber(it)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "field '%s' must be a number", k);
    }
    *dst = (int)it->valuedouble;
    return TP_STATUS_OK;
}

static tp_status tp_opt_float(const cJSON *o, const char *k, float *dst, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!it) {
        return TP_STATUS_OK;
    }
    if (!cJSON_IsNumber(it)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "field '%s' must be a number", k);
    }
    *dst = (float)it->valuedouble;
    return TP_STATUS_OK;
}

static tp_status tp_opt_bool(const cJSON *o, const char *k, bool *dst, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!it) {
        return TP_STATUS_OK;
    }
    if (!cJSON_IsBool(it)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "field '%s' must be a boolean", k);
    }
    *dst = cJSON_IsTrue(it) ? true : false;
    return TP_STATUS_OK;
}

/* Parse a v2 structural shape-ID at `key` into *out, kind-checked (called ONLY for
 * v2 files). An ABSENT key leaves *out nil; since v2 files are NOT legacy-synthesized,
 * that nil reaches tp_project_validate_ids and is rejected ID_MALFORMED (a saved v2
 * always promotes to non-nil IDs). A PRESENT key must be a string holding a valid
 * "<expect>_<32hex>" that is non-nil -- a wrong type -> BAD_PROJECT, a bad shape /
 * wrong kind / nil value -> ID_MALFORMED. */
static tp_status tp_load_id(const cJSON *o, const char *key, tp_id_kind expect_kind, tp_id128 *out, tp_error *err) {
    *out = tp_id128_nil();
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it) {
        return TP_STATUS_OK; /* absent -> synthesize later */
    }
    if (!cJSON_IsString(it) || !it->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "'%s' must be a shape-ID string", key);
    }
    tp_id_kind k = TP_ID_KIND_INVALID;
    tp_id128 id;
    tp_status st = tp_id_parse(it->valuestring, &k, &id, err);
    if (st != TP_STATUS_OK) {
        return st; /* TP_STATUS_ID_MALFORMED with a specific reason in err */
    }
    if (k != expect_kind) {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "'%s' = '%s' has the wrong kind prefix", key,
                            it->valuestring);
    }
    if (tp_id128_is_nil(id)) {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "'%s' is a nil structural id", key);
    }
    *out = id;
    return TP_STATUS_OK;
}

static tp_status tp_load_sprite(tp_project_atlas *a, const cJSON *js, tp_error *err) {
    if (!cJSON_IsObject(js)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite override must be an object");
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(js, "name");
    if (!cJSON_IsString(name) || !name->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite override missing string 'name'");
    }
    tp_project_sprite *s = NULL;
    tp_status st = tp_project_atlas_add_sprite(a, name->valuestring, &s);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "out of memory adding sprite override");
    }
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(js, "origin");
    if (origin) {
        if (!cJSON_IsArray(origin) || cJSON_GetArraySize(origin) != 2) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite 'origin' must be a [x,y] array");
        }
        s->origin_x = (float)cJSON_GetArrayItem(origin, 0)->valuedouble;
        s->origin_y = (float)cJSON_GetArrayItem(origin, 1)->valuedouble;
    }
    const cJSON *rename = cJSON_GetObjectItemCaseSensitive(js, "rename");
    if (rename) {
        if (!cJSON_IsString(rename) || !rename->valuestring) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite 'rename' must be a string");
        }
        free(s->rename);
        s->rename = tp_strdup(rename->valuestring);
        if (!s->rename) {
            return tp_error_set(err, TP_STATUS_OOM, "out of memory reading sprite rename");
        }
    }
    const cJSON *slice9 = cJSON_GetObjectItemCaseSensitive(js, "slice9");
    if (slice9) {
        if (!cJSON_IsArray(slice9) || cJSON_GetArraySize(slice9) != 4) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite 'slice9' must be a [l,r,t,b] array");
        }
        for (int k = 0; k < 4; k++) {
            s->slice9_lrtb[k] = (uint16_t)cJSON_GetArrayItem(slice9, k)->valuedouble;
        }
    }
    /* Per-sprite packing overrides (absent = inherit, already seeded to -1). Values
     * are read verbatim; tp_pack validates the ranges (kept lenient here). */
    static const struct {
        const char *key;
        size_t offset;
    } ov_fields[] = {
        {"shape", offsetof(tp_project_sprite, ov_shape)},
        {"allow_rotate", offsetof(tp_project_sprite, ov_allow_rotate)},
        {"max_vertices", offsetof(tp_project_sprite, ov_max_vertices)},
        {"margin", offsetof(tp_project_sprite, ov_margin)},
        {"extrude", offsetof(tp_project_sprite, ov_extrude)},
    };
    for (size_t i = 0; i < sizeof ov_fields / sizeof ov_fields[0]; i++) {
        const cJSON *jv = cJSON_GetObjectItemCaseSensitive(js, ov_fields[i].key);
        if (jv) {
            if (!cJSON_IsNumber(jv)) {
                return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite override '%s' must be a number",
                                    ov_fields[i].key);
            }
            *(int16_t *)((char *)s + ov_fields[i].offset) = (int16_t)jv->valuedouble;
        }
    }
    return TP_STATUS_OK;
}

static tp_status tp_load_anim(tp_project_atlas *a, const cJSON *ja, bool v2, tp_error *err) {
    if (!cJSON_IsObject(ja)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation must be an object");
    }
    /* id/name split: v2 stores the logical name under "name" and a shape-ID under
     * "id"; v1 stored the logical name under "id" (which migrates into `name`). */
    const char *name_key = v2 ? "name" : "id";
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(ja, name_key);
    if (!cJSON_IsString(nm) || !nm->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation missing string '%s'", name_key);
    }
    tp_project_anim *an = NULL;
    tp_status st = tp_project_atlas_add_animation(a, nm->valuestring, &an);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "out of memory adding animation");
    }
    if (v2 && (st = tp_load_id(ja, "id", TP_ID_KIND_ANIM, &an->id, err)) != TP_STATUS_OK) {
        return st;
    }
    const cJSON *frames = cJSON_GetObjectItemCaseSensitive(ja, "frames");
    if (frames) {
        if (!cJSON_IsArray(frames)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation 'frames' must be an array");
        }
        const cJSON *fr = NULL;
        cJSON_ArrayForEach(fr, frames) {
            if (!cJSON_IsString(fr) || !fr->valuestring) {
                return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation frame must be a string");
            }
            st = tp_project_anim_add_frame(an, fr->valuestring);
            if (st != TP_STATUS_OK) {
                return tp_error_set(err, st, "out of memory adding animation frame");
            }
        }
    }
    if ((st = tp_opt_float(ja, "fps", &an->fps, err)) != TP_STATUS_OK) {
        return st;
    }
    if ((st = tp_opt_int(ja, "playback", &an->playback, err)) != TP_STATUS_OK) {
        return st;
    }
    if ((st = tp_opt_bool(ja, "flip_h", &an->flip_h, err)) != TP_STATUS_OK) {
        return st;
    }
    if ((st = tp_opt_bool(ja, "flip_v", &an->flip_v, err)) != TP_STATUS_OK) {
        return st;
    }
    return TP_STATUS_OK;
}

static tp_status tp_load_target(tp_project_atlas *a, const cJSON *jt, bool v2, tp_error *err) {
    if (!cJSON_IsObject(jt)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "target must be an object");
    }
    const cJSON *exporter = cJSON_GetObjectItemCaseSensitive(jt, "exporter_id");
    const cJSON *out_path = cJSON_GetObjectItemCaseSensitive(jt, "out_path");
    if (!cJSON_IsString(exporter) || !exporter->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "target missing string 'exporter_id'");
    }
    if (!cJSON_IsString(out_path) || !out_path->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "target missing string 'out_path'");
    }
    tp_project_target *t = NULL;
    tp_status st = tp_project_atlas_add_target(a, exporter->valuestring, out_path->valuestring, &t);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "out of memory adding target");
    }
    if (v2 && (st = tp_load_id(jt, "id", TP_ID_KIND_TARGET, &t->id, err)) != TP_STATUS_OK) {
        return st;
    }
    return tp_opt_bool(jt, "enabled", &t->enabled, err);
}

static tp_status tp_load_atlas(tp_project *p, const cJSON *jatlas, bool v2, tp_error *err) {
    if (!cJSON_IsObject(jatlas)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas must be an object");
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(jatlas, "name");
    if (!cJSON_IsString(name) || !name->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas missing string 'name'");
    }
    int idx = 0;
    tp_status st = tp_project_add_atlas(p, name->valuestring, &idx);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "out of memory adding atlas");
    }
    tp_project_atlas *a = &p->atlases[idx];
    if (v2 && (st = tp_load_id(jatlas, "id", TP_ID_KIND_ATLAS, &a->id, err)) != TP_STATUS_OK) {
        return st;
    }

    if ((st = tp_opt_int(jatlas, "max_size", &a->max_size, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "padding", &a->padding, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "margin", &a->margin, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "extrude", &a->extrude, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "alpha_threshold", &a->alpha_threshold, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "max_vertices", &a->max_vertices, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "shape", &a->shape, err)) != TP_STATUS_OK ||
        (st = tp_opt_bool(jatlas, "allow_transform", &a->allow_transform, err)) != TP_STATUS_OK ||
        (st = tp_opt_bool(jatlas, "power_of_two", &a->power_of_two, err)) != TP_STATUS_OK ||
        (st = tp_opt_float(jatlas, "pixels_per_unit", &a->pixels_per_unit, err)) != TP_STATUS_OK) {
        return st;
    }

    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(jatlas, "sources");
    if (sources) {
        if (!cJSON_IsArray(sources)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas 'sources' must be an array");
        }
        const cJSON *src = NULL;
        cJSON_ArrayForEach(src, sources) {
            if (!cJSON_IsString(src) || !src->valuestring) {
                return tp_error_set(err, TP_STATUS_BAD_PROJECT, "source path must be a string");
            }
            st = tp_project_atlas_add_source(a, src->valuestring);
            if (st != TP_STATUS_OK) {
                return tp_error_set(err, st, "out of memory adding source");
            }
        }
    }

    const cJSON *sprites = cJSON_GetObjectItemCaseSensitive(jatlas, "sprites");
    if (sprites) {
        if (!cJSON_IsArray(sprites)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas 'sprites' must be an array");
        }
        const cJSON *js = NULL;
        cJSON_ArrayForEach(js, sprites) {
            if ((st = tp_load_sprite(a, js, err)) != TP_STATUS_OK) {
                return st;
            }
        }
    }

    const cJSON *anims = cJSON_GetObjectItemCaseSensitive(jatlas, "animations");
    if (anims) {
        if (!cJSON_IsArray(anims)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas 'animations' must be an array");
        }
        const cJSON *ja = NULL;
        cJSON_ArrayForEach(ja, anims) {
            if ((st = tp_load_anim(a, ja, v2, err)) != TP_STATUS_OK) {
                return st;
            }
        }
    }

    const cJSON *targets = cJSON_GetObjectItemCaseSensitive(jatlas, "targets");
    if (targets) {
        if (!cJSON_IsArray(targets)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas 'targets' must be an array");
        }
        const cJSON *jt = NULL;
        cJSON_ArrayForEach(jt, targets) {
            if ((st = tp_load_target(a, jt, v2, err)) != TP_STATUS_OK) {
                return st;
            }
        }
    }
    return TP_STATUS_OK;
}

/* Chained-migration hook: bring an older schema up to the current version.
 * Empty at v1 (no prior schema exists); any version below 1 is unsupported. */
static tp_status tp_migrate(int from_version, tp_error *err) {
    int v = from_version;
    while (v < TP_PROJECT_SCHEMA_VERSION) {
        switch (v) {
            case 1:
                /* v1 -> v2: persistent structural IDs (atlas/animation/target) +
                 * the animation id/name split. The v1 tree carries no IDs and
                 * stores the animation's logical name under "id"; the model-level
                 * loader reads that shape (via the v2==false path) and the
                 * post-parse legacy-synthesis pass assigns deterministic IDs.
                 * Nothing to rewrite in the parsed cJSON tree here. */
                v = 2;
                break;
            /* case N: migrate N -> N+1 in the parsed tree; v = N+1; break; */
            default:
                return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                    "unsupported project schema version %d", from_version);
        }
    }
    return TP_STATUS_OK;
}

static char *tp_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    const long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1U);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1U, (size_t)size, f);
    (void)fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* Parse core shared by load (from file) + load_buffer (from memory). Borrows
 * `text` (does not free it); leaves project_dir NULL -- the file loader sets it. */
static tp_status tp_project_parse(const char *text, size_t len, tp_project **out, tp_error *err) {
    *out = NULL;

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, len, &parse_end, 0);
    if (!root) {
        const char *ep = cJSON_GetErrorPtr();
        const long off = (ep && ep >= text) ? (long)(ep - text) : -1L;
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: malformed JSON near offset %ld", off);
    }

    tp_status status = TP_STATUS_OK;
    tp_project *p = NULL;

    if (!cJSON_IsObject(root)) {
        status = tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: root must be an object");
        goto done;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version)) {
        status = tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: missing integer 'version'");
        goto done;
    }
    const int file_version = (int)version->valuedouble;
    if (file_version > TP_PROJECT_SCHEMA_VERSION) {
        status = tp_error_set(err, TP_STATUS_BAD_VERSION,
                              "project schema version %d needs a newer ntpacker (this build supports %d)",
                              file_version, TP_PROJECT_SCHEMA_VERSION);
        goto done;
    }
    if (file_version < TP_PROJECT_SCHEMA_VERSION) {
        status = tp_migrate(file_version, err);
        if (status != TP_STATUS_OK) {
            goto done;
        }
    }

    p = tp_project_alloc_empty();
    if (!p) {
        status = tp_error_set(err, TP_STATUS_OOM, "tp_project_load: out of memory");
        goto done;
    }

    /* v2+ entities carry shape-IDs (atlas/anim `id` = structural id, `name` =
     * logical). A v1 file has none and stores the anim's logical name under "id". */
    const bool v2 = (file_version >= 2);

    const cJSON *atlases = cJSON_GetObjectItemCaseSensitive(root, "atlases");
    if (atlases) {
        if (!cJSON_IsArray(atlases)) {
            status = tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: 'atlases' must be an array");
            goto done;
        }
        const cJSON *ja = NULL;
        cJSON_ArrayForEach(ja, atlases) {
            status = tp_load_atlas(p, ja, v2, err);
            if (status != TP_STATUS_OK) {
                goto done;
            }
        }
    }

    /* Resolve structural IDs. ONLY a v1 (id-less) file gets deterministic legacy
     * synthesis: repeated read-only loads then see stable IDs (master spec §5.5),
     * and a writable session later replaces nils via tp_project_promote_ids (a
     * no-op once every ID is non-nil, so it never re-changes a loaded ID). A v2
     * file is NOT synthesized: a properly-saved v2 always carries non-nil IDs
     * (promote guarantees it), so a nil/missing structural id is a genuine anomaly
     * that must fail loud -- it reaches tp_project_validate_ids and is rejected
     * TP_STATUS_ID_MALFORMED (ADR 0007 point 4), never silently repaired. */
    if (!v2) {
        status = tp_project_assign_legacy_ids(p, err);
        if (status != TP_STATUS_OK) {
            goto done;
        }
    }
    status = tp_project_validate_ids(p, err);
    if (status != TP_STATUS_OK) {
        goto done;
    }

done:
    cJSON_Delete(root);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(p);
        return status;
    }
    *out = p;
    return TP_STATUS_OK;
}

tp_status tp_project_load(const char *path, tp_project **out, tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (!path || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_load: NULL path or out");
    }

    size_t len = 0;
    char *text = tp_read_file(path, &len);
    if (!text) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: cannot open %s", path);
    }

    tp_status status = tp_project_parse(text, len, out, err);
    free(text);
    if (status != TP_STATUS_OK) {
        return status;
    }

    char dir[TP_PATH_MAX];
    status = tp_abs_dir_of(path, dir, sizeof dir);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(*out);
        *out = NULL;
        return tp_error_set(err, status, "tp_project_load: path too long: %s", path);
    }
    (*out)->project_dir = tp_strdup(dir);
    if (!(*out)->project_dir) {
        tp_project_destroy(*out);
        *out = NULL;
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_load: out of memory");
    }
    return TP_STATUS_OK;
}

tp_status tp_project_load_buffer(const char *buf, size_t len, tp_project **out, tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (!buf || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_load_buffer: NULL buf or out");
    }
    return tp_project_parse(buf, len, out, err); /* project_dir stays NULL */
}

/* ======================================================================== */
/* path resolve + packing bridge                                            */
/* ======================================================================== */

tp_status tp_project_resolve_path(const tp_project *p, const char *rel, char *out_abs, size_t cap) {
    if (!p || !rel || !out_abs || cap == 0) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (tp_path_is_absolute(rel)) {
        if ((size_t)snprintf(out_abs, cap, "%s", rel) >= cap) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        tp_normalize_slashes(out_abs);
        return TP_STATUS_OK;
    }
    if (!p->project_dir) {
        return TP_STATUS_INVALID_ARGUMENT; /* no base for a relative path */
    }
    if ((size_t)snprintf(out_abs, cap, "%s/%s", p->project_dir, rel) >= cap) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_normalize_slashes(out_abs);
    return TP_STATUS_OK;
}

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
