/* F1-03 resolved sprite index: scan -> source-local key -> sprite_id derivation,
 * the lookups, and the identity semantics the spec pins (§5.2):
 *   - a resolved sprite's sprite_id == tp_sprite_id(owning source_id, source_key);
 *   - an external source-file rename is old-missing + new-id (key changed);
 *   - the same filename in two sources is ambiguous by export key but distinct by
 *     (source, key) -- the compound-selector disambiguation seam.
 * Uses empty .png files (the index does not decode, only scans by extension). */

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

#include "tp_core/tp_id.h"
#include "tp_core/tp_project.h"
#include "tp_project_mutation_internal.h"
#include "tp_core/tp_sprite_index.h"
#include "unity.h"

static const char *g_dir; /* scratch dir (argv[1]) */

void setUp(void) {}
void tearDown(void) {}

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

static void remove_file(const char *path) {
    (void)remove(path);
}

/* A distinct non-nil id: byte 0 = seed, rest 0. */
static tp_id128 seeded_id(uint8_t seed) {
    tp_id128 id = tp_id128_nil();
    id.bytes[0] = seed;
    id.bytes[15] = 0x5AU;
    return id;
}

/* Fresh project, one atlas, `path` added as a source with id seeded from `seed`. */
static tp_project *proj_with_source(const char *path, uint8_t seed, tp_project_atlas **out_a) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, path));
    a->sources[a->source_count - 1].id = seeded_id(seed);
    if (out_a) {
        *out_a = a;
    }
    return p;
}

/* Index build over a folder with two files derives keys, export keys, and sprite_ids
 * that match the primitive tp_sprite_id(source_id, source_key). */
void test_index_build_and_derivation(void) {
    char root[600];
    (void)snprintf(root, sizeof root, "%s/idx1", g_dir);
    mkdir_p(root);
    char f[800];
    (void)snprintf(f, sizeof f, "%s/hero.png", root);
    write_file(f, "H");
    (void)snprintf(f, sizeof f, "%s/coin.png", root);
    write_file(f, "C");

    tp_project_atlas *a = NULL;
    tp_project *p = proj_with_source(root, 0x11U, &a);
    tp_id128 sid = a->sources[0].id;

    tp_sprite_index idx;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e), e.msg);
    TEST_ASSERT_EQUAL_INT(2, idx.count);

    /* Sorted by rel: coin.png before hero.png. */
    const tp_sprite_ref *coin = tp_sprite_index_by_export_key(&idx, "coin", NULL);
    TEST_ASSERT_NOT_NULL(coin);
    TEST_ASSERT_EQUAL_STRING("coin.png", coin->source_key);
    TEST_ASSERT_EQUAL_STRING("coin.png", coin->raw_name);
    TEST_ASSERT_TRUE(tp_id128_eq(coin->sprite_id, tp_sprite_id(sid, "coin.png")));
    TEST_ASSERT_TRUE(tp_id128_eq(coin->source_id, sid));

    const tp_sprite_ref *hero = tp_sprite_index_by_export_key(&idx, "hero", NULL);
    TEST_ASSERT_NOT_NULL(hero);
    TEST_ASSERT_TRUE(tp_id128_eq(hero->sprite_id, tp_sprite_id(sid, "hero.png")));

    /* by_id round-trips; by_source_key is exact. */
    TEST_ASSERT_EQUAL_PTR(hero, tp_sprite_index_by_id(&idx, tp_sprite_id(sid, "hero.png")));
    TEST_ASSERT_EQUAL_PTR(hero, tp_sprite_index_by_source_key(&idx, sid, "hero.png"));
    TEST_ASSERT_NULL(tp_sprite_index_by_id(&idx, tp_id128_nil()));

    tp_sprite_index_free(&idx);
    tp_project_destroy(p);
}

/* An external source-file rename changes the source-local key, so the old sprite_id
 * disappears and a new one appears (§5.2: remove + add, never an in-place rename). */
