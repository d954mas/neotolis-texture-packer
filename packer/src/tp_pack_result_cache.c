#include "tp_core/tp_pack_result_cache.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "tinycthread.h"

#include "core/nt_assert.h"
#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"

/* Bytes of one page fed to the deflator between cancellation checks. This is the
 * ONLY thing that bounds how long the owning thread waits when it has to join an
 * in-flight compression (eviction, forget, re-store, re-activation, destroy): at
 * the measured ~478 MiB/s encode throughput, 1 MiB is ~2 ms. Compressing a whole
 * 64 MiB page in one mz_compress2 call would have made that join ~134 ms. */
#define COLD_CHUNK_BYTES (1024U * 1024U)

/* ------------------------------------------------------------------------- */
/* Background compression queue                                              */
/*                                                                           */
/* The only shared state in this file, and the only lock. Everything below the */
/* queue is owned by the calling thread alone.                                */
/* ------------------------------------------------------------------------- */

typedef enum cold_job_state {
    COLD_JOB_PENDING = 0, /* queued, worker has not picked it up */
    COLD_JOB_RUNNING,     /* worker owns it */
    COLD_JOB_DONE         /* worker is finished; outputs belong to the owner */
} cold_job_state;

/* A private SNAPSHOT of everything the worker may see. The owning thread fills
 * `source`/`page_count` and guarantees the pages stay alive until the job is
 * joined; the worker allocates and writes ONLY the output fields. No pointer here
 * reaches a cache entry, the entry table, or any session object. */
typedef struct cold_job {
    struct cold_job *next; /* queue link -- guarded by cold_queue::mutex */

    /* input (owner-written before enqueue, worker-read only) */
    const tp_result *source;
    int page_count;

    /* output (worker-written, owner-read only after state == COLD_JOB_DONE) */
    tp_arena *meta_arena;
    tp_result *meta;
    uint64_t meta_bytes;
    uint8_t **blobs;
    size_t *blob_sizes;
    uint64_t raw_bytes;
    uint64_t coded_bytes;
    bool ok;

    /* control -- guarded by cold_queue::mutex */
    cold_job_state state;
    bool cancel;
} cold_job;

typedef struct cold_queue {
    mtx_t mutex;
    cnd_t wake; /* worker waits here for work or shutdown */
    cnd_t done; /* owner waits here for a RUNNING job to finish */
    cold_job *head;
    cold_job *tail;
    bool shutdown;
    bool running; /* the encoder thread exists */
    thrd_t thread;
} cold_queue;

/* ------------------------------------------------------------------------- */
/* Entries                                                                    */
/* ------------------------------------------------------------------------- */

/* One stored Pack result.
 *
 * HOT           pin_owner != NULL, cold_arena == NULL. `result` is the caller's,
 *               its pages live in the caller's pinned arena, and the entry costs
 *               `retained_bytes`.
 * COLD          pin_owner == NULL, cold_arena != NULL, pixels == NULL. `result`
 *               is the store's metadata copy with pages[i].rgba == NULL, and the
 *               entry costs `coded_bytes + meta_bytes` -- both halves are memory
 *               this store allocated and still holds.
 * COLD+RESIDENT as COLD, plus `pixels` -- one decompressed buffer per page,
 *               pointed at by pages[i].rgba. Only the ACTIVE entry is ever here,
 *               so the extra raw bytes are budget-exempt by construction. */
typedef struct cache_entry {
    tp_id128 hash;
    uint64_t sequence; /* monotonic completion sequence */
    uint64_t touch;    /* LRU clock: larger = more recently used */

    const tp_result *result; /* borrowed from `pin_owner`, or == `cold_result` */

    /* HOT */
    void *pin_owner;
    void (*pin_release)(void *owner);
    uint64_t retained_bytes;

    /* COLD */
    tp_arena *cold_arena;    /* owns `cold_result` and everything it points at */
    tp_result *cold_result;  /* metadata copy; pages[i].rgba is ours to set */
    uint8_t **blobs;         /* one raw-deflate blob per page */
    size_t *blob_sizes;
    uint8_t **pixels;        /* decompressed pages, non-NULL only while resident */
    int page_count;
    uint64_t coded_bytes;
    uint64_t cold_raw_bytes;
    uint64_t meta_bytes;

    /* the ratio floor already rejected this entry: never queue it again */
    bool cold_declined;

    cold_job *job; /* in-flight compression, or NULL */
} cache_entry;

struct tp_pack_result_cache {
    cache_entry **entries;
    int count;
    int cap;
    cache_entry *active;
    bool has_selection;
    tp_id128 selected_hash;
    uint64_t budget;
    uint64_t inactive_bytes;
    uint64_t seq_clock;
    uint64_t touch_clock;
    uint64_t evicted;
    uint64_t dropped_corrupt;
    uint64_t encoded;
    uint64_t encode_discarded;
    uint64_t ratio_floor_evicted;
    uint64_t promoted;
    cold_queue queue;
    bool queue_ready; /* mutex/condvars initialised */
};

static bool entry_is_cold(const cache_entry *entry) {
    return entry->cold_arena != NULL;
}

