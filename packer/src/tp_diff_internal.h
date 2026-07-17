#ifndef TP_CORE_SRC_TP_DIFF_INTERNAL_H
#define TP_CORE_SRC_TP_DIFF_INTERNAL_H

/*
 * Semantic-diff internals shared across the diff TUs (entity copy/free +
 * collection primitives / per-op capture / inverse+redo apply / history stack) and
 * the oracle test. NOT a public header -- tests include it from src/ the same way
 * test_transaction.c includes tp_txn_internal.h.
 *
 * The diff is STATE-CAPTURE (docs/decisions/0012): per op we record the touched
 * entity's before/after DATA + ordering position, keyed by the effect class, so the
 * inverse restores the data directly (byte-exact under the array-order-sensitive
 * serializer). Every captured entity/string is OWNED by the diff record and freed
 * exactly once.
 */

#include <stdbool.h>

#include "tp_core/tp_diff.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_transaction.h"

/* ---- which collection an op touches -------------------------------------- */
typedef enum tp_diff_coll {
    TP_DIFF_COLL_ATLAS = 0, /* project->atlases            */
    TP_DIFF_COLL_SOURCE,    /* atlas->sources              */
    TP_DIFF_COLL_ANIM,      /* atlas->animations           */
    TP_DIFF_COLL_TARGET,    /* atlas->targets              */
    TP_DIFF_COLL_FRAME      /* anim->frames (needs anim_id) */
} tp_diff_coll;

/* ---- per-op diff shape (drives capture + inverse/redo) ------------------- *
 * Grouped by C0-02 §6 effect class. Only the shape's own fields are populated per
 * entry; every other owned pointer stays NULL so tp_diff_op_free is a safe no-op on
 * it (the tp_operation_free pattern). */
typedef enum tp_diff_shape {
    TP_DIFF_SHAPE_COLL = 0,      /* CREATE/REMOVE on a collection: entity + position + `created` */
    TP_DIFF_SHAPE_FRAME_MOVE,    /* MOVE a frame within an anim: from_index/to_index            */
    TP_DIFF_SHAPE_ATLAS_NAME,    /* SET atlas.rename: before/after name                          */
    TP_DIFF_SHAPE_ATLAS_KNOBS,   /* SET atlas.settings.set: before/after 10 knobs                */
    TP_DIFF_SHAPE_SOURCE_PATH,   /* SET source.replace: before/after path (entity_id=source_id)  */
    TP_DIFF_SHAPE_TARGET_FIELDS, /* SET target.set: before/after fields (entity_id=target_id)    */
    TP_DIFF_SHAPE_ANIM_SETTINGS, /* SET animation.settings.set: before/after knobs (anim_id)     */
    TP_DIFF_SHAPE_SPRITE_RECORD, /* SET sprite.override.set/.clear/.name.set: sparse record      */
    TP_DIFF_SHAPE_FRAMES_LIST,   /* SET animation.frames.set: before/after whole frames list     */
    TP_DIFF_SHAPE_ANIM_NAME      /* SET animation.rename: before/after name (anim_id)            */
} tp_diff_shape;

/* The 10 atlas packing knobs, captured as a value snapshot for atlas.settings.set. */
typedef struct tp_diff_knobs {
    int max_size, padding, margin, extrude, alpha_threshold, max_vertices, shape;
    bool allow_transform, power_of_two;
    float pixels_per_unit;
} tp_diff_knobs;

/* Animation settings snapshot for animation.settings.set. */
typedef struct tp_diff_anim_settings {
    float fps;
    int playback;
    bool flip_h, flip_v;
} tp_diff_anim_settings;

/* One committed op's reverse-apply record. The inverse/redo path switches on `shape`
 * (the effect class), not the op kind -- the op kind + class were dead reference/debug
 * fields and are dropped. */
