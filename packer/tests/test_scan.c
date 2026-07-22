/* tp_scan unit tests (op-layer step A2): directory recursion + image-extension
 * whitelist moved out of the GUI (gui_scan.c) into tp_core, single source for every
 * frontend that expands a folder source. Builds a fixture tree under the ctest
 * scratch dir (argv[1]) at runtime and pins TODAY's has_image_ext/entry_cmp
 * behavior verbatim (case-insensitive ext match, byte-wise rel sort, a bare
 * dotfile whose whole name equals a whitelisted extension counts as an image). */

#define _CRT_SECURE_NO_WARNINGS

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TP_TEST_MKDIR(p) _mkdir(p)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define TP_TEST_MKDIR(p) mkdir((p), 0777)
#endif

#include "tp_core/tp_identity.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_srckey.h"
#include "../src/tp_fs_internal.h"
#include "unity.h"

static const char *g_dir; /* scratch dir (argv[1]) */
static char g_root[600];  /* g_dir/fixture -- the tree tp_scan_dir walks */

void tp_scan__test_set_alloc_fail(int nth);

void setUp(void) { tp_scan__test_set_alloc_fail(-1); }
void tearDown(void) { tp_scan__test_set_alloc_fail(-1); }

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

    /* Remove artifacts left by older revisions of the UTF-8/long-path tests,
     * which used to place their images below the four-entry fixture root. */
    (void)snprintf(p, sizeof p,
                   "%s/\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5/"
                   "\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82.png",
                   g_root);
    (void)tp_fs_remove_file(p);
    (void)snprintf(p, sizeof p, "%s", g_root);
    for (int i = 0; i < 12; i++) {
        size_t used = strlen(p);
        if (used + strlen("/longpathsegment") + 1U < sizeof p) {
            memcpy(p + used, "/longpathsegment", strlen("/longpathsegment") + 1U);
        }
    }
    size_t used = strlen(p);
    if (used + strlen("/long.png") + 1U < sizeof p) {
        memcpy(p + used, "/long.png", strlen("/long.png") + 1U);
        (void)tp_fs_remove_file(p);
    }

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

/* R5b-2 dir-lister fixture. Under g_root/listdir:
 *   A.ntpjournal            regular file, matches ".ntpjournal"
 *   B.ntpjournal            regular file, matches ".ntpjournal"
 *   A.ntpjournal.lock       regular file, does NOT match ".ntpjournal" (companion lock)
 *   notes.txt               regular file, non-matching suffix
 *   sub/                    subdirectory (must be skipped -- regular files only)
 *   sub/C.ntpjournal        nested match (must NOT appear -- non-recursive)
 */
static char g_listdir[700];
static void build_listdir_fixture(void) {
    (void)snprintf(g_listdir, sizeof g_listdir, "%s/listdir", g_root);
    mkdir_p(g_listdir);
    char p[800];
    (void)snprintf(p, sizeof p, "%s/A.ntpjournal", g_listdir);
    write_file(p, "A");
    (void)snprintf(p, sizeof p, "%s/B.ntpjournal", g_listdir);
    write_file(p, "B");
    (void)snprintf(p, sizeof p, "%s/A.ntpjournal.lock", g_listdir);
    write_file(p, "L");
    (void)snprintf(p, sizeof p, "%s/notes.txt", g_listdir);
    write_file(p, "T");
    (void)snprintf(p, sizeof p, "%s/sub", g_listdir);
    mkdir_p(p);
    (void)snprintf(p, sizeof p, "%s/sub/C.ntpjournal", g_listdir);
    write_file(p, "C");
}

// #endregion

// #region tests
void test_missing_dir_is_structured_and_atomic(void) {
    char missing[700];
    (void)snprintf(missing, sizeof missing, "%s/does_not_exist", g_dir);
    tp_scan_result r;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND,
                          tp_scan_dir(missing, &r, &error));
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    TEST_ASSERT_NOT_EQUAL(0, error.msg[0]);
    tp_scan_free(&r); /* safe on an empty result */
}

void test_null_and_empty_abs_dir_are_safe(void) {
    tp_scan_result r;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_scan_dir(NULL, &r, NULL));
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_scan_dir("", &r, NULL));
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_scan_dir(g_root, NULL, NULL));
    tp_scan_free(NULL); /* no crash */
}