/* The quantity the byte-budget LRU counts for this entry while it is inactive.
 * An inactive entry is never COLD+RESIDENT (demotion frees the pixels), so the
 * cold answer is the compressed pages PLUS the store-owned geometry copy that
 * comes with them, and the hot answer is the caller's raw measure -- exactly the
 * memory the store holds in each state. */
static uint64_t entry_budget_bytes(const cache_entry *entry) {
    return entry_is_cold(entry) ? entry->coded_bytes + entry->meta_bytes
                                : entry->retained_bytes;
}

/* ------------------------------------------------------------------------- */
/* Worker: metadata deep copy                                                 */
/* ------------------------------------------------------------------------- */

/* Every allocation the metadata copy makes is also totalled, so the store can
 * report what the uncompressed geometry actually costs next to the blobs. */
typedef struct meta_sink {
    tp_arena *arena;
    uint64_t bytes;
    bool ok;
} meta_sink;

static void *meta_alloc(meta_sink *sink, size_t size) {
    if (!sink->ok || size == 0U) {
        return NULL;
    }
    void *block = tp_arena_alloc(sink->arena, size);
    if (!block) {
        sink->ok = false;
        return NULL;
    }
    sink->bytes += (uint64_t)size;
    return block;
}

static const char *meta_strdup(meta_sink *sink, const char *text) {
    if (!sink->ok) {
        return NULL;
    }
    const char *src = text ? text : "";
    const size_t size = strlen(src) + 1U;
    char *copy = (char *)meta_alloc(sink, size);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, src, size);
    return copy;
}

/* Deep-copies the result's GEOMETRY into `arena`: atlas/page/sprite records,
 * every name, every hull. Page pixels are deliberately NOT copied -- they are
 * what the blobs replace -- so pages[i].rgba comes out NULL. */
static tp_result *meta_copy(const tp_result *src, tp_arena *arena,
                            uint64_t *out_bytes) {
    meta_sink sink = {arena, 0U, true};
    tp_result *dst = (tp_result *)meta_alloc(&sink, sizeof *dst);
    if (!dst) {
        return NULL;
    }
    memset(dst, 0, sizeof *dst);
    dst->atlas_name = meta_strdup(&sink, src->atlas_name);
    dst->pixels_per_unit = src->pixels_per_unit;

    dst->page_count = src->page_count;
    if (src->page_count > 0) {
        dst->pages = (tp_page *)meta_alloc(
            &sink, (size_t)src->page_count * sizeof *dst->pages);
        if (dst->pages) {
            memset(dst->pages, 0, (size_t)src->page_count * sizeof *dst->pages);
        }
        for (int i = 0; sink.ok && i < src->page_count; ++i) {
            dst->pages[i] = src->pages[i];
            dst->pages[i].image_name =
                meta_strdup(&sink, src->pages[i].image_name);
            dst->pages[i].rgba = NULL; /* the blobs are the pixels now */
        }
    }

    dst->sprite_count = src->sprite_count;
    if (src->sprite_count > 0) {
        dst->sprites = (tp_sprite *)meta_alloc(
            &sink, (size_t)src->sprite_count * sizeof *dst->sprites);
        if (dst->sprites) {
            memset(dst->sprites, 0,
                   (size_t)src->sprite_count * sizeof *dst->sprites);
        }
        for (int i = 0; sink.ok && i < src->sprite_count; ++i) {
            const tp_sprite *from = &src->sprites[i];
            tp_sprite *to = &dst->sprites[i];
            *to = *from;
            to->name = meta_strdup(&sink, from->name);
            to->verts = NULL;
            to->indices = NULL;
            if (from->vert_count > 0 && from->verts) {
                const size_t size =
                    (size_t)from->vert_count * sizeof *from->verts;
                to->verts = (tp_point *)meta_alloc(&sink, size);
                if (to->verts) {
                    memcpy(to->verts, from->verts, size);
                }
            } else {
                to->vert_count = 0;
            }
            if (from->index_count > 0 && from->indices) {
                const size_t size =
                    (size_t)from->index_count * sizeof *from->indices;
                to->indices = (uint16_t *)meta_alloc(&sink, size);
                if (to->indices) {
                    memcpy(to->indices, from->indices, size);
                }
            } else {
                to->index_count = 0;
            }
        }
    }
    if (!sink.ok) {
        return NULL;
    }
    *out_bytes = sink.bytes;
    return dst;
}

/* ------------------------------------------------------------------------- */
/* Worker: page codec                                                         */
/* ------------------------------------------------------------------------- */

static size_t page_raw_bytes(const tp_page *page) {
    if (page->w <= 0 || page->h <= 0) {
        return 0U;
    }
    return (size_t)page->w * (size_t)page->h * 4U;
}

/* True when every page carries pixels the cold tier can actually encode. A
 * result that fails this is simply never queued: it stays HOT for life. */
static bool result_is_coldable(const tp_result *result) {
    if (!result || result->page_count <= 0) {
        return false;
    }
    for (int i = 0; i < result->page_count; ++i) {
        const size_t raw = page_raw_bytes(&result->pages[i]);
        if (raw == 0U || !result->pages[i].rgba) {
            return false;
        }
        if (mz_compressBound((mz_ulong)raw) > (mz_ulong)UINT_MAX) {
            return false; /* one mz_stream pass could not describe it */
        }
    }
    return true;
}