typedef struct tp_diff_op {
    tp_diff_shape shape;

    tp_id128 atlas_id; /* parent atlas (for atlas.* ops, the atlas itself)  */
    tp_id128 anim_id;  /* FRAME coll / FRAME_MOVE / anim SET shapes          */
    tp_id128 entity_id;/* SOURCE_PATH=source_id, TARGET_FIELDS=target_id     */

    /* COLL shape: */
    tp_diff_coll coll;
    int position;      /* element index in the collection                    */
    bool created;      /* true: op CREATED the entity (undo removes, redo inserts);
                        * false: op REMOVED it (undo inserts, redo removes)  */
    void *elem;        /* owned deep copy of the created/removed element, typed by `coll` */

    /* FRAME_MOVE: actual endpoints (clamped, as apply moved them). */
    int from_index, to_index;

    /* ATLAS_NAME + ANIM_NAME (the anim addressed by anim_id): */
    char *name_before, *name_after; /* owned */

    /* ATLAS_KNOBS: */
    tp_diff_knobs knobs_before, knobs_after;

    /* SOURCE_PATH: */
    char *path_before, *path_after; /* owned */

    /* TARGET_FIELDS: */
    char *exporter_before, *out_before; /* owned */
    bool enabled_before;
    char *exporter_after, *out_after; /* owned */
    bool enabled_after;

    /* ANIM_SETTINGS: */
    tp_diff_anim_settings anim_before, anim_after;

    /* SPRITE_RECORD: the sparse override record before/after (create/remove/modify). */
    bool spr_before_present, spr_after_present;
    int spr_before_index, spr_after_index;
    tp_project_sprite spr_before, spr_after; /* value copies; own their name/src_key/rename */

    /* FRAMES_LIST: */
    tp_project_frame *frames_before; /* owned array + its strings */
    int frames_before_count;
    tp_project_frame *frames_after; /* owned array + its strings */
    int frames_after_count;
} tp_diff_op;

/* One committed transaction's diff = its ordered per-op diffs + metadata. */
typedef struct tp_diff_record {
    char *label;  /* owned; NULL when sparse   */
    char *author; /* owned; NULL when sparse   */
    tp_diff_op *ops;
    int op_count;
    int op_cap;
    int64_t revision; /* the revision this transaction produced (for a future DTO) */
    size_t bytes;     /* all owned allocations, including this record */
} tp_diff_record;

/* The in-memory undo/redo stack: records[0..pos-1] are applied (undoable),
 * records[pos..count-1] are the redo branch. */
typedef struct tp_history {
    tp_diff_record **records; /* owned pointer array */
    int count;
    int cap;
    int pos;
    size_t bytes; /* records[0..count), including the redo branch */
} tp_history;

typedef struct tp_history_push_plan {
    int drop_oldest;
    size_t final_bytes;
} tp_history_push_plan;

/* ---- diff-module allocation fault seam (test-only; default off / -1) ------ *
 * ONE global countdown covering every diff-module allocation (record/op-array/
 * label/author dup, captured-entity copies, string dups, inverse-apply element
 * inserts, and the history records-array grow). Set N so the (N+1)th diff
 * allocation returns NULL once, then re-arms to off. Lets the oracle prove (a) a
 * capture-alloc failure fails the whole commit cleanly (model byte-unchanged) and
 * (b) an inverse-apply alloc failure at any staging depth ROLLS BACK. */
void tp_diff__test_set_alloc_fail(int nth);
int tp_diff__test_alloc_count(void); /* allocations the LAST swept op requested */
void tp_diff__test_reset_alloc_count(void);

/* Raw diff allocator (fault-seam aware). Public within the diff module. */
void *tp_diff__alloc(size_t n);
char *tp_diff__dup(const char *s, bool *ok); /* NULL src -> NULL,*ok=true; real dup fail -> *ok=false */
/* One active semantic-record admission scope per thread. Every allocation owned
 * by that record goes through tp_diff__alloc and is rejected before calloc when
 * it would cross the byte limit. */
void tp_diff__record_budget_begin(size_t byte_limit);
bool tp_diff__record_budget_exceeded(void);
bool tp_diff__record_budget_end(size_t *bytes);

/* ---- element deep-copy / free + collection primitives (tp_diff_entity.c) -- */
void tp_diff__free_elem(tp_diff_coll coll, void *elem); /* frees an owned captured element */
void tp_diff__free_frames(tp_project_frame *frames, int count);
void tp_diff__free_sprite_fields(tp_project_sprite *s); /* frees name/src_key/rename */

/* Deep-copy the element at `src` (typed by coll) into a fresh owned copy (*out).
 * TP_STATUS_OOM on allocation failure (nothing leaked). */
tp_status tp_diff__copy_elem(tp_diff_coll coll, const void *src, void **out);
tp_status tp_diff__copy_frames(const tp_project_frame *src, int count, tp_project_frame **out);
tp_status tp_diff__copy_sprite_fields(const tp_project_sprite *src, tp_project_sprite *dst);

/* Insert a deep copy of `elem` into `coll` of `atlas`(/`anim` for frames) at
 * `index`, or remove the element at `index`. Bounds-checked (out-of-range ->
 * TP_STATUS_OUT_OF_BOUNDS). Insert allocates through the diff seam; a mid-insert
 * failure leaves a destroy-safe partial element in the array (the caller discards
 * the whole clone). */
