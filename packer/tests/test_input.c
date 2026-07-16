/* tp_input unit tests (op-layer step A3a): project -> pack-input build, the
 * per-sprite effective-shape rule, and the non-RECT extrude clamp in
 * tp_project_atlas_to_settings.
 *
 * Ported semantics under test (was gui_pack.c assemble/desc_add, arch review §3.1):
 *   - file source -> raw name = basename WITH ext; folder source -> recursive scan,
 *     rel WITH ext, appended in scan order; per-source-then-sorted order, NO global
 *     sort (arch review R2);
 *   - override lookup by tp_sprite_export_key incl. the dotfile-in-folder key case
 *     the A1 fix repaired ("tank/.png" keys as "tank/.png", not "tank/");
 *   - effective shape: slice9 forces RECT, else ov_shape, else atlas shape; the
 *     extrude override is encoded only when the effective shape is RECT;
 *   - missing sources counted (not fatal);
 *   - the export-path clamp hole: a CONCAVE atlas with extrude>0 now packs AND
 *     exports cleanly instead of hard-rejecting in tp_pack.
 *
 * Input-build fixtures use plain files (build does not decode); the clamp
 * regression packs a real RGBA sprite so it exercises tp_pack + tp_export_run. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TP_TEST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define TP_TEST_MKDIR(p) mkdir((p), 0777)
#endif

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "unity.h"

static const char *g_dir;         /* scratch dir (argv[1]) */
static char g_hero[700];          /* <dir>/one/hero.png -- a single reusable file source */
static uint8_t g_rgba[32 * 32 * 4]; /* opaque square for the clamp pack regression */

void setUp(void) {}
void tearDown(void) {}

void test_internal_pack_name_has_one_core_formatter(void) {
    tp_id128 source_id = {{0}};
    source_id.bytes[15] = 0x2AU;
    char name[TP_PACK_INTERNAL_NAME_CAP];
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_pack_input_format_sprite_name(source_id, "dir/hero.png", name,
                                         sizeof name, &error));
    TEST_ASSERT_EQUAL_STRING(
        "source_0000000000000000000000000000002a:dir/hero.png", name);
}

// #region fixture helpers
static void mkdir_p(const char *path) {
    char tmp[600];
    (void)snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) {
        return;
    }
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            (void)TP_TEST_MKDIR(tmp);
            *p = c;
        }
    }
    (void)TP_TEST_MKDIR(tmp);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fwrite(content, 1U, strlen(content), f);
    fclose(f);
}

/* Finds the desc with the given raw name, or NULL. */
static const tp_pack_sprite_desc *find_desc(const tp_pack_input *in, const char *name) {
    for (int i = 0; i < in->count; i++) {
        if ((in->descs[i].source_key &&
             strcmp(in->descs[i].source_key, name) == 0) ||
            strcmp(in->descs[i].name, name) == 0) {
            return &in->descs[i];
        }
    }
    return NULL;
}

/* Builds a single-file-source project pointing at g_hero, with the given atlas
 * shape and one override on "hero", then builds pack input. The input outlives the
 * destroyed project (descs are malloc-owned). */
static void build_hero(int atlas_shape, const tp_project_sprite *ov_template, tp_pack_input *out) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    a->shape = atlas_shape;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, g_hero));
    memset(&a->sources[0].id, 0x11, sizeof a->sources[0].id);
    if (ov_template) {
        tp_project_sprite *sp = NULL;
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_project_atlas_add_sprite_by_source_key(
                                  a, a->sources[0].id, "hero.png", &sp));
        char *keep_name = sp->name;
        char *keep_key = sp->src_key;
        tp_id128 keep_source = sp->source_ref;
        *sp = *ov_template;
        sp->name = keep_name;
        sp->src_key = keep_key;
        sp->source_ref = keep_source;
    }
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_pack_input_build(p, 0, out, &e), e.msg);
    tp_project_destroy(p);
}