void test_empty_subdir_is_empty(void) {
    char empty_dir[700];
    (void)snprintf(empty_dir, sizeof empty_dir, "%s/empty_dir", g_root);
    tp_scan_result r;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_scan_dir(empty_dir, &r, NULL));
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    tp_scan_free(&r);
}

/* Entry set, rel-path construction, sort order (byte-wise strcmp, not natural),
 * non-image exclusion, and today's dotfile/case-insensitive whitelist behavior. */
void test_fixture_walk(void) {
    tp_scan_result r;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_scan_dir(g_root, &r, &error), error.msg);

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

    long long size = -1;
    long long mtime = -1;
    TEST_ASSERT_TRUE(tp_scan_file_stat(hero, &size, &mtime));
    TEST_ASSERT_EQUAL_INT64(13, size);
    TEST_ASSERT_TRUE(mtime >= 0);
    TEST_ASSERT_FALSE(tp_scan_file_stat(g_root, &size, &mtime));
    TEST_ASSERT_FALSE(tp_scan_file_stat(missing, &size, &mtime));
}

typedef struct visit_probe {
    int count;
    uint64_t bytes;
    int stop_after;
} visit_probe;

static bool count_journal_name(void *ctx, const char *name, uint64_t size) {
    visit_probe *probe = (visit_probe *)ctx;
    TEST_ASSERT_NOT_NULL(name);
    probe->count++;
    probe->bytes += size;
    return probe->stop_after == 0 || probe->count < probe->stop_after;
}

void test_visit_dir_streams_matching_names(void) {
    visit_probe all = {0};
    TEST_ASSERT_TRUE(tp_scan_visit_dir(g_listdir, ".ntpjournal", count_journal_name, &all));
    TEST_ASSERT_EQUAL_INT(2, all.count);
    TEST_ASSERT_EQUAL_UINT64(2u, all.bytes); /* fixture files contain one byte each */

    visit_probe bounded = {.stop_after = 1};
    TEST_ASSERT_TRUE(tp_scan_visit_dir(g_listdir, ".ntpjournal", count_journal_name, &bounded));
    TEST_ASSERT_EQUAL_INT(1, bounded.count);
}

#ifndef _WIN32
/* Recovery discovery must never follow symlinks or return special files. The
 * caller opens every returned journal path, so returning a FIFO can block GUI
 * startup indefinitely and following a symlink escapes the recovery directory. */
void test_visit_dir_excludes_fifo_and_symlink(void) {
    char fifo_path[800];
    char link_path[800];
    (void)snprintf(fifo_path, sizeof fifo_path, "%s/pipe.ntpjournal", g_listdir);
    (void)snprintf(link_path, sizeof link_path, "%s/link.ntpjournal", g_listdir);
    (void)unlink(fifo_path);
    (void)unlink(link_path);
    TEST_ASSERT_EQUAL_INT(0, mkfifo(fifo_path, 0600));
    TEST_ASSERT_EQUAL_INT(0, symlink("A.ntpjournal", link_path));

    visit_probe probe = {0};
    TEST_ASSERT_TRUE(tp_scan_visit_dir(g_listdir, ".ntpjournal", count_journal_name, &probe));
    TEST_ASSERT_EQUAL_INT(2, probe.count);
    (void)unlink(link_path);
    (void)unlink(fifo_path);
}
#endif

/* A dir-open or argument failure returns false; startup recovery degrades to an empty collect. */
void test_visit_dir_missing_and_bad_args_return_false(void) {
    char missing[800];
    (void)snprintf(missing, sizeof missing, "%s/does_not_exist", g_listdir);
    visit_probe probe = {0};
    TEST_ASSERT_FALSE(tp_scan_visit_dir(missing, ".ntpjournal", count_journal_name, &probe));
    TEST_ASSERT_FALSE(tp_scan_visit_dir(NULL, "", count_journal_name, &probe));
    TEST_ASSERT_FALSE(tp_scan_visit_dir("", "", count_journal_name, &probe));
    TEST_ASSERT_FALSE(tp_scan_visit_dir(g_listdir, "", NULL, &probe));
    TEST_ASSERT_EQUAL_INT(0, probe.count);
}

