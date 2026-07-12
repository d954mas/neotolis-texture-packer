/* F1-01 identity migration / promotion battery (master spec §5, §5.5, §5.6).
 *
 * Covers: v1 repeated read-only load -> stable synthetic IDs; writable attach
 * promotes once + first save persists those IDs + second save byte-identical;
 * save failure does NOT remap IDs; RNG failure -> structured error (IDs
 * untouched); duplicate / malformed / nil ID on load rejected; ID survives
 * rename / reorder / remove; the v1 -> v2 migration byte-golden; and every
 * checked-in v1 fixture still loads. A scratch dir is argv[1]. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char *g_dir;

static void join(char *out, size_t cap, const char *name) { (void)snprintf(out, cap, "%s/%s", g_dir, name); }

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(c);
    memcpy(c, s, n);
    return c;
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    TEST_ASSERT_TRUE(n >= 0);
    char *buf = (char *)malloc((size_t)n + 1U);
    TEST_ASSERT_NOT_NULL(buf);
    size_t got = fread(buf, 1U, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len) {
        *len = got;
    }
    return buf;
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(text, 1U, strlen(text), f);
    fclose(f);
}

/* --- injectable RNG seams --------------------------------------------------- */
/* Deterministic distinct bytes per call: byte j = counter + j + 1 (never all
 * zero -> never the reserved nil), counter bumped so successive IDs differ. */
static int det_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *ctr = (uint8_t *)ctx;
    for (size_t j = 0; j < len; j++) {
        out[j] = (uint8_t)(*ctr + (uint8_t)j + 1U);
    }
    (*ctr)++;
    return (int)len;
}
static int fail_fill(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    (void)out;
    (void)len;
    return -1;
}

/* A minimal, layout-predictable v1 project (no sources/knobs -> those keys stay
 * absent, so the migrated v2 output is easy to pin exactly). */
static const char *V1_MIN =
    "{\n"
    "  \"version\": 1,\n"
    "  \"atlases\": [\n"
    "    {\n"
    "      \"name\": \"hero\",\n"
    "      \"animations\": [ { \"frames\": [\"a\", \"b\"], \"id\": \"walk\" } ],\n"
    "      \"targets\": [ { \"exporter_id\": \"json-neotolis\", \"out_path\": \"out/hero\" } ]\n"
    "    }\n"
    "  ]\n"
    "}\n";

static tp_id128 atlas0_id(const tp_project *p) { return p->atlases[0].id; }
static tp_id128 anim0_id(const tp_project *p) { return p->atlases[0].animations[0].id; }
static tp_id128 target0_id(const tp_project *p) { return p->atlases[0].targets[0].id; }

/* 1. v1 loaded twice (read-only) yields byte-identical synthetic IDs. */
void test_v1_repeated_load_stable_ids(void) {
    char path[512];
    join(path, sizeof path, "v1_stable.ntpacker_project");
    write_text(path, V1_MIN);

    tp_project *a = NULL;
    tp_project *b = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &a, &err), err.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &b, &err), err.msg);

    TEST_ASSERT_FALSE(tp_id128_is_nil(atlas0_id(a))); /* synthesized, not nil */
    TEST_ASSERT_TRUE(tp_id128_eq(atlas0_id(a), atlas0_id(b)));
    TEST_ASSERT_TRUE(tp_id128_eq(anim0_id(a), anim0_id(b)));
    TEST_ASSERT_TRUE(tp_id128_eq(target0_id(a), target0_id(b)));
    /* the old string id migrated into the logical name */
    TEST_ASSERT_EQUAL_STRING("walk", a->atlases[0].animations[0].name);

    tp_project_destroy(a);
    tp_project_destroy(b);
}

/* 2. writable attach promotes once; first save persists those exact IDs; a
 *    second save is byte-identical; promote is idempotent (a no-op afterwards). */
