/* Normalization pass unit tests (ROADMAP Phase 2): ext/folder strip, per-sprite
 * export-name override (rename), numeric-suffix animation auto-grouping +
 * explicit override/merge, munge collision, alias entries, final-name sort,
 * scale. Pure tp_core -- synthetic tp_results, NO builder. */

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

void test_auto_group_numeric_order(void) {
    tp_arena *ar = tp_arena_create(0);
    /* walk_1/2/10 auto-group "walk"; jump_1 is lone (min 2 -> no group). */
    tp_sprite s[4] = {mk("walk_10", -1), mk("walk_1", -1), mk("walk_2", -1), mk("jump_1", -1)};
    tp_result r = mk_result(s, 4);
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, NULL, ar, &prep, &e));
    const tp_export_anim *walk = find_anim(&prep, "walk");
    TEST_ASSERT_NOT_NULL(walk);
    TEST_ASSERT_EQUAL_INT(3, walk->frame_count);
    /* numeric order, not lexical: 1, 2, 10 */
    TEST_ASSERT_EQUAL_STRING("walk_1", walk->frames[0]);
    TEST_ASSERT_EQUAL_STRING("walk_2", walk->frames[1]);
    TEST_ASSERT_EQUAL_STRING("walk_10", walk->frames[2]);
    TEST_ASSERT_TRUE(fabsf(walk->fps - 30.0F) < 1e-6F);
    TEST_ASSERT_NULL(find_anim(&prep, "jump"));
    tp_arena_destroy(ar);
}

void test_explicit_overrides_auto(void) {
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
    /* exactly one "walk" -- the explicit one wins (auto suppressed). */
    TEST_ASSERT_EQUAL_INT(1, prep.animation_count);
    const tp_export_anim *walk = find_anim(&prep, "walk");
    TEST_ASSERT_NOT_NULL(walk);
    TEST_ASSERT_TRUE(fabsf(walk->fps - 12.0F) < 1e-6F);
    TEST_ASSERT_EQUAL_STRING("walk_02", walk->frames[0]); /* explicit order preserved */
    TEST_ASSERT_EQUAL_STRING("walk_01", walk->frames[1]);
    tp_arena_destroy(ar);
}

void test_override_breaks_group(void) {
    tp_arena *ar = tp_arena_create(0);
    /* walk_01/02/03 would group; rename walk_03 -> idle breaks it to 2 frames
     * (still a group) -- and rename a lone sprite INTO a new group. */
    tp_sprite s[4] = {mk("walk_01", -1), mk("walk_02", -1), mk("walk_03", -1), mk("misc", -1)};
    tp_result r = mk_result(s, 4);
    tp_export_name_override ov[2] = {
        {.raw_name = "walk_03", .final_name = "idle"},   /* leaves walk with 2 */
        {.raw_name = "misc", .final_name = "run_09"},    /* alone -> no run group */
    };
    tp_normalize_opts o;
    tp_normalize_opts_defaults(&o);
    o.overrides = ov;
    o.override_count = 2;
    tp_export_prepared prep;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_normalize(&r, &o, ar, &prep, &e));
    const tp_export_anim *walk = find_anim(&prep, "walk");
    TEST_ASSERT_NOT_NULL(walk);
    TEST_ASSERT_EQUAL_INT(2, walk->frame_count); /* walk_01, walk_02 */
    TEST_ASSERT_NULL(find_anim(&prep, "run"));   /* run_09 is lone */
    TEST_ASSERT_NOT_NULL(find_final(&prep, "idle"));
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
    RUN_TEST(test_auto_group_numeric_order);
    RUN_TEST(test_explicit_overrides_auto);
    RUN_TEST(test_override_breaks_group);
    return UNITY_END();
}