tp_status tp_diff__insert_atlas(tp_project *p, int index, const tp_project_atlas *src);
tp_status tp_diff__remove_atlas(tp_project *p, int index);
tp_status tp_diff__insert_source(tp_project_atlas *a, int index, const tp_project_source *src);
tp_status tp_diff__remove_source(tp_project_atlas *a, int index);
tp_status tp_diff__insert_anim(tp_project_atlas *a, int index, const tp_project_anim *src);
tp_status tp_diff__remove_anim(tp_project_atlas *a, int index);
tp_status tp_diff__insert_target(tp_project_atlas *a, int index, const tp_project_target *src);
tp_status tp_diff__remove_target(tp_project_atlas *a, int index);
tp_status tp_diff__insert_frame(tp_project_anim *an, int index, const tp_project_frame *src);
tp_status tp_diff__remove_frame_at(tp_project_anim *an, int index);
tp_status tp_diff__insert_sprite(tp_project_atlas *a, int index, const tp_project_sprite *src);
tp_status tp_diff__remove_sprite_at(tp_project_atlas *a, int index);
tp_status tp_diff__replace_sprite_at(tp_project_atlas *a, int index, const tp_project_sprite *src);

/* ---- per-op capture (tp_diff_capture.c) ---------------------------------- *
 * capture_before reads the touched region of the PRE-apply clone into `e`; the op
 * is then applied; capture_after reads the POST-apply clone into `e`. `e` must be
 * zero-initialized. Either may allocate (TP_STATUS_OOM). On failure the caller frees
 * `e` (tp_diff_op_free) and discards the whole commit -- model byte-unchanged. */
tp_status tp_diff_capture_before(const tp_project *pre, const tp_operation *op, tp_diff_op *e);
tp_status tp_diff_capture_after(const tp_project *post, const tp_operation *op, tp_diff_op *e);

/* ---- inverse / redo apply (tp_diff_apply.c) ------------------------------ *
 * Apply one op-diff to `clone` -- reverse=true restores the BEFORE state (Undo),
 * reverse=false restores the AFTER state (Redo). Bounds/reference-checked: a stale
 * id or out-of-range index yields a structured status (NOT_FOUND/OUT_OF_BOUNDS),
 * never UB. */
tp_status tp_diff_op_apply(tp_project *clone, const tp_diff_op *e, bool reverse, tp_error *err);
/* Apply a whole record: reverse=true applies ops in REVERSE order (Undo), false in
 * forward order (Redo). First failing op returns its status (caller discards clone). */
tp_status tp_diff_record_apply(tp_project *clone, const tp_diff_record *r, bool reverse, tp_error *err);

/* ---- record + history lifecycle (tp_history.c) --------------------------- */
tp_diff_record *tp_diff_record_new(const char *label, const char *author, int op_cap);
void tp_diff_record_free(tp_diff_record *r);
void tp_diff_op_free(tp_diff_op *e);
/* Move an entry into the record (cap >= op_count guaranteed by op_cap; alloc-free). */
void tp_diff_record_push_op(tp_diff_record *r, tp_diff_op *e);

tp_history *tp_history_create(void);
void tp_history_destroy(tp_history *h);
/* Validate a record against the fixed budgets and calculate the redo/FIFO eviction
 * without mutating history. The prepared push is allocation-free and is performed
 * only after durable ACK. */
tp_status tp_history_prepare_push(const tp_history *h, const tp_diff_record *r, tp_history_push_plan *plan,
                                  tp_error *err);
/* Test-only: force the next prepare to return TP_STATUS_OOM once. */
void tp_history__test_fail_next_reserve(void);
/* Test-only limit override (zero values restore the production constants). */
void tp_history__test_set_limits(int max_steps, size_t max_bytes, size_t max_record_bytes);
size_t tp_history_record_byte_limit(void);
/* Apply the prepared eviction and push. Allocation-free; call only after ACK. */
void tp_history_push_prepared(tp_history *h, tp_diff_record *r, const tp_history_push_plan *plan);

tp_diff_record *tp_history_undo_record(tp_history *h); /* records[pos-1] or NULL (peek) */
tp_diff_record *tp_history_redo_record(tp_history *h); /* records[pos]   or NULL (peek) */
void tp_history_commit_undo(tp_history *h);            /* pos-- */
void tp_history_commit_redo(tp_history *h);            /* pos++ */

#endif /* TP_CORE_SRC_TP_DIFF_INTERNAL_H */
