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

#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char *g_dir;

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
    return p;
}

static void assert_atlas_equal(const tp_project_atlas *a, const tp_project_atlas *b) {
    TEST_ASSERT_EQUAL_STRING(a->name, b->name);
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
        TEST_ASSERT_EQUAL_STRING(a->sources[i], b->sources[i]);
    }
    TEST_ASSERT_EQUAL_INT(a->sprite_count, b->sprite_count);
    for (int i = 0; i < a->sprite_count; i++) {
        TEST_ASSERT_EQUAL_STRING(a->sprites[i].name, b->sprites[i].name);
        feq(a->sprites[i].origin_x, b->sprites[i].origin_x);
        feq(a->sprites[i].origin_y, b->sprites[i].origin_y);
        for (int k = 0; k < 4; k++) {
            TEST_ASSERT_EQUAL_UINT16(a->sprites[i].slice9_lrtb[k], b->sprites[i].slice9_lrtb[k]);
        }
    }
    TEST_ASSERT_EQUAL_INT(a->animation_count, b->animation_count);
    for (int i = 0; i < a->animation_count; i++) {
        TEST_ASSERT_EQUAL_STRING(a->animations[i].id, b->animations[i].id);
        feq(a->animations[i].fps, b->animations[i].fps);
        TEST_ASSERT_EQUAL_INT(a->animations[i].playback, b->animations[i].playback);
        TEST_ASSERT_EQUAL_INT(a->animations[i].flip_h ? 1 : 0, b->animations[i].flip_h ? 1 : 0);
        TEST_ASSERT_EQUAL_INT(a->animations[i].flip_v ? 1 : 0, b->animations[i].flip_v ? 1 : 0);
        TEST_ASSERT_EQUAL_INT(a->animations[i].frame_count, b->animations[i].frame_count);
        for (int k = 0; k < a->animations[i].frame_count; k++) {
            TEST_ASSERT_EQUAL_STRING(a->animations[i].frames[k], b->animations[i].frames[k]);
        }
    }
    TEST_ASSERT_EQUAL_INT(a->target_count, b->target_count);
    for (int i = 0; i < a->target_count; i++) {
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
    TEST_ASSERT_NULL(strstr(buf, "\"fps\""));  /* anim kept default fps */
    TEST_ASSERT_NULL(strstr(buf, "playback")); /* anim kept default playback */
    TEST_ASSERT_NULL(strstr(buf, "flip_h"));
    TEST_ASSERT_NULL(strstr(buf, "enabled")); /* target enabled default true */
    /* present, non-default */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"version\": 1"));
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

/* 4a. newer schema version is refused */
void test_version_newer_refused(void) {
    char path[512];
    join(path, sizeof path, "v2.ntpacker_project");
    write_text(path, "{\n  \"version\": 2,\n  \"atlases\": []\n}\n");

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
    TEST_ASSERT_EQUAL_STRING("art/hero", loaded->atlases[0].sources[0]);
    tp_project_destroy(loaded);
}

/* 5b. resolve_path joins relative onto project_dir; passes absolute through */
void test_resolve_path(void) {
    char path[512];
    join(path, sizeof path, "resolve.ntpacker_project");

    tp_project *p = tp_project_create();
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

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    UNITY_BEGIN();
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
    return UNITY_END();
}