/* A fresh override template seeded to all-inherit defaults (like add_sprite). */
static tp_project_sprite ov_default(void) {
    tp_project_sprite s;
    memset(&s, 0, sizeof s);
    s.name = NULL;
    s.origin_x = TP_PROJECT_ORIGIN_DEFAULT;
    s.origin_y = TP_PROJECT_ORIGIN_DEFAULT;
    s.rename = NULL;
    s.ov_shape = TP_PROJECT_OV_INHERIT;
    s.ov_allow_rotate = TP_PROJECT_OV_INHERIT;
    s.ov_max_vertices = TP_PROJECT_OV_INHERIT;
    s.ov_margin = TP_PROJECT_OV_INHERIT;
    s.ov_extrude = TP_PROJECT_OV_INHERIT;
    return s;
}

static tp_id128 test_id(uint8_t byte) {
    tp_id128 id;
    memset(&id, byte, sizeof id);
    return id;
}
// #endregion

// #region tests
/* The per-sprite effective-shape rule (pure): slice9 > ov_shape > atlas shape. */
void test_effective_shape_matrix(void) {
    /* slice9 forces RECT regardless of ov_shape / atlas shape */
    TEST_ASSERT_EQUAL_INT(0, tp_project_sprite_effective_shape(2 /*CONCAVE*/, true, 1 /*CONVEX ov*/));
    /* ov set, no slice9 -> the override wins */
    TEST_ASSERT_EQUAL_INT(1, tp_project_sprite_effective_shape(2, false, 1));
    /* ov inherit -> the atlas shape */
    TEST_ASSERT_EQUAL_INT(2, tp_project_sprite_effective_shape(2, false, TP_PROJECT_OV_INHERIT));
    TEST_ASSERT_EQUAL_INT(0, tp_project_sprite_effective_shape(0, false, TP_PROJECT_OV_INHERIT));
}

/* File source: raw name keeps its extension, override maps by the stripped key,
 * and the +1 shape encoding + slice9 land on the desc. */
void test_file_source_override(void) {
    tp_project_sprite ov = ov_default();
    ov.origin_x = 0.75F;
    ov.origin_y = 0.25F;
    ov.slice9_lrtb[0] = 3;
    ov.ov_shape = 1; /* CONVEX */
    tp_pack_input in;
    build_hero(0 /*RECT atlas*/, &ov, &in);

    TEST_ASSERT_EQUAL_INT(1, in.count);
    TEST_ASSERT_EQUAL_INT(0, in.missing_sources);
    const tp_pack_sprite_desc *d = &in.descs[0];
    TEST_ASSERT_EQUAL_STRING("hero.png", d->source_key);
    TEST_ASSERT_EQUAL_STRING("hero", d->logical_name);
    TEST_ASSERT_NOT_NULL(d->path);
    TEST_ASSERT_TRUE(d->origin_x > 0.74F && d->origin_x < 0.76F);
    TEST_ASSERT_TRUE(d->origin_y > 0.24F && d->origin_y < 0.26F);
    TEST_ASSERT_EQUAL_UINT16(3, d->slice9_lrtb[0]);
    TEST_ASSERT_TRUE((d->ov_mask & TP_PACK_OV_SHAPE) != 0);
    TEST_ASSERT_EQUAL_UINT8(2, d->ov_shape); /* atlas CONVEX(1) -> engine encoding 2 */
    tp_pack_input_free(&in);
}

/* No override -> defaults: name keeps ext, pivot 0.5,0.5, empty ov_mask. */
void test_file_source_no_override(void) {
    tp_pack_input in;
    build_hero(2 /*CONCAVE atlas*/, NULL, &in);
    TEST_ASSERT_EQUAL_INT(1, in.count);
    const tp_pack_sprite_desc *d = &in.descs[0];
    TEST_ASSERT_EQUAL_STRING("hero.png", d->source_key);
    TEST_ASSERT_TRUE(d->origin_x > 0.49F && d->origin_x < 0.51F);
    TEST_ASSERT_EQUAL_UINT8(0, d->ov_mask);
    tp_pack_input_free(&in);
}