void test_writable_promote_once_byte_stable(void) {
    char p1[512];
    char p2[512];
    join(p1, sizeof p1, "prom1.ntpacker_project");
    join(p2, sizeof p2, "prom2.ntpacker_project");

    tp_project *p = tp_project_create(); /* atlas "atlas1", nil id */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_seed_default_target(p, 0));
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(tp_project_get_atlas(p, 0), "idle", &an));
    TEST_ASSERT_TRUE(tp_id128_is_nil(p->atlases[0].id)); /* nil until promoted */

    uint8_t ctr = 0;
    tp_rng rng = {det_fill, &ctr};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
    tp_id128 a_id = atlas0_id(p);
    tp_id128 an_id = anim0_id(p);
    tp_id128 t_id = target0_id(p);
    TEST_ASSERT_FALSE(tp_id128_is_nil(a_id));

    /* idempotent: a second promote changes nothing (never re-consumes the RNG). */
    uint8_t ctr2 = 200;
    tp_rng rng2 = {det_fill, &ctr2};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng2, &err));
    TEST_ASSERT_TRUE(tp_id128_eq(a_id, atlas0_id(p)));
    TEST_ASSERT_TRUE(tp_id128_eq(an_id, anim0_id(p)));
    TEST_ASSERT_TRUE(tp_id128_eq(t_id, target0_id(p)));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, p1, &err));

    tp_project *loaded = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(p1, &loaded, &err), err.msg);
    TEST_ASSERT_TRUE(tp_id128_eq(a_id, atlas0_id(loaded))); /* first save persisted exactly these */
    TEST_ASSERT_TRUE(tp_id128_eq(an_id, anim0_id(loaded)));
    TEST_ASSERT_TRUE(tp_id128_eq(t_id, target0_id(loaded)));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(loaded, p2, &err));
    size_t n1 = 0;
    size_t n2 = 0;
    char *b1 = read_all(p1, &n1);
    char *b2 = read_all(p2, &n2);
    TEST_ASSERT_EQUAL_size_t(n1, n2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(b1, b2, n1)); /* second save byte-identical */
    free(b1);
    free(b2);
    tp_project_destroy(loaded);
    tp_project_destroy(p);
}

/* 3. a save failure does NOT remap IDs (save never touches them). */
void test_save_failure_does_not_remap_ids(void) {
    tp_project *p = tp_project_create();
    uint8_t ctr = 0;
    tp_rng rng = {det_fill, &ctr};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
    tp_id128 before = atlas0_id(p);

    /* an unwritable path (missing parent directory) -> save fails, never a crash */
    char bad[512];
    join(bad, sizeof bad, "no_such_dir/nested/deep.ntpacker_project");
    tp_status st = tp_project_save(p, bad, &err);
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, st);
    TEST_ASSERT_TRUE(tp_id128_eq(before, atlas0_id(p))); /* IDs unchanged */
    tp_project_destroy(p);
}

/* 4. RNG failure -> structured error AND every ID left unchanged (atomic). */
void test_rng_failure_structured_and_atomic(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_TRUE(tp_id128_is_nil(p->atlases[0].id));
    tp_rng bad = {fail_fill, NULL};
    tp_error err = {0};
    tp_status st = tp_project_promote_ids(p, &bad, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_RNG_FAILED, st);
    TEST_ASSERT_EQUAL_STRING("rng_failed", tp_status_id(st));
    TEST_ASSERT_TRUE(tp_id128_is_nil(p->atlases[0].id)); /* nothing partially assigned */
    tp_project_destroy(p);
}

/* 5a. duplicate structural IDs on load -> TP_STATUS_DUPLICATE_ID. */
void test_duplicate_id_rejected(void) {
    char path[512];
    join(path, sizeof path, "dup.ntpacker_project");
    write_text(path,
               "{\n  \"version\": 2,\n  \"atlases\": [\n"
               "    { \"name\": \"a\", \"id\": \"atlas_00000000000000000000000000000001\" },\n"
               "    { \"name\": \"b\", \"id\": \"atlas_00000000000000000000000000000001\" }\n"
               "  ]\n}\n");
    tp_project *loaded = NULL;
    tp_error err = {0};
    tp_status st = tp_project_load(path, &loaded, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID, st);
    TEST_ASSERT_NULL(loaded);
}

