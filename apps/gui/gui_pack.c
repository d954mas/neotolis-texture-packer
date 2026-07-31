#define NTPACKER_GUI_PACK_IMPLEMENTATION
#include "gui_pack_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log/nt_log.h"

#include "tp_core/tp_input.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_pack_result_cache.h"

#include "gui_project.h"

#define GUI_PACK_MAX_ATLASES 64

/* Inactive-result byte budget. docs/architecture/jobs-pack-and-cache.md fixes the
 * SHAPE -- one pinned active result, every inactive one in a separate byte-budget
 * LRU -- and leaves "concrete budgets and compression details" to implementation
 * policy, so this constant is that policy and the only place it is written down.
 *
 * 512 MiB. Since S28 the store keeps inactive results as COMPRESSED page blobs
 * (raw deflate, level 6), so this number buys far more atlases than the 256 MiB
 * it replaces bought in raw pages. Measured (packer/tests/tp_bench_pack_blob.c,
 * 2026-07-29, 4096x4096 fixtures): real CC0 sprite art compresses 114x, so 512
 * MiB of cold entries stands for ~57 GiB of raw pages -- far more atlases than
 * any project has. The number that actually sizes this constant is the
 * PESSIMISTIC one: deliberately incompressible content measures 1.24x, i.e. below
 * the store's 1.5x ratio floor, so such an entry is evicted rather than kept, and
 * anything that DOES clear the floor costs at most 2/3 of its raw pages. The real
 * worst case is therefore ~768 MiB of raw pages held as 512 MiB of blobs, and the
 * common case is "every atlas you have ever packed this session, still there".
 *
 * The budget is also not the peak. A just-demoted result stays RAW until its
 * background compression lands (the store counts it raw until then), so the true
 * ceiling is budget + the active pin + the highest-sequence entry + whatever is
 * still encoding; the active and max-sequence entries are exempt from eviction by
 * the store contract. */
#define GUI_PACK_RESULT_BUDGET_BYTES (512ULL * 1024ULL * 1024ULL)

typedef struct pack_ref_entry {
    uint64_t hash;
    const char *name;
    int result_index;
    bool occupied;
} pack_ref_entry;

/* Presentation slot for one atlas. It owns no result memory: the result lives in
 * the byte-budget store under `cache_key`, pinned there by the session Pack
 * receipt, and the slot is only the atlas -> cache-key binding plus the version
 * consumers watch. A slot whose entry has been evicted reads as "not packed",
 * which is the §10.4 cache-miss presentation (existing preview out of date; the
 * user runs Pack explicitly, nothing auto-packs). */
typedef struct {
    tp_id128 atlas_id;
    tp_id128 cache_key;
    uint64_t version;
    bool valid;
} pack_slot;

static pack_slot s_slots[GUI_PACK_MAX_ATLASES];
static uint64_t s_next_result_version;

/* The store, and the ONE decompressed result the presentation borrows from it:
 * its active pin. The canonical sprite index follows that one result, so an
 * atlas switch rebuilds it once instead of every atlas carrying its own. */
static tp_pack_result_cache *s_cache;
static uint64_t s_pack_sequence;
static tp_id128 s_resident_key;
static const tp_result *s_resident_result;
static pack_ref_entry *s_ref_index;
static size_t s_ref_index_cap;

#ifdef NTPACKER_GUI_SELFTEST
static gui_pack_ref_index_work s_ref_index_work;

void gui_pack_ref_index_work_reset(void) {
    memset(&s_ref_index_work, 0, sizeof s_ref_index_work);
}

gui_pack_ref_index_work gui_pack_ref_index_work_get(void) {
    return s_ref_index_work;
}
#endif

#ifdef TP_ENABLE_TEST_SEAMS
static uint64_t s_budget_override;
static bool s_fail_next_ref_index_build;

void gui_pack__test_result_cache_stats(tp_pack_result_cache_stats *out) {
    tp_pack_result_cache_stats_get(s_cache, out);
}

void gui_pack__test_result_cache_settle(void) {
    tp_pack_result_cache_settle(s_cache);
}

void gui_pack__test_fail_next_ref_index_build(void) {
    s_fail_next_ref_index_build = true;
}

void gui_pack__test_set_result_budget(uint64_t byte_budget) {
    gui_pack_clear(tp_id128_nil());
    s_budget_override = byte_budget;
    /* Shutdown funnels through here between cases, so an armed one-shot can
     * never leak into the next case. */
    s_fail_next_ref_index_build = false;
}
#endif