/* Folder source: a dotfile nested in a subfolder ("tank/.png") keys as "tank/.png"
 * (the A1 fix -- pre-fix it keyed "tank/" and the override silently no-op'd). */
void test_folder_dotfile_key(void) {
    char root[600];
    (void)snprintf(root, sizeof root, "%s/dot", g_dir);
    char tankdir[700];
    (void)snprintf(tankdir, sizeof tankdir, "%s/tank", root);
    mkdir_p(tankdir);
    char dotf[800];
    (void)snprintf(dotf, sizeof dotf, "%s/tank/.png", root);
    write_file(dotf, "X");
    char plain[800];
    (void)snprintf(plain, sizeof plain, "%s/tank/walk.png", root);
    write_file(plain, "Y");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    a->shape = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, root));
    memset(&a->sources[0].id, 0x12, sizeof a->sources[0].id);
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_sprite_by_source_key(
                              a, a->sources[0].id, "tank/.png", &sp));
    sp->origin_x = 0.1F;

    tp_pack_input in;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_input_build(p, 0, &in, &e));
    tp_project_destroy(p);

    TEST_ASSERT_EQUAL_INT(2, in.count);
    const tp_pack_sprite_desc *dot = find_desc(&in, "tank/.png");
    TEST_ASSERT_NOT_NULL_MESSAGE(dot, "dotfile-in-folder must appear with its full rel name");
    TEST_ASSERT_TRUE_MESSAGE(dot->origin_x > 0.09F && dot->origin_x < 0.11F,
                             "override keyed 'tank/.png' must apply to the dotfile");
    const tp_pack_sprite_desc *walk = find_desc(&in, "tank/walk.png");
    TEST_ASSERT_NOT_NULL(walk);
    TEST_ASSERT_TRUE(walk->origin_x > 0.49F && walk->origin_x < 0.51F); /* no override -> default */
    tp_pack_input_free(&in);
}

/* Extrude override is encoded only when the effective shape is RECT. */
void test_extrude_gating(void) {
    /* A: CONCAVE atlas, extrude override, no slice9, shape inherit -> non-RECT -> dropped. */
    tp_project_sprite a = ov_default();
    a.ov_extrude = 5;
    tp_pack_input in;
    build_hero(2 /*CONCAVE*/, &a, &in);
    TEST_ASSERT_EQUAL_INT(1, in.count);
    TEST_ASSERT_TRUE_MESSAGE((in.descs[0].ov_mask & TP_PACK_OV_EXTRUDE) == 0,
                             "extrude override must be dropped when effective shape is non-RECT");
    tp_pack_input_free(&in);

    /* B: RECT atlas, extrude override -> effective RECT -> encoded with the value. */
    tp_project_sprite b = ov_default();
    b.ov_extrude = 5;
    build_hero(0 /*RECT*/, &b, &in);
    TEST_ASSERT_TRUE((in.descs[0].ov_mask & TP_PACK_OV_EXTRUDE) != 0);
    TEST_ASSERT_EQUAL_UINT8(5, in.descs[0].ov_extrude);
    tp_pack_input_free(&in);

    /* C: CONCAVE atlas but slice9 present forces RECT -> extrude override kept. */
    tp_project_sprite c = ov_default();
    c.ov_extrude = 5;
    c.slice9_lrtb[2] = 4;
    build_hero(2 /*CONCAVE*/, &c, &in);
    TEST_ASSERT_TRUE_MESSAGE((in.descs[0].ov_mask & TP_PACK_OV_EXTRUDE) != 0,
                             "slice9 forces RECT, so the extrude override must be kept");
    tp_pack_input_free(&in);
}

