/* F3-03 T4/T5/T6 + S18 + S28: the memory-only pack-result store, its hash
 * selection policy, the Undo/Redo cache probe, and the COMPRESSED COLD TIER.
 *
 * T4 -- store + active pin, EXACT byte-budget LRU accounting/eviction, and
 *       contained failure when a cold entry can no longer be decoded.
 * T5 -- selection by monotonic completion SEQUENCE (a late-completing earlier
 *       job never overwrites a newer preview), explicit selection by hash, and
 *       cancellation leaving the prior authoritative result untouched.
 * T6 -- Undo/Redo recompute the current pack_input_hash and PROBE the cache: a
 *       hit becomes authoritative via explicit selection, a miss keeps the
 *       existing (stale) result; neither ever auto-packs.
 * S18 -- entries are pinned through an opaque caller receipt released EXACTLY
 *       once, by whichever of eviction / re-store / forget / destroy reaches it
 *       first -- or by the cold transition, which no longer needs the pin.
 * S28 -- background compression on demotion, parallel decode on promotion,
 *       raw -> compressed budget accounting, the ratio floor, and the
 *       cancel-and-join discipline that keeps an in-flight encode safe when the
 *       entry it snapshots is evicted or re-activated under it.
 *
 * The S28 cases drive SYNTHETIC results because the properties under test are
 * about the PAGES: how many there are, and whether they compress. One case still
 * packs a real atlas through tp_pack (the test binary is also its own build
 * worker, dispatched in main) so the metadata deep copy is proven against real
 * geometry -- names, hulls, indices -- and not only against fixture structs. */

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
#include "tp_test_seams.h"
#include "unity.h"

static const char *g_dir;

void setUp(void) {}
void tearDown(void) {}

static tp_id128 id_of(uint8_t byte) {
    tp_id128 id = tp_id128_nil();
    id.bytes[0] = byte;
    return id;
}

/* ---- synthetic pinned results -------------------------------------------- */

/* Stands in for a session Pack receipt: an arena that owns a tp_result, released
 * exactly once through the store's hook. `origin` is an INDEPENDENT copy of the
 * page bytes kept outside the arena, so a cold round-trip can be compared against
 * the original pixels after the pin (and the arena with it) is long gone. */
typedef struct fake_pin {
    tp_arena *arena;
    tp_result *result;
    int releases;
    int page_count;
    uint8_t **origin;
    size_t *origin_sizes;
    uint64_t raw_bytes;
} fake_pin;

static void fake_pin_release(void *owner) {
    fake_pin *pin = (fake_pin *)owner;
    pin->releases++;
    tp_arena_destroy(pin->arena);
    pin->arena = NULL;
    pin->result = NULL; /* lived in the arena: the caller must not read it now */
}

static void fake_pin_free(fake_pin *pin) {
    for (int i = 0; i < pin->page_count; ++i) {
        free(pin->origin[i]);
    }
    free(pin->origin);
    free(pin->origin_sizes);
    pin->origin = NULL;
    pin->origin_sizes = NULL;
    tp_arena_destroy(pin->arena);
    pin->arena = NULL;
    pin->result = NULL;
}

/* Long flat runs plus a small repeating motif: exactly what a packed sprite page
 * looks like to LZ77 (alpha gutters + flat fills), and comfortably over the
 * store's ratio floor. */
static void fill_compressible(uint8_t *px, size_t bytes, uint32_t seed) {
    memset(px, 0, bytes);
    for (size_t i = (seed % 7U) * 4U; i + 4U <= bytes; i += 256U) {
        px[i + 0U] = (uint8_t)(seed * 7U);
        px[i + 1U] = (uint8_t)(seed * 13U);
        px[i + 2U] = (uint8_t)(seed);
        px[i + 3U] = 255U;
    }
}

/* Opaque xorshift noise: the pessimistic floor the bench measured at 1.24x, i.e.
 * the content the ratio floor exists to reject. */
static void fill_incompressible(uint8_t *px, size_t bytes, uint32_t seed) {
    uint32_t state = seed * 2654435761U + 1U;
    for (size_t i = 0; i + 4U <= bytes; i += 4U) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        px[i + 0U] = (uint8_t)(state);
        px[i + 1U] = (uint8_t)(state >> 8);
        px[i + 2U] = (uint8_t)(state >> 16);
        px[i + 3U] = 255U;
    }
}

static void fake_pin_make(fake_pin *pin, const char *atlas_name, int pages,
                          int dim, bool compressible, uint32_t seed) {
    memset(pin, 0, sizeof *pin);
    pin->arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(pin->arena);
    tp_result *result = tp_arena_alloc(pin->arena, sizeof *result);
    TEST_ASSERT_NOT_NULL(result);
    memset(result, 0, sizeof *result);
    result->atlas_name = tp_arena_strdup(pin->arena, atlas_name);
    result->pixels_per_unit = 100.0F;
    result->page_count = pages;
    result->pages = tp_arena_alloc(pin->arena, (size_t)pages * sizeof *result->pages);
    TEST_ASSERT_NOT_NULL(result->pages);
    memset(result->pages, 0, (size_t)pages * sizeof *result->pages);

    pin->page_count = pages;
    pin->origin = calloc((size_t)pages, sizeof *pin->origin);
    pin->origin_sizes = calloc((size_t)pages, sizeof *pin->origin_sizes);
    TEST_ASSERT_NOT_NULL(pin->origin);
    TEST_ASSERT_NOT_NULL(pin->origin_sizes);

    const size_t bytes = (size_t)dim * (size_t)dim * 4U;
    for (int i = 0; i < pages; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "%s_%d", atlas_name, i);
        uint8_t *px = tp_arena_alloc(pin->arena, bytes);
        TEST_ASSERT_NOT_NULL(px);
        if (compressible) {
            fill_compressible(px, bytes, seed + (uint32_t)i);
        } else {
            fill_incompressible(px, bytes, seed + (uint32_t)i);
        }
        result->pages[i].image_name = tp_arena_strdup(pin->arena, name);
        result->pages[i].w = dim;
        result->pages[i].h = dim;
        result->pages[i].rgba = px;
        result->pages[i].premultiplied = true;
        pin->origin[i] = malloc(bytes);
        TEST_ASSERT_NOT_NULL(pin->origin[i]);
        memcpy(pin->origin[i], px, bytes);
        pin->origin_sizes[i] = bytes;
        pin->raw_bytes += (uint64_t)bytes;
    }

    /* Two sprites carrying every variable-length field the metadata deep copy has
     * to reproduce: the name, the hull, and the index list. */
    result->sprite_count = 2;
    result->sprites =
        tp_arena_alloc(pin->arena, 2U * sizeof *result->sprites);
    TEST_ASSERT_NOT_NULL(result->sprites);
    memset(result->sprites, 0, 2U * sizeof *result->sprites);
    for (int i = 0; i < 2; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "%s/sprite%d", atlas_name, i);
        tp_sprite *sprite = &result->sprites[i];
        sprite->name = tp_arena_strdup(pin->arena, name);
        sprite->page = i % pages;
        sprite->frame.x = i * 8;
        sprite->frame.y = 4;
        sprite->frame.w = 8;
        sprite->frame.h = 8;
        sprite->trimmed = true;
        sprite->spriteSourceSize.w = 8;
        sprite->spriteSourceSize.h = 8;
        sprite->sourceSize.w = 16 + i;
        sprite->sourceSize.h = 16;
        sprite->pivot.x = 0.5F;
        sprite->pivot.y = 0.25F;
        sprite->slice9_lrtb[0] = (uint16_t)(1 + i);
        sprite->alias_of = -1;
        sprite->vert_count = 4;
        sprite->verts = tp_arena_alloc(pin->arena, 4U * sizeof *sprite->verts);
        TEST_ASSERT_NOT_NULL(sprite->verts);
        for (int v = 0; v < 4; ++v) {
            sprite->verts[v].x = v * (i + 1);
            sprite->verts[v].y = v;
        }
        sprite->index_count = 6;
        sprite->indices =
            tp_arena_alloc(pin->arena, 6U * sizeof *sprite->indices);
        TEST_ASSERT_NOT_NULL(sprite->indices);
        for (int k = 0; k < 6; ++k) {
            sprite->indices[k] = (uint16_t)(k % 4);
        }
    }
    pin->result = result;
}

