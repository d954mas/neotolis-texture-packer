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

static char *dupz(const char *s) {
    size_t n = strlen(s) + 1U;
    char *p = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(p);
    memcpy(p, s, n);
    return p;
}

void test_project_migration_rejects_null_project(void) {
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_project_migrate_sprite_refs(NULL, &error));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "NULL project"));
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
    tp_project_sprite *sp2 = tp_project_atlas_find_sprite_by_source_key(
        a2, sid, "hero.png");
    TEST_ASSERT_NOT_NULL_MESSAGE(sp2, "the name bridge must still resolve after reload");
    TEST_ASSERT_EQUAL_STRING("hero.png", sp2->src_key);
    TEST_ASSERT_TRUE(tp_id128_eq(sp2->source_ref, sid));
    TEST_ASSERT_TRUE(sp2->origin_x > 0.09F && sp2->origin_x < 0.11F);
    tp_project_destroy(p2);
}

/* A missing legacy reference remains an inert pending orphan. With the normal
 * name fallback removed it cannot apply until exactly one sprite reappears. */
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
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_resolve_atlas_sprites(p, 0, &idx, &e));
    tp_sprite_index_free(&idx);

    TEST_ASSERT_TRUE_MESSAGE(tp_id128_is_nil(sp->source_ref), "an unresolved name stays a pending soft-orphan");
    TEST_ASSERT_NULL(sp->src_key);
    tp_project_destroy(p);
}

void test_ambiguous_legacy_reference_rejects_without_mutation(void) {
    char left[600];
    char right[600];
    (void)snprintf(left, sizeof left, "%s/mig_ambiguous_left", g_dir);
    (void)snprintf(right, sizeof right, "%s/mig_ambiguous_right", g_dir);
    mkdir_p(left);
    mkdir_p(right);
    char path[800];
    (void)snprintf(path, sizeof path, "%s/hero.png", left);
    write_file(path, "L");
    (void)snprintf(path, sizeof path, "%s/hero.png", right);
    write_file(path, "R");

    tp_project *project = tp_project_create();
    tp_project_atlas *atlas = tp_project_get_atlas(project, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, left));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, right));
    atlas->sources[0].id = seeded_id(0x31U);
    atlas->sources[1].id = seeded_id(0x32U);
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_pending_sprite(atlas, "hero",
                                                              &sprite));
    sprite->origin_x = 0.2F;
    tp_sprite_index index;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_sprite_index_build(project, 0, &index, &error));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_project_resolve_atlas_sprites(project, 0, &index,
                                                           &error));
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "ambiguous"));
    tp_sprite_index_free(&index);
    TEST_ASSERT_TRUE(tp_id128_is_nil(sprite->source_ref));
    TEST_ASSERT_NULL(sprite->src_key);
    TEST_ASSERT_TRUE(sprite->origin_x > 0.19F && sprite->origin_x < 0.21F);
    tp_project_destroy(project);
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

/* Animation frame references re-key to {source, key} at resolution, persist in v4
 * form, and survive a source reorder + save/reload (the reference targets the derived
 * sprite id, not an array position or the mutable display name). */
void test_frames_resolve_persist_and_survive_reorder(void) {
    char root[600];
    (void)snprintf(root, sizeof root, "%s/mig_frames", g_dir);
    mkdir_p(root);
    char f[800];
    (void)snprintf(f, sizeof f, "%s/hero.png", root);
    write_file(f, "H");
    (void)snprintf(f, sizeof f, "%s/gem.png", root);
    write_file(f, "G");

    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    a->id = seeded_id(0x0BU);
    /* two sources so a reorder is meaningful; the frames' source is the second one */
    char other[600];
    (void)snprintf(other, sizeof other, "%s/mig_frames_other", g_dir);
    mkdir_p(other);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, other));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, root));
    a->sources[0].id = seeded_id(0x41U);
    a->sources[1].id = seeded_id(0x52U);
    tp_id128 root_sid = a->sources[1].id;

    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "walk", &an));
    an->id = seeded_id(0x0CU); /* real anim id so the saved file re-loads (validate) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "gem"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero"));
    TEST_ASSERT_TRUE(tp_id128_is_nil(an->frames[0].source_ref)); /* pending */

    tp_sprite_index idx;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_sprite_index_build(p, 0, &idx, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_resolve_atlas_sprites(p, 0, &idx, &e));
    tp_sprite_index_free(&idx);

    /* frames re-keyed to the root source + their source-local key */
    TEST_ASSERT_TRUE(tp_id128_eq(an->frames[0].source_ref, root_sid));
    TEST_ASSERT_EQUAL_STRING("gem.png", an->frames[0].src_key);
    TEST_ASSERT_EQUAL_STRING("hero.png", an->frames[1].src_key);
    TEST_ASSERT_EQUAL_STRING("gem", an->frames[0].name); /* order + name bridge preserved */

    char proj[700];
    (void)snprintf(proj, sizeof proj, "%s/frames_v4.ntpacker_project", root);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, proj, &e));
    size_t n = 0;
    char *bytes = read_all(proj, &n);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(bytes, "\"key\": \"gem.png\""), "frame persists its source key");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(bytes, "\"key\": \"hero.png\""), "frame persists its source key");
    free(bytes);
    tp_project_destroy(p);

    /* reload, then REORDER sources (swap): the frame still resolves by (source, key). */
    tp_project *p2 = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(proj, &p2, &e), e.msg);
    tp_project_atlas *a2 = tp_project_get_atlas(p2, 0);
    TEST_ASSERT_EQUAL_INT(2, a2->animations[0].frame_count);
    TEST_ASSERT_EQUAL_STRING("gem", a2->animations[0].frames[0].name);   /* order preserved */
    TEST_ASSERT_EQUAL_STRING("hero", a2->animations[0].frames[1].name);
    TEST_ASSERT_TRUE(tp_id128_eq(a2->animations[0].frames[0].source_ref, root_sid));
    /* swap the two sources; the frame's source_ref is unchanged (id, not index) */
    tp_project_source tmp = a2->sources[0];
    a2->sources[0] = a2->sources[1];
    a2->sources[1] = tmp;
    TEST_ASSERT_TRUE_MESSAGE(tp_id128_eq(a2->animations[0].frames[0].source_ref, root_sid),
                             "frame reference survives a source reorder");
    tp_project_destroy(p2);
}

