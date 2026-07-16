/* Fixed-capacity binary transaction-id retention with deterministic FIFO
 * eviction and an independent open-address lookup index. */

#include "tp_idset_internal.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(TP_TXN_RETAINED_ID_CAP > 0, "retention capacity must be positive");
_Static_assert(TP_TXN_RETAINED_ID_CAP <= UINT16_MAX,
               "ring indexes must fit the open-address slot representation");
_Static_assert((TP_IDSET_TABLE_CAP & (TP_IDSET_TABLE_CAP - 1)) == 0,
               "open-address table capacity must be a power of two");

static _Thread_local int g_test_forced_bucket = -1;
static _Thread_local bool g_test_count_probes = false;
static _Thread_local size_t g_test_probe_count = 0U;

void tp_idset__test_force_bucket(int bucket) {
    g_test_forced_bucket = bucket < 0 ? -1 : bucket & (TP_IDSET_TABLE_CAP - 1);
}

void tp_idset__test_probe_reset(void) {
    g_test_probe_count = 0U;
    g_test_count_probes = true;
}

size_t tp_idset__test_probe_take(void) {
    const size_t probes = g_test_probe_count;
    g_test_count_probes = false;
    return probes;
}

static uint16_t inspect_slot(const tp_idset *s, size_t slot) {
    if (g_test_count_probes) {
        g_test_probe_count++;
    }
    return s->slots[slot];
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool parse_hex32(const char *text, tp_id128 *out) {
    if (!text || !out) {
        return false;
    }
    for (int i = 0; i < TP_IDSET_IDLEN; ++i) {
        if (text[i] == '\0') {
            return false;
        }
    }
    if (text[TP_IDSET_IDLEN] != '\0') {
        return false;
    }
    tp_id128 id = {{0}};
    for (int i = 0; i < 16; ++i) {
        const int hi = hex_nibble(text[i * 2]);
        const int lo = hex_nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        id.bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    *out = id;
    return true;
}

static size_t hash_slot(tp_id128 id) {
    if (g_test_forced_bucket >= 0) {
        return (size_t)g_test_forced_bucket;
    }
    return (size_t)(tp_id128_bucket(id) & (uint64_t)(TP_IDSET_TABLE_CAP - 1));
}

static int find_table_slot(const tp_idset *s, tp_id128 id) {
    if (!s || !s->slots || !s->order) {
        return -1;
    }
    size_t slot = hash_slot(id);
    for (int probe = 0; probe < TP_IDSET_TABLE_CAP; ++probe) {
        const uint16_t entry = inspect_slot(s, slot);
        if (entry == 0U) {
            return -1;
        }
        if (tp_id128_eq(s->order[(int)entry - 1], id)) {
            return (int)slot;
        }
        slot = (slot + 1U) & (size_t)(TP_IDSET_TABLE_CAP - 1);
    }
    return -1;
}

static void insert_ring_index(tp_idset *s, int ring_index) {
    size_t slot = hash_slot(s->order[ring_index]);
    while (inspect_slot(s, slot) != 0U) {
        slot = (slot + 1U) & (size_t)(TP_IDSET_TABLE_CAP - 1);
    }
    s->slots[slot] = (uint16_t)(ring_index + 1);
}

static void remove_id(tp_idset *s, tp_id128 id) {
    int found = find_table_slot(s, id);
    if (found < 0) {
        return;
    }
    const size_t mask = (size_t)(TP_IDSET_TABLE_CAP - 1);
    size_t hole = (size_t)found;
    size_t next = (hole + 1U) & mask;
    uint16_t entry = 0U;
    while ((entry = inspect_slot(s, next)) != 0U) {
        const int ring_index = (int)entry - 1;
        const size_t home = hash_slot(s->order[ring_index]);
        const size_t next_distance = (next - home) & mask;
        const size_t hole_distance = (hole - home) & mask;
        /* Move an entry backward exactly when the hole lies on its wrap-safe
         * linear-probe path. Each cluster slot is inspected once. */
        if (hole_distance < next_distance) {
            s->slots[hole] = entry;
            hole = next;
        }
        next = (next + 1U) & mask;
    }
    s->slots[hole] = 0U;
}

tp_status tp_idset_reserve(tp_idset *s) {
    if (!s) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (s->order && s->slots) {
        return TP_STATUS_OK;
    }
    tp_id128 *order = (tp_id128 *)calloc((size_t)TP_TXN_RETAINED_ID_CAP, sizeof *order);
    uint16_t *slots = (uint16_t *)calloc((size_t)TP_IDSET_TABLE_CAP, sizeof *slots);
    if (!order || !slots) {
        free(order);
        free(slots);
        return TP_STATUS_OOM;
    }
    s->order = order;
    s->slots = slots;
    return TP_STATUS_OK;
}

bool tp_idset_contains(const tp_idset *s, const char *id_hex) {
    tp_id128 id;
    return parse_hex32(id_hex, &id) && find_table_slot(s, id) >= 0;
}

bool tp_idset_valid_hex(const char *id_hex) {
    tp_id128 id;
    return parse_hex32(id_hex, &id);
}

void tp_idset_put_reserved(tp_idset *s, const char *id_hex) {
    tp_id128 id;
    if (!s || !s->order || !s->slots || !parse_hex32(id_hex, &id) || find_table_slot(s, id) >= 0) {
        return;
    }

    int ring_index;
    if (s->count < TP_TXN_RETAINED_ID_CAP) {
        ring_index = (s->head + s->count) % TP_TXN_RETAINED_ID_CAP;
        s->count++;
    } else {
        ring_index = s->head;
        remove_id(s, s->order[ring_index]);
        s->head = (s->head + 1) % TP_TXN_RETAINED_ID_CAP;
    }
    s->order[ring_index] = id;
    insert_ring_index(s, ring_index);
}

tp_status tp_idset_add(tp_idset *s, const char *id_hex) {
    tp_id128 id;
    if (!s || !parse_hex32(id_hex, &id)) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (find_table_slot(s, id) >= 0) {
        return TP_STATUS_OK;
    }
    tp_status status = tp_idset_reserve(s);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_idset_put_reserved(s, id_hex);
    return TP_STATUS_OK;
}

int tp_idset_count(const tp_idset *s) { return s ? s->count : 0; }

bool tp_idset_at(const tp_idset *s, int index, tp_id128 *out) {
    if (!s || !out || index < 0 || index >= s->count) {
        return false;
    }
    *out = s->order[(s->head + index) % TP_TXN_RETAINED_ID_CAP];
    return true;
}

bool tp_idset_format_at(const tp_idset *s, int index, char out[TP_IDSET_IDLEN + 1]) {
    static const char digits[] = "0123456789abcdef";
    tp_id128 id;
    if (!out || !tp_idset_at(s, index, &id)) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = digits[id.bytes[i] >> 4];
        out[i * 2 + 1] = digits[id.bytes[i] & 0x0fU];
    }
    out[TP_IDSET_IDLEN] = '\0';
    return true;
}

void tp_idset_reset(tp_idset *s) {
    if (!s) {
        return;
    }
    if (s->slots) {
        memset(s->slots, 0, (size_t)TP_IDSET_TABLE_CAP * sizeof *s->slots);
    }
    s->count = 0;
    s->head = 0;
}

void tp_idset_dispose(tp_idset *s) {
    if (!s) {
        return;
    }
    free(s->order);
    free(s->slots);
    memset(s, 0, sizeof *s);
}