/* 5b. malformed / wrong-kind / nil IDs on load -> TP_STATUS_ID_MALFORMED. */
void test_malformed_id_rejected(void) {
    tp_project *loaded = NULL;
    tp_error err = {0};
    char path[512];

    join(path, sizeof path, "bad_hex.ntpacker_project");
    write_text(path, "{\n  \"version\": 2,\n  \"atlases\": [ { \"name\": \"a\", "
                     "\"id\": \"atlas_zz000000000000000000000000000000\" } ]\n}\n");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_project_load(path, &loaded, &err));
    TEST_ASSERT_NULL(loaded);

    join(path, sizeof path, "wrong_kind.ntpacker_project");
    write_text(path, "{\n  \"version\": 2,\n  \"atlases\": [ { \"name\": \"a\", "
                     "\"id\": \"target_00000000000000000000000000000001\" } ]\n}\n");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_project_load(path, &loaded, &err));

    join(path, sizeof path, "nil_id.ntpacker_project");
    write_text(path, "{\n  \"version\": 2,\n  \"atlases\": [ { \"name\": \"a\", "
                     "\"id\": \"atlas_00000000000000000000000000000000\" } ]\n}\n");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_project_load(path, &loaded, &err));
}

/* 5d (F6). a v2 file with an ABSENT structural id is NOT synthesized (only v1 files
 *    are): the nil reaches validate and is rejected TP_STATUS_ID_MALFORMED. A saved v2
 *    always promotes to non-nil ids, so a missing id is a genuine anomaly (ADR 0007 pt 4). */
void test_v2_missing_id_rejected(void) {
    char path[512];
    join(path, sizeof path, "v2_missing_id.ntpacker_project");
    write_text(path, "{\n  \"version\": 2,\n  \"atlases\": [ { \"name\": \"a\" } ]\n}\n");
    tp_project *loaded = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_project_load(path, &loaded, &err));
    TEST_ASSERT_NULL(loaded);
}

/* 5c. a v2 animation missing "name" -> BAD_PROJECT (the logical name is required). */
void test_v2_anim_missing_name_rejected(void) {
    char path[512];
    join(path, sizeof path, "no_name.ntpacker_project");
    write_text(path, "{\n  \"version\": 2,\n  \"atlases\": [ { \"name\": \"a\", \"animations\": "
                     "[ { \"frames\": [\"x\"], \"id\": \"anim_00000000000000000000000000000009\" } ] } ]\n}\n");
    tp_project *loaded = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT, tp_project_load(path, &loaded, &err));
    TEST_ASSERT_NULL(loaded);
}

/* 6. an ID survives rename, remove-a-sibling (reorder), and save/reload. */
void test_id_survives_rename_reorder_remove(void) {
    char path[512];
    join(path, sizeof path, "identity.ntpacker_project");

    tp_project *p = tp_project_create(); /* atlas1 */
    int extra = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "extra", &extra));
    tp_project_atlas *a0 = tp_project_get_atlas(p, 0);
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a0, "walk", &an));
    tp_project_target *t = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a0, "json-neotolis", "out/a", &t));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a0, "defold", "out/b", NULL));

    uint8_t ctr = 0;
    tp_rng rng = {det_fill, &ctr};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
    tp_id128 atlas0 = a0->id;
    tp_id128 walk = an->id;
    tp_id128 json_t = a0->targets[0].id;

    /* rename the animation (logical name changes; structural id must not) */
    free(an->name);
    an->name = dupstr("run");
    TEST_ASSERT_TRUE(tp_id128_eq(walk, an->id));

    /* reorder: remove atlas 1 ("extra"); atlas 0's id is unaffected */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_remove_atlas(p, 1));
    TEST_ASSERT_TRUE(tp_id128_eq(atlas0, tp_project_get_atlas(p, 0)->id));

    /* remove the defold target; the json target keeps its id */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_remove_target(tp_project_get_atlas(p, 0), 1));
    TEST_ASSERT_TRUE(tp_id128_eq(json_t, tp_project_get_atlas(p, 0)->targets[0].id));

    /* survives save + reload */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, path, &err));
    tp_project *reloaded = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &reloaded, &err), err.msg);
    tp_project_atlas *ra = tp_project_get_atlas(reloaded, 0);
    TEST_ASSERT_TRUE(tp_id128_eq(atlas0, ra->id));
    TEST_ASSERT_TRUE(tp_id128_eq(walk, ra->animations[0].id));
    TEST_ASSERT_EQUAL_STRING("run", ra->animations[0].name);
    TEST_ASSERT_TRUE(tp_id128_eq(json_t, ra->targets[0].id));

    tp_project_destroy(reloaded);
    tp_project_destroy(p);
}