static tp_status store_pin(tp_pack_result_cache *cache, tp_id128 hash,
                           uint64_t seq, fake_pin *pin) {
    tp_error e = {{0}};
    return tp_pack_result_cache_store_retained(cache, hash, seq, pin->result,
                                               pin->raw_bytes, pin,
                                               fake_pin_release, &e);
}

static const tp_result *authoritative(tp_pack_result_cache *cache,
                                      tp_id128 *out_hash) {
    const tp_result *result = NULL;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_pack_result_cache_authoritative(cache, out_hash, &result, NULL, &e),
        e.msg);
    return result;
}

/* Every byte of every page, plus the geometry the deep copy had to reproduce. */
static void assert_round_trip(const fake_pin *pin, const tp_result *result) {
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(pin->page_count, result->page_count);
    TEST_ASSERT_EQUAL_INT(2, result->sprite_count);
    TEST_ASSERT_EQUAL_STRING("sprite-a/sprite0", result->sprites[0].name);
    TEST_ASSERT_EQUAL_INT(4, result->sprites[0].vert_count);
    TEST_ASSERT_EQUAL_INT(6, result->sprites[0].index_count);
    TEST_ASSERT_EQUAL_INT(6, result->sprites[1].verts[3].x);
    TEST_ASSERT_EQUAL_INT(2, result->sprites[1].slice9_lrtb[0]);
    TEST_ASSERT_EQUAL_INT(17, result->sprites[1].sourceSize.w);
    for (int i = 0; i < pin->page_count; ++i) {
        TEST_ASSERT_NOT_NULL_MESSAGE(result->pages[i].rgba,
                                     "a promoted entry must carry page pixels");
        TEST_ASSERT_TRUE(result->pages[i].premultiplied);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
            pin->origin[i], result->pages[i].rgba, pin->origin_sizes[i],
            "a cold round trip must reproduce the page byte for byte");
    }
}

/* ---- T4 / S18: pin, accounting, eviction --------------------------------- */

void test_store_active_pin_and_authoritative(void) {
    fake_pin a;
    fake_pin_make(&a, "sprite-a", 2, 32, true, 1U);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1U << 20);
    TEST_ASSERT_NOT_NULL(cache);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &a));

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.entry_count);
    TEST_ASSERT_TRUE(st.has_active);
    TEST_ASSERT_TRUE(tp_id128_eq(st.active_hash, id_of(1U)));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0U, st.inactive_bytes,
                                     "the active pin is budget-exempt");

    tp_id128 h = tp_id128_nil();
    uint64_t seq = 0U;
    const tp_result *r = NULL;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, &seq, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    TEST_ASSERT_EQUAL_UINT64(1U, seq);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(a.result, r,
                                  "an active HOT entry hands back the caller's "
                                  "own result, with no copy in between");

    /* Nothing has been demoted, so no compression exists to settle. */
    tp_pack_result_cache_settle(cache);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(0, st.cold_entries);
    TEST_ASSERT_EQUAL_UINT64(0U, st.encoded);

    tp_pack_result_cache_destroy(cache);
    TEST_ASSERT_EQUAL_INT(1, a.releases);
    fake_pin_free(&a);
}

/* S28 successor to the serialized mode's test_inactive_reinflate_from_bytes: an
 * inactive entry no longer keeps raw pages OR a serialized artifact -- it keeps
 * per-page deflate blobs plus a metadata copy, and selecting it inflates them
 * back into fresh buffers. The bytes that come out must equal the bytes that went
 * in, which is the whole premise of the tier. */