static bool cold_job_cancelled(cold_queue *queue, cold_job *job) {
    mtx_lock(&queue->mutex);
    const bool cancelled = job->cancel;
    mtx_unlock(&queue->mutex);
    return cancelled;
}

/* One page in, one exactly-sized owned deflate blob out. Chunked so the owning
 * thread's cancel is observed within COLD_CHUNK_BYTES rather than a whole page,
 * and so the single background thread yields between chunks instead of holding a
 * core for the entire encode. */
static bool cold_encode_page(cold_queue *queue, cold_job *job,
                             const tp_page *page, uint8_t **out_bytes,
                             size_t *out_size) {
    *out_bytes = NULL;
    *out_size = 0U;
    const size_t raw = page_raw_bytes(page);
    const mz_ulong bound = mz_compressBound((mz_ulong)raw);
    uint8_t *dst = (uint8_t *)malloc((size_t)bound);
    if (!dst) {
        return false;
    }
    mz_stream stream;
    memset(&stream, 0, sizeof stream);
    if (mz_deflateInit(&stream, TP_PACK_RESULT_CACHE_DEFLATE_LEVEL) != MZ_OK) {
        free(dst);
        return false;
    }
    stream.next_out = dst;
    stream.avail_out = (unsigned int)bound;
    size_t fed = 0U;
    bool ok = false;
    for (;;) {
        if (cold_job_cancelled(queue, job)) {
            break;
        }
        const size_t remaining = raw - fed;
        const size_t chunk =
            remaining > COLD_CHUNK_BYTES ? (size_t)COLD_CHUNK_BYTES : remaining;
        stream.next_in = page->rgba + fed;
        stream.avail_in = (unsigned int)chunk;
        const int flush = (fed + chunk >= raw) ? MZ_FINISH : MZ_NO_FLUSH;
        const int status = mz_deflate(&stream, flush);
        fed += chunk - (size_t)stream.avail_in;
        if (status == MZ_STREAM_END) {
            ok = true;
            break;
        }
        if (status != MZ_OK) {
            break;
        }
        thrd_yield(); /* one background thread, deliberately not greedy */
    }
    const size_t coded = (size_t)stream.total_out;
    (void)mz_deflateEnd(&stream);
    if (!ok || coded == 0U) {
        free(dst);
        return false;
    }
    /* Hand back an exactly-sized buffer: `bound` is larger than the raw page, so
     * keeping it would make a cold entry cost MORE than the hot one it replaced. */
    uint8_t *exact = (uint8_t *)realloc(dst, coded);
    *out_bytes = exact ? exact : dst;
    *out_size = coded;
    return true;
}

static void cold_job_free_outputs(cold_job *job) {
    if (job->blobs) {
        for (int i = 0; i < job->page_count; ++i) {
            free(job->blobs[i]);
        }
        free(job->blobs);
        job->blobs = NULL;
    }
    free(job->blob_sizes);
    job->blob_sizes = NULL;
    tp_arena_destroy(job->meta_arena);
    job->meta_arena = NULL;
    job->meta = NULL;
    job->ok = false;
    job->coded_bytes = 0U;
}

/* The whole of the worker's work: a private arena for the geometry copy, then one
 * blob per page. Touches nothing but `job`. */
static void cold_job_run(cold_queue *queue, cold_job *job) {
    job->meta_arena = tp_arena_create(0);
    if (!job->meta_arena) {
        return;
    }
    job->meta = meta_copy(job->source, job->meta_arena, &job->meta_bytes);
    if (!job->meta) {
        cold_job_free_outputs(job);
        return;
    }
    job->blobs = (uint8_t **)calloc((size_t)job->page_count, sizeof *job->blobs);
    job->blob_sizes =
        (size_t *)calloc((size_t)job->page_count, sizeof *job->blob_sizes);
    if (!job->blobs || !job->blob_sizes) {
        cold_job_free_outputs(job);
        return;
    }
    uint64_t raw_total = 0U;
    uint64_t coded_total = 0U;
    for (int i = 0; i < job->page_count; ++i) {
        const tp_page *page = &job->source->pages[i];
        if (!cold_encode_page(queue, job, page, &job->blobs[i],
                              &job->blob_sizes[i])) {
            cold_job_free_outputs(job);
            return;
        }
        raw_total += (uint64_t)page_raw_bytes(page);
        coded_total += (uint64_t)job->blob_sizes[i];
    }
    job->raw_bytes = raw_total;
    job->coded_bytes = coded_total;
    job->ok = true;
}

static int cold_worker_main(void *context) {
    cold_queue *queue = (cold_queue *)context;
    mtx_lock(&queue->mutex);
    for (;;) {
        while (!queue->head && !queue->shutdown) {
            cnd_wait(&queue->wake, &queue->mutex);
        }
        if (!queue->head) {
            break; /* shutdown with an empty queue is the only exit */
        }
        cold_job *job = queue->head;
        queue->head = job->next;
        if (!queue->head) {
            queue->tail = NULL;
        }
        job->next = NULL;
        job->state = COLD_JOB_RUNNING;
        const bool cancelled = job->cancel;
        mtx_unlock(&queue->mutex);
        if (!cancelled) {
            cold_job_run(queue, job);
        }
        mtx_lock(&queue->mutex);
        job->state = COLD_JOB_DONE;
        cnd_broadcast(&queue->done);
    }
    mtx_unlock(&queue->mutex);
    return 0;
}