void test_utf8_filesystem_backend_round_trips_non_ascii_paths(void) {
    static const char utf8_dir[] = "\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5"; /* данные */
    static const char utf8_file[] =
        "\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82.png"; /* спрайт.png */
    char dir[800];
    char path[1100];
    char rel[1100];
    char root[800];
    (void)snprintf(root, sizeof root, "%s/utf8_fixture", g_dir);
    (void)snprintf(dir, sizeof dir, "%s/%s", root, utf8_dir);
    (void)snprintf(path, sizeof path, "%s/%s", dir, utf8_file);
    (void)snprintf(rel, sizeof rel, "%s/%s", utf8_dir, utf8_file);

    tp_mkdirs(dir);
    FILE *f = tp_fs_fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(tp_fs_write_all(f, "UTF8", 4U));
    TEST_ASSERT_TRUE(tp_fs_flush(f));
    TEST_ASSERT_TRUE(tp_fs_close(f));

    tp_fs_info info;
    TEST_ASSERT_TRUE(tp_fs_stat(path, &info));
    TEST_ASSERT_EQUAL_INT(TP_FS_KIND_REGULAR, info.kind);
    TEST_ASSERT_EQUAL_UINT64(4U, info.size);
    TEST_ASSERT_TRUE(tp_fs_exists(path));

    tp_scan_result r;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_scan_dir(root, &r, &error), error.msg);
    const tp_scan_entry *entry = find_rel(&r, rel);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_INT(4, (int)entry->size);
    tp_scan_free(&r);
}

/* A source-local key may be much longer than the old 255-byte GUI-era buffer,
 * and the resolved decode path may be longer than 511 bytes.  The scanner is
 * the authority for both values, so neither may be silently dropped. */
void test_deep_utf8_path_is_not_silently_omitted(void) {
    char root[TP_IDENTITY_PATH_MAX];
    char dir[TP_IDENTITY_PATH_MAX];
    char rel[TP_SRCKEY_MAX];
    char path[TP_IDENTITY_PATH_MAX];
    (void)snprintf(root, sizeof root, "%s/deep_utf8_scan", g_dir);
    (void)snprintf(dir, sizeof dir, "%s", root);
    rel[0] = '\0';

    static const char component[] =
        "\xD1\x81\xD0\xB5\xD0\xB3\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82";
    for (int i = 0; i < 24; i++) {
        char segment[96];
        (void)snprintf(segment, sizeof segment, "%s_%02d_abcdefghijklmnop",
                       component, i);
        size_t dir_used = strlen(dir);
        size_t rel_used = strlen(rel);
        size_t segment_len = strlen(segment);
        TEST_ASSERT_TRUE(dir_used + 1U + segment_len + 1U < sizeof dir);
        TEST_ASSERT_TRUE(rel_used + (rel_used ? 1U : 0U) + segment_len + 1U <
                         sizeof rel);
        dir[dir_used++] = '/';
        memcpy(dir + dir_used, segment, segment_len + 1U);
        if (rel_used) {
            rel[rel_used++] = '/';
        }
        memcpy(rel + rel_used, segment, segment_len + 1U);
    }
    static const char filename[] =
        "\xD0\xB3\xD0\xBB\xD1\x83\xD0\xB1\xD0\xBE\xD0\xBA\xD0\xB8\xD0\xB9.png";
    size_t dir_used = strlen(dir);
    size_t rel_used = strlen(rel);
    size_t filename_len = strlen(filename);
    TEST_ASSERT_TRUE(dir_used + 1U + filename_len + 1U < sizeof path);
    (void)snprintf(path, sizeof path, "%s/%s", dir, filename);
    TEST_ASSERT_TRUE(rel_used + 1U + filename_len + 1U < sizeof rel);
    rel[rel_used++] = '/';
    memcpy(rel + rel_used, filename, filename_len + 1U);
    TEST_ASSERT_GREATER_THAN_size_t(255U, strlen(rel));
    TEST_ASSERT_GREATER_THAN_size_t(511U, strlen(path));

    tp_mkdirs(dir);
    FILE *file = tp_fs_fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_TRUE(tp_fs_write_all(file, "DEEP", 4U));
    TEST_ASSERT_TRUE(tp_fs_close(file));

    tp_scan_result result;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_scan_dir(root, &result, &error), error.msg);
    const tp_scan_entry *entry = find_rel(&result, rel);
    TEST_ASSERT_NOT_NULL_MESSAGE(entry,
                                 "deep UTF-8 image must not disappear from scan");
    TEST_ASSERT_EQUAL_STRING(path, entry->abs);
    tp_scan_free(&result);
}