void test_cold_entry_round_trips_pages_byte_identically(void) {
    fake_pin a;
    fake_pin b;
    fake_pin_make(&a, "sprite-a", 3, 64, true, 1U);
    fake_pin_make(&b, "sprite-b", 1, 32, true, 2U);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &b));

    /* A is demoted and queued; the compression lands on the settle, and THAT is
     * what releases A's pin -- the raw pages are redundant once the blobs exist. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, a.releases,
                                  "demotion alone must not release the pin");
    tp_pack_result_cache_settle(cache);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, a.releases, "going cold releases the pin exactly once: the store no "
                       "longer needs the caller's raw pages");

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.cold_entries);
    TEST_ASSERT_EQUAL_UINT64(1U, st.encoded);
    TEST_ASSERT_EQUAL_UINT64(a.raw_bytes, st.cold_raw_bytes);
    TEST_ASSERT_TRUE_MESSAGE(st.cold_coded_bytes < a.raw_bytes,
                             "a cold entry must cost less than its raw pages");
    TEST_ASSERT_TRUE_MESSAGE(
        st.cold_meta_bytes > 0U && st.cold_meta_bytes < a.raw_bytes / 8U,
        "the uncompressed geometry the entry also keeps is small next to the "
        "pages it replaces");

    /* A peek is defined not to decompress, so a cold entry answers with its
     * geometry and NO pixels -- and, crucially, is not a miss. */
    const tp_result *peeked = tp_pack_result_cache_peek(cache, id_of(1U));
    TEST_ASSERT_NOT_NULL_MESSAGE(
        peeked, "a merely cold entry must never read as gone");
    TEST_ASSERT_EQUAL_STRING("sprite-a", peeked->atlas_name);
    TEST_ASSERT_EQUAL_INT(3, peeked->page_count);
    TEST_ASSERT_NULL_MESSAGE(peeked->pages[0].rgba,
                             "a peek promises geometry, never pixels");
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, st.cold_entries,
                                  "a peek must not promote anything");

    tp_pack_result_cache_select(cache, id_of(1U));
    tp_id128 h = tp_id128_nil();
    assert_round_trip(&a, authoritative(cache, &h));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64(1U, st.promoted);
    TEST_ASSERT_TRUE(tp_id128_eq(st.active_hash, id_of(1U)));

    /* Demoting the promoted entry gives the pixels back but keeps the blobs, so
     * a second promote is not a second encode. */
    tp_pack_result_cache_select(cache, id_of(2U));
    (void)authoritative(cache, &h);
    tp_pack_result_cache_settle(cache);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        1U, st.encoded, "a cold entry that was promoted and demoted keeps its "
                        "blobs and is never re-encoded");
    tp_pack_result_cache_select(cache, id_of(1U));
    assert_round_trip(&a, authoritative(cache, &h));

    tp_pack_result_cache_destroy(cache);
    TEST_ASSERT_EQUAL_INT(1, a.releases);
    TEST_ASSERT_EQUAL_INT(1, b.releases);
    fake_pin_free(&a);
    fake_pin_free(&b);
}

/* The promote fans the pages out across threads, so a result with more pages than
 * the fork-join width has to come back exactly as a single-page one does: same
 * bytes, in the right pages, with no slice writing into a neighbour's buffer. The
 * one-page control in the same case is the serial path. */
void test_parallel_page_decode_matches_the_serial_result(void) {
    fake_pin wide;   /* more pages than TP_PACK_RESULT_CACHE_DECODE_THREADS */
    fake_pin serial; /* one page: the fork-join is skipped entirely */
    fake_pin_make(&wide, "sprite-a", TP_PACK_RESULT_CACHE_DECODE_THREADS + 3, 48,
                  true, 5U);
    fake_pin_make(&serial, "sprite-a", 1, 48, true, 5U);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &wide));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          store_pin(cache, id_of(2U), 2U, &serial));
    tp_pack_result_cache_settle(cache);

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.cold_entries);

    tp_id128 h = tp_id128_nil();
    tp_pack_result_cache_select(cache, id_of(1U));
    assert_round_trip(&wide, authoritative(cache, &h));

    tp_pack_result_cache_select(cache, id_of(2U));
    tp_pack_result_cache_settle(cache); /* the wide entry goes cold again */
    assert_round_trip(&serial, authoritative(cache, &h));

    /* And back once more, so the parallel decode runs against blobs that have
     * already survived one promote/demote cycle. */
    tp_pack_result_cache_select(cache, id_of(1U));
    assert_round_trip(&wide, authoritative(cache, &h));

    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&wide);
    fake_pin_free(&serial);
}

/* The budget is not one number for the life of an entry: an inactive entry costs
 * its RAW pages until its background compression lands and its COMPRESSED bytes
 * afterwards, because that is the memory the store is actually holding at each
 * moment. Nothing but a pump may make that transition. */
void test_inactive_budget_moves_from_raw_to_compressed_on_the_pump(void) {
    fake_pin a;
    fake_pin b;
    fake_pin_make(&a, "sprite-a", 2, 64, true, 3U);
    fake_pin_make(&b, "sprite-b", 1, 32, true, 4U);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &b));

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        a.raw_bytes, st.inactive_bytes,
        "a demoted entry is charged RAW until its compression is applied");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, st.encoding,
                                  "demotion queues exactly one compression");
    TEST_ASSERT_EQUAL_INT(0, st.cold_entries);

    tp_pack_result_cache_settle(cache);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(0, st.encoding);
    TEST_ASSERT_EQUAL_INT(1, st.cold_entries);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        st.cold_coded_bytes + st.cold_meta_bytes, st.inactive_bytes,
        "a cold entry is charged EXACTLY what it occupies: its compressed page "
        "bytes PLUS the uncompressed geometry copy it also holds");
    TEST_ASSERT_TRUE(st.inactive_bytes < a.raw_bytes);

    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&a);
    fake_pin_free(&b);
}

/* EXACT accounting across the tier, and the shape budget pressure actually takes
 * once the tier exists.
 *
 * Part 1 -- with room to spare, inactive_bytes is the EXACT sum of what the cold
 * entries occupy -- compressed pages PLUS the geometry copy that comes with them
 * -- for four identical results, three of them cold. Cold entries accumulate in
 * count without a practical bound, so the geometry has to be inside the number
 * the budget bounds; a total that only counted blobs would let real memory grow
 * unwatched.
 *
 * Part 2 -- what pushes a store over its budget is no longer the accumulated
 * cold entries (at a real compression ratio a budget holds hundreds of them) but
 * the RAW transient: a just-demoted entry is charged its full pages until its
 * background compression is applied, so a budget smaller than one raw result
 * evicts the least-recently-used entry on every switch. That is the honest
 * consequence of counting what the store is holding right now, and it is what
 * sizes a real budget: it must clear the raw size of the atlases in flight, not
 * the compressed size of the ones at rest. */