/* Lazily created: a store that never demotes never starts a thread. */
static bool cold_queue_start(tp_pack_result_cache *cache) {
    cold_queue *queue = &cache->queue;
    if (queue->running) {
        return true;
    }
    if (!cache->queue_ready) {
        if (mtx_init(&queue->mutex, mtx_plain) != thrd_success) {
            return false;
        }
        if (cnd_init(&queue->wake) != thrd_success) {
            mtx_destroy(&queue->mutex);
            return false;
        }
        if (cnd_init(&queue->done) != thrd_success) {
            cnd_destroy(&queue->wake);
            mtx_destroy(&queue->mutex);
            return false;
        }
        cache->queue_ready = true;
    }
    if (thrd_create(&queue->thread, cold_worker_main, queue) != thrd_success) {
        return false;
    }
    queue->running = true;
    return true;
}

/* Cancels `job`, waits until the worker has let go of it, and frees it. Safe for
 * a job the worker never started and for one it is halfway through. */
static void cold_job_cancel_and_join(tp_pack_result_cache *cache,
                                     cold_job *job) {
    cold_queue *queue = &cache->queue;
    mtx_lock(&queue->mutex);
    job->cancel = true;
    if (job->state == COLD_JOB_PENDING) {
        /* Not picked up yet: unlink it so the worker never sees it at all. */
        cold_job *prev = NULL;
        for (cold_job *it = queue->head; it; prev = it, it = it->next) {
            if (it != job) {
                continue;
            }
            if (prev) {
                prev->next = job->next;
            } else {
                queue->head = job->next;
            }
            if (queue->tail == job) {
                queue->tail = prev;
            }
            job->next = NULL;
            job->state = COLD_JOB_DONE;
            break;
        }
    }
    while (job->state != COLD_JOB_DONE) {
        cnd_wait(&queue->done, &queue->mutex);
    }
    mtx_unlock(&queue->mutex);
    cold_job_free_outputs(job);
    free(job);
    cache->encode_discarded++;
}

/* Queues a background compression for a HOT inactive entry. Failure to queue is
 * not an error: the entry simply stays hot and raw. */
static void cold_job_enqueue(tp_pack_result_cache *cache, cache_entry *entry) {
    if (entry->job || entry->cold_declined || entry_is_cold(entry) ||
        !result_is_coldable(entry->result)) {
        return;
    }
    if (!cold_queue_start(cache)) {
        return;
    }
    cold_job *job = (cold_job *)calloc(1U, sizeof *job);
    if (!job) {
        return;
    }
    job->source = entry->result;
    job->page_count = entry->result->page_count;
    job->state = COLD_JOB_PENDING;
    entry->job = job;
    mtx_lock(&cache->queue.mutex);
    if (cache->queue.tail) {
        cache->queue.tail->next = job;
    } else {
        cache->queue.head = job;
    }
    cache->queue.tail = job;
    cnd_signal(&cache->queue.wake);
    mtx_unlock(&cache->queue.mutex);
}

/* Any operation that ends the "pages stay alive" guarantee starts here. */
static void entry_drop_job(tp_pack_result_cache *cache, cache_entry *entry) {
    if (!entry->job) {
        return;
    }
    cold_job *job = entry->job;
    entry->job = NULL;
    cold_job_cancel_and_join(cache, job);
}

/* ------------------------------------------------------------------------- */
/* Cold storage on an entry                                                   */
/* ------------------------------------------------------------------------- */

static void entry_free_pixels(cache_entry *entry) {
    if (!entry->pixels) {
        return;
    }
    for (int i = 0; i < entry->page_count; ++i) {
        free(entry->pixels[i]);
    }
    free(entry->pixels);
    entry->pixels = NULL;
    if (entry->cold_result) {
        for (int i = 0; i < entry->cold_result->page_count; ++i) {
            entry->cold_result->pages[i].rgba = NULL;
        }
    }
}

static void entry_free_cold(cache_entry *entry) {
    entry_free_pixels(entry);
    if (entry->blobs) {
        for (int i = 0; i < entry->page_count; ++i) {
            free(entry->blobs[i]);
        }
        free(entry->blobs);
        entry->blobs = NULL;
    }
    free(entry->blob_sizes);
    entry->blob_sizes = NULL;
    tp_arena_destroy(entry->cold_arena);
    entry->cold_arena = NULL;
    entry->cold_result = NULL;
    entry->page_count = 0;
    entry->coded_bytes = 0U;
    entry->cold_raw_bytes = 0U;
    entry->meta_bytes = 0U;
}

/* Releases the caller's receipt exactly once. This is the ONLY path that ends the
 * caller's pin, and it never dismantles the arena behind it. */
static void entry_release_pin(cache_entry *entry) {
    if (!entry->pin_release) {
        return;
    }
    void (*release)(void *) = entry->pin_release;
    void *owner = entry->pin_owner;
    entry->pin_release = NULL;
    entry->pin_owner = NULL;
    entry->retained_bytes = 0U;
    release(owner);
}

static void entry_free(tp_pack_result_cache *cache, cache_entry *entry) {
    if (!entry) {
        return;
    }
    entry_drop_job(cache, entry);
    entry_free_cold(entry);
    entry_release_pin(entry);
    entry->result = NULL;
    free(entry);
}

