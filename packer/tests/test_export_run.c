/* Per-target packing proof (ROADMAP Phase 2 acceptance, SUMMARY.md §5h).
 *
 * One project, one atlas (full D4 allowed), three targets over TEST-ONLY
 * capability-restricted descriptors that reuse the json-neotolis writer:
 *   - "json-neotolis": full caps           -> packs full D4  (>=1 diagonal)
 *   - "test-nopivot" : full EXCEPT pivot    -> SAME layout, drops pivot + notice
 *   - "test-norot"   : no rotate/flip       -> identity repack (zero transforms)
 * Proves: effective-settings REPACK (not post-filter); identical effective
 * settings share ONE pack run; metadata-loss raises a notice, never an error. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "unity.h"

/* wide + tall force the packer to rotate the tall sprite (diagonal bit). */
static uint8_t g_wide[120 * 24 * 4];
static uint8_t g_tall[24 * 100 * 4];
static uint8_t g_piv[30 * 20 * 4];

static const char *g_dir;
static tp_project *g_proj;
static tp_arena *g_arena;
static tp_export_notices g_notices;
static int g_pack_runs;
static char g_A[1024]; /* json-neotolis base */
static char g_B[1024]; /* test-nopivot base  */
static char g_C[1024]; /* test-norot base    */

/* test-only descriptors (borrowed by the registry; must outlive the run). */
static tp_exporter g_nopivot;
static tp_exporter g_norot;

void setUp(void) {}
void tearDown(void) {}

static void fill(uint8_t *p, int n, uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < n; i++) {
        p[i * 4 + 0] = r;
        p[i * 4 + 1] = g;
        p[i * 4 + 2] = b;
        p[i * 4 + 3] = 255;
    }
}

static cJSON *load_json(const char *base) {
    char path[1088];
    (void)snprintf(path, sizeof path, "%s.json", base);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (sz > 0) ? (char *)malloc((size_t)sz + 1) : NULL;
    size_t rd = buf ? fread(buf, 1, (size_t)sz, f) : 0;
    (void)fclose(f);
    if (!buf) {
        return NULL;
    }
    buf[rd] = '\0';
    cJSON *root = cJSON_ParseWithLength(buf, rd);
    free(buf);
    return root;
}

static int count_nonidentity_transforms(cJSON *root) {
    int n = 0;
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    cJSON *s = NULL;
    cJSON_ArrayForEach(s, sprites) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(s, "transform");
        if (cJSON_IsNumber(t) && t->valueint != 0) {
            n++;
        }
    }
    return n;
}

static cJSON *sprite_by_name(cJSON *root, const char *name) {
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    cJSON *s = NULL;
    cJSON_ArrayForEach(s, sprites) {
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(s, "name");
        if (cJSON_IsString(nm) && strcmp(nm->valuestring, name) == 0) {
            return s;
        }
    }
    return NULL;
}

static cJSON *anim_by_id(cJSON *root, const char *id) {
    cJSON *anims = cJSON_GetObjectItemCaseSensitive(root, "animations");
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, anims) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(a, "id");
        if (cJSON_IsString(n) && strcmp(n->valuestring, id) == 0) {
            return a;
        }
    }
    return NULL;
}

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (sz >= 0) ? (char *)malloc((size_t)sz + 1) : NULL;
    size_t rd = buf ? fread(buf, 1, (size_t)sz, f) : 0;
    (void)fclose(f);
    if (!buf) {
        return NULL;
    }
    buf[rd] = '\0';
    return buf;
}

