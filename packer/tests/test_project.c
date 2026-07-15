/* Phase 3 project model + .ntpacker_project load/save (ROADMAP Phase 3).
 *
 * Covers: programmatic round-trip (deep-equal + byte-identical re-save),
 * sparse storage (defaults absent from the text), determinism (same model ->
 * identical bytes), version refusal + malformed + forward-compat, absolute->
 * relative path handling with '/' separators, resolve_path, and the
 * atlas->tp_pack_settings bridge. A scratch dir is passed as argv[1].
 *
 * Unity constraint: no raw float equality asserts -- fabsf epsilon only. */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TP_TEST_MKDIR(p) _mkdir(p)
#define TP_TEST_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TP_TEST_MKDIR(p) mkdir((p), 0777)
#define TP_TEST_RMDIR(p) rmdir(p)
#endif

#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_id.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char *g_dir;

/* A writable session assigns final IDs before saving (master spec §5.5). Tests
 * that build a project programmatically and save it promote first, so the saved
 * file carries non-nil structural IDs and round-trips. */
static void promote(tp_project *p) {
    tp_rng rng = tp_rng_os();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, NULL));
}

#define EPS 1e-6F

static void join(char *out, size_t cap, const char *name) {
    (void)snprintf(out, cap, "%s/%s", g_dir, name);
}

/* Reads a whole file into a malloc'd NUL-terminated buffer; *len excludes NUL. */
static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    TEST_ASSERT_TRUE(n >= 0);
    char *buf = (char *)malloc((size_t)n + 1U);
    TEST_ASSERT_NOT_NULL(buf);
    size_t got = fread(buf, 1U, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len) {
        *len = got;
    }
    return buf;
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(text, 1U, strlen(text), f);
    fclose(f);
}

static void feq(float a, float b) { TEST_ASSERT_TRUE(fabsf(a - b) < EPS); }

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(c);
    memcpy(c, s, n);
    return c;
}

/* --- a rich project built programmatically --- */
static tp_project *build_rich(void) {
    tp_project *p = tp_project_create(); /* one default atlas "atlas1" */
    TEST_ASSERT_NOT_NULL(p);

    /* atlas 0: "hero" -- default knobs, sources + one full sprite override + anim + targets */
    tp_project_atlas *hero = tp_project_get_atlas(p, 0);
    TEST_ASSERT_NOT_NULL(hero);
    free(hero->name);
    hero->name = dupstr("hero");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(hero, "art/hero"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(hero, "art/enemies"));

    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(hero, "hero/walk_01", &sp));
    sp->origin_x = 0.25F;
    sp->origin_y = 0.75F;
    sp->slice9_lrtb[0] = 4;
    sp->slice9_lrtb[1] = 4;
    sp->slice9_lrtb[2] = 8;
    sp->slice9_lrtb[3] = 8;
    sp->rename = dupstr("player_walk_01"); /* export-name override */
    /* per-sprite packing overrides (exercise the sparse override round-trip) */
    sp->ov_shape = 0; /* RECT */
    sp->ov_allow_rotate = 0;
    sp->ov_max_vertices = 6;
    sp->ov_margin = 3;
    sp->ov_extrude = 5;

    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(hero, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero/walk_01"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero/walk_02"));
    an->fps = 24.0F;
    an->playback = 1;
    an->flip_h = true;

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(hero, "json-neotolis", "out/hero.json", NULL));
    tp_project_target *t2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(hero, "defold", "out/hero.tpinfo", &t2));
    t2->enabled = false;

    /* atlas 1: "ui" -- non-default knobs */
    int ui_idx = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "ui", &ui_idx));
    tp_project_atlas *ui = tp_project_get_atlas(p, ui_idx);
    ui->max_size = 1024;
    ui->padding = 4;
    ui->margin = 1;
    ui->extrude = 2;
    ui->alpha_threshold = 200;
    ui->max_vertices = 8;
    ui->shape = 0;
    ui->allow_transform = false;
    ui->power_of_two = false;
    ui->pixels_per_unit = 2.0F;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(ui, "art/ui"));
    promote(p); /* assign final structural IDs before any save (writable session) */
    return p;
}

