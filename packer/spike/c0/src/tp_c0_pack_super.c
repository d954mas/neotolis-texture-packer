/* C0-03 task 5: Pack supersession / preview-selection state machine. Pure logic
 * encoding decision 0004 and master spec §10.3-10.5 / §59 item 24. See
 * tp_c0_pack_super.h. */

#include "tp_c0/tp_c0_pack_super.h"

#include <string.h>

const char *tp_c0_pack_outcome_id(tp_c0_pack_outcome o) {
    switch (o) {
        case TP_C0_PACK_STARTED: return "started";
        case TP_C0_PACK_QUEUED: return "queued";
        case TP_C0_PACK_BECAME_PREVIEW: return "became_preview";
        case TP_C0_PACK_SUPERSEDED: return "superseded";
        case TP_C0_PACK_SELECTED: return "selected";
        case TP_C0_PACK_MISS: return "miss";
        case TP_C0_PACK_CANCELLED: return "cancelled";
        case TP_C0_PACK_NOOP: return "noop";
        case TP_C0_PACK_OUTCOME_COUNT: return "";
    }
    return "unknown";
}

void tp_c0_pack_super_init(tp_c0_pack_super *s) {
    if (s) {
        memset(s, 0, sizeof *s);
    }
}

static bool done_has(const tp_c0_pack_super *s, tp_c0_id128 hash) {
    for (int i = 0; i < s->done_count; i++) {
        if (tp_c0_id128_eq(s->done[i], hash)) {
            return true;
        }
    }
    return false;
}

static void done_add(tp_c0_pack_super *s, tp_c0_id128 hash) {
    if (done_has(s, hash)) {
        return; /* content-addressed: identical result is one cache entry */
    }
    if (s->done_count < TP_C0_PACK_SUPER_MAX_DONE) {
        s->done[s->done_count++] = hash;
    }
    /* Overflow silently drops from the membership view (spike cap); the real
     * cache (task 4) evicts by LRU/budget instead. */
}

tp_c0_pack_outcome tp_c0_pack_super_request(tp_c0_pack_super *s, tp_c0_id128 input_hash) {
    if (!s) {
        return TP_C0_PACK_NOOP;
    }
    /* A new request means the user wants a fresh pack result -> release any
     * explicit preview pin so the eventual completion is adopted (F4). */
    s->preview_is_explicit = false;
    s->latest_seq++;
    if (!s->has_running) {
        s->has_running = true;
        s->running_seq = s->latest_seq;
        s->running_hash = input_hash;
        return TP_C0_PACK_STARTED;
    }
    /* A job is running: replace the single pending intent -- never parallel. */
    s->has_pending = true;
    s->pending_seq = s->latest_seq;
    s->pending_hash = input_hash;
    return TP_C0_PACK_QUEUED;
}

tp_c0_pack_outcome tp_c0_pack_super_complete(tp_c0_pack_super *s) {
    if (!s || !s->has_running) {
        return TP_C0_PACK_NOOP;
    }
    tp_c0_id128 finished_hash = s->running_hash;
    int64_t finished_seq = s->running_seq;

    /* Every successful result enters the cache, superseded or not. */
    done_add(s, finished_hash);

    /* Authoritative ONLY if still the freshest intent AND the preview is not
     * user-pinned. A superseded job (older request seq) never overwrites a newer
     * preview (§10.3); an explicit user selection is sticky until a new request
     * clears it (F4). Either way the result is cached, just not shown -> SUPERSEDED
     * (cached only, preview untouched). */
    bool superseded = finished_seq < s->latest_seq;
    tp_c0_pack_outcome outcome;
    if (!superseded && !s->preview_is_explicit) {
        s->has_preview = true;
        s->preview_hash = finished_hash;
        outcome = TP_C0_PACK_BECAME_PREVIEW;
    } else {
        outcome = TP_C0_PACK_SUPERSEDED;
    }

    /* Retire the running job; promote the one pending intent if present. */
    s->has_running = false;
    if (s->has_pending) {
        s->has_running = true;
        s->running_seq = s->pending_seq;
        s->running_hash = s->pending_hash;
        s->has_pending = false;
    }
    return outcome;
}

tp_c0_pack_outcome tp_c0_pack_super_select(tp_c0_pack_super *s, tp_c0_id128 hash) {
    if (!s) {
        return TP_C0_PACK_NOOP;
    }
    if (!done_has(s, hash)) {
        /* Cache miss: preview left unchanged (out of date); no auto-pack. */
        return TP_C0_PACK_MISS;
    }
    s->has_preview = true;
    s->preview_hash = hash;         /* chosen by hash, independent of completion time */
    s->preview_is_explicit = true;  /* user-pinned: a completing job won't overwrite it (F4) */
    return TP_C0_PACK_SELECTED;
}

tp_c0_pack_outcome tp_c0_pack_super_transfer(tp_c0_pack_super *s) {
    if (!s || !s->has_running) {
        return TP_C0_PACK_NOOP;
    }
    /* An ownership transfer drops the whole session pack INTENT: cancel the running
     * Pack and drop the never-run pending intent (F3). Only the preview + cache
     * survive (decision 0004). Leaving has_pending set would let a later
     * request+complete resurrect the stale pre-transfer pending as a running job,
     * executing an unrequested Pack. */
    s->has_running = false;
    s->has_pending = false;
    return TP_C0_PACK_CANCELLED;
}

bool tp_c0_pack_super_is_fresh(const tp_c0_pack_super *s, tp_c0_id128 current_hash) {
    return s && s->has_preview && tp_c0_id128_eq(s->preview_hash, current_hash);
}

bool tp_c0_pack_super_in_cache(const tp_c0_pack_super *s, tp_c0_id128 hash) { return s && done_has(s, hash); }