/* Per-source-then-sorted order, no global sort across sources (arch review R2).
 * ord1 = {y.png, a.png} sorts to [a, y]; ord2 = {b.png}. Expected desc order
 * [a.png, y.png, b.png] -- a global sort would put b before y. */
void test_per_source_order(void) {
    char o1[600];
    (void)snprintf(o1, sizeof o1, "%s/ord1", g_dir);
    mkdir_p(o1);
    char o2[600];
    (void)snprintf(o2, sizeof o2, "%s/ord2", g_dir);
    mkdir_p(o2);
    char f[800];
    (void)snprintf(f, sizeof f, "%s/y.png", o1);
    write_file(f, "y");
    (void)snprintf(f, sizeof f, "%s/a.png", o1);
    write_file(f, "a");
    (void)snprintf(f, sizeof f, "%s/b.png", o2);
    write_file(f, "b");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, o1));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, o2));
    tp_pack_input in;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_input_build(p, 0, &in, &e));
    tp_project_destroy(p);

    TEST_ASSERT_EQUAL_INT(3, in.count);
    TEST_ASSERT_EQUAL_STRING("a.png", in.descs[0].source_key);
    TEST_ASSERT_EQUAL_STRING("y.png", in.descs[1].source_key);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("b.png", in.descs[2].source_key,
                                     "second source's child must follow the first source's (no global sort)");
    tp_pack_input_free(&in);
}

void test_same_source_key_in_two_sources_keeps_distinct_override_identity(void) {
    char first_dir[700];
    char second_dir[700];
    char path[800];
    char project_path[800];
    (void)snprintf(first_dir, sizeof first_dir, "%s/same_key_first", g_dir);
    (void)snprintf(second_dir, sizeof second_dir, "%s/same_key_second", g_dir);
    mkdir_p(first_dir);
    mkdir_p(second_dir);
    (void)snprintf(path, sizeof path, "%s/hero.png", first_dir);
    write_file(path, "A");
    (void)snprintf(path, sizeof path, "%s/hero.png", second_dir);
    write_file(path, "B");
    (void)snprintf(project_path, sizeof project_path,
                   "%s/two_source_identity.ntpacker_project", g_dir);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = tp_project_get_atlas(project, 0);
    atlas->id = test_id(0x11U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, first_dir));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, second_dir));
    atlas->sources[0].id = test_id(0x21U);
    atlas->sources[1].id = test_id(0x22U);

    tp_operation set = {0};
    set.kind = TP_OP_SPRITE_OVERRIDE_SET;
    set.atlas_id = atlas->id;
    set.u.sprite_set.src_key = (char *)"hero.png";
    set.u.sprite_set.mask = TP_SPF_ORIGIN;
    set.u.sprite_set.origin_x = 0.1F;
    set.u.sprite_set.origin_y = 0.2F;
    set.u.sprite_set.source_id = atlas->sources[0].id;
    tp_op_reject reject;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_operation_apply(project, &set, &reject));
    set.u.sprite_set.source_id = atlas->sources[1].id;
    set.u.sprite_set.origin_x = 0.8F;
    set.u.sprite_set.origin_y = 0.9F;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_operation_apply(project, &set, &reject));
    TEST_ASSERT_EQUAL_INT(2, atlas->sprite_count);

    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_project_save(project, project_path,
                                                  &error), error.msg);
    tp_project_destroy(project);
    project = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_project_load(project_path, &project,
                                                  &error), error.msg);
    atlas = tp_project_get_atlas(project, 0);
    const tp_project_sprite *first =
        tp_project_atlas_find_sprite_by_source_key(
            atlas, atlas->sources[0].id, "hero.png");
    const tp_project_sprite *second =
        tp_project_atlas_find_sprite_by_source_key(
            atlas, atlas->sources[1].id, "hero.png");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(first != second);
    TEST_ASSERT_TRUE(first->origin_x > 0.09F && first->origin_x < 0.11F);
    TEST_ASSERT_TRUE(second->origin_x > 0.79F && second->origin_x < 0.81F);

    tp_pack_input input = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_pack_input_build(project, 0, &input,
                                                      &error), error.msg);
    TEST_ASSERT_EQUAL_INT(2, input.count);
    TEST_ASSERT_TRUE(input.descs[0].origin_x > 0.09F &&
                     input.descs[0].origin_x < 0.11F);
    TEST_ASSERT_TRUE(input.descs[1].origin_x > 0.79F &&
                     input.descs[1].origin_x < 0.81F);
    tp_pack_input_free(&input);

    tp_operation clear = {0};
    clear.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    clear.atlas_id = atlas->id;
    clear.u.sprite_clear.source_id = atlas->sources[0].id;
    clear.u.sprite_clear.src_key = (char *)"hero.png";
    clear.u.sprite_clear.mask = TP_SPF_ALL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_operation_apply(project, &clear, &reject));
    TEST_ASSERT_NULL(tp_project_atlas_find_sprite_by_source_key(
        atlas, atlas->sources[0].id, "hero.png"));
    TEST_ASSERT_NOT_NULL(tp_project_atlas_find_sprite_by_source_key(
        atlas, atlas->sources[1].id, "hero.png"));
    tp_project_destroy(project);
}