static void assert_atlas_equal(const tp_project_atlas *a, const tp_project_atlas *b) {
    TEST_ASSERT_EQUAL_STRING(a->name, b->name);
    TEST_ASSERT_TRUE(tp_id128_eq(a->id, b->id)); /* structural id survives save/reload */
    TEST_ASSERT_EQUAL_INT(a->max_size, b->max_size);
    TEST_ASSERT_EQUAL_INT(a->padding, b->padding);
    TEST_ASSERT_EQUAL_INT(a->margin, b->margin);
    TEST_ASSERT_EQUAL_INT(a->extrude, b->extrude);
    TEST_ASSERT_EQUAL_INT(a->alpha_threshold, b->alpha_threshold);
    TEST_ASSERT_EQUAL_INT(a->max_vertices, b->max_vertices);
    TEST_ASSERT_EQUAL_INT(a->shape, b->shape);
    TEST_ASSERT_EQUAL_INT(a->allow_transform ? 1 : 0, b->allow_transform ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(a->power_of_two ? 1 : 0, b->power_of_two ? 1 : 0);
    feq(a->pixels_per_unit, b->pixels_per_unit);

    TEST_ASSERT_EQUAL_INT(a->source_count, b->source_count);
    for (int i = 0; i < a->source_count; i++) {
        TEST_ASSERT_EQUAL_STRING(a->sources[i].path, b->sources[i].path);
        TEST_ASSERT_TRUE(tp_id128_eq(a->sources[i].id, b->sources[i].id)); /* source id survives save/reload */
        TEST_ASSERT_EQUAL_INT((int)a->sources[i].kind, (int)b->sources[i].kind);
    }
    TEST_ASSERT_EQUAL_INT(a->sprite_count, b->sprite_count);
    for (int i = 0; i < a->sprite_count; i++) {
        TEST_ASSERT_EQUAL_STRING(a->sprites[i].name, b->sprites[i].name);
        feq(a->sprites[i].origin_x, b->sprites[i].origin_x);
        feq(a->sprites[i].origin_y, b->sprites[i].origin_y);
        for (int k = 0; k < 4; k++) {
            TEST_ASSERT_EQUAL_UINT16(a->sprites[i].slice9_lrtb[k], b->sprites[i].slice9_lrtb[k]);
        }
        if (a->sprites[i].rename == NULL) {
            TEST_ASSERT_NULL(b->sprites[i].rename);
        } else {
            TEST_ASSERT_NOT_NULL(b->sprites[i].rename);
            TEST_ASSERT_EQUAL_STRING(a->sprites[i].rename, b->sprites[i].rename);
        }
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_shape, b->sprites[i].ov_shape);
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_allow_rotate, b->sprites[i].ov_allow_rotate);
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_max_vertices, b->sprites[i].ov_max_vertices);
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_margin, b->sprites[i].ov_margin);
        TEST_ASSERT_EQUAL_INT(a->sprites[i].ov_extrude, b->sprites[i].ov_extrude);
    }
    TEST_ASSERT_EQUAL_INT(a->animation_count, b->animation_count);
    for (int i = 0; i < a->animation_count; i++) {
        TEST_ASSERT_EQUAL_STRING(a->animations[i].name, b->animations[i].name);
        TEST_ASSERT_TRUE(tp_id128_eq(a->animations[i].id, b->animations[i].id));
        feq(a->animations[i].fps, b->animations[i].fps);
        TEST_ASSERT_EQUAL_INT(a->animations[i].playback, b->animations[i].playback);
        TEST_ASSERT_EQUAL_INT(a->animations[i].flip_h ? 1 : 0, b->animations[i].flip_h ? 1 : 0);
        TEST_ASSERT_EQUAL_INT(a->animations[i].flip_v ? 1 : 0, b->animations[i].flip_v ? 1 : 0);
        TEST_ASSERT_EQUAL_INT(a->animations[i].frame_count, b->animations[i].frame_count);
        for (int k = 0; k < a->animations[i].frame_count; k++) {
            TEST_ASSERT_EQUAL_STRING(a->animations[i].frames[k].name, b->animations[i].frames[k].name);
        }
    }
    TEST_ASSERT_EQUAL_INT(a->target_count, b->target_count);
    for (int i = 0; i < a->target_count; i++) {
        TEST_ASSERT_TRUE(tp_id128_eq(a->targets[i].id, b->targets[i].id));
        TEST_ASSERT_EQUAL_STRING(a->targets[i].exporter_id, b->targets[i].exporter_id);
        TEST_ASSERT_EQUAL_STRING(a->targets[i].out_path, b->targets[i].out_path);
        TEST_ASSERT_EQUAL_INT(a->targets[i].enabled ? 1 : 0, b->targets[i].enabled ? 1 : 0);
    }
}

static void assert_project_equal(const tp_project *a, const tp_project *b) {
    TEST_ASSERT_EQUAL_INT(a->schema_version, b->schema_version);
    TEST_ASSERT_EQUAL_INT(a->atlas_count, b->atlas_count);
    for (int i = 0; i < a->atlas_count; i++) {
        assert_atlas_equal(&a->atlases[i], &b->atlases[i]);
    }
}

/* 1. round-trip: create -> save -> load -> deep-equal -> save -> byte-identical */
void test_roundtrip_and_byte_identical(void) {
    char p1[512];
    char p2[512];
    join(p1, sizeof p1, "rt1.ntpacker_project");
    join(p2, sizeof p2, "rt2.ntpacker_project");

    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save(p, p1, &err), err.msg);

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(p1, &loaded, &err), err.msg);
    TEST_ASSERT_NOT_NULL(loaded);
    assert_project_equal(p, loaded);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(loaded, p2, &err));

    size_t n1 = 0;
    size_t n2 = 0;
    char *b1 = read_all(p1, &n1);
    char *b2 = read_all(p2, &n2);
    TEST_ASSERT_EQUAL_size_t(n1, n2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(b1, b2, n1));
    /* trailing newline + LF-only */
    TEST_ASSERT_TRUE(n1 > 0 && b1[n1 - 1] == '\n');
    TEST_ASSERT_NULL(strchr(b1, '\r'));

    free(b1);
    free(b2);
    tp_project_destroy(loaded);
    tp_project_destroy(p);
}

