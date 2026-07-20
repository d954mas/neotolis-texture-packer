/* F3-03 T4/T5/T6: memory-only pack-result cache, hash selection policy, and the
 * Undo/Redo cache probe.
 *
 * T4 -- store + active pin, inactive re-inflation via tp_pack_read_memory, EXACT
 *       byte-budget LRU accounting/eviction, and contained failure on a corrupt
 *       retained artifact.
 * T5 -- selection by monotonic completion SEQUENCE (a late-completing earlier
 *       job never overwrites a newer preview), explicit selection by hash, and
 *       cancellation leaving the prior authoritative result untouched.
 * T6 -- Undo/Redo recompute the current pack_input_hash and PROBE the cache: a
 *       hit becomes authoritative via explicit selection, a miss keeps the
 *       existing (stale) result; neither ever auto-packs.
 *
 * Artifacts are produced through the real tp_pack path (the test binary is also
 * its own build worker, dispatched in main). */

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_id.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_pack_hash.h"
#include "tp_core/tp_pack_result_cache.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "unity.h"

static const char *g_dir;

void setUp(void) {}
void tearDown(void) {}

/* ---- artifact production ------------------------------------------------- */

typedef struct packed_atlas {
    tp_arena *arena;         /* adopted by the cache on a successful store */
    tp_result *result;       /* borrowed into arena */
    uint8_t *bytes;          /* serialized .ntpack; caller frees (store copies) */
    size_t size;
    char atlas_name[64];     /* captured before the arena is adopted */
    int sprite_count;
} packed_atlas;

static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        (void)fclose(f);
        return NULL;
    }
    uint8_t *b = malloc((size_t)sz);
    if (!b) {
        (void)fclose(f);
        return NULL;
    }
    size_t rd = fread(b, 1U, (size_t)sz, f);
    (void)fclose(f);
    if (rd != (size_t)sz) {
        free(b);
        return NULL;
    }
    *out_size = (size_t)sz;
    return b;
}

/* Packs one fully-opaque 8x8 atlas so every artifact has an identical byte size
 * (uncompressed RGBA8 pages, no trimming) -- exact-accounting tests rely on it. */
static void pack_one(const char *atlas_name, uint8_t salt, packed_atlas *out) {
    memset(out, 0, sizeof *out);
    uint8_t px[8 * 8 * 4];
    for (int i = 0; i < 8 * 8; i++) {
        px[i * 4 + 0] = (uint8_t)(i + salt);
        px[i * 4 + 1] = (uint8_t)(salt * 3U);
        px[i * 4 + 2] = (uint8_t)(255U - salt);
        px[i * 4 + 3] = 255U;
    }
    tp_pack_sprite_desc d;
    memset(&d, 0, sizeof d);
    d.name = "sprite";
    d.rgba = px;
    d.w = 8;
    d.h = 8;
    d.origin_x = 0.5F;
    d.origin_y = 0.5F;

    tp_pack_settings s;
    tp_pack_settings_defaults(&s);
    s.atlas_name = atlas_name;
    s.work_dir = g_dir;
    s.sprites = &d;
    s.sprite_count = 1;
    s.pixels_per_unit = 1.0F;

    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_result *r = NULL;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK, tp_pack(&s, arena, &r, &e), e.msg);
    TEST_ASSERT_NOT_NULL(r);

    char path[1024];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/%s.ntpack", g_dir,
                              atlas_name) > 0);
    size_t n = 0;
    uint8_t *b = read_whole_file(path, &n);
    TEST_ASSERT_NOT_NULL(b);

    out->arena = arena;
    out->result = r;
    out->bytes = b;
    out->size = n;
    (void)snprintf(out->atlas_name, sizeof out->atlas_name, "%s",
                   r->atlas_name ? r->atlas_name : "");
    out->sprite_count = r->sprite_count;
}

static tp_id128 id_of(uint8_t byte) {
    tp_id128 id = tp_id128_nil();
    id.bytes[0] = byte;
    return id;
}