/* 7. migration byte-golden: a checked-in-shape v1 file migrates all the way to an
 *    exact v4 layout (v1->v2->v3->v4 chained) whose IDs are the deterministic legacy
 *    synthesis of stable tuples (tied to the golden hash pinned in test_migrate),
 *    and re-saves identically. V1_MIN has no sources and no sprite records, and its
 *    animation frames stay in PENDING name form (the v3->v4 sprite/frame re-key is
 *    lazy and needs a scan), so the ONLY delta from the old v3 golden is the version
 *    number (3 -> 4). */
void test_migration_golden_v1_to_v4(void) {
    char path[512];
    join(path, sizeof path, "mig_v1.ntpacker_project");
    write_text(path, V1_MIN);

    tp_project *p = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &p, &err), err.msg);

    char saved[512];
    join(saved, sizeof saved, "mig_v2.ntpacker_project");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, saved, &err));

    /* Expected IDs: the loader assigns each nil id at salt 0 (distinct tuples ->
     * no collision), so id == legacy_hash_default(kind, tuple, 0). */
    char atlas_txt[TP_ID_TEXT_CAP];
    char anim_txt[TP_ID_TEXT_CAP];
    char tgt_txt[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS,
                          tp_legacy_hash_default(NULL, TP_ID_KIND_ATLAS, "0", 0), atlas_txt, sizeof atlas_txt, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ANIM,
                          tp_legacy_hash_default(NULL, TP_ID_KIND_ANIM, "0|walk", 0), anim_txt, sizeof anim_txt, NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_TARGET,
                          tp_legacy_hash_default(NULL, TP_ID_KIND_TARGET, "0|json-neotolis|out/hero", 0), tgt_txt,
                          sizeof tgt_txt, NULL));

    char expect[2048];
    (void)snprintf(expect, sizeof expect,
                   "{\n"
                   "  \"version\": 4,\n"
                   "  \"atlases\": [\n"
                   "    {\n"
                   "      \"animations\": [\n"
                   "        {\n"
                   "          \"frames\": [\n"
                   "            \"a\",\n"
                   "            \"b\"\n"
                   "          ],\n"
                   "          \"id\": \"%s\",\n"
                   "          \"name\": \"walk\"\n"
                   "        }\n"
                   "      ],\n"
                   "      \"id\": \"%s\",\n"
                   "      \"name\": \"hero\",\n"
                   "      \"targets\": [\n"
                   "        {\n"
                   "          \"exporter_id\": \"json-neotolis\",\n"
                   "          \"id\": \"%s\",\n"
                   "          \"out_path\": \"out/hero\"\n"
                   "        }\n"
                   "      ]\n"
                   "    }\n"
                   "  ]\n"
                   "}\n",
                   anim_txt, atlas_txt, tgt_txt);

    size_t sn = 0;
    char *sbytes = read_all(saved, &sn);
    TEST_ASSERT_EQUAL_size_t(strlen(expect), sn);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(expect, sbytes, sn), sbytes); /* exact migration byte-golden */

    /* deterministic: a second migration of the same v1 file is byte-identical */
    char saved2[512];
    join(saved2, sizeof saved2, "mig_v2b.ntpacker_project");
    tp_project *p2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load(path, &p2, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p2, saved2, &err));
    size_t sn2 = 0;
    char *sbytes2 = read_all(saved2, &sn2);
    TEST_ASSERT_EQUAL_size_t(sn, sn2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(sbytes, sbytes2, sn));

    free(sbytes);
    free(sbytes2);
    tp_project_destroy(p);
    tp_project_destroy(p2);
}

/* 7b (F1-02). v2 -> v3 source migration byte-golden: a v2 file carries atlas/anim/
 *    target ids but bare-STRING sources (no source ids). Loading migrates the
 *    sources to tagged objects, synthesizing ONLY the source id from the stable
 *    tuple "<atlasIdx>|<path>" (kind=folder, omitted) while the atlas id is
 *    preserved verbatim. Re-save is byte-identical (now at v4: no sprite records, so
 *    the only delta from the old v3 golden is the version number). */