/* 2. sparse: default-valued fields never appear in the JSON text */
void test_sparse_defaults_absent(void) {
    char path[512];
    join(path, sizeof path, "sparse.ntpacker_project");

    tp_project *p = tp_project_create(); /* atlas "atlas1", all-default knobs */
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art"));

    tp_project_sprite *sp = NULL; /* origin only -> slice9 stays default */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "s1", &sp));
    sp->origin_x = 0.1F;

    tp_project_anim *an = NULL; /* default fps/playback/flips */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "idle", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "s1"));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", "out.json", NULL));

    promote(p);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));

    size_t n = 0;
    char *buf = read_all(path, &n);
    /* default knobs absent */
    TEST_ASSERT_NULL(strstr(buf, "padding"));
    TEST_ASSERT_NULL(strstr(buf, "margin"));
    TEST_ASSERT_NULL(strstr(buf, "max_size"));
    TEST_ASSERT_NULL(strstr(buf, "power_of_two"));
    TEST_ASSERT_NULL(strstr(buf, "allow_transform"));
    TEST_ASSERT_NULL(strstr(buf, "pixels_per_unit"));
    /* default sub-fields absent */
    TEST_ASSERT_NULL(strstr(buf, "slice9"));   /* sprite kept default slice9 */
    TEST_ASSERT_NULL(strstr(buf, "rename"));   /* sprite kept default (NULL) rename */
    TEST_ASSERT_NULL(strstr(buf, "\"fps\""));  /* anim kept default fps */
    TEST_ASSERT_NULL(strstr(buf, "playback")); /* anim kept default playback */
    TEST_ASSERT_NULL(strstr(buf, "flip_h"));
    TEST_ASSERT_NULL(strstr(buf, "enabled")); /* target enabled default true */
    /* present, non-default */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"version\": 4"));
    TEST_ASSERT_NULL(strstr(buf, "\"kind\"")); /* source kept the folder default -> kind omitted (sparse) */
    TEST_ASSERT_NOT_NULL(strstr(buf, "origin"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"frames\""));
    /* "version" must be the first key */
    TEST_ASSERT_NOT_NULL(strstr(buf, "{\n  \"version\":"));

    free(buf);
    tp_project_destroy(p);
}

/* 3. determinism: same model saved twice -> identical bytes */
void test_determinism(void) {
    char pa[512];
    char pb[512];
    join(pa, sizeof pa, "det_a.ntpacker_project");
    join(pb, sizeof pb, "det_b.ntpacker_project");

    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, pa, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, pb, &err));

    size_t na = 0;
    size_t nb = 0;
    char *ba = read_all(pa, &na);
    char *bb = read_all(pb, &nb);
    TEST_ASSERT_EQUAL_size_t(na, nb);
    TEST_ASSERT_EQUAL_INT(0, memcmp(ba, bb, na));
    free(ba);
    free(bb);
    tp_project_destroy(p);
}

/* 4a. newer schema version is refused (schema v4 is current -> use a future v5) */
void test_version_newer_refused(void) {
    char path[512];
    join(path, sizeof path, "v5.ntpacker_project");
    write_text(path, "{\n  \"version\": 5,\n  \"atlases\": []\n}\n");

    tp_project *loaded = NULL;
    tp_error err = {0};
    tp_status st = tp_project_load(path, &loaded, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_VERSION, st);
    TEST_ASSERT_NULL(loaded);
    TEST_ASSERT_TRUE(strlen(err.msg) > 0);
}

/* 4b. malformed JSON -> BAD_PROJECT */
void test_malformed_bad_project(void) {
    char path[512];
    join(path, sizeof path, "garbage.ntpacker_project");
    write_text(path, "{ this is not valid json ]");

    tp_project *loaded = NULL;
    tp_error err = {0};
    tp_status st = tp_project_load(path, &loaded, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT, st);
    TEST_ASSERT_NULL(loaded);
}

/* 4c. wrong-typed field -> BAD_PROJECT */
void test_wrong_type_bad_project(void) {
    char path[512];
    join(path, sizeof path, "wrongtype.ntpacker_project");
    write_text(path, "{\n  \"version\": 1,\n  \"atlases\": [ { \"name\": \"a\", \"padding\": \"oops\" } ]\n}\n");

    tp_project *loaded = NULL;
    tp_error err = {0};
    tp_status st = tp_project_load(path, &loaded, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT, st);
    TEST_ASSERT_NULL(loaded);
}

/* 4d. unknown extra keys ignored (forward compat) */
void test_unknown_keys_ignored(void) {
    char path[512];
    join(path, sizeof path, "future.ntpacker_project");
    write_text(path,
               "{\n"
               "  \"version\": 1,\n"
               "  \"future_root\": 42,\n"
               "  \"atlases\": [ { \"name\": \"a\", \"future_atlas\": true, \"padding\": 3 } ]\n"
               "}\n");

    tp_project *loaded = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &loaded, &err), err.msg);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQUAL_INT(1, loaded->atlas_count);
    TEST_ASSERT_EQUAL_STRING("a", loaded->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(3, loaded->atlases[0].padding);
    tp_project_destroy(loaded);
}