void test_scan_allocation_failure_is_oom_and_never_partial(void) {
    tp_scan_result result = {0};
    tp_error error = {0};
    tp_scan__test_set_alloc_fail(2);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM,
                          tp_scan_dir(g_root, &result, &error));
    TEST_ASSERT_NULL(result.entries);
    TEST_ASSERT_EQUAL_INT(0, result.count);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "allocation"));
}

void test_scan_root_overflow_is_structured_and_never_touches_disk(void) {
    char *oversized = (char *)malloc((size_t)TP_IDENTITY_PATH_MAX + 1U);
    TEST_ASSERT_NOT_NULL(oversized);
    memset(oversized, 'x', (size_t)TP_IDENTITY_PATH_MAX);
    oversized[TP_IDENTITY_PATH_MAX] = '\0';
    tp_scan_result result = {0};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_scan_dir(oversized, &result, &error));
    TEST_ASSERT_NULL(result.entries);
    TEST_ASSERT_EQUAL_INT(0, result.count);
    free(oversized);
}

void test_existing_non_directory_is_a_scan_error_not_empty_success(void) {
    char file[TP_IDENTITY_PATH_MAX];
    (void)snprintf(file, sizeof file, "%s/hero.png", g_root);
    tp_scan_result result = {0};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_PATH_RESOLVE_FAILED,
                          tp_scan_dir(file, &result, &error));
    TEST_ASSERT_NULL(result.entries);
    TEST_ASSERT_EQUAL_INT(0, result.count);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "not a direct directory"));
}

static void assert_file_byte(const char *path, char expected) {
    FILE *file = tp_fs_fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char actual = '\0';
    TEST_ASSERT_TRUE(tp_fs_read_all(file, &actual, 1U));
    TEST_ASSERT_EQUAL_INT(expected, actual);
    TEST_ASSERT_TRUE(tp_fs_close(file));
}

void test_filesystem_publication_primitives_are_no_clobber_and_replace_safe(void) {
    char source[800];
    char destination[800];
    char moved[800];
    (void)snprintf(source, sizeof source, "%s/fs-publish-source.tmp", g_dir);
    (void)snprintf(destination, sizeof destination, "%s/fs-publish-destination.tmp", g_dir);
    (void)snprintf(moved, sizeof moved, "%s/fs-publish-moved.tmp", g_dir);
    (void)tp_fs_remove_file(source);
    (void)tp_fs_remove_file(destination);
    (void)tp_fs_remove_file(moved);

    FILE *f = tp_fs_create_exclusive(source, false);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(tp_fs_write_all(f, "A", 1U));
    TEST_ASSERT_TRUE(tp_fs_sync(f));
    TEST_ASSERT_TRUE(tp_fs_close(f));
    TEST_ASSERT_NULL(tp_fs_create_exclusive(source, false));
    TEST_ASSERT_TRUE(tp_fs_write_file(destination, "B", 1U));

    TEST_ASSERT_EQUAL_INT(TP_FS_MOVE_DESTINATION_EXISTS, tp_fs_move_no_replace(source, destination));
    TEST_ASSERT_TRUE(tp_fs_exists(source));
    TEST_ASSERT_TRUE(tp_fs_exists(destination));
    assert_file_byte(source, 'A');
    assert_file_byte(destination, 'B');
    TEST_ASSERT_TRUE(tp_fs_replace(source, destination));
    TEST_ASSERT_FALSE(tp_fs_exists(source));
    assert_file_byte(destination, 'A');

    f = tp_fs_create_exclusive(source, true);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(tp_fs_write_all(f, "C", 1U));
    TEST_ASSERT_TRUE(tp_fs_close(f));
    TEST_ASSERT_EQUAL_INT(TP_FS_MOVE_OK, tp_fs_move_no_replace(source, moved));
    TEST_ASSERT_FALSE(tp_fs_exists(source));
    TEST_ASSERT_TRUE(tp_fs_exists(moved));
    TEST_ASSERT_TRUE(tp_fs_sync_parent(moved));

    TEST_ASSERT_TRUE(tp_fs_remove_file(destination));
    TEST_ASSERT_TRUE(tp_fs_remove_file(moved));
}

