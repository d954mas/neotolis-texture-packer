/*
 * The transaction-id idempotency retention store, in-memory default
 * behind the pluggable tp_txn_idstore interface (master spec §7.2). Re-submitting a
 * committed 32-hex transaction id is a duplicate the caller rejects (duplicate_id);
 * the on-disk journal can back the SAME interface later without touching the
 * apply core. Only COMMITTED ids are ever recorded, so idempotency blocks exactly
 * the retries of applied transactions.
 *
 * The set itself is the shared bounded binary tp_idset -- the SAME primitive the
 * journal rebuilds during recovery. Its fixed arrays are allocated with the model,
 * then every lookup/record/eviction is allocation-free.
 */

#include "tp_core/tp_transaction.h"

#include <stdlib.h>

#include "tp_idset_internal.h"

static bool mem_contains(void *ctx, const char *id_hex) { return tp_idset_contains((const tp_idset *)ctx, id_hex); }

static tp_status mem_record(void *ctx, const char *id_hex, tp_error *err) {
    tp_status st = tp_idset_add((tp_idset *)ctx, id_hex);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "idempotency retention insert failed");
    }
    return TP_STATUS_OK;
}

static void mem_destroy(void *ctx) {
    tp_idset *s = (tp_idset *)ctx;
    if (s) {
        tp_idset_dispose(s);
        free(s);
    }
}

/* The internal set behind a memory idstore (or NULL for a foreign
 * store), so tp_model_attach_journal can migrate ids committed BEFORE a journal was
 * attached into the fresh journal's retained-id index. Mirrors mem_of() in
 * tp_journal_io.c: recognized by the `contains` function pointer. */
const tp_idset *tp_txn_idstore_mem_view(const tp_txn_idstore *store) {
    return (store && store->contains == mem_contains) ? (const tp_idset *)store->ctx : NULL;
}

tp_txn_idstore *tp_txn_idstore_memory_create(void) {
    tp_txn_idstore *store = (tp_txn_idstore *)calloc(1, sizeof *store);
    tp_idset *s = (tp_idset *)calloc(1, sizeof *s);
    if (!store || !s) {
        free(store);
        free(s);
        return NULL;
    }
    if (tp_idset_reserve(s) != TP_STATUS_OK) {
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