/* 5. absolute source path loads; save relativizes it; output uses '/' */
void test_absolute_path_relativized(void) {
    char norm_dir[512];
    (void)snprintf(norm_dir, sizeof norm_dir, "%s", g_dir);
    for (char *c = norm_dir; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }

    /* an absolute source under the project dir -> relativizes to "art/hero" */
    char json[1600];
    (void)snprintf(json, sizeof json,
                   "{\n  \"version\": 1,\n  \"atlases\": [ { \"name\": \"a\", \"sources\": [ \"%s/art/hero\" ] } ]\n}\n",
                   norm_dir);

    char path[512];
    join(path, sizeof path, "abs.ntpacker_project");
    write_text(path, json);

    tp_project *loaded = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &loaded, &err), err.msg);
    TEST_ASSERT_NOT_NULL(loaded);
    /* loaded model still holds the absolute path (relativize is a save step) */
    TEST_ASSERT_EQUAL_INT(1, loaded->atlases[0].source_count);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(loaded, path, &err));

    size_t n = 0;
    char *buf = read_all(path, &n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"art/hero\"")); /* relativized */
    TEST_ASSERT_NULL(strchr(buf, '\\'));               /* '/' separators only */
    TEST_ASSERT_NULL(strstr(buf, norm_dir));           /* absolute prefix gone */
    free(buf);

    /* model source is now the relative form */
    TEST_ASSERT_EQUAL_STRING("art/hero", loaded->atlases[0].sources[0].path);
    tp_project_destroy(loaded);
}

/* 5b. resolve_path joins relative onto project_dir; passes absolute through */
void test_resolve_path(void) {
    char path[512];
    join(path, sizeof path, "resolve.ntpacker_project");

    tp_project *p = tp_project_create();
    promote(p);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err)); /* sets project_dir */
    TEST_ASSERT_NOT_NULL(p->project_dir);

    char out[1024];
    char expect[1024];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_resolve_path(p, "art/hero.png", out, sizeof out));
    (void)snprintf(expect, sizeof expect, "%s/art/hero.png", p->project_dir);
    TEST_ASSERT_EQUAL_STRING(expect, out);

    /* absolute passes through (normalized to '/') */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_resolve_path(p, "C:/abs/x.png", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("C:/abs/x.png", out);

    tp_project_destroy(p);
}

/* 6. atlas -> tp_pack_settings mapping; defaults flow from one place */
void test_to_settings_mapping(void) {
    tp_project *p = build_rich();

    tp_pack_settings s;
    tp_error err = {0};
    /* atlas 1 "ui" has the non-default knobs */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_to_settings(p, 1, &s, &err));
    TEST_ASSERT_EQUAL_STRING("ui", s.atlas_name);
    TEST_ASSERT_EQUAL_INT(1024, s.max_size);
    TEST_ASSERT_EQUAL_INT(4, s.padding);
    TEST_ASSERT_EQUAL_INT(1, s.margin);
    TEST_ASSERT_EQUAL_INT(2, s.extrude);
    TEST_ASSERT_EQUAL_INT(200, s.alpha_threshold);
    TEST_ASSERT_EQUAL_INT(8, s.max_vertices);
    TEST_ASSERT_EQUAL_INT(0, s.shape);
    TEST_ASSERT_FALSE(s.allow_transform);
    TEST_ASSERT_FALSE(s.power_of_two);
    feq(s.pixels_per_unit, 2.0F);
    /* the bridge fills knobs only -- work_dir/sprites are the call site's job */
    TEST_ASSERT_NULL(s.work_dir);
    TEST_ASSERT_NULL(s.sprites);
    TEST_ASSERT_EQUAL_INT(0, s.sprite_count);

    /* all-default atlas 0 "hero" -> knobs equal the shared defaults */
    tp_pack_settings def;
    tp_pack_settings_defaults(&def);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_to_settings(p, 0, &s, &err));
    TEST_ASSERT_EQUAL_INT(def.max_size, s.max_size);
    TEST_ASSERT_EQUAL_INT(def.padding, s.padding);
    TEST_ASSERT_EQUAL_INT(def.shape, s.shape);
    TEST_ASSERT_EQUAL_INT(def.allow_transform ? 1 : 0, s.allow_transform ? 1 : 0);
    feq(s.pixels_per_unit, def.pixels_per_unit);

    /* out-of-range index -> error, not a crash */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_atlas_to_settings(p, 9, &s, &err));

    tp_project_destroy(p);
}

/* 7. mutation helpers: add/find/remove behave and keep the model consistent */
void test_mutation_helpers(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "dup", NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "dup", NULL)); /* idempotent */
    TEST_ASSERT_EQUAL_INT(1, a->sprite_count);
    TEST_ASSERT_NOT_NULL(tp_project_atlas_find_sprite(a, "dup"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_remove_sprite(a, "dup"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_atlas_remove_sprite(a, "dup"));
    TEST_ASSERT_NULL(tp_project_atlas_find_sprite(a, "dup"));

    int idx = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "extra", &idx));
    TEST_ASSERT_EQUAL_INT(2, p->atlas_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_remove_atlas(p, idx));
    TEST_ASSERT_EQUAL_INT(1, p->atlas_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_remove_atlas(p, 5));

    tp_project_destroy(p);
}