/* FNV-1a/64 over one NUL-terminated field. Process-local bucket index only:
 * every hit is confirmed by a full strcmp, so the value is never an identity. */
static uint64_t pack_ref_hash(const char *text) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        hash ^= (uint64_t)*p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void pack_resident_clear(void) {
    free(s_ref_index);
    s_ref_index = NULL;
    s_ref_index_cap = 0U;
    s_resident_key = tp_id128_nil();
    s_resident_result = NULL;
}

static bool pack_ref_index_build(const tp_result *result,
                                 pack_ref_entry **out_entries,
                                 size_t *out_capacity) {
    *out_entries = NULL;
    *out_capacity = 0U;
    if (!result || result->sprite_count <= 0) {
        return true;
    }
    const size_t count = (size_t)result->sprite_count;
    if (count > SIZE_MAX / 2U) {
        return false;
    }
    const size_t wanted = count * 2U;
    size_t capacity = 8U;
    while (capacity < wanted) {
        if (capacity > SIZE_MAX / 2U) {
            return false;
        }
        capacity *= 2U;
    }
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_fail_next_ref_index_build) {
        s_fail_next_ref_index_build = false;
        return false;
    }
#endif
    pack_ref_entry *entries = calloc(capacity, sizeof *entries);
    if (!entries) {
        return false;
    }
    const size_t mask = capacity - 1U;
    for (int i = 0; i < result->sprite_count; ++i) {
        const char *name = result->sprites[i].name;
        if (!name) {
            free(entries);
            return false;
        }
        const uint64_t hash = pack_ref_hash(name);
        size_t at = (size_t)hash & mask;
        while (entries[at].occupied) {
#ifdef NTPACKER_GUI_SELFTEST
            s_ref_index_work.build_probes++;
#endif
            at = (at + 1U) & mask;
        }
#ifdef NTPACKER_GUI_SELFTEST
        s_ref_index_work.build_items++;
        s_ref_index_work.build_probes++;
#endif
        entries[at] = (pack_ref_entry){hash, name, i, true};
    }
    *out_entries = entries;
    *out_capacity = capacity;
    return true;
}

/* Releases the session Pack receipt the store pinned for us. This is the ONLY
 * release path for a published native result, so the receipt -- and with it the
 * Pack arena -- is destroyed exactly once, by whichever of eviction, re-store,
 * forget, or store destruction reaches it first. */
static void pack_receipt_release(void *owner) {
    tp_session_job_result owned = {0};
    owned._owner = (tp_session_job_result_handle *)owner;
    tp_session_job_result_destroy(&owned);
}

/* What one pinned result costs: its raw RGBA8 pages. Metadata/name bytes are not
 * counted, exactly as the store does not count its own name lists, so the budget
 * stays an honest sum of the memory that actually matters. */
static uint64_t result_page_bytes(const tp_result *result) {
    uint64_t bytes = 0U;
    for (int i = 0; result && i < result->page_count; ++i) {
        const tp_page *page = &result->pages[i];
        if (page->w > 0 && page->h > 0) {
            bytes += (uint64_t)page->w * (uint64_t)page->h * 4ULL;
        }
    }
    return bytes;
}

/* Per-frame tick for the store's background page compression. The
 * store is single-threaded and is pumped only from here, on the UI thread that
 * owns it; nothing else in the GUI knows a compression exists. */
void gui_pack__cold_pump(void) {
    tp_pack_result_cache_pump(s_cache);
}

static tp_pack_result_cache *pack_cache(void) {
    if (!s_cache) {
        uint64_t budget = GUI_PACK_RESULT_BUDGET_BYTES;
#ifdef TP_ENABLE_TEST_SEAMS
        if (s_budget_override != 0U) {
            budget = s_budget_override;
        }
#endif
        s_cache = tp_pack_result_cache_create(budget);
    }
    return s_cache;
}

/* The store is keyed by pack_input_hash, which is content-addressed: two atlases
 * whose packed result would be byte-identical legitimately share one entry. They
 * may then also share its lifetime, so a slot only forgets an entry no OTHER
 * live slot is still bound to. */