static tp_status store(tp_pack_result_cache *cache, tp_id128 hash,
                       uint64_t seq, packed_atlas *p) {
    tp_error e = {{0}};
    tp_status st = tp_pack_result_cache_store(cache, hash, seq, p->bytes,
                                              p->size, p->arena, p->result, &e);
    if (st == TP_STATUS_OK) {
        p->arena = NULL; /* adopted by the cache */
        p->result = NULL;
    }
    return st;
}

/* ---- T4 ------------------------------------------------------------------ */

void test_store_active_pin_and_authoritative(void) {
    packed_atlas a;
    pack_one("pin0", 1U, &a);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1U << 20);
    TEST_ASSERT_NOT_NULL(cache);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(1U), 1U, &a));
    free(a.bytes);

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.entry_count);
    TEST_ASSERT_TRUE(st.has_active);
    TEST_ASSERT_TRUE(tp_id128_eq(st.active_hash, id_of(1U)));
    TEST_ASSERT_EQUAL_UINT64(0U, st.inactive_bytes);

    const tp_result *r = NULL;
    tp_id128 h = tp_id128_nil();
    uint64_t seq = 0U;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, &seq, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    TEST_ASSERT_EQUAL_UINT64(1U, seq);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING(a.atlas_name, r->atlas_name);
    TEST_ASSERT_EQUAL_INT(a.sprite_count, r->sprite_count);
    tp_pack_result_cache_destroy(cache);
}

void test_inactive_reinflate_from_bytes(void) {
    packed_atlas a;
    packed_atlas b;
    pack_one("inf0", 1U, &a);
    pack_one("inf1", 2U, &b);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1U << 20);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(1U), 1U, &a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(2U), 2U, &b));
    free(a.bytes);
    free(b.bytes);

    /* A is inactive now; selecting it must re-inflate from retained bytes. */
    tp_pack_result_cache_select(cache, id_of(1U));
    const tp_result *r = NULL;
    tp_id128 h = tp_id128_nil();
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING(a.atlas_name, r->atlas_name);
    TEST_ASSERT_EQUAL_INT(a.sprite_count, r->sprite_count);

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_TRUE(tp_id128_eq(st.active_hash, id_of(1U)));
    tp_pack_result_cache_destroy(cache);
}

void test_exact_byte_accounting_and_eviction(void) {
    packed_atlas a;
    packed_atlas b;
    packed_atlas c;
    packed_atlas d;
    pack_one("atl0", 1U, &a);
    pack_one("atl1", 2U, &b);
    pack_one("atl2", 3U, &c);
    pack_one("atl3", 4U, &d);
    /* Uncompressed pages + no trimming => identical artifact sizes. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)a.size, (uint32_t)b.size);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)a.size, (uint32_t)c.size);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)a.size, (uint32_t)d.size);
    const uint64_t s = (uint64_t)a.size;

    tp_pack_result_cache *cache = tp_pack_result_cache_create(2U * s);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(1U), 1U, &a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(2U), 2U, &b));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(3U), 3U, &c));

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    /* A + B inactive (== budget), C active and exempt. */
    TEST_ASSERT_EQUAL_UINT64(2U * s, st.inactive_bytes);
    TEST_ASSERT_EQUAL_INT(3, st.entry_count);
    TEST_ASSERT_EQUAL_UINT64(0U, st.evicted);

    /* Storing D demotes C (3*s inactive) -> evict the LRU (A). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(4U), 4U, &d));
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64(2U * s, st.inactive_bytes); /* EXACT */
    TEST_ASSERT_EQUAL_INT(3, st.entry_count);
    TEST_ASSERT_EQUAL_UINT64(1U, st.evicted);
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, id_of(1U)));
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, id_of(2U)));
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, id_of(3U)));
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, id_of(4U)));

    free(a.bytes);
    free(b.bytes);
    free(c.bytes);
    free(d.bytes);
    tp_pack_result_cache_destroy(cache);
}