/* A resolvable-but-absent source is counted as missing and contributes no descs;
 * a present source alongside it still yields its sprite. */
void test_missing_source_count(void) {
    char nope[700];
    (void)snprintf(nope, sizeof nope, "%s/does_not_exist_%d.png", g_dir, 12345);

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, g_hero));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, nope));
    tp_pack_input in;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_input_build(p, 0, &in, &e));
    tp_project_destroy(p);

    TEST_ASSERT_EQUAL_INT(1, in.count);
    TEST_ASSERT_EQUAL_INT(1, in.missing_sources);
    tp_pack_input_free(&in);
}

void test_long_resolved_source_path_is_not_silently_dropped(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(a, "sprite.png"));

    const size_t dir_len = 700U;
    p->project_dir = (char *)malloc(dir_len + 1U);
    TEST_ASSERT_NOT_NULL(p->project_dir);
    memcpy(p->project_dir, "C:/", 3U);
    memset(p->project_dir + 3U, 'a', dir_len - 3U);
    p->project_dir[dir_len] = '\0';

    tp_pack_input in = {0};
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_input_build(p, 0, &in, &e));
    TEST_ASSERT_EQUAL_INT(0, in.count);
    TEST_ASSERT_EQUAL_INT(1, in.missing_sources);
    tp_pack_input_free(&in);
    tp_project_destroy(p);
}

void test_source_path_beyond_identity_limit_is_structured_failure(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(a, "sprite.png"));

    const size_t dir_len = (size_t)TP_IDENTITY_PATH_MAX + 32U;
    p->project_dir = (char *)malloc(dir_len + 1U);
    TEST_ASSERT_NOT_NULL(p->project_dir);
    memcpy(p->project_dir, "C:/", 3U);
    memset(p->project_dir + 3U, 'b', dir_len - 3U);
    p->project_dir[dir_len] = '\0';

    tp_pack_input in = {0};
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_pack_input_build(p, 0, &in, &e));
    TEST_ASSERT_NULL(in.descs);
    TEST_ASSERT_EQUAL_INT(0, in.count);
    TEST_ASSERT_NOT_NULL(strstr(e.msg, "source path"));
    tp_project_destroy(p);
}

/* Bad arguments return structured errors and leave *out empty, never crash.
 * (A true malloc-failure OOM path is not simulated: tp_pack_input_build uses the
 * CRT allocator directly, matching the GUI it replaced -- no injection point.) */