/* 7b. animation frame edit: add preserves order; remove shifts; move reorders with clamping. */
void test_anim_frame_edit(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);

    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f0"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f1"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f2"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f3"));
    TEST_ASSERT_EQUAL_INT(4, an->frame_count);

    /* move first frame down by 2 -> f1,f2,f0,f3 */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 0, 2));
    TEST_ASSERT_EQUAL_STRING("f1", an->frames[0].name);
    TEST_ASSERT_EQUAL_STRING("f2", an->frames[1].name);
    TEST_ASSERT_EQUAL_STRING("f0", an->frames[2].name);
    TEST_ASSERT_EQUAL_STRING("f3", an->frames[3].name);

    /* move last frame up by 1 -> f1,f2,f3,f0 */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 3, -1));
    TEST_ASSERT_EQUAL_STRING("f3", an->frames[2].name);
    TEST_ASSERT_EQUAL_STRING("f0", an->frames[3].name);

    /* over-clamp: moving index 0 up stays put (no-op), moving down past the end clamps to last */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 0, -5));
    TEST_ASSERT_EQUAL_STRING("f1", an->frames[0].name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 0, 99));
    TEST_ASSERT_EQUAL_STRING("f1", an->frames[3].name); /* f1 rode to the end */

    /* remove the middle -> shifts the tail down */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_remove_frame(an, 1));
    TEST_ASSERT_EQUAL_INT(3, an->frame_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_anim_remove_frame(an, 9));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_anim_move_frame(an, 9, 1));

    tp_project_destroy(p);
}

/* 8. buffer save/load: save_buffer bytes == file bytes; load_buffer deep-equals a
 * from-file load (project_dir aside). Sources are relative, so file == buffer. */
void test_buffer_roundtrip(void) {
    char p1[512];
    join(p1, sizeof p1, "buf1.ntpacker_project");

    tp_project *p = build_rich();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save(p, p1, &err), err.msg);

    size_t nf = 0;
    char *bf = read_all(p1, &nf);

    char *bb = NULL;
    size_t nb = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save_buffer(p, &bb, &nb, &err), err.msg);
    TEST_ASSERT_NOT_NULL(bb);
    TEST_ASSERT_EQUAL_size_t(nf, nb);
    TEST_ASSERT_EQUAL_INT(0, memcmp(bf, bb, nf));

    tp_project *from_file = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(p1, &from_file, &err), err.msg);
    tp_project *from_buf = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load_buffer(bb, nb, &from_buf, &err), err.msg);
    TEST_ASSERT_NULL(from_buf->project_dir);  /* buffer load leaves it NULL */
    TEST_ASSERT_NOT_NULL(from_file->project_dir);
    assert_project_equal(from_file, from_buf);

    free(bf);
    free(bb);
    tp_project_destroy(from_file);
    tp_project_destroy(from_buf);
    tp_project_destroy(p);
}

/* 9. add_source dedupe: an exact ('/'-normalized) duplicate is an OK no-op. */
void test_add_source_dedupe(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art/hero"));
    TEST_ASSERT_EQUAL_INT(1, a->source_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art/hero")); /* dup no-op */
    TEST_ASSERT_EQUAL_INT(1, a->source_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art\\hero")); /* normalizes equal */
    TEST_ASSERT_EQUAL_INT(1, a->source_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art/ui")); /* distinct -> appends */
    TEST_ASSERT_EQUAL_INT(2, a->source_count);
    tp_project_destroy(p);
}

/* 9b (F1-02). tagged source round-trip: a folder source (default kind, omitted) and
 * a file source (kind=file, written) survive save/reload with id + kind intact. */
void test_source_kind_roundtrip(void) {
    char path[512];
    join(path, sizeof path, "srckind.ntpacker_project");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art/folder"));            /* folder (default) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source_kind(a, "art/one.png", TP_SOURCE_KIND_FILE)); /* file */
    TEST_ASSERT_EQUAL_INT(2, a->source_count);
    promote(p); /* assign real ids to the fresh sources */
    TEST_ASSERT_FALSE(tp_id128_is_nil(a->sources[0].id));
    TEST_ASSERT_FALSE(tp_id128_is_nil(a->sources[1].id));

    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));

    /* on disk: the file source writes "kind": "file"; the folder source omits kind. */
    size_t n = 0;
    char *buf = read_all(path, &n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\": \"file\""));
    TEST_ASSERT_NULL(strstr(buf, "\"kind\": \"folder\"")); /* folder default is sparse */
    free(buf);

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &loaded, &err), err.msg);
    tp_project_atlas *la = tp_project_get_atlas(loaded, 0);
    TEST_ASSERT_EQUAL_INT(2, la->source_count);
    TEST_ASSERT_EQUAL_INT((int)TP_SOURCE_KIND_FOLDER, (int)la->sources[0].kind);
    TEST_ASSERT_EQUAL_INT((int)TP_SOURCE_KIND_FILE, (int)la->sources[1].kind);
    TEST_ASSERT_TRUE(tp_id128_eq(a->sources[0].id, la->sources[0].id)); /* id persisted */
    TEST_ASSERT_TRUE(tp_id128_eq(a->sources[1].id, la->sources[1].id));

    tp_project_destroy(loaded);
    tp_project_destroy(p);
}