void test_migration_golden_v2_to_v4_sources(void) {
    const char *atlas_id = "atlas_0000000000000000000000000000abcd";
    char v2[512];
    (void)snprintf(v2, sizeof v2,
                   "{\n  \"version\": 2,\n  \"atlases\": [\n"
                   "    { \"name\": \"hero\", \"id\": \"%s\", \"sources\": [ \"sprites\" ] }\n"
                   "  ]\n}\n",
                   atlas_id);
    char path[512];
    join(path, sizeof path, "mig_v2_src.ntpacker_project");
    write_text(path, v2);

    tp_project *p = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &p, &err), err.msg);
    /* atlas id preserved; source id synthesized (non-nil), kind defaults folder. */
    TEST_ASSERT_EQUAL_INT(4, p->schema_version);
    TEST_ASSERT_EQUAL_INT(1, p->atlases[0].source_count);
    TEST_ASSERT_FALSE(tp_id128_is_nil(p->atlases[0].sources[0].id));
    TEST_ASSERT_EQUAL_INT((int)TP_SOURCE_KIND_FOLDER, (int)p->atlases[0].sources[0].kind);

    char saved[512];
    join(saved, sizeof saved, "mig_v3_src.ntpacker_project");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, saved, &err));

    char src_txt[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_SOURCE,
                          tp_legacy_hash_default(NULL, TP_ID_KIND_SOURCE, "0|sprites", 0), src_txt, sizeof src_txt,
                          NULL));

    char expect[1024];
    (void)snprintf(expect, sizeof expect,
                   "{\n"
                   "  \"version\": 4,\n"
                   "  \"atlases\": [\n"
                   "    {\n"
                   "      \"id\": \"%s\",\n"
                   "      \"name\": \"hero\",\n"
                   "      \"sources\": [\n"
                   "        {\n"
                   "          \"id\": \"%s\",\n"
                   "          \"path\": \"sprites\"\n"
                   "        }\n"
                   "      ]\n"
                   "    }\n"
                   "  ]\n"
                   "}\n",
                   atlas_id, src_txt);

    size_t sn = 0;
    char *sbytes = read_all(saved, &sn);
    TEST_ASSERT_EQUAL_size_t(strlen(expect), sn);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(expect, sbytes, sn), sbytes);

    /* second migration of the same v2 file is byte-identical */
    char saved2[512];
    join(saved2, sizeof saved2, "mig_v3_src_b.ntpacker_project");
    tp_project *p2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load(path, &p2, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p2, saved2, &err));
    size_t sn2 = 0;
    char *sbytes2 = read_all(saved2, &sn2);
    TEST_ASSERT_EQUAL_size_t(sn, sn2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(sbytes, sbytes2, sn));

    free(sbytes);
    free(sbytes2);
    tp_project_destroy(p);
    tp_project_destroy(p2);
}

/* 7c (F1-03). v3 -> v4 migration byte-golden: a v3 file with a NAME-keyed sprite
 *    override loads as a PENDING v4 record (load never scans, so it cannot re-key to
 *    {source, key}), and re-saves at version 4 with the override STILL in {name} form.
 *    Proves the chained v3->v4 hook + that a pending record is a valid, byte-stable v4
 *    state until a resolution scan migrates it. Ids are preserved verbatim (v3 has them). */