/* fix [3]: a MIGRATED {source, key} record and a PENDING {name} record that share a name
 * bridge must load to the SAME model regardless of JSON array order -- never merge/shadow
 * by element position. v4 record loading dedups pending-against-pending only and never
 * merges across forms, so the two always COEXIST (2 records, each field intact) in either
 * order. Before the fix, [migrated, pending] collapsed to 1 record (the pending merged into
 * the migrated one) while [pending, migrated] kept 2 -- order-dependent. */
static void assert_two_records_either_order(const char *sprites_json) {
    char proj[900];
    (void)snprintf(proj, sizeof proj, "%s/order_test.ntpacker_project", g_dir);
    char content[1400];
    (void)snprintf(content, sizeof content,
                   "{\n  \"version\": 4,\n  \"atlases\": [\n"
                   "    { \"id\": \"atlas_0000000000000000000000000000a001\", \"name\": \"art\",\n"
                   "      \"sources\": [ { \"id\": \"source_0000000000000000000000000000b001\", \"path\": \"art\" } ],\n"
                   "      \"sprites\": [ %s ] }\n  ]\n}\n",
                   sprites_json);
    write_file(proj, content);

    tp_project *p = NULL;
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(proj, &p, &e), e.msg);
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, a->sprite_count, "records sharing a name bridge must not merge/shadow");

    const tp_project_sprite *pending = NULL;
    const tp_project_sprite *migrated = NULL;
    for (int i = 0; i < a->sprite_count; i++) {
        const tp_project_sprite *s = &a->sprites[i];
        if (!tp_id128_is_nil(s->source_ref) && s->src_key != NULL) {
            migrated = s;
        } else {
            pending = s;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(pending, "the pending {name} record must survive");
    TEST_ASSERT_NOT_NULL_MESSAGE(migrated, "the migrated {source,key} record must survive");
    TEST_ASSERT_EQUAL_STRING("hero", pending->name);
    TEST_ASSERT_NULL(pending->src_key);
    TEST_ASSERT_TRUE_MESSAGE(pending->origin_x > 0.09F && pending->origin_x < 0.11F, "pending override intact");
    TEST_ASSERT_NULL_MESSAGE(pending->rename, "pending record must not inherit the migrated record's rename");
    TEST_ASSERT_EQUAL_STRING("hero", migrated->name);
    TEST_ASSERT_EQUAL_STRING("hero.png", migrated->src_key);
    TEST_ASSERT_EQUAL_STRING("champ", migrated->rename);
    tp_project_destroy(p);
}

void test_v4_sprite_record_load_order_independent(void) {
    const char *pending_rec = "{ \"name\": \"hero\", \"origin\": [0.1, 0.2] }";
    const char *migrated_rec =
        "{ \"key\": \"hero.png\", \"rename\": \"champ\", \"source\": \"source_0000000000000000000000000000b001\" }";
    char order_a[600];
    char order_b[600];
    (void)snprintf(order_a, sizeof order_a, "%s, %s", pending_rec, migrated_rec); /* pending first */
    (void)snprintf(order_b, sizeof order_b, "%s, %s", migrated_rec, pending_rec); /* migrated first */
    assert_two_records_either_order(order_a);
    assert_two_records_either_order(order_b);
}

/* fix [6]: byte-golden over the MIGRATED serializer branch. Existing goldens only cover
 * version deltas / pending {name} form; none pins the migrated {source,key} sprite record
 * or the {key,source} frame object. Construct both in memory (every override set so the
 * full key order allow_rotate < extrude < key < margin < max_vertices < origin < rename <
 * shape < slice9 < source is exercised, and a migrated frame object) and assert the EXACT
 * serialized bytes -- indentation, key ordering, and frame-object shape. This guards the
 * branch cli_mutate_stable would not otherwise reach (real files still hold pending
 * records until the F2 re-key trigger lands). */
void test_migrated_record_frame_byte_golden(void) {
    tp_project *p = tp_project_create();
    tp_project_atlas *a = tp_project_get_atlas(p, 0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_set_atlas_name(a, "art"));
    a->id = seeded_id(0x0AU);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "art"));
    a->sources[0].id = seeded_id(0x11U);
    tp_id128 sid = a->sources[0].id;

    /* migrated sprite override: every optional field non-default */
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "hero", &sp));
    sp->source_ref = sid;
    sp->src_key = dupz("hero.png"); /* migrated: {source, key}, name bridge dropped on save */
    sp->ov_allow_rotate = 0;
    sp->ov_extrude = 3;
    sp->ov_margin = 2;
    sp->ov_max_vertices = 8;
    sp->origin_x = 0.25F;
    sp->origin_y = 0.75F;
    sp->rename = dupz("champion");
    sp->ov_shape = 1;
    sp->slice9_lrtb[0] = 1;
    sp->slice9_lrtb[1] = 2;
    sp->slice9_lrtb[2] = 3;
    sp->slice9_lrtb[3] = 4;

    /* migrated animation frame: serializes as a {key, source} object */
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "walk", &an));
    an->id = seeded_id(0x0CU);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "gem"));
    an->frames[0].source_ref = sid;
    an->frames[0].src_key = dupz("gem.png");

    char proj[900];
    (void)snprintf(proj, sizeof proj, "%s/migrated_golden.ntpacker_project", g_dir);
    tp_error e = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, proj, &e));

    const char *ATLAS = "atlas_0a00000000000000000000000000007e";
    const char *SRC = "source_1100000000000000000000000000007e";
    const char *ANIM = "anim_0c00000000000000000000000000007e";
    char expect[2048];
    (void)snprintf(expect, sizeof expect,
                   "{\n"
                   "  \"version\": 4,\n"
                   "  \"atlases\": [\n"
                   "    {\n"
                   "      \"animations\": [\n"
                   "        {\n"
                   "          \"frames\": [\n"
                   "            {\n"
                   "              \"key\": \"gem.png\",\n"
                   "              \"source\": \"%s\"\n"
                   "            }\n"
                   "          ],\n"
                   "          \"id\": \"%s\",\n"
                   "          \"name\": \"walk\"\n"
                   "        }\n"
                   "      ],\n"
                   "      \"id\": \"%s\",\n"
                   "      \"name\": \"art\",\n"
                   "      \"sources\": [\n"
                   "        {\n"
                   "          \"id\": \"%s\",\n"
                   "          \"path\": \"art\"\n"
                   "        }\n"
                   "      ],\n"
                   "      \"sprites\": [\n"
                   "        {\n"
                   "          \"allow_rotate\": 0,\n"
                   "          \"extrude\": 3,\n"
                   "          \"key\": \"hero.png\",\n"
                   "          \"margin\": 2,\n"
                   "          \"max_vertices\": 8,\n"
                   "          \"origin\": [0.25, 0.75],\n"
                   "          \"rename\": \"champion\",\n"
                   "          \"shape\": 1,\n"
                   "          \"slice9\": [1, 2, 3, 4],\n"
                   "          \"source\": \"%s\"\n"
                   "        }\n"
                   "      ]\n"
                   "    }\n"
                   "  ]\n"
                   "}\n",
                   SRC, ANIM, ATLAS, SRC, SRC);

    size_t n = 0;
    char *bytes = read_all(proj, &n);
    TEST_ASSERT_EQUAL_size_t(strlen(expect), n);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(expect, bytes, n), bytes); /* exact migrated byte-golden */
    free(bytes);
    tp_project_destroy(p);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    mkdir_p(g_dir);
    UNITY_BEGIN();
    RUN_TEST(test_project_migration_rejects_null_project);
    RUN_TEST(test_pending_resolves_and_persists_v4);
    RUN_TEST(test_unresolved_stays_pending);
    RUN_TEST(test_ambiguous_legacy_reference_rejects_without_mutation);
    RUN_TEST(test_orphan_reactivates_on_key_return);
    RUN_TEST(test_frames_resolve_persist_and_survive_reorder);
    RUN_TEST(test_v4_sprite_record_load_order_independent);
    RUN_TEST(test_migrated_record_frame_byte_golden);
    return UNITY_END();
}
