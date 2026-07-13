/*
 * The shared 32-hex idempotency id-set (see tp_idset_internal.h). One place for the
 * membership + growth logic that the idstore and the recovery journal both need.
 */

#include "tp_idset_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool tp_idset_contains(const tp_idset *s, const char *id_hex) {
    if (!s || !id_hex) {
        return false;
    }
    /* O(n) linear scan; matches the idstore's existing O(n) trait and is bounded by
     * compaction (deferred to the F2-05 perf pass, ADR 0013 §D2). A hash-set is not
     * warranted here -- do not add speculative index infrastructure. */
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->ids[i], id_hex) == 0) {
            return true;
        }
    }
    return false;
}

tp_status tp_idset_reserve(tp_idset *s) {
    if (s->count < s->cap) {
        return TP_STATUS_OK;
    }
    int ncap = (s->cap == 0) ? 16 : (s->cap * 2);
    char(*n)[TP_IDSET_IDLEN + 1] = (char(*)[TP_IDSET_IDLEN + 1])realloc(s->ids, (size_t)ncap * sizeof(*n));
    if (!n) {
        return TP_STATUS_OOM;
    }
    s->ids = n;
    s->cap = ncap;
    return TP_STATUS_OK;
}

void tp_idset_put_reserved(tp_idset *s, const char *id_hex) {
    (void)snprintf(s->ids[s->count], sizeof s->ids[s->count], "%s", id_hex);
    s->count++;
}

tp_status tp_idset_add(tp_idset *s, const char *id_hex) {
    if (tp_idset_contains(s, id_hex)) {
        return TP_STATUS_OK; /* already present: an idempotent no-op */
    }
    tp_status rs = tp_idset_reserve(s);
    if (rs != TP_STATUS_OK) {
        return rs;
    }
    tp_idset_put_reserved(s, id_hex);
    return TP_STATUS_OK;
}

int tp_idset_count(const tp_idset *s) { return s ? s->count : 0; }

const char *tp_idset_at(const tp_idset *s, int index) {
    if (!s || index < 0 || index >= s->count) {
        return NULL;
    }
    return s->ids[index];
}

void tp_idset_reset(tp_idset *s) {
    if (s) {
        s->count = 0;
    }
}

void tp_idset_dispose(tp_idset *s) {
    if (!s) {
        return;
    }
    free(s->ids);
    s->ids = NULL;
    s->count = 0;
    s->cap = 0;
}