/* Decompresses one contiguous range of pages. Runs on a transient fork-join
 * thread and touches nothing but the entry's own blob/pixel arrays. */
typedef struct decode_slice {
    cache_entry *entry;
    int begin;
    int end;
    bool ok;
} decode_slice;

static bool decode_page_range(cache_entry *entry, int begin, int end) {
    for (int i = begin; i < end; ++i) {
        const size_t raw = page_raw_bytes(&entry->cold_result->pages[i]);
        mz_ulong out_len = (mz_ulong)raw;
        const int status = mz_uncompress(entry->pixels[i], &out_len,
                                         entry->blobs[i],
                                         (mz_ulong)entry->blob_sizes[i]);
        if (status != MZ_OK || (size_t)out_len != raw) {
            return false;
        }
    }
    return true;
}

static int decode_slice_main(void *context) {
    decode_slice *slice = (decode_slice *)context;
    slice->ok = decode_page_range(slice->entry, slice->begin, slice->end);
    return 0;
}

/* COLD -> COLD+RESIDENT. Allocates every destination first (so a partial failure
 * frees cleanly), then inflates the pages in parallel: they are independent, and
 * the decode is the one cost an atlas switch pays for the cold tier. */
static bool entry_promote_cold(cache_entry *entry) {
    if (entry->pixels) {
        return true;
    }
    const int pages = entry->page_count;
    if (pages <= 0) {
        return false;
    }
    entry->pixels = (uint8_t **)calloc((size_t)pages, sizeof *entry->pixels);
    if (!entry->pixels) {
        return false;
    }
    for (int i = 0; i < pages; ++i) {
        const size_t raw = page_raw_bytes(&entry->cold_result->pages[i]);
        entry->pixels[i] = (uint8_t *)malloc(raw);
        if (!entry->pixels[i]) {
            entry_free_pixels(entry);
            return false;
        }
    }

    int workers = pages < TP_PACK_RESULT_CACHE_DECODE_THREADS
                      ? pages
                      : TP_PACK_RESULT_CACHE_DECODE_THREADS;
    bool ok = true;
    if (workers > 1) {
        decode_slice slices[TP_PACK_RESULT_CACHE_DECODE_THREADS];
        thrd_t threads[TP_PACK_RESULT_CACHE_DECODE_THREADS];
        int spawned = 0;
        const int per = (pages + workers - 1) / workers;
        for (int i = 0; i < workers; ++i) {
            slices[i].entry = entry;
            slices[i].begin = i * per;
            slices[i].end = slices[i].begin + per;
            if (slices[i].end > pages) {
                slices[i].end = pages;
            }
            slices[i].ok = true;
        }
        /* Slice 0 stays on this thread: one fewer thread to create, and the
         * fallback below needs no special case when creation fails. */
        for (int i = 1; i < workers; ++i) {
            if (slices[i].begin >= slices[i].end) {
                continue;
            }
            if (thrd_create(&threads[spawned], decode_slice_main, &slices[i]) !=
                thrd_success) {
                /* Creation failed: this thread finishes the remaining slices. */
                for (int j = i; j < workers; ++j) {
                    ok = ok && decode_page_range(entry, slices[j].begin,
                                                 slices[j].end);
                }
                break;
            }
            spawned++;
        }
        ok = decode_page_range(entry, slices[0].begin, slices[0].end) && ok;
        for (int i = 0; i < spawned; ++i) {
            (void)thrd_join(threads[i], NULL);
        }
        for (int i = 1; ok && i < workers; ++i) {
            ok = slices[i].ok;
        }
    } else {
        ok = decode_page_range(entry, 0, pages);
    }

    if (!ok) {
        entry_free_pixels(entry);
        return false;
    }
    for (int i = 0; i < pages; ++i) {
        entry->cold_result->pages[i].rgba = entry->pixels[i];
    }
    entry->result = entry->cold_result;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Entry table                                                                */
/* ------------------------------------------------------------------------- */

static cache_entry *find_entry(const tp_pack_result_cache *cache,
                               tp_id128 hash) {
    for (int i = 0; i < cache->count; i++) {
        if (tp_id128_eq(cache->entries[i]->hash, hash)) {
            return cache->entries[i];
        }
    }
    return NULL;
}

static bool entries_push(tp_pack_result_cache *cache, cache_entry *entry) {
    if (cache->count == cache->cap) {
        const int new_cap = cache->cap ? cache->cap * 2 : 8;
        cache_entry **grown =
            realloc(cache->entries, (size_t)new_cap * sizeof *grown);
        if (!grown) {
            return false;
        }
        cache->entries = grown;
        cache->cap = new_cap;
    }
    cache->entries[cache->count++] = entry;
    return true;
}

/* Removes `entry` from the store and frees it. Accounting for inactive bytes is
 * the caller's responsibility (it knows the entry's active/inactive state). */
static void remove_entry(tp_pack_result_cache *cache, cache_entry *entry) {
    int idx = -1;
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i] == entry) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return;
    }
    if (cache->active == entry) {
        cache->active = NULL;
    }
    entry_free(cache, entry);
    for (int i = idx; i < cache->count - 1; i++) {
        cache->entries[i] = cache->entries[i + 1];
    }
    cache->count--;
}