void test_filesystem_backend_rejects_invalid_utf8(void) {
    char invalid[900];
    char sentinel[900];
    (void)snprintf(invalid, sizeof invalid, "%s/bad-\xC3\x28.tmp", g_root);
    (void)snprintf(sentinel, sizeof sentinel, "%s/invalid-sentinel.tmp", g_dir);
    TEST_ASSERT_TRUE(tp_fs_write_file(sentinel, "S", 1U));
    errno = 0;
    TEST_ASSERT_NULL(tp_fs_fopen(invalid, "wb"));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_FALSE(tp_fs_exists(invalid));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_FALSE(tp_fs_create_dir(invalid));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_NULL(tp_fs_dir_open(invalid));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_NULL(tp_fs_create_exclusive(invalid, false));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_FALSE(tp_fs_remove_file(invalid));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_FALSE(tp_fs_replace(invalid, sentinel));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_FALSE(tp_fs_replace(sentinel, invalid));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_EQUAL_INT(TP_FS_MOVE_ERROR,
                          tp_fs_move_no_replace(invalid, sentinel));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_EQUAL_INT(TP_FS_MOVE_ERROR,
                          tp_fs_move_no_replace(sentinel, invalid));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    TEST_ASSERT_FALSE(tp_fs_sync_parent(invalid));
    TEST_ASSERT_EQUAL_INT(EILSEQ, errno);
    assert_file_byte(sentinel, 'S');

    tp_scan_result result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_scan_dir(invalid, &result, &error));
    TEST_ASSERT_NULL(result.entries);
    TEST_ASSERT_EQUAL_INT(0, result.count);
    TEST_ASSERT_TRUE(tp_fs_remove_file(sentinel));
}

#ifndef _WIN32
void test_posix_directory_iterator_rejects_invalid_utf8_entry_atomically(void) {
    char invalid_entry[900];
    (void)snprintf(invalid_entry, sizeof invalid_entry,
                   "%s/raw-bad-\xC3\x28.png", g_root);
    errno = 0;
    int fd = open(invalid_entry, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0 && errno == EILSEQ) {
        TEST_IGNORE_MESSAGE(
            "filesystem rejects invalid UTF-8 filenames before enumeration");
    }
    TEST_ASSERT_TRUE_MESSAGE(
        fd >= 0, "invalid UTF-8 fixture creation failed unexpectedly");
    TEST_ASSERT_EQUAL_INT(0, close(fd));

    tp_scan_result result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_scan_dir(g_root, &result, &error));
    TEST_ASSERT_NULL(result.entries);
    TEST_ASSERT_EQUAL_INT(0, result.count);
    TEST_ASSERT_EQUAL_INT(0, unlink(invalid_entry));
}
#endif

#ifdef _WIN32
void test_windows_filesystem_utf_conversion_reports_small_buffers(void) {
    wchar_t too_small_wide[2];
    errno = 0;
    TEST_ASSERT_FALSE(tp_fs_win32_utf8_to_utf16("abc", too_small_wide, 2U));
    TEST_ASSERT_EQUAL_INT(ERANGE, errno);
    char too_small_utf8[2];
    errno = 0;
    TEST_ASSERT_FALSE(tp_fs_win32_utf16_to_utf8(L"abc", too_small_utf8, 2U));
    TEST_ASSERT_EQUAL_INT(ERANGE, errno);
}

