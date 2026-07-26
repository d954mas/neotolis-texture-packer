#include "gui_pack_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log/nt_log.h"

#include "tp_core/tp_arena.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_names.h"

#include "gui_project.h"

#define GUI_PACK_MAX_ATLASES 64

typedef struct pack_ref_entry {
    uint64_t hash;
    const char *name;
    int result_index;
    bool occupied;
} pack_ref_entry;

typedef struct {
    tp_arena *arena;
    tp_result *result;
    pack_ref_entry *ref_index;
    size_t ref_index_cap;
    tp_id128 atlas_id;
    uint64_t version;
    bool valid;
} pack_slot;

static pack_slot s_slots[GUI_PACK_MAX_ATLASES];
static uint64_t s_next_result_version;

#ifdef NTPACKER_GUI_SELFTEST
static gui_pack_ref_index_work s_ref_index_work;

void gui_pack_ref_index_work_reset(void) {
    memset(&s_ref_index_work, 0, sizeof s_ref_index_work);
}

gui_pack_ref_index_work gui_pack_ref_index_work_get(void) {
    return s_ref_index_work;
}
#endif

static uint64_t pack_ref_hash(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        hash ^= (uint64_t)*p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void pack_ref_index_free(pack_slot *slot) {
    free(slot->ref_index);
    slot->ref_index = NULL;
    slot->ref_index_cap = 0U;
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

static void pack_slot_clear(pack_slot *slot) {
    pack_ref_index_free(slot);
    if (slot->arena) {
        tp_arena_destroy(slot->arena);
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

static pack_slot *pack_slot_for_atlas_index(int atlas_index) {
    if (atlas_index < 0 || atlas_index >= GUI_PACK_MAX_ATLASES) {
        return NULL;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!atlas) {
        return NULL;
    }
    if (s_slots[atlas_index].valid &&
        tp_id128_eq(s_slots[atlas_index].atlas_id, atlas->id)) {
        return &s_slots[atlas_index];
    }
    return pack_slot_for_atlas_id(atlas->id);
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

static pack_slot *pack_slot_for_publish(int atlas_index, tp_id128 atlas_id) {
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
    return &s_slots[atlas_index];
}

bool gui_pack_publish_native(tp_session_pack_job_result *pack,
                             int atlas_index, double elapsed_ms,
                             gui_pack_result_info *out) {
    pack_ref_entry *ref_index = NULL;
    size_t ref_index_cap = 0U;
    if (!pack_ref_index_build(pack->result, &ref_index, &ref_index_cap)) {
        if (out) {
            (void)snprintf(out->err, sizeof out->err,
                           "out of memory building canonical sprite lookup");
        }
        return false;
    }
    pack_slot *slot = pack_slot_for_publish(atlas_index, pack->atlas_id);
    pack_slot_clear(slot);
    slot->arena = pack->arena;
    slot->result = pack->result;
    slot->ref_index = ref_index;
    slot->ref_index_cap = ref_index_cap;
    slot->atlas_id = pack->atlas_id;
    slot->version = next_result_version();
    slot->valid = true;
    pack->arena = NULL;
    nt_log_info("gui_pack(async): atlas '%s' packed %d sprite(s), %d page(s) in %.1f ms",
                slot->result->atlas_name, slot->result->sprite_count,
                slot->result->page_count, elapsed_ms);
    return true;
}

void gui_pack_clear(int atlas_index) {
    if (atlas_index >= 0) {
        pack_slot *slot = pack_slot_for_atlas_index(atlas_index);
        if (slot) {
            pack_slot_clear(slot);
        }
    } else {
        for (int i = 0; i < GUI_PACK_MAX_ATLASES; i++) {
            pack_slot_clear(&s_slots[i]);
        }
    }
    if (atlas_index < 0 || gui_pack_preview_belongs_to(atlas_index)) {
        gui_pack_preview_clear();
    }
}

const tp_result *gui_pack_result(int atlas_index) {
    const pack_slot *slot = pack_slot_for_atlas_index(atlas_index);
    return slot ? slot->result : NULL;
}

uint64_t gui_pack_result_version(int atlas_index) {
    const pack_slot *slot = pack_slot_for_atlas_index(atlas_index);
    return slot ? slot->version : 0U;
}

bool gui_pack_sprite_matches_ref(int atlas_index, int sprite_index,
                                 tp_id128 source_id,
                                 const char *source_key) {
    const tp_result *result = gui_pack_result(atlas_index);
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

int gui_pack_find_sprite_ref(int atlas_index, tp_id128 source_id,
                             const char *source_key) {
    if (atlas_index < 0 || atlas_index >= GUI_PACK_MAX_ATLASES) {
        return -1;
    }
    static const pack_slot *last_slot;
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, atlas_index) : NULL;
    if (!atlas) {
        return -1;
    }
    const pack_slot *slot = last_slot;
    if (!slot || !slot->valid || !tp_id128_eq(slot->atlas_id, atlas->id)) {
        slot = pack_slot_for_atlas_id(atlas->id);
        last_slot = slot;
    }
    if (!slot) {
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
    if (!slot->ref_index || slot->ref_index_cap == 0U) {
        return -1;
    }
    const uint64_t hash = pack_ref_hash(canonical_name);
    const size_t mask = slot->ref_index_cap - 1U;
    size_t at = (size_t)hash & mask;
    for (size_t probe = 0U; probe < slot->ref_index_cap; ++probe) {
        const pack_ref_entry *entry = &slot->ref_index[at];
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
