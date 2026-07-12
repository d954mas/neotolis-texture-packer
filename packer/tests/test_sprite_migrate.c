/* F1-03 v4 sprite-record identity lifecycle (decision 0009):
 *   - a MIGRATED override persists {source, key} and re-derives its `name` bridge on
 *     load with NO scan;
 *   - a PENDING (v3 name-keyed) override re-keys to {source, key} lazily at first
 *     resolution against the scanned index; the override still applies by name;
 *   - a name that resolves to nothing stays a pending soft-orphan;
 *   - a migrated record whose file disappears keeps its identity (stored orphan) and
 *     REACTIVATES -- same sprite_id -- when the source key returns. */

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
#include "tp_core/tp_project_migrate.h"
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

static char *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz + 1U);
    *n = fread(b, 1U, (size_t)sz, f);
    b[*n] = '\0';
    fclose(f);
    return b;
}

static tp_id128 seeded_id(uint8_t seed) {
    tp_id128 id = tp_id128_nil();
    id.bytes[0] = seed;
    id.bytes[15] = 0x7EU;
    return id;
}

/* A pending v3 name-keyed override re-keys to {source, key} at first resolution, and
 * the next save persists the v4 form; reload re-derives the name bridge, override
 * intact. */
void test_pending_resolves_and_persists_v4(void) {
    char root[600];
    (void)snprintf(root, sizeof root, "%s/mig_src", g_dir);
    mkdir_p(root);
    char f[800];
    (void)snprintf(f, sizeof f, "%s/hero.png", root);
    write_file(f, "H");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    a->id = seeded_id(0x0AU); /* give the atlas a real id so the saved file re-loads (validate) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, root));
    a->sources[0].id = seeded_id(0x11U);
    tp_id128 sid = a->sources[0].id;

    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "hero", &sp));
    sp->origin_x = 0.1F;
    TEST_ASSERT_TRUE_MESSAGE(tp_id128_is_nil(sp->source_ref), "a name-added override starts PENDING");
    TEST_ASSERT_NULL(sp->src_key);

    tp_sprite_index idx;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_resolve_atlas_sprites(p, 0, &idx, &e), e.msg);
    tp_sprite_index_free(&idx);

    /* now migrated: canonical (source, key), name bridge unchanged */
    TEST_ASSERT_TRUE(tp_id128_eq(sp->source_ref, sid));
    TEST_ASSERT_EQUAL_STRING("hero.png", sp->src_key);
    TEST_ASSERT_EQUAL_STRING("hero", sp->name);

    char proj[700];
    (void)snprintf(proj, sizeof proj, "%s/p_v4.ntpacker_project", root);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, proj, &e));
    size_t n = 0;
    char *bytes = read_all(proj, &n);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(bytes, "\"key\": \"hero.png\""), "migrated record persists its source key");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(bytes, "\"source\": \"source_"), "migrated record persists its source ref");
    TEST_ASSERT_NULL_MESSAGE(strstr(bytes, "\"name\": \"hero\""), "migrated record drops the mutable name key");
    free(bytes);
    tp_project_destroy(p);

    /* reload: identity intact, name re-derived with NO scan, override preserved */
    tp_project *p2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load(proj, &p2, &e));
    tp_project_atlas *a2 = tp_project_get_atlas(p2, 0);
    tp_project_sprite *sp2 = tp_project_atlas_find_sprite(a2, "hero");
    TEST_ASSERT_NOT_NULL_MESSAGE(sp2, "the name bridge must still resolve after reload");
    TEST_ASSERT_EQUAL_STRING("hero.png", sp2->src_key);
    TEST_ASSERT_TRUE(tp_id128_eq(sp2->source_ref, sid));
    TEST_ASSERT_TRUE(sp2->origin_x > 0.09F && sp2->origin_x < 0.11F);
    tp_project_destroy(p2);
}

/* A pending override whose name resolves to no scanned sprite stays pending (a soft
 * orphan), never guessed onto some other sprite. */
void test_unresolved_stays_pending(void) {
    char root[600];
    (void)snprintf(root, sizeof root, "%s/mig_orphan", g_dir);
    mkdir_p(root);
    char f[800];
    (void)snprintf(f, sizeof f, "%s/hero.png", root);
    write_file(f, "H");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, root));
    a->sources[0].id = seeded_id(0x22U);
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "ghost", &sp)); /* no ghost.png */
    sp->origin_x = 0.2F;

    tp_sprite_index idx;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_resolve_atlas_sprites(p, 0, &idx, &e));
    tp_sprite_index_free(&idx);

    TEST_ASSERT_TRUE_MESSAGE(tp_id128_is_nil(sp->source_ref), "an unresolved name stays a pending soft-orphan");
    TEST_ASSERT_NULL(sp->src_key);
    tp_project_destroy(p);
}

/* A migrated record whose source file disappears keeps its stored identity and
 * REACTIVATES with the SAME sprite_id when the source key returns (§5.2). */
void test_orphan_reactivates_on_key_return(void) {
    char root[600];
    (void)snprintf(root, sizeof root, "%s/mig_react", g_dir);
    mkdir_p(root);
    char f[800];
    (void)snprintf(f, sizeof f, "%s/hero.png", root);
    write_file(f, "H");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, root));
    a->sources[0].id = seeded_id(0x33U);
    tp_id128 sid = a->sources[0].id;
    tp_id128 want = tp_sprite_id(sid, "hero.png");

    /* resolve while present -> migrated */
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "hero", &sp));
    sp->origin_x = 0.3F;
    tp_sprite_index idx;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_resolve_atlas_sprites(p, 0, &idx, &e));
    TEST_ASSERT_NOT_NULL(tp_sprite_index_by_id(&idx, want));
    tp_sprite_index_free(&idx);
    TEST_ASSERT_TRUE(tp_id128_eq(sp->source_ref, sid));

    /* file disappears: the record is now a stored orphan; identity unchanged */
    (void)remove(f);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    TEST_ASSERT_NULL_MESSAGE(tp_sprite_index_by_id(&idx, want), "orphaned key is absent from the live index");
    /* resolve is a no-op on an already-migrated record (keeps its identity) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_resolve_atlas_sprites(p, 0, &idx, &e));
    tp_sprite_index_free(&idx);
    TEST_ASSERT_TRUE_MESSAGE(tp_id128_eq(sp->source_ref, sid), "orphan keeps its stored source ref");
    TEST_ASSERT_EQUAL_STRING("hero.png", sp->src_key);

    /* key returns -> same sprite_id reappears (reactivation) */
    write_file(f, "H2");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    const tp_sprite_ref *r = tp_sprite_index_by_id(&idx, want);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "returning key reactivates the same sprite_id");
    TEST_ASSERT_TRUE(tp_id128_eq(r->source_id, sid));
    tp_sprite_index_free(&idx);
    tp_project_destroy(p);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    mkdir_p(g_dir);
    UNITY_BEGIN();
    RUN_TEST(test_pending_resolves_and_persists_v4);
    RUN_TEST(test_unresolved_stays_pending);
    RUN_TEST(test_orphan_reactivates_on_key_return);
    return UNITY_END();
}
