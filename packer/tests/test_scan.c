/* tp_scan unit tests (op-layer step A2): directory recursion + image-extension
 * whitelist moved out of the GUI (gui_scan.c) into tp_core, single source for every
 * frontend that expands a folder source. Builds a fixture tree under the ctest
 * scratch dir (argv[1]) at runtime and pins TODAY's has_image_ext/entry_cmp
 * behavior verbatim (case-insensitive ext match, byte-wise rel sort, a bare
 * dotfile whose whole name equals a whitelisted extension counts as an image). */

#define _CRT_SECURE_NO_WARNINGS

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

#include "tp_core/tp_scan.h"
#include "unity.h"

static const char *g_dir; /* scratch dir (argv[1]) */
static char g_root[600];  /* g_dir/fixture -- the tree tp_scan_dir walks */

void setUp(void) {}
void tearDown(void) {}

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

/* Reads the whole file at `abs` and asserts it equals `expect` -- proves an entry's
 * abs path actually resolves to the file its rel/size were derived from, without
 * hardcoding a platform separator style in the assertion. */
static void assert_file_content(const char *abs, const char *expect) {
    FILE *f = fopen(abs, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, abs);
    char buf[128] = {0};
    size_t n = fread(buf, 1U, sizeof buf - 1U, f);
    fclose(f);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING(expect, buf);
}

/* Builds:
 *   fixture/hero.png            image, top-level
 *   fixture/readme.txt          non-image, top-level (excluded)
 *   fixture/.png                dotfile whose whole name IS a whitelisted ext (included today)
 *   fixture/BADGE.PNG           uppercase ext (case-insensitive match, included)
 *   fixture/empty_dir/          empty subfolder (no entries)
 *   fixture/tank/walk_01.png    image, nested
 *   fixture/tank/notes.txt      non-image, nested (excluded)
 */
static void build_fixture(void) {
    (void)snprintf(g_root, sizeof g_root, "%s/fixture", g_dir);
    mkdir_p(g_root);
    char p[700];

    (void)snprintf(p, sizeof p, "%s/hero.png", g_root);
    write_file(p, "HERO-IMG-DATA");

    (void)snprintf(p, sizeof p, "%s/readme.txt", g_root);
    write_file(p, "not an image");

    (void)snprintf(p, sizeof p, "%s/.png", g_root);
    write_file(p, "X");

    (void)snprintf(p, sizeof p, "%s/BADGE.PNG", g_root);
    write_file(p, "UPPERCASE-EXT-IMG");

    (void)snprintf(p, sizeof p, "%s/empty_dir", g_root);
    mkdir_p(p);

    (void)snprintf(p, sizeof p, "%s/tank", g_root);
    mkdir_p(p);
    (void)snprintf(p, sizeof p, "%s/tank/walk_01.png", g_root);
    write_file(p, "WALK-FRAME-01");
    (void)snprintf(p, sizeof p, "%s/tank/notes.txt", g_root);
    write_file(p, "skip");
}

static const tp_scan_entry *find_rel(const tp_scan_result *r, const char *rel) {
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].rel, rel) == 0) {
            return &r->entries[i];
        }
    }
    return NULL;
}
// #endregion

// #region tests
void test_missing_dir_is_empty(void) {
    char missing[700];
    (void)snprintf(missing, sizeof missing, "%s/does_not_exist", g_dir);
    tp_scan_result r;
    tp_scan_dir(missing, &r);
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    tp_scan_free(&r); /* safe on an empty result */
}

void test_null_and_empty_abs_dir_are_safe(void) {
    tp_scan_result r;
    tp_scan_dir(NULL, &r);
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    tp_scan_dir("", &r);
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    tp_scan_free(NULL); /* no crash */
}

void test_empty_subdir_is_empty(void) {
    char empty_dir[700];
    (void)snprintf(empty_dir, sizeof empty_dir, "%s/empty_dir", g_root);
    tp_scan_result r;
    tp_scan_dir(empty_dir, &r);
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    tp_scan_free(&r);
}

/* Entry set, rel-path construction, sort order (byte-wise strcmp, not natural),
 * non-image exclusion, and today's dotfile/case-insensitive whitelist behavior. */