/* The entry holding the maximum completion sequence -- the one authoritative()
 * resolves to absent an explicit selection. Ties keep the
 * earliest-stored entry, exactly as resolve_target() does. */
static cache_entry *max_sequence_entry(const tp_pack_result_cache *cache) {
    cache_entry *best = NULL;
    for (int i = 0; i < cache->count; i++) {
        cache_entry *e = cache->entries[i];
        if (!best || e->sequence > best->sequence) {
            best = e;
        }
    }
    return best;
}

static void evict_over_budget(tp_pack_result_cache *cache) {
    /* The highest-sequence entry must stay resolvable regardless of budget size
     * (store contract): an out-of-order store can demote it to
     * inactive, but authoritative() must still return it, never a stale lower
     * sequence. So it is exempt from eviction unconditionally, the same way the
     * pinned active entry is -- even at budget 0. Its bytes stay counted in
     * inactive_bytes, so inactive_bytes may exceed the budget by at most this
     * one entry when it is inactive. */
    cache_entry *keep = max_sequence_entry(cache);
    while (cache->inactive_bytes > cache->budget) {
        cache_entry *victim = NULL;
        for (int i = 0; i < cache->count; i++) {
            cache_entry *e = cache->entries[i];
            if (e == cache->active || e == keep) {
                continue;
            }
            if (!victim || e->touch < victim->touch) {
                victim = e;
            }
        }
        if (!victim) {
            break; /* only the active + highest-sequence entries remain */
        }
        cache->inactive_bytes -= entry_budget_bytes(victim);
        remove_entry(cache, victim);
        cache->evicted++;
    }
}

/* Demotes the currently active entry into the byte-budget LRU. A COLD+RESIDENT
 * entry gives its decompressed pixels back immediately (the blobs stay, so it is
 * never re-encoded); a HOT entry keeps the caller's pin and is QUEUED for
 * background compression, staying raw-accounted until a pump applies the result. */
static void demote_active(tp_pack_result_cache *cache) {
    cache_entry *prev = cache->active;
    if (!prev) {
        return;
    }
    cache->active = NULL;
    if (entry_is_cold(prev)) {
        entry_free_pixels(prev);
        prev->result = prev->cold_result;
    } else {
        cold_job_enqueue(cache, prev);
    }
    prev->touch = ++cache->touch_clock;
    cache->inactive_bytes += entry_budget_bytes(prev);
}

/* ------------------------------------------------------------------------- */
/* Pump: applying finished compressions                                       */
/* ------------------------------------------------------------------------- */

/* Moves the finished job's outputs onto the entry and releases the caller's pin.
 * Called only from the pump, and only for an INACTIVE entry. */
static void entry_adopt_cold(tp_pack_result_cache *cache, cache_entry *entry,
                             cold_job *job) {
    /* A job only ever exists for an entry that was demoted, and every path that
     * re-activates one cancels and joins it first, so the entry the pump adopts
     * onto is inactive by construction -- and its bytes are in inactive_bytes. */
    NT_ASSERT(entry != cache->active);
    cache->inactive_bytes -= entry_budget_bytes(entry);
    entry->cold_arena = job->meta_arena;
    entry->cold_result = job->meta;
    entry->blobs = job->blobs;
    entry->blob_sizes = job->blob_sizes;
    entry->page_count = job->page_count;
    entry->coded_bytes = job->coded_bytes;
    entry->cold_raw_bytes = job->raw_bytes;
    entry->meta_bytes = job->meta_bytes;
    job->meta_arena = NULL;
    job->meta = NULL;
    job->blobs = NULL;
    job->blob_sizes = NULL;
    /* The raw pages are now redundant, so the receipt that pinned them goes. */
    entry_release_pin(entry);
    entry->result = entry->cold_result;
    cache->inactive_bytes += entry_budget_bytes(entry);
    cache->encoded++;
}

static void pump_apply(tp_pack_result_cache *cache, cache_entry *entry,
                       cold_job *job) {
    entry->job = NULL;
    const bool usable =
        job->ok && job->coded_bytes > 0U &&
        (double)job->raw_bytes >=
            TP_PACK_RESULT_CACHE_MIN_RATIO * (double)job->coded_bytes;
    if (usable) {
        entry_adopt_cold(cache, entry, job);
        cold_job_free_outputs(job);
        free(job);
        return;
    }
    const bool floor_failed = job->ok && job->coded_bytes > 0U;
    cold_job_free_outputs(job);
    free(job);
    /* Either the encode failed outright or the result does not compress. Both
     * answers are about this entry's CONTENT, which a re-store is the only thing
     * that changes, so the entry is not queued again for the rest of its life. */
    entry->cold_declined = true;
    if (!floor_failed) {
        return; /* nothing was learned about the ratio: just stay hot */
    }
    /* A result that does not compress buys almost nothing back while still
     * charging a full parallel decode to the next atlas switch, so it is not
     * worth keeping cold. The two entries the store contract may never evict
     * (the active pin and the highest sequence) stay HOT instead -- the
     * exemption is a correctness rule and outranks this policy. */
    if (entry == cache->active || entry == max_sequence_entry(cache)) {
        return;
    }
    cache->inactive_bytes -= entry_budget_bytes(entry);
    remove_entry(cache, entry);
    cache->ratio_floor_evicted++;
    cache->evicted++;
}