void test_exact_byte_accounting_and_eviction(void) {
    fake_pin pins[4];
    for (int i = 0; i < 4; ++i) {
        fake_pin_make(&pins[i], "sprite-a", 1, 64, true, 9U);
    }
    const uint64_t raw = pins[0].raw_bytes;
    tp_pack_result_cache_stats st;

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              store_pin(cache, id_of((uint8_t)(i + 1)),
                                        (uint64_t)(i + 1), &pins[i]));
        tp_pack_result_cache_settle(cache);
    }
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(4, st.entry_count);
    TEST_ASSERT_EQUAL_INT(3, st.cold_entries);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        st.cold_coded_bytes + st.cold_meta_bytes, st.inactive_bytes,
        "EXACT: the accounted total IS the sum of the cold entries' compressed "
        "pages and their geometry copies");
    TEST_ASSERT_TRUE_MESSAGE(
        st.cold_meta_bytes > 0U && st.inactive_bytes > st.cold_coded_bytes,
        "the geometry copy is real memory the store holds, so it is inside the "
        "budgeted total and not merely reported beside it");
    TEST_ASSERT_EQUAL_UINT64(0U, st.evicted);
    /* Identical content, so the per-entry cost is the third of the total. */
    const uint64_t unit = (st.cold_coded_bytes + st.cold_meta_bytes) / 3U;
    TEST_ASSERT_TRUE(unit > 0U && unit < raw);
    tp_pack_result_cache_destroy(cache);
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT(1, pins[i].releases);
        fake_pin_free(&pins[i]);
        fake_pin_make(&pins[i], "sprite-a", 1, 64, true, 9U);
    }

    /* Budget == exactly one raw result: one demotion fits, a second one on top of
     * an already-cold neighbour does not. */
    cache = tp_pack_result_cache_create(raw);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &pins[0]));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &pins[1]));
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        raw, st.inactive_bytes,
        "the demoted entry is charged raw and exactly fills the budget");
    TEST_ASSERT_EQUAL_UINT64(0U, st.evicted);
    tp_pack_result_cache_settle(cache);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64(unit, st.inactive_bytes);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(3U), 3U, &pins[2]));
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        1U, st.evicted,
        "cold(unit) + raw(one result) is over budget, so the LRU reclaims -- and "
        "the newly demoted entry is the most recently touched, so the victim is "
        "the cold neighbour, not the one that caused the pressure");
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, id_of(1U)));
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, id_of(2U)));
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, id_of(3U)));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(raw, st.inactive_bytes, "EXACT");
    TEST_ASSERT_EQUAL_INT(2, st.entry_count);

    tp_pack_result_cache_destroy(cache);
    for (int i = 0; i < 4; ++i) {
        fake_pin_free(&pins[i]);
    }
}

/* Zero budget must never demote the "highest sequence is always resolvable"
 * guarantee (store contract). An out-of-order store (seq 2 stored
 * first/active, seq 1 stored late, demoting seq 2 to inactive) would otherwise let
 * the budget-0 LRU evict seq 2 immediately, leaving authoritative() returning the
 * stale seq 1. The max-sequence entry is exempt from eviction like the active pin;
 * every OTHER inactive entry is still reclaimed at budget 0. */
void test_zero_budget_keeps_max_sequence_resolvable(void) {
    fake_pin s2; /* seq 2 -- the newest request */
    fake_pin s1; /* seq 1 -- an earlier request completing late */
    fake_pin s0; /* seq 0 -- an even earlier request, must be reclaimed */
    fake_pin_make(&s2, "sprite-a", 1, 32, true, 11U);
    fake_pin_make(&s1, "sprite-a", 1, 32, true, 12U);
    fake_pin_make(&s0, "sprite-a", 1, 32, true, 13U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(0U);
    TEST_ASSERT_NOT_NULL(cache);

    /* seq 2 stores first (active); seq 1 completes late and demotes seq 2 to
     * inactive. At budget 0 the LRU would evict seq 2 -- it must not. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &s2));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &s1));

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_TRUE_MESSAGE(tp_pack_result_cache_contains(cache, id_of(2U)),
                             "zero budget must not evict the highest sequence");
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, id_of(1U)));
    TEST_ASSERT_EQUAL_UINT64(0U, st.evicted);
    TEST_ASSERT_EQUAL_UINT64(s2.raw_bytes, st.inactive_bytes);

    /* The exempt entry still compresses: the exemption is about eviction, not
     * about staying raw, so the one entry a zero budget is forced to keep costs
     * the store its compressed size rather than its full pages. */
    tp_pack_result_cache_settle(cache);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.cold_entries);
    TEST_ASSERT_EQUAL_UINT64(st.cold_coded_bytes + st.cold_meta_bytes,
                             st.inactive_bytes);
    TEST_ASSERT_TRUE(st.inactive_bytes < s2.raw_bytes);
    TEST_ASSERT_EQUAL_UINT64(0U, st.evicted);

    /* A third, lower-sequence store (seq 0) becomes active and demotes seq 1. seq 1
     * is neither active nor the max sequence, so the zero-budget sweep reclaims it
     * while seq 2 stays exempt -- eviction is not globally disabled by the exempt. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(3U), 0U, &s0));
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_TRUE_MESSAGE(tp_pack_result_cache_contains(cache, id_of(2U)),
                             "highest sequence stays exempt across further stores");
    TEST_ASSERT_FALSE_MESSAGE(tp_pack_result_cache_contains(cache, id_of(1U)),
                              "a non-max inactive entry must still be reclaimed");
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, id_of(3U))); /* active */
    TEST_ASSERT_EQUAL_UINT64(1U, st.evicted);
    TEST_ASSERT_EQUAL_INT(1, s1.releases);

    /* authoritative() decompresses the cold seq-2 entry and returns it, not the
     * active seq-0 entry: the highest sequence is authoritative. */
    tp_id128 h = tp_id128_nil();
    uint64_t seq = 0U;
    const tp_result *r = NULL;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, &seq, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(2U)));
    TEST_ASSERT_EQUAL_UINT64(2U, seq);
    assert_round_trip(&s2, r);

    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&s2);
    fake_pin_free(&s1);
    fake_pin_free(&s0);
}

/* S28 successor to test_corrupt_retained_entry_is_contained: the artifact that can
 * fail to decode is now a page blob, not a serialized .ntpack, but the contract is
 * the same -- drop the entry, count it, fall back to the next candidate, and stay
 * usable. Reached through a test seam because the store both writes and reads its
 * own blobs, so production has no route to a damaged one. */
void test_corrupt_cold_entry_is_contained(void) {
    fake_pin victim; /* highest sequence: authoritative resolves to it first */
    fake_pin good;
    fake_pin_make(&victim, "sprite-a", 2, 32, true, 21U);
    fake_pin_make(&good, "sprite-a", 1, 32, true, 22U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(9U), 9U, &victim));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &good));
    tp_pack_result_cache_settle(cache);
    TEST_ASSERT_TRUE(tp_pack_result_cache__test_damage_cold_blob(cache, id_of(9U)));

    tp_id128 h = tp_id128_nil();
    const tp_result *r = authoritative(cache, &h);
    TEST_ASSERT_TRUE_MESSAGE(tp_id128_eq(h, id_of(1U)),
                             "resolution falls back past the undecodable entry");
    TEST_ASSERT_EQUAL_PTR(good.result, r);

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64(1U, st.dropped_corrupt);
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, id_of(9U)));
    TEST_ASSERT_EQUAL_INT(1, st.entry_count);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        0U, st.inactive_bytes,
        "the dropped entry's bytes leave the accounting with it");

    /* Cache stays usable. */
    TEST_ASSERT_EQUAL_PTR(good.result, authoritative(cache, &h));
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&victim);
    fake_pin_free(&good);
}