void test_windows_filesystem_backend_handles_extended_absolute_paths(void) {
    char long_dir[900];
    (void)snprintf(long_dir, sizeof long_dir, "%s/long_fixture", g_dir);
    for (int i = 0; i < 12; i++) {
        size_t used = strlen(long_dir);
        TEST_ASSERT_TRUE(used + strlen("/longpathsegment") + 1U < sizeof long_dir);
        memcpy(long_dir + used, "/longpathsegment", strlen("/longpathsegment") + 1U);
    }
    TEST_ASSERT_TRUE(strlen(long_dir) >= 248U);
    tp_mkdirs(long_dir);
    TEST_ASSERT_TRUE(tp_fs_is_dir(long_dir));

    char path[960];
    (void)snprintf(path, sizeof path, "%s/long.png", long_dir);
    FILE *f = tp_fs_fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(tp_fs_write_all(f, "LONG", 4U));
    TEST_ASSERT_TRUE(tp_fs_close(f));
    TEST_ASSERT_TRUE(tp_fs_exists(path));

    wchar_t *native = tp_fs_win32_path_alloc(path);
    TEST_ASSERT_NOT_NULL(native);
    TEST_ASSERT_EQUAL_INT(L'\\', native[0]);
    TEST_ASSERT_EQUAL_INT(L'\\', native[1]);
    TEST_ASSERT_EQUAL_INT(L'?', native[2]);
    TEST_ASSERT_EQUAL_INT(L'\\', native[3]);
    free(native);

    TEST_ASSERT_NULL(tp_fs_win32_path_alloc("\\\\.\\PhysicalDrive0"));
    TEST_ASSERT_NULL(tp_fs_win32_path_alloc("\\\\?\\C:\\raw-verbatim.tmp"));
}
#endif

/* U-02 F11: the recursive walk polls the cancel token per directory entry, so a
 * cancel raised mid-scan aborts BEFORE the tree is finished and returns
 * TP_STATUS_CANCELLED with a clean, EMPTY (never partial) result. The callback
 * reports cancellation once it has been polled past `cancel_after`, so the walk
 * provably stops after only a couple of entries -- fewer than g_root's tree. A token
 * that never cancels (and a NULL token) scans the whole tree, proving the added
 * parameter is backward compatible. */
typedef struct {
    int polls;
    int cancel_after;
} scan_cancel_ctx;

static bool scan_cancel_after_n(void *ctx) {
    scan_cancel_ctx *c = (scan_cancel_ctx *)ctx;
    return ++c->polls > c->cancel_after;
}

void test_cancellable_scan_stops_mid_walk(void) {
    scan_cancel_ctx ctx = {0, 1}; /* one poll through, then cancel */
    const tp_cancel_token token = {scan_cancel_after_n, &ctx};
    tp_scan_result r = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_CANCELLED,
        tp_scan_dir_cancellable(g_root, &r, &token, &error), error.msg);
    /* Clean stop: the partial walk was freed, never surfaced as a corrupt result. */
    TEST_ASSERT_EQUAL_INT(0, r.count);
    TEST_ASSERT_NULL(r.entries);
    /* Provably early: it aborted right after the cancel, not after the whole tree. */
    TEST_ASSERT_TRUE_MESSAGE(ctx.polls <= 2,
                             "walk must abort at the cancel, not scan the full tree");
    tp_scan_free(&r);

    /* A token that never fires scans the whole 4-image tree (backward compatible). */
    scan_cancel_ctx never = {0, 1000000};
    const tp_cancel_token never_token = {scan_cancel_after_n, &never};
    tp_scan_result full = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_scan_dir_cancellable(g_root, &full, &never_token, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(4, full.count);
    tp_scan_free(&full);

    /* A NULL token is identical to tp_scan_dir(). */
    tp_scan_result null_tok = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_scan_dir_cancellable(g_root, &null_tok, NULL, &error));
    TEST_ASSERT_EQUAL_INT(4, null_tok.count);
    tp_scan_free(&null_tok);
}

/* U-02: beyond the per-entry loop-top poll, the walk also polls the cancel token at
 * two points that loop cannot cover -- once at function entry (BEFORE the blocking root
 * stat) and once AFTER the walk returns OK but BEFORE the qsort of a large tree. This
 * pins the PRE-SORT poll specifically: a never-cancelling counting token first learns
 * the total poll count N for this fixture (N counts the pre-sort poll as its LAST poll);
 * rerunning with cancel_after = N-1 lets every earlier poll pass so ONLY the pre-sort
 * poll reports cancel. Were that poll deleted the walk would have just N-1 polls, poll
 * #N would never happen, and the rerun would finish OK (count 4) instead of CANCELLED --
 * so a CANCELLED result at exactly N polls is what the pre-sort poll uniquely produces.
 * The ENTRY poll is pinned separately: cancel_after = 0 aborts on the first poll, before
 * any entry is read, with a clean EMPTY result. */
