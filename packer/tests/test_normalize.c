/* Normalization pass unit tests (ROADMAP Phase 2): ext/folder strip, per-sprite
 * export-name override (rename), EXPLICIT animation assembly (no auto-grouping,
 * ux.md 3.7b), munge collision, alias entries, final-name sort, scale. Pure
 * tp_core -- synthetic tp_results, NO builder. */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_model.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// #region helpers
static tp_sprite mk(const char *name, int alias_of) {
    tp_sprite s;
    memset(&s, 0, sizeof s);
    s.name = name;
    s.alias_of = alias_of;
    s.pivot.x = 0.5F;
    s.pivot.y = 0.5F;
    s.frame.w = 8;
    s.frame.h = 8;
    s.sourceSize.w = 8;
    s.sourceSize.h = 8;
    s.spriteSourceSize.w = 8;
    s.spriteSourceSize.h = 8;
    return s;
}

static tp_result mk_result(tp_sprite *arr, int n) {
    tp_result r;
    memset(&r, 0, sizeof r);
    r.atlas_name = "t";
    r.pixels_per_unit = 1.0F;
    r.sprites = arr;
    r.sprite_count = n;
    return r;
}

static const tp_export_sprite *find_final(const tp_export_prepared *p, const char *fn) {
    for (int i = 0; i < p->sprite_count; i++) {
        if (strcmp(p->sprites[i].final_name, fn) == 0) {
            return &p->sprites[i];
        }
    }
    return NULL;
}

static const tp_export_anim *find_anim(const tp_export_prepared *p, const char *id) {
    for (int i = 0; i < p->animation_count; i++) {
        if (strcmp(p->animations[i].id, id) == 0) {
            return &p->animations[i];
        }
    }
    return NULL;
}
// #endregion

// #region tests
void test_ext_strip(void) {
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[2] = {mk("hero.png", -1), mk("enemy.gif", -1)};
    tp_result r = mk_result(s, 2);
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_normalize(&r, NULL, ar, &prep, &e), e.msg);
    TEST_ASSERT_NOT_NULL(find_final(&prep, "hero"));
    TEST_ASSERT_NOT_NULL(find_final(&prep, "enemy"));
    /* sorted ascending: enemy before hero */
    TEST_ASSERT_EQUAL_STRING("enemy", prep.sprites[0].final_name);
    TEST_ASSERT_EQUAL_STRING("hero", prep.sprites[1].final_name);
    tp_arena_destroy(ar);
}

void test_folder_strip_toggle(void) {
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[1] = {mk("chars/hero.png", -1)};
    tp_result r = mk_result(s, 1);
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    tp_export_prepared prep;
    tp_error e = {{0}};
    /* keep folder (default): ext stripped, folder kept */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, &o, ar, &prep, &e));
    TEST_ASSERT_EQUAL_STRING("chars/hero", prep.sprites[0].final_name);
    /* strip folder: basename only */
    o.strip_folders = true;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, &o, ar, &prep, &e));
    TEST_ASSERT_EQUAL_STRING("hero", prep.sprites[0].final_name);
    tp_arena_destroy(ar);
}

void test_override_verbatim(void) {
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[1] = {mk("hero.png", -1)};
    tp_result r = mk_result(s, 1);
    tp_export_name_override ov = {.raw_name = "hero.png", .final_name = "Boss.King"};
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    o.overrides = &ov;
    o.override_count = 1;
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, &o, ar, &prep, &e));
    /* override is verbatim -- the '.King' is NOT ext-stripped */
    TEST_ASSERT_EQUAL_STRING("Boss.King", prep.sprites[0].final_name);
    tp_arena_destroy(ar);
}

void test_munge_collision(void) {
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[2] = {mk("a.png", -1), mk("a.jpg", -1)};
    tp_result r = mk_result(s, 2);
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_normalize(&r, NULL, ar, &prep, &e));
    TEST_ASSERT_TRUE(strlen(e.msg) > 0);
    tp_arena_destroy(ar);
}

