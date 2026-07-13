/*
 * F2-02 task 6: the transaction-id idempotency retention store, in-memory default
 * behind the pluggable tp_txn_idstore interface (master spec §7.2). Re-submitting a
 * committed 32-hex transaction id is a duplicate the caller rejects (duplicate_id);
 * F2-04's on-disk journal can back the SAME interface later without touching the
 * apply core. Only COMMITTED ids are ever recorded, so idempotency blocks exactly
 * the retries of applied transactions.
 *
 * `record` is transactional: it grows first and only appends on success, so an OOM
 * leaves the set unchanged and the caller discards the transaction's clone -- the
 * model stays byte-unchanged.
 */

#include "tp_core/tp_transaction.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char (*ids)[33]; /* growable array of 32-hex + NUL */
    int count;
    int cap;
} mem_idset;

static bool mem_contains(void *ctx, const char *id_hex) {
    mem_idset *s = (mem_idset *)ctx;
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->ids[i], id_hex) == 0) {
            return true;
        }
    }
    return false;
}

static tp_status mem_record(void *ctx, const char *id_hex, tp_error *err) {
    mem_idset *s = (mem_idset *)ctx;
    if (mem_contains(ctx, id_hex)) {
        return TP_STATUS_OK; /* already recorded: an idempotent no-op */
    }
    if (s->count >= s->cap) {
        int new_cap = (s->cap == 0) ? 16 : s->cap * 2;
        char(*n)[33] = (char(*)[33])realloc(s->ids, (size_t)new_cap * sizeof(*n));
        if (!n) {
            return tp_error_set(err, TP_STATUS_OOM, "idempotency set grow failed");
        }
        s->ids = n;
        s->cap = new_cap;
    }
    (void)snprintf(s->ids[s->count], sizeof s->ids[s->count], "%s", id_hex);
    s->count++;
    return TP_STATUS_OK;
}

static void mem_destroy(void *ctx) {
    mem_idset *s = (mem_idset *)ctx;
    if (s) {
        free(s->ids);
        free(s);
    }
}

tp_txn_idstore *tp_txn_idstore_memory_create(void) {
    tp_txn_idstore *store = (tp_txn_idstore *)calloc(1, sizeof *store);
    mem_idset *s = (mem_idset *)calloc(1, sizeof *s);
    if (!store || !s) {
        free(store);
        free(s);
        return NULL;
    }
    store->ctx = s;
    store->contains = mem_contains;
    store->record = mem_record;
    store->destroy = mem_destroy;
    return store;
}