static bool pack_key_is_shared(const pack_slot *slot) {
    for (int i = 0; i < GUI_PACK_MAX_ATLASES; ++i) {
        const pack_slot *other = &s_slots[i];
        if (other != slot && other->valid &&
            tp_id128_eq(other->cache_key, slot->cache_key)) {
            return true;
        }
    }
    return false;
}

/* Drops the slot's binding AND the store entry behind it. Used when the atlas'
 * result must be gone for good (project new/open, atlas removed, explicit
 * clear); an ordinary atlas switch demotes instead and keeps the entry. */
static void pack_slot_clear(pack_slot *slot) {
    if (slot->valid && s_cache && !pack_key_is_shared(slot)) {
        if (tp_id128_eq(slot->cache_key, s_resident_key)) {
            pack_resident_clear();
        }
        tp_pack_result_cache_forget(s_cache, slot->cache_key);
    }
    memset(slot, 0, sizeof *slot);
}

static uint64_t next_result_version(void) {
    s_next_result_version++;
    if (s_next_result_version == 0U) {
        s_next_result_version = 1U;
    }
    return s_next_result_version;
}

static pack_slot *pack_slot_for_atlas_id(tp_id128 atlas_id) {
    for (int i = 0; i < GUI_PACK_MAX_ATLASES; ++i) {
        if (s_slots[i].valid && tp_id128_eq(s_slots[i].atlas_id, atlas_id)) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static bool snapshot_has_atlas(const tp_session_snapshot *snapshot,
                               tp_id128 atlas_id) {
    const int count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int i = 0; i < count; ++i) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, i);
        if (atlas && tp_id128_eq(atlas->id, atlas_id)) {
            return true;
        }
    }
    return false;
}