void test_override_collision(void) {
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[2] = {mk("a.png", -1), mk("b.png", -1)};
    tp_result r = mk_result(s, 2);
    /* rename b -> a: now two finals equal "a" */
    tp_export_name_override ov = {.raw_name = "b.png", .final_name = "a"};
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    o.overrides = &ov;
    o.override_count = 1;
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_normalize(&r, &o, ar, &prep, &e));
    tp_arena_destroy(ar);
}

void test_alias_entries(void) {
    tp_arena *ar = tp_arena_create(0);
    /* sprite[1] "b" aliases sprite[0] "a" (result index 0). */
    tp_sprite s[2] = {mk("a", -1), mk("b", 0)};
    tp_result r = mk_result(s, 2);
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, NULL, ar, &prep, &e));
    TEST_ASSERT_EQUAL_INT(2, prep.sprite_count);
    const tp_export_sprite *a = find_final(&prep, "a");
    const tp_export_sprite *b = find_final(&prep, "b");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(-1, a->alias_of);
    /* b's alias_of points at the prepared index of "a". */
    int ia = (int)(a - prep.sprites);
    TEST_ASSERT_EQUAL_INT(ia, b->alias_of);
    tp_arena_destroy(ar);
}

void test_final_name_sort(void) {
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[3] = {mk("zebra", -1), mk("apple", -1), mk("mango", -1)};
    tp_result r = mk_result(s, 3);
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, NULL, ar, &prep, &e));
    TEST_ASSERT_EQUAL_STRING("apple", prep.sprites[0].final_name);
    TEST_ASSERT_EQUAL_STRING("mango", prep.sprites[1].final_name);
    TEST_ASSERT_EQUAL_STRING("zebra", prep.sprites[2].final_name);
    tp_arena_destroy(ar);
}

void test_scale_stored(void) {
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[1] = {mk("x", -1)};
    tp_result r = mk_result(s, 1);
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    o.scale = 2.0F;
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, &o, ar, &prep, &e));
    TEST_ASSERT_TRUE(fabsf(prep.scale - 2.0F) < 1e-6F);
    tp_arena_destroy(ar);
}

void test_explicit_animation(void) {
    /* Explicit animation assembled verbatim (order + fps preserved). Numeric
     * suffixes are NOT auto-grouped -- only the explicit "walk" appears, and the
     * lone-looking walk_01/walk_02 sprites never form a group of their own. */
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[2] = {mk("walk_01", -1), mk("walk_02", -1)};
    tp_result r = mk_result(s, 2);
    const char *frames[2] = {"walk_02", "walk_01"}; /* deliberately reversed */
    tp_export_anim_in ein;
    memset(&ein, 0, sizeof ein);
    ein.id = "walk";
    ein.frames = frames;
    ein.frame_count = 2;
    ein.fps = 12.0F;
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    o.animations = &ein;
    o.animation_count = 1;
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, &o, ar, &prep, &e));
    /* exactly one animation -- the explicit one; no auto group is added. */
    TEST_ASSERT_EQUAL_INT(1, prep.animation_count);
    const tp_export_anim *walk = find_anim(&prep, "walk");
    TEST_ASSERT_NOT_NULL(walk);
    TEST_ASSERT_TRUE(fabsf(walk->fps - 12.0F) < 1e-6F);
    TEST_ASSERT_EQUAL_STRING("walk_02", walk->frames[0]); /* explicit order preserved */
    TEST_ASSERT_EQUAL_STRING("walk_01", walk->frames[1]);
    tp_arena_destroy(ar);
}