void test_fixture_walk(void) {
    tp_scan_result r;
    tp_scan_dir(g_root, &r);

    TEST_ASSERT_EQUAL_INT(4, r.count); /* .png, BADGE.PNG, hero.png, tank/walk_01.png -- .txt files excluded */
    TEST_ASSERT_NOT_NULL(r.entries);

    /* sort order: byte-wise strcmp on rel ('.' < 'B' < 'h' < 't') */
    TEST_ASSERT_EQUAL_STRING(".png", r.entries[0].rel);
    TEST_ASSERT_EQUAL_STRING("BADGE.PNG", r.entries[1].rel);
    TEST_ASSERT_EQUAL_STRING("hero.png", r.entries[2].rel);
    TEST_ASSERT_EQUAL_STRING("tank/walk_01.png", r.entries[3].rel);

    /* non-image files never made it in */
    TEST_ASSERT_NULL(find_rel(&r, "readme.txt"));
    TEST_ASSERT_NULL(find_rel(&r, "tank/notes.txt"));

    /* rel/abs/size/content agree, and mtime is populated (opaque, just nonzero) */
    const tp_scan_entry *hero = find_rel(&r, "hero.png");
    TEST_ASSERT_NOT_NULL(hero);
    TEST_ASSERT_EQUAL_INT((int)strlen("HERO-IMG-DATA"), (int)hero->size);
    TEST_ASSERT_NOT_EQUAL(0, hero->mtime);
    assert_file_content(hero->abs, "HERO-IMG-DATA");

    const tp_scan_entry *dotfile = find_rel(&r, ".png");
    TEST_ASSERT_NOT_NULL(dotfile); /* bare dotfile whose whole name is a whitelisted ext: pinned as included */
    TEST_ASSERT_EQUAL_INT(1, (int)dotfile->size);
    TEST_ASSERT_NOT_EQUAL(0, dotfile->mtime);
    assert_file_content(dotfile->abs, "X");

    const tp_scan_entry *badge = find_rel(&r, "BADGE.PNG");
    TEST_ASSERT_NOT_NULL(badge); /* uppercase ext: case-insensitive match pinned */
    TEST_ASSERT_EQUAL_INT((int)strlen("UPPERCASE-EXT-IMG"), (int)badge->size);
    TEST_ASSERT_NOT_EQUAL(0, badge->mtime);
    assert_file_content(badge->abs, "UPPERCASE-EXT-IMG");

    const tp_scan_entry *walk = find_rel(&r, "tank/walk_01.png");
    TEST_ASSERT_NOT_NULL(walk); /* nested rel-path construction: "tank/walk_01.png", '/'-normalized */
    TEST_ASSERT_EQUAL_INT((int)strlen("WALK-FRAME-01"), (int)walk->size);
    TEST_ASSERT_NOT_EQUAL(0, walk->mtime);
    assert_file_content(walk->abs, "WALK-FRAME-01");

    tp_scan_free(&r);
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
}

void test_is_dir_and_exists(void) {
    char hero[700];
    (void)snprintf(hero, sizeof hero, "%s/hero.png", g_root);
    char missing[700];
    (void)snprintf(missing, sizeof missing, "%s/nope.png", g_root);

    TEST_ASSERT_TRUE(tp_scan_is_dir(g_root));
    TEST_ASSERT_FALSE(tp_scan_is_dir(hero)); /* a file, not a dir */
    TEST_ASSERT_FALSE(tp_scan_is_dir(missing));
    TEST_ASSERT_FALSE(tp_scan_is_dir(NULL));
    TEST_ASSERT_FALSE(tp_scan_is_dir(""));

    TEST_ASSERT_TRUE(tp_scan_exists(g_root));
    TEST_ASSERT_TRUE(tp_scan_exists(hero));
    TEST_ASSERT_FALSE(tp_scan_exists(missing));
    TEST_ASSERT_FALSE(tp_scan_exists(NULL));
    TEST_ASSERT_FALSE(tp_scan_exists(""));
}
// #endregion

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    build_fixture();

    UNITY_BEGIN();
    RUN_TEST(test_missing_dir_is_empty);
    RUN_TEST(test_null_and_empty_abs_dir_are_safe);
    RUN_TEST(test_empty_subdir_is_empty);
    RUN_TEST(test_fixture_walk);
    RUN_TEST(test_is_dir_and_exists);
    return UNITY_END();
}
