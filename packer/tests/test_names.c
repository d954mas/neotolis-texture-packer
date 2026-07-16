/* tp_names unit tests (op-layer step A1): the canonical sprite-name key,
 * natural order, and common-prefix helpers -- the governing gate for the
 * dot-basename key fix (the showcase draw-hash gate is blind to dotfiles). */

#include "tp_core/tp_names.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* --- (a) export-key table ------------------------------------------------- */

static const char *key_of(const char *raw) {
    static char buf[256];
    tp_sprite_export_key(raw, buf, sizeof buf);
    return buf;
}

void test_key_table(void) {
    TEST_ASSERT_EQUAL_STRING("hero", key_of("hero.png"));            /* plain */
    TEST_ASSERT_EQUAL_STRING("tank/walk_01", key_of("tank/walk_01.png")); /* folder child keeps folder */
    TEST_ASSERT_EQUAL_STRING(".png", key_of(".png"));               /* bare dotfile kept */
    TEST_ASSERT_EQUAL_STRING("tank/.png", key_of("tank/.png"));     /* dotfile in folder kept (the fix) */
    TEST_ASSERT_EQUAL_STRING("a.b", key_of("a.b.png"));             /* multi-dot: last only */
    TEST_ASSERT_EQUAL_STRING("hero", key_of("hero"));               /* no extension */
    TEST_ASSERT_EQUAL_STRING("tank/", key_of("tank/"));             /* trailing slash: empty basename, kept */
    TEST_ASSERT_EQUAL_STRING(".gitkeep", key_of(".gitkeep"));       /* dotfile kept */
    TEST_ASSERT_EQUAL_STRING("dir/.gitkeep", key_of("dir/.gitkeep")); /* dotfile in folder kept (the fix) */
}

void test_key_null_and_zero_cap_are_safe(void) {
    char buf[8];
    tp_sprite_export_key(NULL, buf, sizeof buf); /* NULL raw -> "" */
    TEST_ASSERT_EQUAL_STRING("", buf);
    tp_sprite_export_key("hero.png", NULL, 8); /* NULL out: no crash */
    tp_sprite_export_key("hero.png", buf, 0);  /* zero cap: no write */
}

/* --- (b) caller-composition: file sources strip the folder FIRST ---------- */
/* A file source stored WITH a folder ("sprites/hero.png") must produce key
 * "hero", NOT "sprites/hero". The GUI/pack path strips the folder via
 * path_last/base_name BEFORE keying (gui_pack.c pre-strips file sources); the
 * key fn itself is folder-keeping and would otherwise retain "sprites/". This
 * pins that the composition -- path_last then key -- yields the bare stem, and
 * that it matches keying a folder child (which keeps its atlas-relative folder). */

static const char *path_last_local(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') {
            b = q + 1;
        }
    }
    return b;
}

void test_key_file_source_composition(void) {
    char buf[256];
    tp_sprite_export_key(path_last_local("sprites/hero.png"), buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("hero", buf); /* folder dropped by path_last, ext by key */

    /* Folder-child keeps its relative folder (rel passed to key directly). */
    tp_sprite_export_key("hero.png", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("hero", buf); /* pack-path base_name pre-strip agrees */

    /* Windows-style separator handled by path_last too. */
    tp_sprite_export_key(path_last_local("sprites\\hero.png"), buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("hero", buf);
}

/* --- (c) natural-order table ---------------------------------------------- */

static int sign(int v) { return (v < 0) ? -1 : (v > 0) ? 1 : 0; }

void test_nat_cmp_table(void) {
    TEST_ASSERT_EQUAL_INT(-1, sign(tp_nat_cmp("walk_2", "walk_10")));  /* numeric run: 2 < 10 */
    TEST_ASSERT_EQUAL_INT(1, sign(tp_nat_cmp("walk_10", "walk_2")));
    TEST_ASSERT_EQUAL_INT(0, sign(tp_nat_cmp("img_01", "img_1")));     /* leading zeros ignored */
    TEST_ASSERT_EQUAL_INT(1, sign(tp_nat_cmp("a10", "a9")));           /* 10 > 9 */
    TEST_ASSERT_EQUAL_INT(-1, sign(tp_nat_cmp("frame_9", "frame_10")));
    TEST_ASSERT_EQUAL_INT(-1, sign(tp_nat_cmp("apple", "banana")));    /* byte-wise fallback */
    TEST_ASSERT_EQUAL_INT(0, sign(tp_nat_cmp("same", "same")));
    TEST_ASSERT_EQUAL_INT(-1, sign(tp_nat_cmp("run", "run_1")));       /* prefix shorter first */
    TEST_ASSERT_EQUAL_INT(0, sign(tp_nat_cmp("", "")));
}

/* --- (d) common-prefix table ---------------------------------------------- */

void test_common_prefix_table(void) {
    char out[192];

    const char *two[2] = {"walk_01", "walk_02"};
    tp_names_common_prefix(two, 2, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("walk", out); /* trailing digits + '_' trimmed */

    const char *three[3] = {"walk_01", "walk_02", "walk_03"};
    tp_names_common_prefix(three, 3, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("walk", out);

    const char *folder[2] = {"tank/walk_1", "tank/walk_2"};
    tp_names_common_prefix(folder, 2, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("tank/walk", out);

    const char *none[2] = {"run_1", "jump_1"};
    tp_names_common_prefix(none, 2, out, sizeof out); /* no shared prefix */
    TEST_ASSERT_EQUAL_STRING("", out);

    const char *one[1] = {"hero"};
    tp_names_common_prefix(one, 1, out, sizeof out); /* single: whole name, no trailing to trim */
    TEST_ASSERT_EQUAL_STRING("hero", out);

    tp_names_common_prefix(two, 0, out, sizeof out); /* empty set -> "" */
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_key_table);
    RUN_TEST(test_key_null_and_zero_cap_are_safe);
    RUN_TEST(test_key_file_source_composition);
    RUN_TEST(test_nat_cmp_table);
    RUN_TEST(test_common_prefix_table);
    return UNITY_END();
}