/* ---- S28: the ratio floor ------------------------------------------------ */

/* Content that does not compress buys almost no memory back while still charging
 * a full parallel decode to the next atlas switch, so the store refuses to keep it
 * cold and evicts it instead. */
void test_incompressible_entry_is_evicted_instead_of_kept_cold(void) {
    fake_pin noise;
    fake_pin art;
    fake_pin_make(&noise, "sprite-a", 1, 128, false, 31U);
    fake_pin_make(&art, "sprite-b", 1, 64, true, 32U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &noise));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &art));
    tp_pack_result_cache_settle(cache);

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_FALSE_MESSAGE(
        tp_pack_result_cache_contains(cache, id_of(1U)),
        "an entry under the ratio floor is not worth a cold slot");
    TEST_ASSERT_EQUAL_UINT64(1U, st.ratio_floor_evicted);
    TEST_ASSERT_EQUAL_UINT64(1U, st.evicted);
    TEST_ASSERT_EQUAL_INT(0, st.cold_entries);
    TEST_ASSERT_EQUAL_UINT64(0U, st.encoded);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        0U, st.inactive_bytes, "the rejected entry leaves the budget with it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, noise.releases,
                                  "the rejected entry releases its pin once");
    TEST_ASSERT_EQUAL_INT(1, st.entry_count);
    TEST_ASSERT_TRUE(tp_pack_result_cache_contains(cache, id_of(2U)));

    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&noise);
    fake_pin_free(&art);
}

/* The floor is a policy; "the active pin and the highest sequence are never
 * evicted" is a contract. The contract wins: such an entry stays
 * HOT and raw-accounted, and is never queued for compression again. */
void test_ratio_floor_never_evicts_the_max_sequence_entry(void) {
    fake_pin noise; /* the HIGHEST sequence, and incompressible */
    fake_pin art;   /* a lower sequence stored later, so noise is demoted */
    fake_pin_make(&noise, "sprite-a", 1, 128, false, 41U);
    fake_pin_make(&art, "sprite-b", 1, 64, true, 42U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 9U, &noise));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &art));
    tp_pack_result_cache_settle(cache);

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_TRUE_MESSAGE(
        tp_pack_result_cache_contains(cache, id_of(1U)),
        "the max-sequence entry is exempt from eviction, ratio floor included");
    TEST_ASSERT_EQUAL_UINT64(0U, st.ratio_floor_evicted);
    TEST_ASSERT_EQUAL_UINT64(0U, st.evicted);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, st.cold_entries,
                                  "and it is not kept cold either");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        noise.raw_bytes, st.inactive_bytes,
        "so it goes on costing its raw pages, honestly accounted");
    TEST_ASSERT_EQUAL_INT(0, noise.releases);

    /* A declined entry is not re-queued on the next demotion: the answer was
     * about its content, and only a re-store changes that. */
    tp_pack_result_cache_select(cache, id_of(1U));
    tp_id128 h = tp_id128_nil();
    assert_round_trip(&noise, authoritative(cache, &h));
    tp_pack_result_cache_select(cache, id_of(2U));
    (void)authoritative(cache, &h);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, st.encoding,
                                  "a declined entry is never queued again");

    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&noise);
    fake_pin_free(&art);
}

/* ---- S28: in-flight compression safety ----------------------------------- */

/* The worker holds borrowed page pointers into the pinned Pack arena. Eviction is
 * one of the two things that ends that guarantee, so it must cancel and JOIN the
 * job before releasing the pin -- otherwise the encoder reads freed pages. A
 * budget of 0 makes the eviction happen in the same call that queued the job, so
 * the race is exercised at its tightest. */
void test_in_flight_compression_is_discarded_safely_on_eviction(void) {
    fake_pin doomed;
    fake_pin winner;
    /* Big enough that the encode is real work rather than an instant return. */
    fake_pin_make(&doomed, "sprite-a", 2, 512, false, 51U);
    fake_pin_make(&winner, "sprite-b", 1, 64, true, 52U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(0U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &doomed));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &winner));

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, id_of(1U)));
    TEST_ASSERT_EQUAL_UINT64(1U, st.evicted);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        1U, st.encode_discarded,
        "the eviction cancelled and joined the encode it was racing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, doomed.releases,
        "and only released the pin once the encoder had let the pages go");
    TEST_ASSERT_EQUAL_INT(0, st.encoding);

    /* forget() is the other eviction-shaped path onto a live job. */
    fake_pin forgotten;
    fake_pin_make(&forgotten, "sprite-c", 2, 512, false, 53U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          store_pin(cache, id_of(3U), 3U, &forgotten));
    fake_pin third;
    fake_pin_make(&third, "sprite-d", 1, 64, true, 54U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(4U), 4U, &third));
    tp_pack_result_cache_forget(cache, id_of(3U));
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, forgotten.releases);
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, id_of(3U)));

    /* The store is unharmed by either discard. */
    tp_id128 h = tp_id128_nil();
    (void)authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(4U)));
    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&doomed);
    fake_pin_free(&winner);
    fake_pin_free(&forgotten);
    fake_pin_free(&third);
}

/* Re-activation is the other end of the guarantee: the entry is going back to
 * being the live result, so the compression it was queued for is moot. It is
 * cancelled and joined (never left running against pages the promote is about to
 * hand out), the entry stays HOT, and its pin is untouched. */
void test_in_flight_compression_is_discarded_safely_on_reactivation(void) {
    fake_pin a;
    fake_pin b;
    fake_pin_make(&a, "sprite-a", 2, 512, true, 61U);
    fake_pin_make(&b, "sprite-b", 1, 64, true, 62U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &b));

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.encoding);

    tp_pack_result_cache_select(cache, id_of(1U));
    tp_id128 h = tp_id128_nil();
    const tp_result *r = authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    TEST_ASSERT_EQUAL_PTR_MESSAGE(
        a.result, r,
        "a re-activated HOT entry is still the caller's own result");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, a.releases,
                                  "re-activation must not release the pin");

    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64(1U, st.encode_discarded);
    TEST_ASSERT_EQUAL_INT(0, st.cold_entries);
    /* The one job still in flight belongs to B, which this promote demoted --
     * A's was cancelled and joined, not merely forgotten about. */
    TEST_ASSERT_EQUAL_INT(1, st.encoding);
    tp_pack_result_cache_settle(cache);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.cold_entries);
    TEST_ASSERT_EQUAL_INT(1, b.releases);
    TEST_ASSERT_EQUAL_INT(0, a.releases);

    /* Switching away from A again queues a fresh compression, which lands
     * normally: the discard cost the work, never the entry. */
    tp_pack_result_cache_select(cache, id_of(2U));
    (void)authoritative(cache, &h);
    tp_pack_result_cache_settle(cache);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(2, st.cold_entries);
    TEST_ASSERT_EQUAL_INT(1, a.releases);
    tp_pack_result_cache_select(cache, id_of(1U));
    assert_round_trip(&a, authoritative(cache, &h));

    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&a);
    fake_pin_free(&b);
}