// #region tests
void test_shared_run_count(void) {
    /* json-neotolis and test-nopivot have identical effective settings (both
     * full D4) -> ONE pack run; test-norot differs -> a second. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_pack_runs, "identical effective settings must share one pack run");
}

void test_full_target_has_diagonal(void) {
    cJSON *root = load_json(g_A);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE_MESSAGE(count_nonidentity_transforms(root) >= 1,
                             "full-D4 target must produce >=1 non-identity transform");
    cJSON_Delete(root);
}

void test_norot_target_is_identity(void) {
    cJSON *root = load_json(g_C);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_nonidentity_transforms(root),
                                  "no-rotation target must repack to an all-identity layout");
    cJSON_Delete(root);
}

void test_nopivot_drops_pivot_with_notice(void) {
    /* pivot present in the full target... */
    cJSON *ra = load_json(g_A);
    TEST_ASSERT_NOT_NULL(ra);
    cJSON *pa = sprite_by_name(ra, "piv");
    TEST_ASSERT_NOT_NULL(pa);
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(pa, "pivot")));
    cJSON_Delete(ra);

    /* ...but dropped in the pivot-less target. */
    cJSON *rb = load_json(g_B);
    TEST_ASSERT_NOT_NULL(rb);
    cJSON *pb = sprite_by_name(rb, "piv");
    TEST_ASSERT_NOT_NULL(pb);
    TEST_ASSERT_NULL_MESSAGE(cJSON_GetObjectItemCaseSensitive(pb, "pivot"), "pivot must be dropped for a pivot-less target");
    cJSON_Delete(rb);

    /* and a metadata-loss notice was raised (never an error). */
    bool found = false;
    for (int i = 0; i < g_notices.count; i++) {
        if (strstr(g_notices.items[i].msg, "pivot")) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "dropping a non-default pivot must raise a notice");
}

void test_rename_and_anim_through_run(void) {
    /* A4 end-to-end through tp_export_run (L-3/L-4): a renamed sprite that an
     * animation references. Both json and defold outputs must carry the rename
     * into the sprite name AND the animation frame (frames follow the rename). */
    static uint8_t hero[32 * 32 * 4];
    static uint8_t gem[24 * 24 * 4];
    fill(hero, 32 * 32, 10, 20, 30);
    fill(gem, 24 * 24, 200, 150, 100);

    tp_project *proj = tp_project_create();
    TEST_ASSERT_NOT_NULL(proj);
    tp_project_atlas *a = tp_project_get_atlas(proj, 0);
    a->shape = 0; /* RECT */
    a->allow_transform = false;
    a->power_of_two = false;
    a->padding = 0;
    a->margin = 0;
    a->alpha_threshold = 1;
    a->max_size = 1024;
    a->pixels_per_unit = 1.0F;

    /* rename the sprite whose KEY is "hero" (raw desc "hero.png") */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_set_sprite_rename(a, "hero", "champion"));
    /* an animation whose frames are stored in KEY space (ext stripped) */
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "run", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "gem"));

    char jbase[1024];
    char dbase[1024];
    (void)snprintf(jbase, sizeof jbase, "%s/rn_json", g_dir);
    (void)snprintf(dbase, sizeof dbase, "%s/rn_defold", g_dir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", jbase, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "defold", dbase, NULL));

    tp_pack_sprite_desc sprites[2];
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "hero.png", .rgba = hero, .w = 32, .h = 32, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[1] = (tp_pack_sprite_desc){.name = "gem.png", .rgba = gem, .w = 24, .h = 24, .origin_x = 0.5F, .origin_y = 0.5F};

    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_export_notices nts;
    tp_export_notices_init(&nts);
    tp_error e = {{0}};
    tp_status st = tp_export_run(proj, 0, sprites, 2, g_dir, ar, &nts, NULL, &e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, e.msg);
    tp_export_notices_free(&nts);

    /* --- json: sprite renamed + frame follows --- */
    cJSON *root = load_json(jbase);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NOT_NULL_MESSAGE(sprite_by_name(root, "champion"), "renamed sprite present as 'champion'");
    TEST_ASSERT_NULL_MESSAGE(sprite_by_name(root, "hero"), "old name must be gone from sprites");
    TEST_ASSERT_NOT_NULL(sprite_by_name(root, "gem"));
    cJSON *run = anim_by_id(root, "run");
    TEST_ASSERT_NOT_NULL(run);
    cJSON *frames = cJSON_GetObjectItemCaseSensitive(run, "frames");
    TEST_ASSERT_TRUE(cJSON_IsArray(frames) && cJSON_GetArraySize(frames) == 2);
    TEST_ASSERT_EQUAL_STRING("champion", cJSON_GetArrayItem(frames, 0)->valuestring); /* follows rename */
    TEST_ASSERT_EQUAL_STRING("gem", cJSON_GetArrayItem(frames, 1)->valuestring);
    cJSON_Delete(root);

    /* --- defold: rename lands in the sprite (.tpinfo) and the frame (.tpatlas) --- */
    char path[1088];
    (void)snprintf(path, sizeof path, "%s.tpinfo", dbase);
    char *tpi = read_text_file(path);
    TEST_ASSERT_NOT_NULL(tpi);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tpi, "name: \"champion\""), "defold .tpinfo carries the rename");
    TEST_ASSERT_NULL_MESSAGE(strstr(tpi, "name: \"hero\""), "old name gone from .tpinfo");
    free(tpi);
    (void)snprintf(path, sizeof path, "%s.tpatlas", dbase);
    char *tpa = read_text_file(path);
    TEST_ASSERT_NOT_NULL(tpa);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tpa, "images: \"champion\""), "defold anim frame follows the rename");
    free(tpa);

    tp_arena_destroy(ar);
    tp_project_destroy(proj);
}