static pack_slot *pack_slot_for_publish(tp_id128 atlas_id) {
    pack_slot *slot = pack_slot_for_atlas_id(atlas_id);
    if (slot) {
        return slot;
    }
    for (int i = 0; i < GUI_PACK_MAX_ATLASES; ++i) {
        if (!s_slots[i].valid) {
            return &s_slots[i];
        }
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    for (int i = 0; i < GUI_PACK_MAX_ATLASES; ++i) {
        if (!snapshot_has_atlas(snapshot, s_slots[i].atlas_id)) {
            return &s_slots[i];
        }
    }
    return NULL;
}

bool gui_pack_publish_native(tp_session_job_result *job_result,
                             double elapsed_ms, gui_pack_result_info *out) {
    if (!job_result || job_result->kind != TP_SESSION_JOB_PACK ||
        !job_result->_owner || !job_result->pack.result) {
        if (out) {
            out->status = TP_STATUS_INVALID_ARGUMENT;
            (void)snprintf(
                out->err, sizeof out->err,
                "Pack result publication requires a retained owner.");
        }
        return false;
    }
    tp_session_pack_job_result *pack = &job_result->pack;
    /* The receipt the store is about to pin retains the whole live-job
     * wrapper. Only the arena behind `pack->result` has to survive the pin, so
     * the request-side baggage -- the serialized project JSON, the worker
     * process handle with its second encoded copy of that JSON, the request
     * paths -- is dropped NOW, before the entry starts a cache lifetime the
     * byte budget accounts only in page bytes. */
    tp_session_job_result_compact(job_result);
    tp_pack_result_cache *cache = pack_cache();
    if (!cache) {
        if (out) {
            out->status = TP_STATUS_OOM;
            (void)snprintf(out->err, sizeof out->err,
                           "out of memory creating the pack-result store");
        }
        return false;
    }
    pack_ref_entry *ref_index = NULL;
    size_t ref_index_cap = 0U;
    if (!pack_ref_index_build(pack->result, &ref_index, &ref_index_cap)) {
        if (out) {
            out->status = TP_STATUS_OOM;
            (void)snprintf(out->err, sizeof out->err,
                           "out of memory building canonical sprite lookup");
        }
        return false;
    }
    /* Keyed by the canonical pack_input_hash (§10.2-10.3), which is what makes a
     * later Undo/Redo probe find this exact result. A nil hash means the core
     * could not compute one (unreadable source): it must never become a shared
     * key, and it never matches a current hash anyway, so such a result is filed
     * under the atlas' own stable ID instead. */
    const tp_id128 key = tp_id128_is_nil(pack->pack_input_hash)
                             ? pack->atlas_id
                             : pack->pack_input_hash;
    /* Resolve the slot BEFORE storing: recycling a slot whose atlas is gone must
     * release that atlas' entry, and doing it after the store would clear the
     * residency this publication is about to install. A slot reused for the SAME
     * atlas keeps its previous entry on purpose -- an earlier hash of this atlas
     * is exactly what an Undo/Redo probe looks for, and the LRU bounds it. */
    pack_slot *slot = pack_slot_for_publish(pack->atlas_id);
    if (!slot) {
        free(ref_index);
        if (out) {
            out->status = TP_STATUS_OUT_OF_BOUNDS;
            (void)snprintf(out->err, sizeof out->err,
                           "pack-result atlas slot capacity is exhausted");
        }
        return false;
    }
    if (slot->valid && !tp_id128_eq(slot->atlas_id, pack->atlas_id)) {
        pack_slot_clear(slot);
    }
    tp_result *result = pack->result;
    tp_error error = {{0}};
    if (tp_pack_result_cache_store_retained(
            cache, key, ++s_pack_sequence, result, result_page_bytes(result),
            job_result->_owner, pack_receipt_release, &error) != TP_STATUS_OK) {
        free(ref_index);
        if (out) {
            out->status = TP_STATUS_OOM;
            (void)snprintf(out->err, sizeof out->err, "%s", error.msg);
        }
        return false; /* store failed -> the caller still owns the receipt */
    }
    /* The receipt now belongs to the store, which pinned this result ACTIVE and
     * demoted whatever was active before into the byte-budget LRU. */
    job_result->_owner = NULL;
    pack->arena = NULL;
    pack->result = NULL;

    pack_resident_clear();
    s_ref_index = ref_index;
    s_ref_index_cap = ref_index_cap;
    s_resident_key = key;
    s_resident_result = result;
    slot->atlas_id = pack->atlas_id;
    slot->cache_key = key;
    slot->version = next_result_version();
    slot->valid = true;
    nt_log_info("gui_pack(async): atlas '%s' packed %d sprite(s), %d page(s) in %.1f ms",
                result->atlas_name, result->sprite_count, result->page_count,
                elapsed_ms);
    return true;
}

/* Makes the slot's cached result the resident (pinned) one and returns it.
 *
 * This is the atlas-switch path: selecting another atlas' entry demotes the
 * outgoing one into the byte-budget LRU instead of dropping it, so switching
 * back is a store hit and never a repack. A miss means the LRU evicted the entry
 * under budget pressure -- the slot is retired and the atlas honestly reads as
 * not packed (§10.4: on a miss the preview is out of date; nothing auto-packs). */
static const tp_result *pack_slot_reside(pack_slot *slot) {
    if (!slot || !slot->valid) {
        return NULL;
    }
    if (s_resident_result && tp_id128_eq(slot->cache_key, s_resident_key)) {
        return s_resident_result;
    }
    if (!s_cache || !tp_pack_result_cache_contains(s_cache, slot->cache_key)) {
        memset(slot, 0, sizeof *slot);
        return NULL;
    }
    /* The switch below can FREE the outgoing resident: authoritative() demotes
     * it into the byte-budget LRU, which may evict it and release its pin --
     * and the Pack arena the resident pointers describe dies with that pin.
     * The resident is therefore cleared BEFORE the store can evict, so every
     * exit below, success or failure, leaves either the new resident or none;
     * a stale resident would be handed out by the fast path above as freed
     * memory. */
    pack_resident_clear();
    tp_pack_result_cache_select(s_cache, slot->cache_key);
    const tp_result *result = NULL;
    tp_id128 hash = tp_id128_nil();
    tp_error error = {{0}};
    if (tp_pack_result_cache_authoritative(s_cache, &hash, &result, NULL,
                                           &error) != TP_STATUS_OK ||
        !result || !tp_id128_eq(hash, slot->cache_key)) {
        memset(slot, 0, sizeof *slot);
        return NULL;
    }
    pack_ref_entry *ref_index = NULL;
    size_t ref_index_cap = 0U;
    if (!pack_ref_index_build(result, &ref_index, &ref_index_cap)) {
        return NULL; /* the entry stays cached; the lookup index is retried */
    }
    s_ref_index = ref_index;
    s_ref_index_cap = ref_index_cap;
    s_resident_key = slot->cache_key;
    s_resident_result = result;
    return result;
}

void gui_pack_clear(tp_id128 atlas_id) {
    if (!tp_id128_is_nil(atlas_id)) {
        pack_slot *slot = pack_slot_for_atlas_id(atlas_id);
        if (slot) {
            pack_slot_clear(slot);
        }
    } else {
        /* Whole-store reset (project new/open, shutdown): destroying the store
         * releases every pinned receipt, including entries whose slot was
         * already recycled, so nothing survives a project boundary. */
        memset(s_slots, 0, sizeof s_slots);
        pack_resident_clear();
        tp_pack_result_cache_destroy(s_cache);
        s_cache = NULL;
    }
    if (tp_id128_is_nil(atlas_id) ||
        gui_pack_preview_belongs_to(atlas_id)) {
        gui_pack_preview_clear();
    }
}

const tp_result *gui_pack_result(tp_id128 atlas_id) {
    return pack_slot_reside(pack_slot_for_atlas_id(atlas_id));
}

const tp_result *gui_pack_result_peek(tp_id128 atlas_id) {
    const pack_slot *slot = pack_slot_for_atlas_id(atlas_id);
    if (!slot || !slot->valid) {
        return NULL;
    }
    /* The resident IS the store's active entry, so the fast path returns the same
     * pointer without the table walk. Neither path mutates anything -- and a miss
     * deliberately does NOT retire the slot, because retiring is a residency
     * decision and this is only a read (gui_pack_result_version already reports an
     * evicted entry as version 0). */
    if (s_resident_result && tp_id128_eq(slot->cache_key, s_resident_key)) {
        return s_resident_result;
    }
    return tp_pack_result_cache_peek(s_cache, slot->cache_key);
}

uint64_t gui_pack_result_version(tp_id128 atlas_id) {
    const pack_slot *slot = pack_slot_for_atlas_id(atlas_id);
    /* A slot whose entry the LRU already evicted is only retired by the next
     * result read; its version must not outlive the entry it numbered. */
    if (!slot ||
        (s_cache && !tp_pack_result_cache_contains(s_cache, slot->cache_key))) {
        return 0U;
    }
    return slot->version;
}

bool gui_pack_sprite_matches_ref(tp_id128 atlas_id, int sprite_index,
                                 tp_id128 source_id,
                                 const char *source_key) {
    const tp_result *result = gui_pack_result(atlas_id);
    if (!result || sprite_index < 0 || sprite_index >= result->sprite_count ||
        tp_id128_is_nil(source_id) || !source_key) {
        return false;
    }
    char canonical_name[TP_PACK_INTERNAL_NAME_CAP];
    if (tp_pack_input_format_sprite_name(
            source_id, source_key, canonical_name, sizeof canonical_name,
            NULL) != TP_STATUS_OK) {
        return false;
    }
    const char *packed_name = result->sprites[sprite_index].name;
    return packed_name && strcmp(packed_name, canonical_name) == 0;
}

int gui_pack_find_sprite_ref_in_result(const tp_result *result,
                                       tp_id128 source_id,
                                       const char *source_key) {
    if (!result || tp_id128_is_nil(source_id) || !source_key) {
        return -1;
    }
    char canonical_name[TP_PACK_INTERNAL_NAME_CAP];
    if (tp_pack_input_format_sprite_name(
            source_id, source_key, canonical_name, sizeof canonical_name,
            NULL) != TP_STATUS_OK) {
        return -1;
    }
    for (int i = 0; i < result->sprite_count; ++i) {
        const char *packed_name = result->sprites[i].name;
        if (packed_name && strcmp(packed_name, canonical_name) == 0) {
            return i;
        }
    }
    return -1;
}

int gui_pack_find_sprite_ref(tp_id128 atlas_id, tp_id128 source_id,
                             const char *source_key) {
    if (tp_id128_is_nil(atlas_id)) {
        return -1;
    }
    /* The canonical index follows the ONE resident result, so making the atlas
     * resident is what binds this lookup to the right result. */
    if (!gui_pack_result(atlas_id)) {
        return -1;
    }
    char canonical_name[TP_PACK_INTERNAL_NAME_CAP];
    if (tp_pack_input_format_sprite_name(
            source_id, source_key, canonical_name, sizeof canonical_name,
            NULL) != TP_STATUS_OK) {
        return -1;
    }
#ifdef NTPACKER_GUI_SELFTEST
    s_ref_index_work.lookup_calls++;
#endif
    if (!s_ref_index || s_ref_index_cap == 0U) {
        return -1;
    }
    const uint64_t hash = pack_ref_hash(canonical_name);
    const size_t mask = s_ref_index_cap - 1U;
    size_t at = (size_t)hash & mask;
    for (size_t probe = 0U; probe < s_ref_index_cap; ++probe) {
        const pack_ref_entry *entry = &s_ref_index[at];
#ifdef NTPACKER_GUI_SELFTEST
        s_ref_index_work.lookup_probes++;
#endif
        if (!entry->occupied) {
            return -1;
        }
        if (entry->hash == hash && strcmp(entry->name, canonical_name) == 0) {
            return entry->result_index;
        }
        at = (at + 1U) & mask;
    }
    return -1;
}

#if defined(TP_ENABLE_TEST_SEAMS) || \
    defined(NTPACKER_GUI_SELFTEST)
static tp_id128 gui_pack_test_atlas_id(int atlas_index) {
    if (atlas_index < 0) {
        return tp_id128_nil();
    }
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot
            ? tp_session_snapshot_atlas_at(
                  snapshot, atlas_index)
            : NULL;
    return atlas ? atlas->id : tp_id128_nil();
}

bool gui_pack_atlas_at_index(
    int atlas_index, double *out_ms, char *err,
    size_t err_cap, char *notice, size_t notice_cap) {
    return gui_pack_atlas(
        gui_pack_test_atlas_id(atlas_index), out_ms,
        err, err_cap, notice, notice_cap);
}

const tp_result *gui_pack_result_at_index(int atlas_index) {
    return gui_pack_result(
        gui_pack_test_atlas_id(atlas_index));
}

const tp_result *gui_pack_result_peek_at_index(int atlas_index) {
    return gui_pack_result_peek(
        gui_pack_test_atlas_id(atlas_index));
}

uint64_t gui_pack_result_version_at_index(int atlas_index) {
    return gui_pack_result_version(
        gui_pack_test_atlas_id(atlas_index));
}

int gui_pack_find_sprite_ref_at_index(
    int atlas_index, tp_id128 source_id,
    const char *source_key) {
    return gui_pack_find_sprite_ref(
        gui_pack_test_atlas_id(atlas_index),
        source_id, source_key);
}

bool gui_pack_sprite_matches_ref_at_index(
    int atlas_index, int sprite_index,
    tp_id128 source_id, const char *source_key) {
    return gui_pack_sprite_matches_ref(
        gui_pack_test_atlas_id(atlas_index),
        sprite_index, source_id, source_key);
}

bool gui_pack_export_at_index(
    int atlas_index, int *out_targets, int *out_notices,
    char *err, size_t err_cap, char *notice,
    size_t notice_cap) {
    return gui_pack_export(
        gui_pack_test_atlas_id(atlas_index), out_targets,
        out_notices, err, err_cap, notice, notice_cap);
}

void gui_pack_clear_at_index(int atlas_index) {
    gui_pack_clear(gui_pack_test_atlas_id(atlas_index));
}

bool gui_pack_preview_blocking_at_index(
    int atlas_index, const char *exporter_id,
    char *err, size_t err_cap) {
    return gui_pack_preview_blocking(
        gui_pack_test_atlas_id(atlas_index),
        exporter_id, err, err_cap);
}

bool gui_pack_preview_async_start_at_index(
    int atlas_index, const char *exporter_id,
    char *err, size_t err_cap) {
    return gui_pack_preview_async_start(
        gui_pack_test_atlas_id(atlas_index),
        exporter_id, err, err_cap);
}

const tp_result *gui_pack_preview_result_at_index(
    int atlas_index) {
    return gui_pack_preview_result(
        gui_pack_test_atlas_id(atlas_index));
}

uint64_t gui_pack_preview_result_version_at_index(
    int atlas_index) {
    return gui_pack_preview_result_version(
        gui_pack_test_atlas_id(atlas_index));
}

int gui_pack_preview_diff_at_index(
    int atlas_index, const char *exporter_id,
    char *chip, size_t chip_cap, char *tip,
    size_t tip_cap) {
    return gui_pack_preview_diff(
        gui_pack_test_atlas_id(atlas_index),
        exporter_id, chip, chip_cap, tip, tip_cap);
}

bool gui_pack_async_start_at_index(
    int atlas_index, char *err, size_t err_cap) {
    return gui_pack_async_start(
        gui_pack_test_atlas_id(atlas_index),
        err, err_cap);
}
#endif