/* ---- T5 ------------------------------------------------------------------ */

void test_selection_by_sequence_ignores_store_order(void) {
    fake_pin newer; /* higher sequence (newer request) */
    fake_pin older; /* lower sequence (earlier request) */
    fake_pin_make(&newer, "sprite-a", 1, 32, true, 71U);
    fake_pin_make(&older, "sprite-a", 1, 32, true, 72U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    /* The NEWER request (seq 2) completes/stores FIRST; the OLDER request
     * (seq 1) completes LATE and stores second. The older completion must NOT
     * become authoritative. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &newer));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &older));

    tp_id128 h = tp_id128_nil();
    uint64_t seq = 0U;
    const tp_result *r = NULL;
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &h, &r, &seq, &e));
    TEST_ASSERT_TRUE_MESSAGE(tp_id128_eq(h, id_of(2U)),
                             "newer sequence must win regardless of store order");
    TEST_ASSERT_EQUAL_UINT64(2U, seq);
    assert_round_trip(&newer, r);

    /* Explicit selection by hash overrides sequence; clearing reverts. */
    tp_pack_result_cache_select(cache, id_of(1U));
    (void)authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));

    tp_pack_result_cache_select(cache, tp_id128_nil());
    (void)authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(2U)));

    /* Selecting an absent hash clears the selection (revert to latest). */
    tp_pack_result_cache_select(cache, id_of(200U));
    (void)authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(2U)));
    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&newer);
    fake_pin_free(&older);
}

void test_cancellation_leaves_prior_authoritative(void) {
    fake_pin a;
    fake_pin_make(&a, "sprite-a", 1, 32, true, 81U);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &a));

    tp_id128 h = tp_id128_nil();
    (void)authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));

    /* A cancelled job publishes nothing to the cache, so the authoritative
     * result is unchanged. */
    (void)authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&a);
}

/* ---- S18: pin release paths ---------------------------------------------- */

/* Demotion alone is not a release: switching away only starts charging the entry
 * to the budget and queues its compression, and switching back before that
 * compression is applied hands the SAME result pointer back with no decode. */
void test_retained_pin_demotes_without_releasing_or_reinflating(void) {
    fake_pin a;
    fake_pin b;
    fake_pin_make(&a, "sprite-a", 1, 32, true, 91U);
    fake_pin_make(&b, "sprite-b", 1, 32, true, 92U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_NOT_NULL(cache);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &b));

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(2, st.entry_count);
    TEST_ASSERT_TRUE(tp_id128_eq(st.active_hash, id_of(2U)));
    TEST_ASSERT_EQUAL_UINT64(a.raw_bytes, st.inactive_bytes);
    TEST_ASSERT_EQUAL_INT(0, a.releases);
    TEST_ASSERT_EQUAL_INT(0, b.releases);

    tp_pack_result_cache_select(cache, id_of(1U));
    tp_id128 h = tp_id128_nil();
    const tp_result *r = authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    TEST_ASSERT_EQUAL_PTR(a.result, r);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_TRUE(tp_id128_eq(st.active_hash, id_of(1U)));
    TEST_ASSERT_EQUAL_UINT64(b.raw_bytes, st.inactive_bytes);
    TEST_ASSERT_EQUAL_UINT64(0U, st.dropped_corrupt);
    TEST_ASSERT_EQUAL_INT(0, a.releases);
    TEST_ASSERT_EQUAL_INT(0, b.releases);

    tp_pack_result_cache_destroy(cache);
    TEST_ASSERT_EQUAL_INT(1, a.releases);
    TEST_ASSERT_EQUAL_INT(1, b.releases);
    fake_pin_free(&a);
    fake_pin_free(&b);
}

/* Budget pressure evicts the least-recently-used inactive entry and releases its
 * owner EXACTLY once -- the release hook is the only path that destroys the Pack
 * arena behind a retained pin. */
void test_retained_pin_eviction_releases_the_owner_exactly_once(void) {
    fake_pin a;
    fake_pin b;
    fake_pin c;
    fake_pin_make(&a, "sprite-a", 1, 32, true, 101U);
    fake_pin_make(&b, "sprite-b", 1, 32, true, 102U);
    fake_pin_make(&c, "sprite-c", 1, 32, true, 103U);

    /* Budget == one raw entry, and nothing is pumped, so the accounting stays in
     * the raw units this case is expressed in. */
    tp_pack_result_cache *cache = tp_pack_result_cache_create(a.raw_bytes);
    TEST_ASSERT_NOT_NULL(cache);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &a));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &b));
    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64(0U, st.evicted);
    TEST_ASSERT_EQUAL_INT(0, a.releases);

    /* C becomes active and max-sequence, demoting B: A is now the LRU victim. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(3U), 3U, &c));
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_UINT64(1U, st.evicted);
    TEST_ASSERT_EQUAL_INT(2, st.entry_count);
    TEST_ASSERT_EQUAL_UINT64(b.raw_bytes, st.inactive_bytes);
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, id_of(1U)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, a.releases,
                                  "eviction releases the pin exactly once");
    TEST_ASSERT_EQUAL_INT(0, b.releases);
    TEST_ASSERT_EQUAL_INT(0, c.releases);

    tp_pack_result_cache_destroy(cache);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, a.releases,
                                  "destroy must not re-release an evicted pin");
    TEST_ASSERT_EQUAL_INT(1, b.releases);
    TEST_ASSERT_EQUAL_INT(1, c.releases);
    fake_pin_free(&a);
    fake_pin_free(&b);
    fake_pin_free(&c);
}

/* The other two release paths: re-storing a hash releases the pin it supersedes,
 * and forget() drops an entry outright. Neither may release twice or leak. */