/* 9c (F1-02 Fix A). A hand-written/corrupt v3 file that lists the same path twice
 * (two source objects with distinct valid ids) loads with the duplicate collapsed
 * to the FIRST object -- else `pack` (which does NOT run validate) would scan the
 * folder twice and double every sprite. The later object's id is dropped. */
void test_v3_source_dup_collapse(void) {
    /* 32-hex ids written as 4x8 concatenated literals so the length is self-evident. */
    static const char v3_dup[] =
        "{\n"
        "  \"version\": 3,\n"
        "  \"atlases\": [\n"
        "    {\n"
        "      \"name\": \"a1\",\n"
        "      \"id\": \"atlas_"
        "00000000"
        "00000000"
        "00000000"
        "00000abc"
        "\",\n"
        "      \"sources\": [\n"
        "        { \"id\": \"source_"
        "00000000"
        "00000000"
        "00000000"
        "00000001"
        "\", \"path\": \"dup/path\" },\n"
        "        { \"id\": \"source_"
        "00000000"
        "00000000"
        "00000000"
        "00000002"
        "\", \"path\": \"dup/path\" }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    tp_project *p = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load_buffer(v3_dup, sizeof v3_dup - 1U, &p, &err), err.msg);
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(1, a->source_count); /* duplicate path collapsed */
    TEST_ASSERT_EQUAL_STRING("dup/path", a->sources[0].path);

    /* The FIRST object survives; the later object's id is dropped. */
    tp_id_kind k = TP_ID_KIND_INVALID;
    tp_id128 id1;
    tp_id128 id2;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_id_parse("source_"
                                      "00000000"
                                      "00000000"
                                      "00000000"
                                      "00000001",
                                      &k, &id1, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_id_parse("source_"
                                      "00000000"
                                      "00000000"
                                      "00000000"
                                      "00000002",
                                      &k, &id2, NULL));
    TEST_ASSERT_TRUE(tp_id128_eq(a->sources[0].id, id1));
    TEST_ASSERT_FALSE(tp_id128_eq(a->sources[0].id, id2));

    tp_project_destroy(p);
}

/* 10. sprite rename override: set creates the sparse entry; clear removes it when
 * the entry would then hold only defaults, else keeps it. */
void test_sprite_rename_override(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_sprite_rename(a, "walk_01", "hero_walk"));
    tp_project_sprite *s = tp_project_atlas_find_sprite(a, "walk_01");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("hero_walk", s->rename);
    TEST_ASSERT_EQUAL_INT(1, a->sprite_count);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_sprite_rename(a, "walk_01", NULL)); /* clears + drops */
    TEST_ASSERT_NULL(tp_project_atlas_find_sprite(a, "walk_01"));
    TEST_ASSERT_EQUAL_INT(0, a->sprite_count);

    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "walk_02", &sp));
    sp->origin_x = 0.1F; /* non-default -> the entry survives a rename clear */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_sprite_rename(a, "walk_02", "hero_walk2"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_sprite_rename(a, "walk_02", ""));
    sp = tp_project_atlas_find_sprite(a, "walk_02");
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_NULL(sp->rename);

    tp_project_destroy(p);
}

/* 11. set_target replaces fields + is deterministic; sparse enabled default holds. */
void test_set_target(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    tp_project_target *t = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", "out/a.json", &t));
    TEST_ASSERT_TRUE(t->enabled);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_target(a, 0, "defold", "out/a.tpinfo", false));
    TEST_ASSERT_EQUAL_STRING("defold", a->targets[0].exporter_id);
    TEST_ASSERT_EQUAL_STRING("out/a.tpinfo", a->targets[0].out_path);
    TEST_ASSERT_FALSE(a->targets[0].enabled);

    /* bounds + arg validation */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_atlas_set_target(a, 3, "defold", "x", true));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_project_atlas_set_target(a, 0, "", "x", true));
    tp_project_destroy(p);
}

/* C3: the shared project-wide duplicate-out_path detector. Two atlases whose targets
 * export to one out_path silently overwrite each other -- tp_project_out_path_shared is
 * the single source of truth CLI `validate` and the GUI target panel both surface. */