void test_external_rename_is_remove_add(void) {
    char root[600];
    (void)snprintf(root, sizeof root, "%s/idx_rename", g_dir);
    mkdir_p(root);
    char oldf[800];
    char newf[800];
    (void)snprintf(oldf, sizeof oldf, "%s/walk.png", root);
    (void)snprintf(newf, sizeof newf, "%s/run.png", root);
    /* Start from a known state: a prior run may have left run.png behind. */
    remove_file(newf);
    write_file(oldf, "W");

    tp_project_atlas *a = NULL;
    tp_project *p = proj_with_source(root, 0x22U, &a);
    tp_id128 sid = a->sources[0].id;
    tp_id128 old_id = tp_sprite_id(sid, "walk.png");
    tp_id128 new_id = tp_sprite_id(sid, "run.png");

    tp_sprite_index idx;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    TEST_ASSERT_NOT_NULL(tp_sprite_index_by_id(&idx, old_id));
    TEST_ASSERT_NULL(tp_sprite_index_by_id(&idx, new_id));
    tp_sprite_index_free(&idx);

    /* Rename on disk, rebuild. */
    remove_file(oldf);
    write_file(newf, "R");

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    TEST_ASSERT_NULL_MESSAGE(tp_sprite_index_by_id(&idx, old_id), "old key must be gone after external rename");
    TEST_ASSERT_NOT_NULL_MESSAGE(tp_sprite_index_by_id(&idx, new_id), "new key must appear as a new sprite");
    tp_sprite_index_free(&idx);
    tp_project_destroy(p);
}

/* The same filename in two sources: ambiguous by export key (match count 2) but a
 * distinct (source, key) -> distinct sprite_id each -- resolvable by source. */
void test_same_filename_two_sources(void) {
    char rootA[600];
    char rootB[600];
    (void)snprintf(rootA, sizeof rootA, "%s/idx_a", g_dir);
    (void)snprintf(rootB, sizeof rootB, "%s/idx_b", g_dir);
    mkdir_p(rootA);
    mkdir_p(rootB);
    char f[800];
    (void)snprintf(f, sizeof f, "%s/hero.png", rootA);
    write_file(f, "A");
    (void)snprintf(f, sizeof f, "%s/hero.png", rootB);
    write_file(f, "B");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, rootA));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, rootB));
    a->sources[0].id = seeded_id(0xA1U);
    a->sources[1].id = seeded_id(0xB2U);
    tp_id128 sidA = a->sources[0].id;
    tp_id128 sidB = a->sources[1].id;

    tp_sprite_index idx;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    TEST_ASSERT_EQUAL_INT(2, idx.count);

    int matches = 0;
    const tp_sprite_ref *first = tp_sprite_index_by_export_key(&idx, "hero", &matches);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, matches, "same filename in two sources must be ambiguous by name");

    const tp_sprite_ref *a_hero = tp_sprite_index_by_source_key(&idx, sidA, "hero.png");
    const tp_sprite_ref *b_hero = tp_sprite_index_by_source_key(&idx, sidB, "hero.png");
    TEST_ASSERT_NOT_NULL(a_hero);
    TEST_ASSERT_NOT_NULL(b_hero);
    TEST_ASSERT_FALSE_MESSAGE(tp_id128_eq(a_hero->sprite_id, b_hero->sprite_id),
                             "distinct sources -> distinct sprite_id for the same filename");
    TEST_ASSERT_TRUE(tp_id128_eq(a_hero->sprite_id, tp_sprite_id(sidA, "hero.png")));
    TEST_ASSERT_TRUE(tp_id128_eq(b_hero->sprite_id, tp_sprite_id(sidB, "hero.png")));

    tp_sprite_index_free(&idx);
    tp_project_destroy(p);
}

/* sprite-id text round-trips: format -> parse -> equal; bad text is structured. */
void test_sprite_id_text_roundtrip(void) {
    tp_id128 id = tp_id128_nil();
    for (int i = 0; i < 16; i++) {
        id.bytes[i] = (uint8_t)(i * 7 + 1);
    }
    char text[TP_ID_TEXT_CAP];
    tp_sprite_id_format(id, text, sizeof text);
    TEST_ASSERT_EQUAL_INT(0, strncmp(text, "sprite_", 7));
    tp_id128 back = tp_id128_nil();
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_id_parse(text, &back, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(id, back));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_sprite_id_parse("atlas_00", NULL, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_sprite_id_parse("sprite_zz", NULL, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_sprite_id_parse(NULL, NULL, &e));
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    mkdir_p(g_dir);
    UNITY_BEGIN();
    RUN_TEST(test_index_build_and_derivation);
    RUN_TEST(test_external_rename_is_remove_add);
    RUN_TEST(test_same_filename_two_sources);
    RUN_TEST(test_sprite_id_text_roundtrip);
    return UNITY_END();
}
