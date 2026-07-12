/* F1-03 canonical selector resolver (§5.4): a human selector resolves to EXACTLY
 * one id; ambiguity yields a STABLE candidate list and is never guessed; a compound
 * selector disambiguates. Structural kinds resolve with no disk; sprites resolve via
 * a supplied resolved index. */

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
#include "tp_core/tp_selector.h"
#include "tp_core/tp_sprite_index.h"
#include "unity.h"

static const char *g_dir;

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

static tp_id128 seeded_id(uint8_t seed) {
    tp_id128 id = tp_id128_nil();
    id.bytes[0] = seed;
    id.bytes[15] = 0x33U;
    return id;
}

/* --- structural resolution (no disk) --- */

void test_resolve_atlas_by_name_and_id(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a0 = tp_project_get_atlas(p, 0);
    a0->id = seeded_id(0x01U);
    int i1 = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "hero", &i1));
    p->atlases[i1].id = seeded_id(0x02U);

    tp_selector_result r;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_selector_resolve(p, "hero", NULL, -1, &r, NULL, &e), e.msg);
    TEST_ASSERT_EQUAL_INT(TP_SEL_ATLAS, r.kind);
    TEST_ASSERT_TRUE(tp_id128_eq(r.id, seeded_id(0x02U)));

    /* by canonical id text */
    char idtext[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, seeded_id(0x01U), idtext, sizeof idtext, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_selector_resolve(p, idtext, NULL, -1, &r, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(r.id, seeded_id(0x01U)));

    tp_project_destroy(p);
}

void test_ambiguous_atlas_stable_candidates(void) {
    tp_project *p = tp_project_create();
    tp_project_get_atlas(p, 0)->id = seeded_id(0x01U);
    int i1 = 0;
    int i2 = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "dup", &i1));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "dup", &i2));
    p->atlases[i1].id = seeded_id(0x40U);
    p->atlases[i2].id = seeded_id(0x50U);

    tp_selector_result r;
    tp_selector_candidates c = {0};
    tp_error e = {0};
    tp_status st = tp_selector_resolve(p, "dup", NULL, -1, &r, &c, &e);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_AMBIGUOUS_SELECTOR, st);
    TEST_ASSERT_EQUAL_INT(2, c.count);
    TEST_ASSERT_EQUAL_INT(TP_SEL_ATLAS, c.v[0].kind);
    /* stable order == project order: atlas index i1 (byte 0x40) before i2 (0x50). */
    char id0[TP_ID_TEXT_CAP];
    char id1[TP_ID_TEXT_CAP];
    (void)snprintf(id0, sizeof id0, "%s", c.v[0].idtext);
    (void)snprintf(id1, sizeof id1, "%s", c.v[1].idtext);
    tp_selector_candidates_free(&c);

    /* second call yields the same order */
    tp_selector_candidates c2 = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_AMBIGUOUS_SELECTOR, tp_selector_resolve(p, "dup", NULL, -1, &r, &c2, &e));
    TEST_ASSERT_EQUAL_STRING(id0, c2.v[0].idtext);
    TEST_ASSERT_EQUAL_STRING(id1, c2.v[1].idtext);
    tp_selector_candidates_free(&c2);

    /* disambiguate by id -> exactly one */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_selector_resolve(p, id0, NULL, -1, &r, NULL, &e));
    tp_project_destroy(p);
}

void test_not_found_and_empty(void) {
    tp_project *p = tp_project_create();
    tp_selector_result r;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_selector_resolve(p, "nope", NULL, -1, &r, NULL, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_selector_resolve(p, "", NULL, -1, &r, NULL, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_selector_resolve(p, NULL, NULL, -1, &r, NULL, &e));
    tp_project_destroy(p);
}

/* --- sprite resolution (disk + index) --- */

void test_sprite_ambiguous_by_name_resolvable_by_source(void) {
    char rootA[600];
    char rootB[600];
    (void)snprintf(rootA, sizeof rootA, "%s/sel_a", g_dir);
    (void)snprintf(rootB, sizeof rootB, "%s/sel_b", g_dir);
    mkdir_p(rootA);
    mkdir_p(rootB);
    char f[800];
    (void)snprintf(f, sizeof f, "%s/hero.png", rootA);
    write_file(f, "A");
    (void)snprintf(f, sizeof f, "%s/hero.png", rootB);
    write_file(f, "B");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    a->id = seeded_id(0x01U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, rootA));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, rootB));
    a->sources[0].id = seeded_id(0xA1U);
    a->sources[1].id = seeded_id(0xB2U);

    tp_sprite_index idx;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));

    /* bare name -> ambiguous, 2 candidates, both sprites */
    tp_selector_result r;
    tp_selector_candidates c = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_AMBIGUOUS_SELECTOR, tp_selector_resolve(p, "hero", &idx, 0, &r, &c, &e));
    TEST_ASSERT_EQUAL_INT(2, c.count);
    TEST_ASSERT_EQUAL_INT(TP_SEL_SPRITE, c.v[0].kind);
    tp_selector_candidates_free(&c);

    /* compound "source_<idA>:hero" -> exactly one, sprite in source A */
    char selA[TP_ID_TEXT_CAP + 16];
    char sidA[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_SOURCE, a->sources[0].id, sidA, sizeof sidA, &e));
    (void)snprintf(selA, sizeof selA, "%s:hero", sidA);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_selector_resolve(p, selA, &idx, 0, &r, NULL, &e), e.msg);
    TEST_ASSERT_EQUAL_INT(TP_SEL_SPRITE, r.kind);
    TEST_ASSERT_TRUE(tp_id128_eq(r.id, tp_sprite_id(a->sources[0].id, "hero.png")));

    /* "source:<pathA>" resolves the SOURCE (not a sprite) */
    char selSrc[700];
    (void)snprintf(selSrc, sizeof selSrc, "source:%s", rootA);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_selector_resolve(p, selSrc, &idx, 0, &r, NULL, &e));
    TEST_ASSERT_EQUAL_INT(TP_SEL_SOURCE, r.kind);
    TEST_ASSERT_TRUE(tp_id128_eq(r.id, a->sources[0].id));

    /* sprite_<id> resolves that exact sprite */
    char spidText[TP_ID_TEXT_CAP];
    tp_sprite_id_format(tp_sprite_id(a->sources[1].id, "hero.png"), spidText, sizeof spidText);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_selector_resolve(p, spidText, &idx, 0, &r, NULL, &e));
    TEST_ASSERT_EQUAL_INT(TP_SEL_SPRITE, r.kind);
    TEST_ASSERT_TRUE(tp_id128_eq(r.id, tp_sprite_id(a->sources[1].id, "hero.png")));

    tp_sprite_index_free(&idx);
    tp_project_destroy(p);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    mkdir_p(g_dir);
    UNITY_BEGIN();
    RUN_TEST(test_resolve_atlas_by_name_and_id);
    RUN_TEST(test_ambiguous_atlas_stable_candidates);
    RUN_TEST(test_not_found_and_empty);
    RUN_TEST(test_sprite_ambiguous_by_name_resolvable_by_source);
    return UNITY_END();
}