void test_dangling_frame_through_run(void) {
    /* A4 (L-4): tp_export_run surfaces the dangling-frame error (naming anim +
     * frame) so the CLI/GUI can report it. */
    static uint8_t px[16 * 16 * 4];
    fill(px, 16 * 16, 90, 90, 90);
    tp_project *proj = tp_project_create();
    TEST_ASSERT_NOT_NULL(proj);
    tp_project_atlas *a = tp_project_get_atlas(proj, 0);
    a->shape = 0;
    a->allow_transform = false;
    a->power_of_two = false;
    a->padding = 0;
    a->margin = 0;
    a->alpha_threshold = 1;
    a->max_size = 256;
    a->pixels_per_unit = 1.0F;
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "run", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "ghost")); /* never packed */
    char jbase[1024];
    (void)snprintf(jbase, sizeof jbase, "%s/rn_dangling", g_dir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", jbase, NULL));
    tp_pack_sprite_desc sprites[1];
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "hero.png", .rgba = px, .w = 16, .h = 16, .origin_x = 0.5F, .origin_y = 0.5F};
    tp_arena *ar = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ar);
    tp_export_notices nts;
    tp_export_notices_init(&nts);
    tp_error e = {{0}};
    tp_status st = tp_export_run(proj, 0, sprites, 1, g_dir, ar, &nts, NULL, &e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_INVALID_ARGUMENT, st, "dangling frame must fail the export");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.msg, "run"), e.msg);   /* names the animation */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.msg, "ghost"), e.msg); /* names the frame */
    tp_export_notices_free(&nts);
    tp_arena_destroy(ar);
    tp_project_destroy(proj);
}
// #endregion

