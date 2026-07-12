#ifndef TP_C0_PACK_SUPER_H
#define TP_C0_PACK_SUPER_H

/*
 * C0-03 task 5: Pack supersession / preview-selection policy as a pure state
 * machine. Encodes decision docs/decisions/0004-pack-supersession.md EXACTLY.
 *
 * Master spec §10.3 (a Pack job requested earlier must not silently overwrite a
 * newer active preview after the newer request completed; the user may select a
 * cached older result and Undo may make an older hash current again), §10.4
 * (memory cache; a cache miss marks the preview out of date, Undo/Redo never
 * auto-starts Pack), §59 item 24 (ownership transfer cancels only the running
 * Pack). Decision 0004: one running Pack job per session PLUS one latest
 * requested intent (a request during a running Pack replaces the pending intent,
 * never spawns a parallel job); a superseded/earlier job cannot itself become
 * the authoritative preview; every successful result enters the memory cache;
 * explicit selection and an Undo cache-hit pick the result BY pack_input_hash,
 * not by completion time.
 *
 * The struct is transparent so table-driven fixtures can construct exact
 * scenarios (including a "straggler" job whose request seq is older than the
 * latest, to pin that a late-finishing superseded job never overwrites the
 * authoritative preview). It carries NO blob storage: the "in cache" set is
 * membership-by-hash, which maps to tp_c0_cache in production (task 4).
 */

#include <stdbool.h>
#include <stdint.h>

#include "tp_c0/tp_c0_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spike cap on remembered completed-result hashes (maps to the memory cache). */
#define TP_C0_PACK_SUPER_MAX_DONE 64

typedef struct tp_c0_pack_super {
    int64_t latest_seq; /* monotonic: the seq of the most recent request */

    bool has_running;
    int64_t running_seq; /* the request seq this running job was started for */
    tp_c0_id128 running_hash;

    bool has_pending; /* the one latest intent queued while a job runs */
    int64_t pending_seq;
    tp_c0_id128 pending_hash;

    bool has_preview; /* authoritative preview selected for display */
    tp_c0_id128 preview_hash;

    tp_c0_id128 done[TP_C0_PACK_SUPER_MAX_DONE]; /* successful results in the cache, by hash */
    int done_count;
} tp_c0_pack_super;

typedef enum tp_c0_pack_outcome {
    TP_C0_PACK_STARTED = 0,    /* request started a new running job (session was idle) */
    TP_C0_PACK_QUEUED,         /* request set/replaced the pending intent (a job is running) */
    TP_C0_PACK_BECAME_PREVIEW, /* completed job is the freshest intent -> authoritative preview */
    TP_C0_PACK_SUPERSEDED,     /* completed job was superseded -> cached only, NOT preview */
    TP_C0_PACK_SELECTED,       /* explicit/Undo selection by hash: cache hit -> preview */
    TP_C0_PACK_MISS,           /* selection by hash: cache miss -> preview unchanged (out of date) */
    TP_C0_PACK_CANCELLED,      /* ownership transfer cancelled the running job */
    TP_C0_PACK_NOOP,           /* nothing to act on (e.g. complete with no running job) */
    TP_C0_PACK_OUTCOME_COUNT
} tp_c0_pack_outcome;

const char *tp_c0_pack_outcome_id(tp_c0_pack_outcome o); /* stable machine token (test-pinned) */

void tp_c0_pack_super_init(tp_c0_pack_super *s);

/* Press Pack for `input_hash`. Idle -> STARTED (running job). A job already
 * running -> QUEUED (replaces the single pending intent; never a parallel job).
 * Always advances latest_seq (request ordering). */
tp_c0_pack_outcome tp_c0_pack_super_request(tp_c0_pack_super *s, tp_c0_id128 input_hash);

/* The running job finishes successfully. Its hash enters the cache. It becomes
 * the authoritative preview ONLY if it is still the freshest intent
 * (running_seq == latest_seq); otherwise SUPERSEDED (cached only, preview
 * untouched -- pins that a late/earlier job never overwrites a newer preview).
 * If a pending intent exists it is promoted to the running job. NOOP if no job
 * is running. */
tp_c0_pack_outcome tp_c0_pack_super_complete(tp_c0_pack_super *s);

/* Explicit selection or Undo cache-hit: choose the preview BY hash. Cache hit ->
 * SELECTED (preview := hash). Cache miss -> MISS (preview unchanged; caller shows
 * "out of date"; no Pack is auto-started). Selection is by hash, NOT by
 * completion time. */
tp_c0_pack_outcome tp_c0_pack_super_select(tp_c0_pack_super *s, tp_c0_id128 hash);

/* Ownership transfer: cancel ONLY the running Pack (§59 item 24). Preview, cache
 * membership, and the pending intent are preserved. CANCELLED if a job was
 * running, else NOOP. */
tp_c0_pack_outcome tp_c0_pack_super_transfer(tp_c0_pack_super *s);

/* Freshness (§10.1): the preview is current iff it exists and its hash equals the
 * current pack input hash. */
bool tp_c0_pack_super_is_fresh(const tp_c0_pack_super *s, tp_c0_id128 current_hash);

bool tp_c0_pack_super_in_cache(const tp_c0_pack_super *s, tp_c0_id128 hash);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_PACK_SUPER_H */