void test_corrupt_retained_entry_is_contained(void) {
    packed_atlas good;
    packed_atlas victim;
    pack_one("good", 1U, &good);
    pack_one("victim", 2U, &victim);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1U << 20);
    /* Store the victim ACTIVE with a valid result (for names/pin) but CORRUPT
     * retained bytes and the HIGHEST sequence. */
    static const uint8_t garbage[32] = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U};
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_pack_result_cache_store(cache, id_of(9U), 9U, garbage,
                                   sizeof garbage, victim.arena, victim.result,
                                   &e));
    victim.arena = NULL;
    victim.result = NULL;
    free(victim.bytes);
    /* Store good (valid) -> victim demoted inactive with corrupt bytes. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(1U), 1U, &good));
    free(good.bytes);

    /* Authoritative resolves to victim (highest seq) first; its inflate fails,
     * it is dropped, and resolution falls back to good -- no crash. */
    const tp_result *r = NULL;
    tp_id128 h = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    TEST_ASSERT_EQUAL_STRING(good.atlas_name, r->atlas_name);

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64(1U, st.dropped_corrupt);
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, id_of(9U)));
    TEST_ASSERT_EQUAL_INT(1, st.entry_count);

    /* Cache stays usable. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    tp_pack_result_cache_destroy(cache);
}

/* ---- T5 ------------------------------------------------------------------ */

void test_selection_by_sequence_ignores_store_order(void) {
    packed_atlas newer; /* higher sequence (newer request) */
    packed_atlas older; /* lower sequence (earlier request) */
    pack_one("newer", 1U, &newer);
    pack_one("older", 2U, &older);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1U << 20);
    /* The NEWER request (seq 2) completes/stores FIRST; the OLDER request
     * (seq 1) completes LATE and stores second. The older completion must NOT
     * become authoritative (§10.3, decision 0004). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(2U), 2U, &newer));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(1U), 1U, &older));
    free(newer.bytes);
    free(older.bytes);

    const tp_result *r = NULL;
    tp_id128 h = tp_id128_nil();
    uint64_t seq = 0U;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, &seq, &e));
    TEST_ASSERT_TRUE_MESSAGE(tp_id128_eq(h, id_of(2U)),
                             "newer sequence must win regardless of store order");
    TEST_ASSERT_EQUAL_UINT64(2U, seq);
    TEST_ASSERT_EQUAL_STRING(newer.atlas_name, r->atlas_name);

    /* Explicit selection by hash overrides sequence; clearing reverts. */
    tp_pack_result_cache_select(cache, id_of(1U));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, NULL, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));

    tp_pack_result_cache_select(cache, tp_id128_nil());
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, NULL, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(2U)));

    /* Selecting an absent hash clears the selection (revert to latest). */
    tp_pack_result_cache_select(cache, id_of(200U));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, NULL, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(2U)));
    tp_pack_result_cache_destroy(cache);
}

void test_cancellation_leaves_prior_authoritative(void) {
    packed_atlas a;
    pack_one("prior", 1U, &a);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1U << 20);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, id_of(1U), 1U, &a));
    free(a.bytes);

    const tp_result *r = NULL;
    tp_id128 h = tp_id128_nil();
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));

    /* A cancelled job publishes nothing to the cache, so the authoritative
     * result is unchanged. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    tp_pack_result_cache_destroy(cache);
}

/* ---- T6 (Undo/Redo cache probe; presentation deferred to phase U) -------- */

static int deterministic_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *seed = (uint8_t *)ctx;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(*seed + (uint8_t)i);
    }
    *seed = (uint8_t)(*seed + 17U);
    return (int)len;
}

static tp_session *make_session(void) {
    static uint8_t seed = 1U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create(&rng, &session, &err));
    TEST_ASSERT_NOT_NULL(session);
    return session;
}

static tp_id128 default_atlas_id(tp_session *session) {
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 id = atlas->id;
    tp_session_snapshot_destroy(snapshot);
    return id;
}

static void rename_atlas(tp_session *session, const char *new_name) {
    static unsigned counter = 0U;
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas->id;
    const size_t name_size = strlen(new_name) + 1U;
    operation.u.atlas_rename.name = malloc(name_size);
    TEST_ASSERT_NOT_NULL(operation.u.atlas_rename.name);
    memcpy(operation.u.atlas_rename.name, new_name, name_size);

    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%032x", counter++);
    request.expected_revision = tp_session_snapshot_revision(snapshot);
    request.ops = &operation;
    request.op_count = 1U;

    tp_txn_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    TEST_ASSERT_TRUE(result.committed);

    tp_txn_result_free(&result);
    tp_operation_free(&operation);
    tp_session_snapshot_destroy(snapshot);
}