void test_restore_and_forget_release_the_superseded_pin_once(void) {
    fake_pin old_pin;
    fake_pin new_pin;
    fake_pin_make(&old_pin, "sprite-a", 1, 32, true, 111U);
    fake_pin_make(&new_pin, "sprite-b", 1, 32, true, 112U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_NOT_NULL(cache);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          store_pin(cache, id_of(7U), 1U, &old_pin));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          store_pin(cache, id_of(7U), 2U, &new_pin));
    TEST_ASSERT_EQUAL_INT(1, old_pin.releases);
    TEST_ASSERT_EQUAL_INT(0, new_pin.releases);

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.entry_count);
    TEST_ASSERT_EQUAL_UINT64(0U, st.inactive_bytes);

    tp_pack_result_cache_forget(cache, id_of(7U));
    TEST_ASSERT_FALSE(tp_pack_result_cache_contains(cache, id_of(7U)));
    TEST_ASSERT_EQUAL_INT(1, new_pin.releases);
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(0, st.entry_count);
    TEST_ASSERT_FALSE(st.has_active);
    TEST_ASSERT_EQUAL_UINT64(0U, st.inactive_bytes);

    /* Forgetting an absent hash is a no-op, and the store stays usable. */
    tp_pack_result_cache_forget(cache, id_of(7U));
    TEST_ASSERT_EQUAL_INT(1, new_pin.releases);
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_NOT_FOUND,
        tp_pack_result_cache_authoritative(cache, NULL, NULL, NULL, &e));

    tp_pack_result_cache_destroy(cache);
    TEST_ASSERT_EQUAL_INT(1, old_pin.releases);
    TEST_ASSERT_EQUAL_INT(1, new_pin.releases);
    fake_pin_free(&old_pin);
    fake_pin_free(&new_pin);
}

/* A rejected retained store leaves the pin with the CALLER: it must not be
 * released, or the caller's own release would be the second one. */
void test_rejected_retained_store_leaves_the_pin_with_the_caller(void) {
    fake_pin pin;
    fake_pin_make(&pin, "sprite-a", 1, 32, true, 121U);
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack_result_cache_store_retained(
                              cache, id_of(1U), 1U, pin.result, 512U, &pin,
                              NULL, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_pack_result_cache_store_retained(
                              cache, id_of(1U), 1U, NULL, 512U, &pin,
                              fake_pin_release, &e));
    TEST_ASSERT_EQUAL_INT(0, pin.releases);
    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(0, st.entry_count);
    tp_pack_result_cache_destroy(cache);
    TEST_ASSERT_EQUAL_INT(0, pin.releases);
    fake_pin_free(&pin);
}

/* S28 successor to test_retained_and_serialized_entries_share_one_budget: the two
 * flavours the store used to mix are gone, and what mixes now is the two RESIDENCY
 * states of one flavour. A hot entry and a cold one live in one store under one
 * budget, each accounted in its own units, and forget() picks the right teardown
 * for each. */
void test_hot_and_cold_entries_share_one_budget(void) {
    fake_pin cold;
    fake_pin hot;
    fake_pin active;
    fake_pin_make(&cold, "sprite-a", 1, 64, true, 131U);
    fake_pin_make(&hot, "sprite-b", 1, 64, true, 132U);
    fake_pin_make(&active, "sprite-c", 1, 32, true, 133U);

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(1U), 1U, &cold));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &hot));
    tp_pack_result_cache_settle(cache); /* only `cold` has been demoted so far */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          store_pin(cache, id_of(3U), 3U, &active));
    /* `hot` is demoted now, and deliberately NOT pumped. */

    tp_pack_result_cache_stats st;
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(3, st.entry_count);
    TEST_ASSERT_EQUAL_INT(1, st.cold_entries);
    TEST_ASSERT_EQUAL_INT(1, st.encoding);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        st.cold_coded_bytes + st.cold_meta_bytes + hot.raw_bytes,
        st.inactive_bytes,
        "one budget, two units: compressed pages plus geometry for the cold "
        "entry, raw for the one whose compression has not landed");
    TEST_ASSERT_EQUAL_INT(1, cold.releases);
    TEST_ASSERT_EQUAL_INT(0, hot.releases);

    tp_pack_result_cache_forget(cache, id_of(2U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, hot.releases,
        "forgetting a hot entry with a live encode joins it, then releases");
    tp_pack_result_cache_forget(cache, id_of(1U));
    tp_pack_result_cache_stats_get(cache, &st);
    TEST_ASSERT_EQUAL_INT(1, st.entry_count);
    TEST_ASSERT_EQUAL_INT(0, st.cold_entries);
    TEST_ASSERT_EQUAL_UINT64(0U, st.inactive_bytes);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, cold.releases, "forgetting a cold entry must not re-release the pin "
                          "the cold transition already gave back");

    tp_pack_result_cache_destroy(cache);
    fake_pin_free(&cold);
    fake_pin_free(&hot);
    fake_pin_free(&active);
}

/* ---- S28: the cold copy against a REAL packed atlas ---------------------- */

typedef struct packed_atlas {
    tp_arena *arena;
    tp_result *result;
    int releases;
    uint8_t **origin;
    size_t *origin_sizes;
    uint64_t raw_bytes;
    char atlas_name[64];
    int sprite_count;
    int page_count;
} packed_atlas;

static void packed_atlas_release(void *owner) {
    packed_atlas *packed = (packed_atlas *)owner;
    packed->releases++;
    tp_arena_destroy(packed->arena);
    packed->arena = NULL;
    packed->result = NULL;
}

static void packed_atlas_free(packed_atlas *packed) {
    for (int i = 0; i < packed->page_count; ++i) {
        free(packed->origin[i]);
    }
    free(packed->origin);
    free(packed->origin_sizes);
    tp_arena_destroy(packed->arena);
    memset(packed, 0, sizeof *packed);
}

/* Packs one real atlas through tp_pack so the metadata deep copy meets real
 * geometry: builder-assigned names, hulls, index lists and page records. */
static void pack_one(const char *atlas_name, uint8_t salt, packed_atlas *out) {
    memset(out, 0, sizeof *out);
    uint8_t px[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; i++) {
        px[i * 4 + 0] = (uint8_t)((i / 16) + salt);
        px[i * 4 + 1] = (uint8_t)(salt * 3U);
        px[i * 4 + 2] = (uint8_t)(255U - salt);
        px[i * 4 + 3] = 255U;
    }
    tp_pack_sprite_desc d;
    memset(&d, 0, sizeof d);
    d.name = "sprite";
    d.rgba = px;
    d.w = 16;
    d.h = 16;
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

    out->arena = arena;
    out->result = r;
    out->page_count = r->page_count;
    out->sprite_count = r->sprite_count;
    (void)snprintf(out->atlas_name, sizeof out->atlas_name, "%s",
                   r->atlas_name ? r->atlas_name : "");
    out->origin = calloc((size_t)r->page_count, sizeof *out->origin);
    out->origin_sizes = calloc((size_t)r->page_count, sizeof *out->origin_sizes);
    TEST_ASSERT_NOT_NULL(out->origin);
    TEST_ASSERT_NOT_NULL(out->origin_sizes);
    for (int i = 0; i < r->page_count; ++i) {
        const size_t bytes =
            (size_t)r->pages[i].w * (size_t)r->pages[i].h * 4U;
        out->origin[i] = malloc(bytes);
        TEST_ASSERT_NOT_NULL(out->origin[i]);
        memcpy(out->origin[i], r->pages[i].rgba, bytes);
        out->origin_sizes[i] = bytes;
        out->raw_bytes += (uint64_t)bytes;
    }
}