void test_bad_args(void) {
    tp_project *p = tp_project_create();
    tp_pack_input in;
    tp_error e = {0};

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_pack_input_build(p, 7, &in, &e));
    TEST_ASSERT_NULL(in.descs);
    TEST_ASSERT_EQUAL_INT(0, in.count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_pack_input_build(NULL, 0, &in, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_pack_input_build(p, 0, NULL, &e));
    tp_project_destroy(p);
}

/* The export-path clamp hole (arch review §3.1 / plan A3a step 3): a CONCAVE atlas
 * with extrude>0 must (a) map to settings with extrude clamped to 0, (b) pack via
 * those settings, and (c) export through tp_export_run -- pre-clamp it hard-rejected
 * at tp_pack ("extrude > 0 is only valid for shape RECT"). */
void test_concave_extrude_clamp_exports(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    a->shape = 2; /* CONCAVE_CONTOUR */
    a->extrude = 4;
    a->padding = 0;
    a->margin = 0;
    a->alpha_threshold = 1;
    a->max_size = 256;
    a->power_of_two = false;
    a->allow_transform = true;
    a->pixels_per_unit = 1.0F;

    /* (a) to_settings clamps extrude for the non-RECT shape */
    tp_pack_settings s;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_to_settings(p, 0, &s, &e));
    TEST_ASSERT_EQUAL_INT(2, s.shape);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.extrude, "non-RECT shape must clamp extrude to 0");

    /* (b) tp_pack accepts the clamped settings */
    tp_pack_sprite_desc d;
    memset(&d, 0, sizeof d);
    d.name = "solid";
    d.rgba = g_rgba;
    d.w = 32;
    d.h = 32;
    d.origin_x = 0.5F;
    d.origin_y = 0.5F;
    s.work_dir = g_dir;
    s.sprites = &d;
    s.sprite_count = 1;
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_result *r = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_pack(&s, ar, &r, &e), e.msg);
    tp_arena_destroy(ar);

    /* (c) tp_export_run (which reads to_settings internally) exports cleanly */
    char outbase[700];
    (void)snprintf(outbase, sizeof outbase, "%s/clamp_out", g_dir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", outbase, NULL));
    tp_export_notices nts;
    tp_export_notices_init(&nts);
    tp_arena *ar2 = tp_arena_create(0);
    int runs = 0;
    tp_status st = tp_export_run(p, 0, &d, 1, g_dir, ar2, &nts, &runs, &e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, e.msg);
    tp_export_notices_free(&nts);
    tp_arena_destroy(ar2);
    tp_project_destroy(p);
}
// #endregion

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";

    char onedir[700];
    (void)snprintf(onedir, sizeof onedir, "%s/one", g_dir);
    mkdir_p(onedir);
    (void)snprintf(g_hero, sizeof g_hero, "%s/hero.png", onedir);
    write_file(g_hero, "HERO");
    for (int i = 0; i < 32 * 32; i++) {
        g_rgba[i * 4 + 0] = 200;
        g_rgba[i * 4 + 1] = 120;
        g_rgba[i * 4 + 2] = 60;
        g_rgba[i * 4 + 3] = 255;
    }

    UNITY_BEGIN();
    RUN_TEST(test_internal_pack_name_has_one_core_formatter);
    RUN_TEST(test_effective_shape_matrix);
    RUN_TEST(test_file_source_override);
    RUN_TEST(test_file_source_no_override);
    RUN_TEST(test_folder_dotfile_key);
    RUN_TEST(test_extrude_gating);
    RUN_TEST(test_per_source_order);
    RUN_TEST(test_same_source_key_in_two_sources_keeps_distinct_override_identity);
    RUN_TEST(test_missing_source_count);
    RUN_TEST(test_long_resolved_source_path_is_not_silently_dropped);
    RUN_TEST(test_source_path_beyond_identity_limit_is_structured_failure);
    RUN_TEST(test_bad_args);
    RUN_TEST(test_concave_extrude_clamp_exports);
    return UNITY_END();
}