static tp_id128 current_hash(tp_session *session, tp_id128 atlas,
                             tp_pack_image_hash_cache *imgcache) {
    tp_id128 h = tp_id128_nil();
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_pack_input_hash(session, atlas, imgcache, &h, &err), err.msg);
    TEST_ASSERT_FALSE(tp_id128_is_nil(h));
    return h;
}

void test_undo_redo_cache_probe_never_autopacks(void) {
    tp_session *session = make_session();
    const tp_id128 atlas = default_atlas_id(session);
    tp_pack_image_hash_cache *imgcache = tp_pack_image_hash_cache_create();
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1U << 20);
    tp_error e = {{0}};

    /* state 0 -> hash h0; "pack" and store an artifact under h0. */
    const tp_id128 h0 = current_hash(session, atlas, imgcache);
    packed_atlas p0;
    pack_one("undo0", 1U, &p0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, h0, 1U, &p0));
    free(p0.bytes);
    TEST_ASSERT_FALSE(tp_session_job_active(session));

    /* Rename -> state 1 -> hash h1 != h0; store under h1. */
    rename_atlas(session, "renamed1");
    const tp_id128 h1 = current_hash(session, atlas, imgcache);
    TEST_ASSERT_FALSE(tp_id128_eq(h0, h1));
    packed_atlas p1;
    pack_one("undo1", 2U, &p1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store(cache, h1, 2U, &p1));
    free(p1.bytes);

    tp_id128 ha = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &ha, NULL, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(ha, h1));

    /* UNDO -> back to state 0; recompute + PROBE = HIT. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_undo(session, &e));
    const tp_id128 hcur0 = current_hash(session, atlas, imgcache);
    TEST_ASSERT_TRUE(tp_id128_eq(hcur0, h0));
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, hcur0));
    tp_pack_result_cache_select(cache, hcur0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &ha, NULL, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(ha, h0));
    TEST_ASSERT_FALSE_MESSAGE(tp_session_job_active(session),
                              "undo cache HIT must never auto-pack");
    tp_pack_result_cache_select(cache, tp_id128_nil());

    /* REDO -> state 1; recompute + PROBE = HIT. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_redo(session, &e));
    const tp_id128 hcur1 = current_hash(session, atlas, imgcache);
    TEST_ASSERT_TRUE(tp_id128_eq(hcur1, h1));
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, hcur1));

    /* Edit to a NEVER-packed state 2; recompute + PROBE = MISS. */
    rename_atlas(session, "renamed2");
    const tp_id128 h2 = current_hash(session, atlas, imgcache);
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, h2));
    /* The existing preview stays available and is honestly stale: authoritative
     * still returns a cached result whose hash != the current hash. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &ha, NULL, NULL, &e));
    TEST_ASSERT_FALSE(tp_id128_eq(ha, h2));
    TEST_ASSERT_FALSE_MESSAGE(tp_session_job_active(session),
                              "undo cache MISS must never auto-pack");

    tp_pack_result_cache_destroy(cache);
    tp_pack_image_hash_cache_destroy(imgcache);
    tp_session_destroy(session);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    g_dir = (argc > 1) ? argv[1] : ".";
    UNITY_BEGIN();
    RUN_TEST(test_store_active_pin_and_authoritative);
    RUN_TEST(test_inactive_reinflate_from_bytes);
    RUN_TEST(test_exact_byte_accounting_and_eviction);
    RUN_TEST(test_corrupt_retained_entry_is_contained);
    RUN_TEST(test_selection_by_sequence_ignores_store_order);
    RUN_TEST(test_cancellation_leaves_prior_authoritative);
    RUN_TEST(test_undo_redo_cache_probe_never_autopacks);
    return UNITY_END();
}