void test_out_path_shared(void) {
    tp_project *p = tp_project_create();
    /* atlas 0 (default "atlas1"): a target at out/x + one at a unique path. */
    tp_project_atlas *a0 = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a0, "defold", "out/x", NULL));       /* [0] */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a0, "json-neotolis", "out/uniq", NULL)); /* [1] */
    /* a SECOND atlas: a target REUSING out/x (cross-atlas collision), a unique out/y, and a target that
     * ALSO writes out/uniq but is DISABLED below (must NOT make out/uniq collide -- the exporter skips it). */
    int a1i = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "atlas2", &a1i));
    tp_project_atlas *a1 = tp_project_get_atlas(p, a1i);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a1, "defold", "out/x", NULL));       /* [0] */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a1, "json-neotolis", "out/y", NULL)); /* [1] */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a1, "defold", "out/uniq", NULL));    /* [2] disabled */

    /* NO promote(): ids stay NIL, proving the detector excludes `self` by POINTER identity (review finding
     * on the nil-id self-match), not by id. */
    a0 = tp_project_get_atlas(p, 0);
    a1 = tp_project_get_atlas(p, a1i);
    a1->targets[2].enabled = false; /* the 2nd out/uniq writer is OFF -> the exporter skips it */
    const tp_project_target *x0 = &a0->targets[0];    /* enabled, out/x */
    const tp_project_target *uniq0 = &a0->targets[1]; /* enabled, out/uniq */
    const tp_project_target *x1 = &a1->targets[0];    /* enabled, out/x (cross-atlas dup) */
    const tp_project_target *y1 = &a1->targets[1];    /* enabled, out/y */

    /* out/x is shared by two ENABLED targets across atlases: each sees the OTHER. */
    TEST_ASSERT_TRUE(tp_project_out_path_shared(p, "out/x", x0));
    TEST_ASSERT_TRUE(tp_project_out_path_shared(p, "out/x", x1));
    /* separator normalization: a backslash spelling resolves to the same file the exporter writes. */
    TEST_ASSERT_TRUE(tp_project_out_path_shared(p, "out\\x", x0));
    /* out/uniq's only OTHER writer is DISABLED -> no live overwrite. */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, "out/uniq", uniq0));
    /* a unique path never collides; self excluded by identity. */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, "out/y", y1));
    /* a path present on NO enabled target is not shared. */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, "out/absent", NULL));
    /* empty / NULL out_path never collides (that is the separate empty-out_path check). */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, "", NULL));
    TEST_ASSERT_FALSE(tp_project_out_path_shared(p, NULL, NULL));
    /* NULL project -> false. */
    TEST_ASSERT_FALSE(tp_project_out_path_shared(NULL, "out/x", NULL));

    tp_project_destroy(p);
}

/* 12. per-sprite packing overrides: absent stays absent (sparse), present round-trips
 * and re-saves byte-identical. */
void test_sprite_override_sparse(void) {
    char path[512];
    char path2[512];
    join(path, sizeof path, "ov1.ntpacker_project");
    join(path2, sizeof path2, "ov2.ntpacker_project");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "s1", &sp));
    /* defaults inherit -> all override keys absent */
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sp->ov_shape);
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sp->ov_extrude);
    sp->ov_shape = 0;        /* RECT */
    sp->ov_max_vertices = 4; /* explicit */

    promote(p);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));
    size_t n = 0;
    char *buf = read_all(path, &n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"shape\""));        /* present override */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"max_vertices\"")); /* present override */
    TEST_ASSERT_NULL(strstr(buf, "\"margin\""));           /* inherited -> absent */
    TEST_ASSERT_NULL(strstr(buf, "\"allow_rotate\""));     /* inherited -> absent */
    free(buf);

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load(path, &loaded, &err));
    tp_project_sprite *ls = tp_project_atlas_find_sprite(tp_project_get_atlas(loaded, 0), "s1");
    TEST_ASSERT_NOT_NULL(ls);
    TEST_ASSERT_EQUAL_INT(0, ls->ov_shape);
    TEST_ASSERT_EQUAL_INT(4, ls->ov_max_vertices);
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, ls->ov_margin);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(loaded, path2, &err));
    size_t n1 = 0;
    size_t n2 = 0;
    char *b1 = read_all(path, &n1);
    char *b2 = read_all(path2, &n2);
    TEST_ASSERT_EQUAL_size_t(n1, n2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(b1, b2, n1));
    free(b1);
    free(b2);
    tp_project_destroy(loaded);
    tp_project_destroy(p);
}

/* 13. default-target seed helper (A5): seeds one json-neotolis target at
 * "out/<name>"; tp_project_create itself stays target-free (L-5). */
void test_seed_default_target(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, a->target_count, "tp_project_create must stay target-free (L-5)");

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_seed_default_target(p, 0));
    TEST_ASSERT_EQUAL_INT(1, a->target_count);
    TEST_ASSERT_EQUAL_STRING(TP_EXPORTER_ID_JSON_NEOTOLIS, a->targets[0].exporter_id);
    char want[128];
    (void)snprintf(want, sizeof want, "out/%s", a->name);
    TEST_ASSERT_EQUAL_STRING(want, a->targets[0].out_path);
    TEST_ASSERT_TRUE(a->targets[0].enabled);

    /* out-of-range atlas -> OUT_OF_BOUNDS, never a crash. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_project_atlas_seed_default_target(p, 5));
    tp_project_destroy(p);
}

/* 14. B4 trivial mutator: rename an atlas in place; reject empty/NULL. */
void test_set_atlas_name(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_STRING("atlas1", a->name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_set_atlas_name(a, "renamed"));
    TEST_ASSERT_EQUAL_STRING("renamed", a->name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_project_set_atlas_name(a, ""));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_project_set_atlas_name(a, NULL));
    TEST_ASSERT_EQUAL_STRING("renamed", a->name); /* unchanged on rejection */
    tp_project_destroy(p);
}