void test_cancellable_scan_polls_entry_and_before_sort(void) {
    /* 1. Full walk, never cancel: N counts every poll INCLUDING the pre-sort one. */
    scan_cancel_ctx full_ctx = {0, 1000000};
    const tp_cancel_token full_token = {scan_cancel_after_n, &full_ctx};
    tp_scan_result full = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_scan_dir_cancellable(g_root, &full, &full_token, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(4, full.count);
    const int total_polls = full_ctx.polls;
    tp_scan_free(&full);
    TEST_ASSERT_TRUE_MESSAGE(total_polls >= 2,
                             "walk must poll at least the entry + pre-sort points");

    /* 2. Cancel ONLY on the last poll: cancel_after = N-1 passes every scan-phase poll
     * and fires on the pre-sort poll. The accumulated (unsorted) walk is dropped whole. */
    scan_cancel_ctx sort_ctx = {0, total_polls - 1};
    const tp_cancel_token sort_token = {scan_cancel_after_n, &sort_ctx};
    tp_scan_result sorted = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_CANCELLED,
        tp_scan_dir_cancellable(g_root, &sorted, &sort_token, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(0, sorted.count);
    TEST_ASSERT_NULL(sorted.entries);
    /* Every earlier poll passed, so the walk ran to completion and the pre-sort poll (#N)
     * is the one that caught the cancel -- deleting it makes this run finish OK instead. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        total_polls, sort_ctx.polls,
        "the walk must complete and cancel on the pre-sort poll, not earlier");
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "cancel"));
    tp_scan_free(&sorted);

    /* 3. Entry poll: cancel before the first stat -- exactly one poll, empty result. */
    scan_cancel_ctx entry_ctx = {0, 0};
    const tp_cancel_token entry_token = {scan_cancel_after_n, &entry_ctx};
    tp_scan_result entry = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_CANCELLED,
        tp_scan_dir_cancellable(g_root, &entry, &entry_token, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(0, entry.count);
    TEST_ASSERT_NULL(entry.entries);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, entry_ctx.polls,
        "the entry poll must abort before the walk reads any entry");
    tp_scan_free(&entry);
}
// #endregion

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    build_fixture();
    build_listdir_fixture();

    UNITY_BEGIN();
    RUN_TEST(test_missing_dir_is_structured_and_atomic);
    RUN_TEST(test_null_and_empty_abs_dir_are_safe);
    RUN_TEST(test_empty_subdir_is_empty);
    RUN_TEST(test_fixture_walk);
    RUN_TEST(test_cancellable_scan_stops_mid_walk);
    RUN_TEST(test_cancellable_scan_polls_entry_and_before_sort);
    RUN_TEST(test_is_dir_and_exists);
    RUN_TEST(test_visit_dir_streams_matching_names);
#ifndef _WIN32
    RUN_TEST(test_visit_dir_excludes_fifo_and_symlink);
#endif
    RUN_TEST(test_visit_dir_missing_and_bad_args_return_false);
    RUN_TEST(test_utf8_filesystem_backend_round_trips_non_ascii_paths);
    RUN_TEST(test_deep_utf8_path_is_not_silently_omitted);
    RUN_TEST(test_scan_allocation_failure_is_oom_and_never_partial);
    RUN_TEST(test_scan_root_overflow_is_structured_and_never_touches_disk);
    RUN_TEST(test_existing_non_directory_is_a_scan_error_not_empty_success);
    RUN_TEST(test_filesystem_publication_primitives_are_no_clobber_and_replace_safe);
    RUN_TEST(test_filesystem_backend_rejects_invalid_utf8);
#ifndef _WIN32
    RUN_TEST(test_posix_directory_iterator_rejects_invalid_utf8_entry_atomically);
#endif
#ifdef _WIN32
    RUN_TEST(test_windows_filesystem_utf_conversion_reports_small_buffers);
    RUN_TEST(test_windows_filesystem_backend_handles_extended_absolute_paths);
#endif
    return UNITY_END();
}