static bool setup_all(const char *dir) {
    g_dir = dir;
    fill(g_wide, 120 * 24, 200, 60, 60);
    fill(g_tall, 24 * 100, 60, 200, 60);
    fill(g_piv, 30 * 20, 60, 60, 200);
    tp_export_notices_init(&g_notices);

    /* register test-only descriptors over the json writer. */
    g_nopivot = (tp_exporter){.id = "test-nopivot",
                              .display_name = "test nopivot",
                              .extension = "json",
                              .caps = {.rotate90 = true,
                                       .flips = true,
                                       .polygons = true,
                                       .pivot = false,
                                       .slice9 = true,
                                       .multipage = true,
                                       .aliases = true},
                              .write = tp_export_json_neotolis_write};
    g_norot = (tp_exporter){.id = "test-norot",
                            .display_name = "test norot",
                            .extension = "json",
                            .caps = {.rotate90 = false,
                                     .flips = false,
                                     .polygons = true,
                                     .pivot = true,
                                     .slice9 = true,
                                     .multipage = true,
                                     .aliases = true},
                            .write = tp_export_json_neotolis_write};
    if (tp_exporter_register(&g_nopivot) != TP_STATUS_OK || tp_exporter_register(&g_norot) != TP_STATUS_OK) {
        return false;
    }

    g_proj = tp_project_create();
    if (!g_proj) {
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(g_proj, 0);
    a->shape = 0; /* RECT */
    a->allow_transform = true;
    a->power_of_two = false;
    a->padding = 0;
    a->margin = 0;
    a->alpha_threshold = 1;
    a->max_size = 1024;
    a->pixels_per_unit = 1.0F;

    (void)snprintf(g_A, sizeof g_A, "%s/run_A", dir);
    (void)snprintf(g_B, sizeof g_B, "%s/run_B", dir);
    (void)snprintf(g_C, sizeof g_C, "%s/run_C", dir);
    if (tp_project_atlas_add_target(a, "json-neotolis", g_A, NULL) != TP_STATUS_OK ||
        tp_project_atlas_add_target(a, "test-nopivot", g_B, NULL) != TP_STATUS_OK ||
        tp_project_atlas_add_target(a, "test-norot", g_C, NULL) != TP_STATUS_OK) {
        return false;
    }

    tp_pack_sprite_desc sprites[3];
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "wide", .rgba = g_wide, .w = 120, .h = 24, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[1] = (tp_pack_sprite_desc){.name = "tall", .rgba = g_tall, .w = 24, .h = 100, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[2] = (tp_pack_sprite_desc){.name = "piv", .rgba = g_piv, .w = 30, .h = 20, .origin_x = 1.5F, .origin_y = -0.25F};

    g_arena = tp_arena_create(0);
    if (!g_arena) {
        return false;
    }
    tp_error e = {{0}};
    tp_status st = tp_export_run(g_proj, 0, sprites, 3, dir, g_arena, &g_notices, &g_pack_runs, &e);
    if (st != TP_STATUS_OK) {
        (void)fprintf(stderr, "tp_export_run failed: %s (%s)\n", tp_status_str(st), e.msg);
        return false;
    }
    return true;
}

/* B3a: the structured report — shape, file existence, and determinism across
 * two runs (pages/occupancy are pack-derived; timings live outside the report). */
static void test_report_ex(void) {
    tp_pack_sprite_desc sprites[3];
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "wide", .rgba = g_wide, .w = 120, .h = 24, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[1] = (tp_pack_sprite_desc){.name = "tall", .rgba = g_tall, .w = 24, .h = 100, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[2] = (tp_pack_sprite_desc){.name = "piv", .rgba = g_piv, .w = 30, .h = 20, .origin_x = 1.5F, .origin_y = -0.25F};

    tp_export_report rep[2];
    double occ0 = 0.0;
    for (int pass = 0; pass < 2; pass++) {
        tp_arena *ar = tp_arena_create(0);
        TEST_ASSERT_NOT_NULL(ar);
        tp_export_notices nts;
        tp_export_notices_init(&nts);
        tp_error e = {{0}};
        memset(&rep[pass], 0, sizeof rep[pass]);
        tp_export_run_opts opts = {.report = &rep[pass]};
        tp_status st = tp_export_run_ex(g_proj, 0, sprites, 3, g_dir, ar, &nts, NULL, &opts, &e);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, st);
        TEST_ASSERT_FALSE(rep[pass].pack_failed);
        TEST_ASSERT_FALSE(rep[pass].dry_run);
        TEST_ASSERT_EQUAL_INT(2, rep[pass].run_count); /* json+nopivot share, norot repacks */
        TEST_ASSERT_EQUAL_INT(3, rep[pass].target_count);
        for (int r = 0; r < rep[pass].run_count; r++) {
            const tp_export_report_run *run = &rep[pass].runs[r];
            TEST_ASSERT_TRUE(run->page_count >= 1);
            TEST_ASSERT_EQUAL_INT(3, run->sprite_count);
            for (int g = 0; g < run->page_count; g++) {
                TEST_ASSERT_TRUE(run->pages[g].w > 0 && run->pages[g].h > 0);
                TEST_ASSERT_TRUE(run->pages[g].occupancy_pct > 0.0 && run->pages[g].occupancy_pct <= 100.0);
            }
        }
        for (int t = 0; t < rep[pass].target_count; t++) {
            const tp_export_report_target *tg = &rep[pass].targets[t];
            TEST_ASSERT_TRUE(tg->ok);
            TEST_ASSERT_NULL(tg->error);
            TEST_ASSERT_TRUE(tg->pack_run >= 0 && tg->pack_run < rep[pass].run_count);
            TEST_ASSERT_TRUE(tg->written_file_count >= 1);
            TEST_ASSERT_TRUE(tg->notice_begin <= tg->notice_end);
            for (int f = 0; f < tg->written_file_count; f++) {
                FILE *fp = fopen(tg->written_files[f], "rb");
                TEST_ASSERT_NOT_NULL_MESSAGE(fp, tg->written_files[f]);
                (void)fclose(fp);
            }
        }
        if (pass == 0) {
            occ0 = rep[0].runs[0].pages[0].occupancy_pct;
            /* rep[0]'s arena strings die with the arena; only scalars compare below. */
            tp_export_notices_free(&nts);
            tp_arena_destroy(ar);
        } else {
            TEST_ASSERT_EQUAL_INT(rep[0].run_count, rep[1].run_count);
            const double d = occ0 - rep[1].runs[0].pages[0].occupancy_pct;
            TEST_ASSERT_TRUE_MESSAGE(d > -1e-9 && d < 1e-9, "occupancy not deterministic");
            tp_export_notices_free(&nts);
            tp_arena_destroy(ar);
        }
    }
}

/* B3b: dry-run packs + predicts but writes NO files. Strategy that stays clean on
 * a reused ctest build dir: WET-run first to a base (files land), capture the wet
 * report scalars + file list, DELETE those files, then DRY-run the SAME project to
 * the SAME base and assert (a) report.dry_run set, (b) identical pages/occupancy +
 * run count, (c) would_write == the wet written set, (d) written_files empty, and
 * (e) NONE of the would_write paths exist on disk (dry wrote nothing back). */
static void test_dry_run(void) {
    tp_pack_sprite_desc sprites[3];
    memset(sprites, 0, sizeof sprites);
    sprites[0] = (tp_pack_sprite_desc){.name = "wide", .rgba = g_wide, .w = 120, .h = 24, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[1] = (tp_pack_sprite_desc){.name = "tall", .rgba = g_tall, .w = 24, .h = 100, .origin_x = 0.5F, .origin_y = 0.5F};
    sprites[2] = (tp_pack_sprite_desc){.name = "piv", .rgba = g_piv, .w = 30, .h = 20, .origin_x = 1.5F, .origin_y = -0.25F};

    tp_project *proj = tp_project_create();
    TEST_ASSERT_NOT_NULL(proj);
    tp_project_atlas *a = tp_project_get_atlas(proj, 0);
    a->shape = 0;
    a->allow_transform = true;
    a->power_of_two = false;
    a->padding = 0;
    a->margin = 0;
    a->alpha_threshold = 1;
    a->max_size = 1024;
    a->pixels_per_unit = 1.0F;
    char dbase[1024];
    (void)snprintf(dbase, sizeof dbase, "%s/dryrun_base", g_dir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", dbase, NULL));

    /* --- wet run: files land; capture the report scalars + the file list --- */
    tp_arena *arw = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arw);
    tp_export_notices nw;
    tp_export_notices_init(&nw);
    tp_export_report rw;
    memset(&rw, 0, sizeof rw);
    tp_export_run_opts wopts = {.report = &rw};
    tp_error e = {{0}};
    tp_status st = tp_export_run_ex(proj, 0, sprites, 3, g_dir, arw, &nw, NULL, &wopts, &e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, e.msg);
    TEST_ASSERT_FALSE(rw.dry_run);
    const int wet_run_count = rw.run_count;
    const double wet_occ = rw.runs[0].pages[0].occupancy_pct;
    const tp_export_report_target *wt = &rw.targets[0];
    TEST_ASSERT_TRUE(wt->ok);
    TEST_ASSERT_TRUE_MESSAGE(wt->written_file_count >= 1, "wet run writes >=1 file");
    const int wet_written = wt->written_file_count;
    /* copy the paths out of the arena so we can delete them after freeing arw. */
    char paths[16][1088];
    int npaths = wt->written_file_count < 16 ? wt->written_file_count : 16;
    for (int f = 0; f < npaths; f++) {
        FILE *fp = fopen(wt->written_files[f], "rb");
        TEST_ASSERT_NOT_NULL_MESSAGE(fp, wt->written_files[f]);
        if (fp) {
            (void)fclose(fp);
        }
        (void)snprintf(paths[f], sizeof paths[f], "%s", wt->written_files[f]);
    }
    tp_export_notices_free(&nw);
    tp_arena_destroy(arw);

    /* delete the wet outputs so "absent after dry-run" is unambiguous. */
    for (int f = 0; f < npaths; f++) {
        (void)remove(paths[f]);
    }

    /* --- dry run of the SAME project: no writes, identical pages, would_write set --- */
    tp_arena *ard = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(ard);
    tp_export_notices nd;
    tp_export_notices_init(&nd);
    tp_export_report rd;
    memset(&rd, 0, sizeof rd);
    tp_export_run_opts dopts = {.report = &rd, .dry_run = true};
    st = tp_export_run_ex(proj, 0, sprites, 3, g_dir, ard, &nd, NULL, &dopts, &e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, st, e.msg);
    TEST_ASSERT_TRUE_MESSAGE(rd.dry_run, "report must record dry_run");
    TEST_ASSERT_FALSE(rd.pack_failed);
    TEST_ASSERT_EQUAL_INT(1, rd.target_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(wet_run_count, rd.run_count, "dry and wet must pack the same runs");
    const double d = wet_occ - rd.runs[0].pages[0].occupancy_pct;
    TEST_ASSERT_TRUE_MESSAGE(d > -1e-9 && d < 1e-9, "dry-run occupancy must match the wet run");
    const tp_export_report_target *dt = &rd.targets[0];
    TEST_ASSERT_TRUE_MESSAGE(dt->ok, "dry-run target is ok (nothing can fail to write)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dt->written_file_count, "dry-run writes nothing");
    TEST_ASSERT_NULL(dt->written_files);
    TEST_ASSERT_EQUAL_INT_MESSAGE(wet_written, dt->would_write_count,
                                  "would_write must match what the wet run actually wrote");
    for (int f = 0; f < dt->would_write_count; f++) {
        FILE *fp = fopen(dt->would_write[f], "rb");
        TEST_ASSERT_NULL_MESSAGE(fp, "dry-run must not create any output file");
        if (fp) {
            (void)fclose(fp);
        }
    }
    tp_export_notices_free(&nd);
    tp_arena_destroy(ard);
    tp_project_destroy(proj);
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    if (!setup_all(dir)) {
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_shared_run_count);
    RUN_TEST(test_full_target_has_diagonal);
    RUN_TEST(test_norot_target_is_identity);
    RUN_TEST(test_nopivot_drops_pivot_with_notice);
    RUN_TEST(test_rename_and_anim_through_run);
    RUN_TEST(test_dangling_frame_through_run);
    RUN_TEST(test_report_ex);
    RUN_TEST(test_dry_run);
    int rc = UNITY_END();
    tp_export_notices_free(&g_notices);
    tp_project_destroy(g_proj);
    tp_arena_destroy(g_arena);
    return rc;
}