/* The first entry whose job the worker has finished with, or NULL. The queue lock
 * is held only for the flag test -- nothing else in the pump touches it. */
static cache_entry *pump_take_ready(tp_pack_result_cache *cache) {
    cache_entry *ready = NULL;
    mtx_lock(&cache->queue.mutex);
    for (int i = 0; i < cache->count; ++i) {
        cache_entry *entry = cache->entries[i];
        if (entry->job && entry->job->state == COLD_JOB_DONE) {
            ready = entry;
            break;
        }
    }
    mtx_unlock(&cache->queue.mutex);
    return ready;
}

void tp_pack_result_cache_pump(tp_pack_result_cache *cache) {
    if (!cache || !cache->queue_ready) {
        return;
    }
    bool applied = false;
    for (cache_entry *entry = pump_take_ready(cache); entry;
         entry = pump_take_ready(cache)) {
        pump_apply(cache, entry, entry->job);
        applied = true;
    }
    if (applied) {
        evict_over_budget(cache);
    }
}

void tp_pack_result_cache_settle(tp_pack_result_cache *cache) {
    if (!cache || !cache->queue_ready) {
        return;
    }
    mtx_lock(&cache->queue.mutex);
    for (;;) {
        bool waiting = false;
        for (int i = 0; i < cache->count; ++i) {
            const cold_job *job = cache->entries[i]->job;
            if (job && job->state != COLD_JOB_DONE) {
                waiting = true;
                break;
            }
        }
        if (!waiting) {
            break;
        }
        cnd_wait(&cache->queue.done, &cache->queue.mutex);
    }
    mtx_unlock(&cache->queue.mutex);
    tp_pack_result_cache_pump(cache);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

tp_pack_result_cache *tp_pack_result_cache_create(uint64_t byte_budget) {
    tp_pack_result_cache *cache = calloc(1U, sizeof *cache);
    if (!cache) {
        return NULL;
    }
    cache->budget = byte_budget;
    return cache;
}

void tp_pack_result_cache_destroy(tp_pack_result_cache *cache) {
    if (!cache) {
        return;
    }
    /* Every in-flight job is cancelled and joined FIRST: the worker's page
     * pointers reach into pins this loop is about to release. */
    for (int i = 0; i < cache->count; i++) {
        entry_drop_job(cache, cache->entries[i]);
    }
    if (cache->queue.running) {
        mtx_lock(&cache->queue.mutex);
        cache->queue.shutdown = true;
        cnd_broadcast(&cache->queue.wake);
        mtx_unlock(&cache->queue.mutex);
        (void)thrd_join(cache->queue.thread, NULL);
        cache->queue.running = false;
    }
    if (cache->queue_ready) {
        cnd_destroy(&cache->queue.done);
        cnd_destroy(&cache->queue.wake);
        mtx_destroy(&cache->queue.mutex);
        cache->queue_ready = false;
    }
    for (int i = 0; i < cache->count; i++) {
        entry_free(cache, cache->entries[i]);
    }
    free(cache->entries);
    free(cache);
}

tp_status tp_pack_result_cache_store_retained(
    tp_pack_result_cache *cache, tp_id128 hash, uint64_t sequence,
    const struct tp_result *result, uint64_t retained_bytes, void *pin_owner,
    void (*pin_release)(void *pin_owner), tp_error *err) {
    if (!cache || !result || !pin_release) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "tp_pack_result_cache: retained store requires cache, result, and a "
            "release hook");
    }

    /* Adopt the pin only once nothing else can fail: on every error return above
     * and below, the caller still owns `pin_owner` and must release it. */
    cache_entry *entry = find_entry(cache, hash);
    if (entry) {
        entry_drop_job(cache, entry); /* the superseded pages are about to go */
        if (entry != cache->active) {
            cache->inactive_bytes -= entry_budget_bytes(entry);
        }
        entry_free_cold(entry);
        entry_release_pin(entry); /* releases the SUPERSEDED pin, not this one */
        entry->cold_declined = false;
    } else {
        entry = calloc(1U, sizeof *entry);
        if (!entry) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_pack_result_cache: entry alloc failed");
        }
        entry->hash = hash;
        if (!entries_push(cache, entry)) {
            free(entry);
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_pack_result_cache: entry table grow failed");
        }
    }

    entry->sequence = sequence;
    if (sequence > cache->seq_clock) {
        cache->seq_clock = sequence;
    }
    entry->touch = ++cache->touch_clock;
    entry->pin_owner = pin_owner; /* ADOPT: released exactly once from now on */
    entry->pin_release = pin_release;
    entry->retained_bytes = retained_bytes;
    entry->result = result;

    if (cache->active != entry) {
        demote_active(cache);
    }
    cache->active = entry;

    evict_over_budget(cache);
    return TP_STATUS_OK;
}

void tp_pack_result_cache_forget(tp_pack_result_cache *cache, tp_id128 hash) {
    if (!cache) {
        return;
    }
    cache_entry *entry = find_entry(cache, hash);
    if (!entry) {
        return;
    }
    if (entry != cache->active) {
        cache->inactive_bytes -= entry_budget_bytes(entry);
    }
    if (cache->has_selection && tp_id128_eq(cache->selected_hash, hash)) {
        cache->has_selection = false;
    }
    remove_entry(cache, entry);
}