void test_frame_follows_rename(void) {
    /* A4: a renamed sprite. Frames are stored in override-KEY space and must
     * resolve to the FINAL name -- the rename -- in the exported frame list
     * (arch review 3.2). An un-renamed frame keeps its key. */
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[2] = {mk("hero.png", -1), mk("gem.png", -1)};
    tp_result r = mk_result(s, 2);
    tp_export_name_override ov = {.raw_name = "hero.png", .final_name = "champion"};
    const char *frames[2] = {"hero", "gem"}; /* KEY space (ext stripped) */
    tp_export_anim_in ein;
    memset(&ein, 0, sizeof ein);
    ein.id = "run";
    ein.frames = frames;
    ein.frame_count = 2;
    ein.fps = 10.0F;
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    o.overrides = &ov;
    o.override_count = 1;
    o.animations = &ein;
    o.animation_count = 1;
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_normalize(&r, &o, ar, &prep, &e), e.msg);
    TEST_ASSERT_NOT_NULL(find_final(&prep, "champion"));
    TEST_ASSERT_NULL(find_final(&prep, "hero"));
    const tp_export_anim *run = find_anim(&prep, "run");
    TEST_ASSERT_NOT_NULL(run);
    TEST_ASSERT_EQUAL_INT(2, run->frame_count);
    TEST_ASSERT_EQUAL_STRING("champion", run->frames[0]); /* follows the rename */
    TEST_ASSERT_EQUAL_STRING("gem", run->frames[1]);      /* un-renamed keeps its key */
    tp_arena_destroy(ar);
}

void test_frame_key_ext_resolves(void) {
    /* A4 back-compat: frames stored in KEY space (folder kept, ext stripped)
     * resolve to the matching sprite even though the raw name carries the ext.
     * With no rename the final name equals the key -> byte-identical to pre-A4. */
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[2] = {mk("chars/walk_01.png", -1), mk("chars/walk_02.png", -1)};
    tp_result r = mk_result(s, 2);
    const char *frames[2] = {"chars/walk_02", "chars/walk_01"}; /* reversed, keys */
    tp_export_anim_in ein;
    memset(&ein, 0, sizeof ein);
    ein.id = "walk";
    ein.frames = frames;
    ein.frame_count = 2;
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    o.animations = &ein;
    o.animation_count = 1;
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_normalize(&r, &o, ar, &prep, &e), e.msg);
    const tp_export_anim *walk = find_anim(&prep, "walk");
    TEST_ASSERT_NOT_NULL(walk);
    TEST_ASSERT_EQUAL_STRING("chars/walk_02", walk->frames[0]); /* explicit order preserved */
    TEST_ASSERT_EQUAL_STRING("chars/walk_01", walk->frames[1]);
    tp_arena_destroy(ar);
}

void test_dangling_frame(void) {
    /* A4: an animation frame with no matching packed sprite fails loudly, naming
     * the animation id and the frame string (plan L-4 / boundary rule 4). */
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[1] = {mk("hero.png", -1)};
    tp_result r = mk_result(s, 1);
    const char *frames[1] = {"ghost"};
    tp_export_anim_in ein;
    memset(&ein, 0, sizeof ein);
    ein.id = "run";
    ein.frames = frames;
    ein.frame_count = 1;
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    o.animations = &ein;
    o.animation_count = 1;
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_normalize(&r, &o, ar, &prep, &e));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.msg, "run"), e.msg);   /* names the animation */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(e.msg, "ghost"), e.msg); /* names the frame */
    tp_arena_destroy(ar);
}

void test_no_auto_grouping(void) {
    /* Numeric-suffix sprites with NO explicit animation produce ZERO animations
     * (auto-grouping was removed, ux.md 3.7b -- bob still auto-promotes each
     * sprite to a 1-frame anim on the engine side, independent of this list). */
    tp_arena *ar = tp_arena_create(0);
    tp_sprite s[3] = {mk("walk_10", -1), mk("walk_1", -1), mk("walk_2", -1)};
    tp_result r = mk_result(s, 3);
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, NULL, ar, &prep, &e));
    TEST_ASSERT_EQUAL_INT(0, prep.animation_count);
    TEST_ASSERT_NULL(find_anim(&prep, "walk"));
    tp_arena_destroy(ar);
}
// #endregion

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ext_strip);
    RUN_TEST(test_folder_strip_toggle);
    RUN_TEST(test_override_verbatim);
    RUN_TEST(test_munge_collision);
    RUN_TEST(test_override_collision);
    RUN_TEST(test_alias_entries);
    RUN_TEST(test_final_name_sort);
    RUN_TEST(test_scale_stored);
    RUN_TEST(test_explicit_animation);
    RUN_TEST(test_frame_follows_rename);
    RUN_TEST(test_frame_key_ext_resolves);
    RUN_TEST(test_dangling_frame);
    RUN_TEST(test_no_auto_grouping);
    return UNITY_END();
}