/* 15. B4 trivial mutator: prune drops an all-default override, keeps a non-default
 * one, and no-ops on an absent entry (the `sprite set <field>=inherit` sparse path). */
void test_prune_sprite(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    tp_project_sprite *s = NULL;

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "s1", &s));
    TEST_ASSERT_EQUAL_INT(1, a->sprite_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_prune_sprite(a, "s1")); /* all-default -> dropped */
    TEST_ASSERT_EQUAL_INT(0, a->sprite_count);
    TEST_ASSERT_NULL(tp_project_atlas_find_sprite(a, "s1"));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_prune_sprite(a, "absent")); /* no-op OK */

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "s2", &s));
    s->slice9_lrtb[0] = 4; /* non-default -> survives prune */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_prune_sprite(a, "s2"));
    TEST_ASSERT_EQUAL_INT(1, a->sprite_count);
    s = tp_project_atlas_find_sprite(a, "s2");
    TEST_ASSERT_NOT_NULL(s);
    s->slice9_lrtb[0] = 0; /* cleared back to default -> now prunes */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_prune_sprite(a, "s2"));
    TEST_ASSERT_EQUAL_INT(0, a->sprite_count);
    tp_project_destroy(p);
}

/* 16. Atomic save: a FAILED tp_project_save leaves the pre-existing target file
 * byte-identical. We save a valid project, snapshot its bytes, then force the
 * next save to fail deterministically by pre-creating a NON-EMPTY *directory* at
 * the sibling `<path>.savetmp`. It must be non-empty: the core does `remove(tmp)`
 * before `fopen(tmp,"wb")`, and on POSIX remove() would rmdir an EMPTY dir (then
 * the save would succeed) -- a pinned file inside makes remove() fail (ENOTEMPTY)
 * AND fopen() fail (EISDIR/directory) on every platform. The save must return
 * non-OK and the original `path` must be untouched (byte-identical). */
void test_project_save_atomic_failure_keeps_target(void) {
    char path[512];
    char tmpdir[600];
    char pin[640];
    join(path, sizeof path, "atomic.ntpacker_project");
    (void)snprintf(tmpdir, sizeof tmpdir, "%s.savetmp", path);
    (void)snprintf(pin, sizeof pin, "%s/pin", tmpdir);

    tp_project *p = build_rich();
    tp_error err = {0};

    /* 1. initial good save */
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_save(p, path, &err), err.msg);

    /* 2. snapshot the on-disk bytes */
    size_t n1 = 0;
    char *v1 = read_all(path, &n1);
    TEST_ASSERT_TRUE(n1 > 0);

    /* 3. inject a deterministic failure: a NON-EMPTY directory where the temp file must be created */
    (void)remove(pin);           /* clear any leftover pin from a prior run */
    (void)TP_TEST_RMDIR(tmpdir); /* clear any leftover dir from a prior run */
    (void)remove(tmpdir);        /* or a leftover regular temp file */
    TEST_ASSERT_EQUAL_INT(0, TP_TEST_MKDIR(tmpdir));
    write_text(pin, "pin"); /* make it non-empty so remove() cannot rmdir it (POSIX) */

    /* 4. the save must fail (fopen of the temp cannot open a directory; remove() cannot clear it) */
    tp_error err2 = {0};
    tp_status st = tp_project_save(p, path, &err2);
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, st);

    /* 5. the pre-existing target is byte-identical -- never truncated or partially written */
    size_t n2 = 0;
    char *v2 = read_all(path, &n2);
    TEST_ASSERT_EQUAL_size_t(n1, n2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(v1, v2, n1));

    /* 6. cleanup */
    (void)remove(pin);
    (void)TP_TEST_RMDIR(tmpdir);
    (void)remove(path);
    free(v1);
    free(v2);
    tp_project_destroy(p);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    UNITY_BEGIN();
    RUN_TEST(test_seed_default_target);
    RUN_TEST(test_roundtrip_and_byte_identical);
    RUN_TEST(test_sparse_defaults_absent);
    RUN_TEST(test_determinism);
    RUN_TEST(test_version_newer_refused);
    RUN_TEST(test_malformed_bad_project);
    RUN_TEST(test_wrong_type_bad_project);
    RUN_TEST(test_unknown_keys_ignored);
    RUN_TEST(test_absolute_path_relativized);
    RUN_TEST(test_resolve_path);
    RUN_TEST(test_to_settings_mapping);
    RUN_TEST(test_mutation_helpers);
    RUN_TEST(test_anim_frame_edit);
    RUN_TEST(test_buffer_roundtrip);
    RUN_TEST(test_add_source_dedupe);
    RUN_TEST(test_source_kind_roundtrip);
    RUN_TEST(test_v3_source_dup_collapse);
    RUN_TEST(test_sprite_rename_override);
    RUN_TEST(test_set_target);
    RUN_TEST(test_out_path_shared);
    RUN_TEST(test_sprite_override_sparse);
    RUN_TEST(test_set_atlas_name);
    RUN_TEST(test_prune_sprite);
    RUN_TEST(test_project_save_atomic_failure_keeps_target);
    return UNITY_END();
}