bool tp_pack_result_cache_contains(const tp_pack_result_cache *cache,
                                   tp_id128 hash) {
    if (!cache) {
        return false;
    }
    return find_entry(cache, hash) != NULL;
}

const struct tp_result *tp_pack_result_cache_peek(
    const tp_pack_result_cache *cache, tp_id128 hash) {
    if (!cache) {
        return NULL;
    }
    const cache_entry *entry = find_entry(cache, hash);
    /* Every present entry answers, in every residency state -- a HOT one with the
     * caller's own result, a COLD one with the store's geometry copy whose page
     * pixels are NULL. That is what keeps "merely cold" distinguishable from
     * "gone" here, and handing it back changes nothing: the touch clock, the
     * active pin, the selection, and inactive_bytes are untouched by design. */
    return entry ? entry->result : NULL;
}

void tp_pack_result_cache_select(tp_pack_result_cache *cache, tp_id128 hash) {
    if (!cache) {
        return;
    }
    if (tp_id128_is_nil(hash) || !find_entry(cache, hash)) {
        cache->has_selection = false;
        return;
    }
    cache->has_selection = true;
    cache->selected_hash = hash;
}

static cache_entry *resolve_target(tp_pack_result_cache *cache) {
    if (cache->has_selection) {
        cache_entry *selected = find_entry(cache, cache->selected_hash);
        if (selected) {
            return selected;
        }
        /* selection lost (dropped/corrupt) -> fall back to latest sequence */
    }
    cache_entry *best = NULL;
    for (int i = 0; i < cache->count; i++) {
        if (!best || cache->entries[i]->sequence > best->sequence) {
            best = cache->entries[i];
        }
    }
    return best;
}

tp_status tp_pack_result_cache_authoritative(tp_pack_result_cache *cache,
                                             tp_id128 *out_hash,
                                             const struct tp_result **out_result,
                                             uint64_t *out_sequence,
                                             tp_error *err) {
    if (!cache) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack_result_cache: cache is required");
    }
    for (;;) {
        cache_entry *target = resolve_target(cache);
        if (!target) {
            return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                "tp_pack_result_cache: no cached result");
        }
        if (target != cache->active) {
            /* Re-activation ends the in-flight job's page-lifetime guarantee and
             * makes its work moot: cancel, join, discard. */
            entry_drop_job(cache, target);
            cache->inactive_bytes -= entry_budget_bytes(target);
            const bool was_cold = entry_is_cold(target);
            if (was_cold && !entry_promote_cold(target)) {
                /* Contained failure: drop the entry and retry with the next
                 * candidate. Its bytes are already out of the accounting. */
                cache->dropped_corrupt++;
                if (cache->has_selection &&
                    tp_id128_eq(cache->selected_hash, target->hash)) {
                    cache->has_selection = false;
                }
                remove_entry(cache, target);
                continue;
            }
            if (was_cold) {
                cache->promoted++;
            }
            demote_active(cache);
            cache->active = target;
            target->touch = ++cache->touch_clock;
            evict_over_budget(cache);
        }
        if (out_hash) {
            *out_hash = target->hash;
        }
        if (out_result) {
            *out_result = target->result;
        }
        if (out_sequence) {
            *out_sequence = target->sequence;
        }
        return TP_STATUS_OK;
    }
}

#ifdef TP_ENABLE_TEST_SEAMS
#include "tp_test_seams.h"

bool tp_pack_result_cache__test_damage_cold_blob(
    struct tp_pack_result_cache *cache, tp_id128 hash) {
    if (!cache) {
        return false;
    }
    cache_entry *entry = find_entry(cache, hash);
    if (!entry || !entry_is_cold(entry) || !entry->blobs || !entry->blobs[0] ||
        entry->blob_sizes[0] == 0U) {
        return false;
    }
    for (size_t i = 0; i < entry->blob_sizes[0]; ++i) {
        entry->blobs[0][i] = (uint8_t)(entry->blobs[0][i] ^ 0x5AU);
    }
    return true;
}
#endif

void tp_pack_result_cache_stats_get(const tp_pack_result_cache *cache,
                                    tp_pack_result_cache_stats *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (!cache) {
        return;
    }
    out->entry_count = cache->count;
    out->has_active = cache->active != NULL;
    if (cache->active) {
        out->active_hash = cache->active->hash;
    }
    out->inactive_bytes = cache->inactive_bytes;
    out->byte_budget = cache->budget;
    out->last_sequence = cache->seq_clock;
    out->evicted = cache->evicted;
    out->dropped_corrupt = cache->dropped_corrupt;
    out->encoded = cache->encoded;
    out->encode_discarded = cache->encode_discarded;
    out->ratio_floor_evicted = cache->ratio_floor_evicted;
    out->promoted = cache->promoted;
    for (int i = 0; i < cache->count; ++i) {
        const cache_entry *entry = cache->entries[i];
        if (entry->job) {
            out->encoding++;
        }
        if (!entry_is_cold(entry)) {
            continue;
        }
        out->cold_entries++;
        out->cold_coded_bytes += entry->coded_bytes;
        out->cold_raw_bytes += entry->cold_raw_bytes;
        out->cold_meta_bytes += entry->meta_bytes;
    }
}