void test_migration_golden_v3_to_v4(void) {
    const char *atlas_id = "atlas_0000000000000000000000000000a001";
    const char *src_id = "source_0000000000000000000000000000b001";
    char v3[768];
    (void)snprintf(v3, sizeof v3,
                   "{\n  \"version\": 3,\n  \"atlases\": [\n"
                   "    { \"name\": \"hero\", \"id\": \"%s\",\n"
                   "      \"sources\": [ { \"id\": \"%s\", \"path\": \"sprites\" } ],\n"
                   "      \"sprites\": [ { \"name\": \"hero\", \"origin\": [0.25, 0.75] } ] }\n"
                   "  ]\n}\n",
                   atlas_id, src_id);
    char path[512];
    join(path, sizeof path, "mig_v3_sprite.ntpacker_project");
    write_text(path, v3);

    tp_project *p = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &p, &err), err.msg);
    TEST_ASSERT_EQUAL_INT(4, p->schema_version);
    /* the override loaded PENDING (name bridge set, canonical identity unresolved) */
    TEST_ASSERT_EQUAL_INT(1, p->atlases[0].sprite_count);
    TEST_ASSERT_EQUAL_STRING("hero", p->atlases[0].sprites[0].name);
    TEST_ASSERT_TRUE(tp_id128_is_nil(p->atlases[0].sprites[0].source_ref));
    TEST_ASSERT_NULL(p->atlases[0].sprites[0].src_key);

    char saved[512];
    join(saved, sizeof saved, "mig_v4_sprite.ntpacker_project");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save(p, saved, &err));

    char expect[1024];
    (void)snprintf(expect, sizeof expect,
                   "{\n"
                   "  \"version\": 4,\n"
                   "  \"atlases\": [\n"
                   "    {\n"
                   "      \"id\": \"%s\",\n"
                   "      \"name\": \"hero\",\n"
                   "      \"sources\": [\n"
                   "        {\n"
                   "          \"id\": \"%s\",\n"
                   "          \"path\": \"sprites\"\n"
                   "        }\n"
                   "      ],\n"
                   "      \"sprites\": [\n"
                   "        {\n"
                   "          \"name\": \"hero\",\n"
                   "          \"origin\": [0.25, 0.75]\n"
                   "        }\n"
                   "      ]\n"
                   "    }\n"
                   "  ]\n"
                   "}\n",
                   atlas_id, src_id);

    size_t sn = 0;
    char *sbytes = read_all(saved, &sn);
    TEST_ASSERT_EQUAL_size_t(strlen(expect), sn);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(expect, sbytes, sn), sbytes);
    free(sbytes);
    tp_project_destroy(p);
}

/* 8. every checked-in v1 fixture still loads under the current schema (forward-
 *    compat); the intentionally-malformed one still fails cleanly (structured error). */
void test_checked_in_v1_fixtures_still_load(void) {
    const char *ok_fixtures[] = {"clean.ntpacker_project", "parity.ntpacker_project", "problems.ntpacker_project"};
    tp_error err = {0};
    for (size_t i = 0; i < sizeof ok_fixtures / sizeof ok_fixtures[0]; i++) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/%s", TP_CLI_TESTDATA_DIR, ok_fixtures[i]);
        tp_project *p = NULL;
        TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_project_load(path, &p, &err), path);
        TEST_ASSERT_NOT_NULL(p);
        /* every structural entity now carries a non-nil synthetic id */
        for (int ai = 0; ai < p->atlas_count; ai++) {
            TEST_ASSERT_FALSE(tp_id128_is_nil(p->atlases[ai].id));
        }
        tp_project_destroy(p);
    }
    char bad[1024];
    (void)snprintf(bad, sizeof bad, "%s/bad.ntpacker_project", TP_CLI_TESTDATA_DIR);
    tp_project *p = NULL;
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_project_load(bad, &p, &err)); /* malformed -> structured error */
    TEST_ASSERT_NULL(p);
}

int main(int argc, char **argv) {
    g_dir = (argc > 1) ? argv[1] : ".";
    UNITY_BEGIN();
    RUN_TEST(test_v1_repeated_load_stable_ids);
    RUN_TEST(test_writable_promote_once_byte_stable);
    RUN_TEST(test_save_failure_does_not_remap_ids);
    RUN_TEST(test_rng_failure_structured_and_atomic);
    RUN_TEST(test_duplicate_id_rejected);
    RUN_TEST(test_malformed_id_rejected);
    RUN_TEST(test_v2_missing_id_rejected);
    RUN_TEST(test_v2_anim_missing_name_rejected);
    RUN_TEST(test_id_survives_rename_reorder_remove);
    RUN_TEST(test_migration_golden_v1_to_v4);
    RUN_TEST(test_migration_golden_v2_to_v4_sources);
    RUN_TEST(test_migration_golden_v3_to_v4);
    RUN_TEST(test_checked_in_v1_fixtures_still_load);
    return UNITY_END();
}