void test_cold_copy_reproduces_a_real_packed_atlas(void) {
    packed_atlas real;
    fake_pin other;
    pack_one("coldreal", 3U, &real);
    fake_pin_make(&other, "sprite-b", 1, 32, true, 141U);

    /* Captured before the pin is adopted: the result dies with the arena. */
    const int sprite_count = real.sprite_count;
    const int page_count = real.page_count;
    char sprite_name[128];
    (void)snprintf(sprite_name, sizeof sprite_name, "%s",
                   real.result->sprites[0].name);
    const int vert_count = real.result->sprites[0].vert_count;
    const int index_count = real.result->sprites[0].index_count;
    const int source_w = real.result->sprites[0].sourceSize.w;
    const float ppu = real.result->pixels_per_unit;

    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    tp_error e = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_pack_result_cache_store_retained(
                              cache, id_of(1U), 1U, real.result,
                              real.raw_bytes, &real, packed_atlas_release, &e));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, id_of(2U), 2U, &other));
    tp_pack_result_cache_settle(cache);
    TEST_ASSERT_EQUAL_INT(1, real.releases);

    tp_pack_result_cache_select(cache, id_of(1U));
    tp_id128 h = tp_id128_nil();
    const tp_result *r = authoritative(cache, &h);
    TEST_ASSERT_TRUE(tp_id128_eq(h, id_of(1U)));
    TEST_ASSERT_EQUAL_STRING(real.atlas_name, r->atlas_name);
    TEST_ASSERT_EQUAL_INT(sprite_count, r->sprite_count);
    TEST_ASSERT_EQUAL_INT(page_count, r->page_count);
    TEST_ASSERT_EQUAL_STRING(sprite_name, r->sprites[0].name);
    TEST_ASSERT_EQUAL_INT(vert_count, r->sprites[0].vert_count);
    TEST_ASSERT_EQUAL_INT(index_count, r->sprites[0].index_count);
    TEST_ASSERT_EQUAL_INT(source_w, r->sprites[0].sourceSize.w);
    TEST_ASSERT_TRUE(r->pixels_per_unit == ppu);
    for (int i = 0; i < page_count; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(real.origin[i], r->pages[i].rgba,
                                 real.origin_sizes[i]);
    }

    tp_pack_result_cache_destroy(cache);
    TEST_ASSERT_EQUAL_INT(1, real.releases);
    packed_atlas_free(&real);
    fake_pin_free(&other);
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
    tp_pack_result_cache *cache = tp_pack_result_cache_create(1ULL << 30);
    tp_error e = {{0}};

    /* state 0 -> hash h0; "pack" and store a result under h0. */
    const tp_id128 h0 = current_hash(session, atlas, imgcache);
    fake_pin p0;
    fake_pin_make(&p0, "sprite-a", 1, 32, true, 151U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, h0, 1U, &p0));
    TEST_ASSERT_FALSE(tp_session_job_active(session));

    /* Rename -> state 1 -> hash h1 != h0; store under h1. */
    rename_atlas(session, "renamed1");
    const tp_id128 h1 = current_hash(session, atlas, imgcache);
    TEST_ASSERT_FALSE(tp_id128_eq(h0, h1));
    fake_pin p1;
    fake_pin_make(&p1, "sprite-b", 1, 32, true, 152U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, store_pin(cache, h1, 2U, &p1));
    /* The h0 entry is demoted and compressed -- a COLD hit must still probe as a
     * hit, or Undo would think a perfectly available preview was gone. */
    tp_pack_result_cache_settle(cache);

    tp_id128 ha = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_result_cache_authoritative(
                                            cache, &ha, NULL, NULL, &e));
    TEST_ASSERT_TRUE(tp_id128_eq(ha, h1));

    /* UNDO -> back to state 0; recompute + PROBE = HIT. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_undo(session, &e));
    const tp_id128 hcur0 = current_hash(session, atlas, imgcache);
    TEST_ASSERT_TRUE(tp_id128_eq(hcur0, h0));
    TEST_ASSERT_TRUE_MESSAGE(tp_pack_result_cache_contains(cache, hcur0),
                             "a cold entry is present, so the probe hits");
    TEST_ASSERT_NOT_NULL_MESSAGE(tp_pack_result_cache_peek(cache, hcur0),
                                 "and a peek answers for it too");
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
    TEST_ASSERT_NULL(tp_pack_result_cache_peek(cache, h2));
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
    fake_pin_free(&p0);
    fake_pin_free(&p1);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    g_dir = (argc > 1) ? argv[1] : ".";
    UNITY_BEGIN();
    RUN_TEST(test_store_active_pin_and_authoritative);
    RUN_TEST(test_cold_entry_round_trips_pages_byte_identically);
    RUN_TEST(test_parallel_page_decode_matches_the_serial_result);
    RUN_TEST(test_inactive_budget_moves_from_raw_to_compressed_on_the_pump);
    RUN_TEST(test_exact_byte_accounting_and_eviction);
    RUN_TEST(test_zero_budget_keeps_max_sequence_resolvable);
    RUN_TEST(test_corrupt_cold_entry_is_contained);
    RUN_TEST(test_incompressible_entry_is_evicted_instead_of_kept_cold);
    RUN_TEST(test_ratio_floor_never_evicts_the_max_sequence_entry);
    RUN_TEST(test_in_flight_compression_is_discarded_safely_on_eviction);
    RUN_TEST(test_in_flight_compression_is_discarded_safely_on_reactivation);
    RUN_TEST(test_selection_by_sequence_ignores_store_order);
    RUN_TEST(test_cancellation_leaves_prior_authoritative);
    RUN_TEST(test_retained_pin_demotes_without_releasing_or_reinflating);
    RUN_TEST(test_retained_pin_eviction_releases_the_owner_exactly_once);
    RUN_TEST(test_restore_and_forget_release_the_superseded_pin_once);
    RUN_TEST(test_rejected_retained_store_leaves_the_pin_with_the_caller);
    RUN_TEST(test_hot_and_cold_entries_share_one_budget);
    RUN_TEST(test_cold_copy_reproduces_a_real_packed_atlas);
    RUN_TEST(test_undo_redo_cache_probe_never_autopacks);
    return UNITY_END();
}
