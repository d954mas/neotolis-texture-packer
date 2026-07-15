#ifndef TP_CORE_TP_TRANSACTION_H
#define TP_CORE_TP_TRANSACTION_H

/*
 * Atomic multi-operation transactions, a canonical revision counter,
 * expected_revision optimistic concurrency, semantic-state dirty tracking,
 * transaction-id idempotency, and the versioned transaction request/result JSON
 * contract (F2-02, master spec §7-8, §9.1, §59 items 12-19). Promoted from the
 * accepted, 3-OS-green C0-02 spike (packer/spike/c0 tp_c0_txn / tp_c0_semantic /
 * tp_c0_ack), made REAL against the live tp_project model + the F2-01
 * tp_operation engine.
 *
 * ATOMICITY MECHANISM -- transactional CLONE (docs/decisions/0011): a transaction
 * deep-clones the project (tp_project_clone), applies its ops to the clone op-by-op
 * via the F2-01 tp_operation_apply, and only on FULL success swaps the clone into
 * place (freeing the old model) and bumps the revision exactly once. On ANY op
 * rejection or allocator failure the clone is discarded and the LIVE model is
 * byte-unchanged (§416). The commit point is an allocation-free pointer swap, which
 * is why the mechanism is provably atomic. It reuses F2-01 apply UNCHANGED and does
 * NOT compute per-op inverse diffs -- that is F2-03.
 *
 * HONEST SCOPE (the F1-03/F2-01 lesson -- see docs/decisions/0011): F2-02 builds and
 * CORE-TESTS the transaction engine. It does NOT route the shipping CLI/GUI mutators
 * (apps/cli/cli_mutate.c, apps/gui/gui_project.c) through it -- that FRONTEND CUTOVER
 * is F2-05. Nothing here claims a live frontend is wired. Per-op inverse diffs are
 * F2-03; the on-disk recovery journal is F2-04; the session/network protocol is F3.
 *
 * cJSON (engine-vendored) is a PRIVATE dependency confined to tp_txn_parse.c: it
 * never appears on this public header (request/result structs are hand-typed).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"
#include "tp_core/tp_journal.h"   /* F2-04 recovery journal (io seam + recovery types) */
#include "tp_core/tp_operation.h" /* the F2-01 typed op the batch carries + applies */
#include "tp_core/tp_project.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Current transaction JSON schema version -- the only version this build accepts. */
#define TP_TXN_SCHEMA 1

/* ---- idempotency retention store (pluggable interface, task 6) ------------ *
 * The transaction-id retention set is an INTERFACE so F2-04's on-disk journal can
 * back it later; F2-02 ships an in-memory default. Keys are the 32-lowercase-hex
 * transaction id. `contains` answers "already committed?"; `record` remembers a
 * just-committed id (only committed ids are recorded, so idempotency blocks exactly
 * the retries of applied transactions). `record` is transactional: on OOM it
 * returns non-OK WITHOUT recording, so the caller discards the clone and the model
 * stays byte-unchanged. */
typedef struct tp_txn_idstore {
    void *ctx;
    bool (*contains)(void *ctx, const char *id_hex);
    tp_status (*record)(void *ctx, const char *id_hex, tp_error *err);
    void (*destroy)(void *ctx); /* frees ctx; may be NULL for a borrowed store */
} tp_txn_idstore;

/* The in-memory default retention store (a growable hex-id set). Returns a store
 * whose ctx it owns; free the whole thing with the store's destroy hook (or via
 * tp_model_destroy when embedded in a model). NULL on OOM. */
tp_txn_idstore *tp_txn_idstore_memory_create(void);

/* Forward: the F2-03 in-memory undo/redo history (opaque here; tp_diff.h +
 * tp_history.c own it). NULL unless tp_model_enable_history is called. */
struct tp_history;

/* Forward: the F2-04 recovery journal (tp_journal.h) and the side-effect
 * coordinator (defined below). Both are opt-in, NULL by default -> exactly the
 * F2-02/F2-03 behavior. */
struct tp_journal;
struct tp_side_effect_coordinator;

/* ---- the model-state wrapper --------------------------------------------- *
 * project + canonical revision + saved-baseline identity + idempotency store. The
 * revision is RUNTIME: it is never serialized to the project file (§414) and starts
 * at 0. The wrapper OWNS the project (the atomic swap frees and replaces it). */
typedef struct tp_model {
    tp_project *project;      /* the authoritative live model (owned) */
    int64_t revision;         /* canonical monotonic counter; runtime, not persisted */
    tp_id128 saved_identity;  /* semantic identity of the saved baseline (dirty anchor) */
    bool recovered_unsaved;   /* F2-04 fix C5: a crash-recovered model whose committed state is
                               * ahead of the on-disk project file -> DIRTY until an explicit Save
                               * (tp_model_mark_saved) re-baselines it, regardless of identity. */
    tp_txn_idstore *idstore;  /* idempotency retention (owned unless borrowed) */
    bool owns_idstore;
    struct tp_history *history; /* F2-03 undo/redo (NULL unless enabled); owned. When
                                 * set, each committed transaction captures a semantic
                                 * diff (tp_diff.h). NULL => exactly the F2-02 behavior. */
    struct tp_journal *journal; /* F2-04 recovery journal (NULL unless attached); owned.
                                 * When set, a commit is not acknowledged until its record
                                 * is durably appended, and the journal's retained-id index
                                 * answers idempotency. NULL => exactly the F2-02/03 path. */
    struct tp_side_effect_coordinator *coordinator; /* F2-04 side-effect coordinator
                                 * (NULL unless set); BORROWED. Ties published side-effects
                                 * (B1 Extract) to the journal-gated commit. NULL => none. */
} tp_model;

/* Wrap an existing project (TAKES OWNERSHIP) in a model at revision 0 with a fresh
 * in-memory idstore; the saved baseline is the wrapped project's current identity
 * (so a freshly-wrapped model is clean). NULL on OOM (the project is NOT freed on
 * failure -- the caller still owns it). */
tp_model *tp_model_wrap(tp_project *project);

/* tp_model_wrap(tp_project_create()): a fresh one-atlas project wrapped in a model.
 * NULL on OOM. */
tp_model *tp_model_create(void);

/* Frees the model, its project, and its owned idstore. NULL-safe. */
void tp_model_destroy(tp_model *m);

/* The live project (borrowed; valid until the next committed transaction swaps it
 * or the model is destroyed). */
tp_project *tp_model_project(tp_model *m);

/* The canonical revision. Increments by exactly 1 per committed transaction; Save
 * does NOT change it (§420). */
int64_t tp_model_revision(const tp_model *m);

/* dirty = current semantic identity != saved-baseline identity (NOT derived from
 * revision). See tp_semantic_identity. */
bool tp_model_dirty(const tp_model *m);

/* Re-baselines the dirty anchor to the current identity WITHOUT changing the
 * revision (the "mark saved" that Save performs, §8/§420). */
void tp_model_mark_saved(tp_model *m);

/* ---- semantic-state identity (task 5) ------------------------------------ *
 * Deterministic, endian-stable 128-bit identity over the participating persistent
 * partition (see tp_semantic.c). Order-normalized for ID-keyed collections; an
 * animation's frames are order-semantic. Excludes all runtime fields, the project
 * path, and schema_version. NULL project -> a fixed empty identity. */
tp_id128 tp_semantic_identity(const tp_project *p);

/* ---- revision precondition (task 4) -------------------------------------- *
 * expected == current -> OK; expected < current -> TP_STATUS_REVISION_CONFLICT
 * (stale; rebuild & retry); expected > current -> TP_STATUS_INVALID_REVISION. No
 * CRDT/merge (§8). Fills `err` prose on a mismatch. */
tp_status tp_revision_check(int64_t expected_revision, int64_t current_revision, tp_error *err);

/* ---- transaction request (typed) ----------------------------------------- *
 * A transaction = a 32-hex idempotency id + an expected_revision precondition +
 * optional label/author + an ordered batch of F2-01 typed operations. The ops are
 * malloc-owned tp_operation values (free the whole request with tp_txn_request_free,
 * which frees each op's arms + the ops array + label/author). */
typedef struct tp_txn_request {
    int schema;                /* == TP_TXN_SCHEMA */
    char id_hex[33];           /* 32 lowercase hex + NUL (128-bit idempotency token) */
    int64_t expected_revision;
    char *label;               /* NULL when absent (sparse) */
    char *author;              /* NULL when absent (sparse) */
    tp_operation *ops;         /* dynamic; NO fixed cap (large batches allowed, §7) */
    int op_count;
} tp_txn_request;

void tp_txn_request_free(tp_txn_request *req);

/* ---- transaction result -------------------------------------------------- *
 * Committed: echoes each op's wire + addressing ids + the new revision. Rejected:
 * the collected errors (op_index -1 = envelope/revision level) + the UNCHANGED
 * revision. NO `diff` object -- computing before/after diffs from the live model is
 * F2-03. */
typedef struct tp_txn_error {
    int op_index;      /* -1 = envelope/revision level; >= 0 = operation index */
    tp_status code;
    char field[64];    /* offending closed-vocabulary field, or "" */
    char message[192];
} tp_txn_error;

/* Echoed addressing field on a committed op: an id (idk != INVALID) or a plain
 * string (idk == INVALID, e.g. src_key). */
typedef struct tp_txn_addr {
    char key[16];
    tp_id_kind idk;    /* INVALID => `str` holds a plain string value */
    tp_id128 id;
    char str[64];      /* src_key (bounded echo) when idk == INVALID */
} tp_txn_addr;

typedef struct tp_txn_result_op {
    char wire[64];
    tp_txn_addr addr[3];
    int addr_count;
} tp_txn_result_op;

typedef struct tp_txn_result {
    int schema;
    char transaction_id[33];
    bool committed;
    int64_t revision;          /* new if committed; unchanged if rejected */
    tp_txn_result_op *ops;     /* committed: echoed ops (dynamic) */
    int op_count;
    tp_txn_error *errors;      /* rejected: collected errors (dynamic) */
    int error_count;
} tp_txn_result;

void tp_txn_result_free(tp_txn_result *res);

/* ---- apply (tasks 1-4, the atomic core) ---------------------------------- *
 * Apply a typed transaction to the model. Order (docs/decisions/0011): idempotency
 * (a seen id -> duplicate_id, model unchanged) -> revision precondition (mismatch
 * rejects ALONE, op_index -1, before any per-op work) -> clone the model -> validate
 * + apply each op to the CLONE op-by-op via tp_operation_apply (so an op may depend
 * on an earlier op in the same batch) -> on FULL success record the id, swap the
 * clone in, and bump revision by exactly 1. On ANY rejection/allocator failure the
 * clone is discarded and the live model is BYTE-UNCHANGED.
 *
 * Returns TP_STATUS_OK on commit, else the first/short-circuit status; `*out` (if
 * non-NULL) is always filled (committed or rejected) and must be freed with
 * tp_txn_result_free. Never aborts on caller data. */
tp_status tp_model_apply(tp_model *m, const tp_txn_request *req, tp_txn_result *out, tp_error *err);

/* ---- F2-04 side-effect coordinator (task 5, spec §44.2 Extract) ----------- *
 * Ties external side-effects (B1 Extract's published PNG files) to a model
 * transaction so file publication and the model commit succeed/fail together. The
 * commit path drives it around the acknowledgement gate: prepare() runs after the
 * ops apply to the clone but BEFORE the gate (the durable journal append when a
 * journal is attached, else the in-memory id record); on an acknowledged commit
 * publish() makes staged side-effects live; on ANY rollback (gate failure after a
 * successful prepare) abort() discards them. This is the INTERFACE + a default
 * no-op; the full Extract binding is B1. */
typedef struct tp_side_effect_coordinator {
    void *ctx;
    /* Stage side-effects for `req` before the commit. Non-OK aborts the commit
     * (rollback, no acknowledgement). May be NULL (treated as OK). */
    tp_status (*prepare)(void *ctx, const tp_txn_request *req, tp_error *err);
    /* Called AFTER the transaction is durably journaled (acknowledged): make staged
     * side-effects live. Post-acknowledgement, so it must not fail the commit. May be NULL. */
    void (*publish)(void *ctx, const tp_txn_request *req);
    /* Called when the commit rolls back after a successful prepare: discard staged
     * side-effects. May be NULL. */
    void (*abort)(void *ctx, const tp_txn_request *req);
} tp_side_effect_coordinator;

/* The default no-op coordinator (all hooks succeed / do nothing). Useful as an
 * explicit "no side-effects" binding and as a test baseline. */
tp_side_effect_coordinator tp_side_effect_coordinator_noop(void);

/* Attach a BORROWED coordinator to the model (NULL clears it). Not owned; the
 * caller keeps it alive across commits. */
void tp_model_set_coordinator(tp_model *m, tp_side_effect_coordinator *c);

/* ---- F2-04 model <-> journal glue (tasks 2-4) ---------------------------- *
 * Attach an OWNED recovery journal to `m` (tp_model_destroy frees it). Writes an
 * initial CHECKPOINT capturing the current committed project state + revision so the
 * journal is self-sufficient for recovery. Fails INVALID_ARGUMENT if a journal is
 * already attached; on a checkpoint-write failure returns the journal status and does
 * NOT attach (the caller still owns `j`). Once attached, every committed transaction
 * appends to the journal before it is acknowledged (§7.1) and the journal's retained-id
 * index answers idempotency (§7.2). */
tp_status tp_model_attach_journal(tp_model *m, struct tp_journal *j, tp_error *err);

/* R3 (plan S18 R / spec §22.3): compact the attached recovery journal to a single fresh
 * CHECKPOINT capturing the model's CURRENT committed state + revision + retained-id set -- the
 * Save-window reset. Call it AFTER a durable Save so the journal's baseline == the just-saved
 * bytes and the unsaved replay window is zero (a later crash then recovers exactly the saved
 * state + any edits made after the Save), bounding the replay cost. A no-op (returns OK) when no
 * journal is attached. The checkpoint is proven to round-trip (the same guarantee as attach), and
 * the retained-id set is PRESERVED so an already-acknowledged id still de-duplicates post-Save
 * (§7.2). On failure the journal is left unchanged (it keeps its correct-but-larger log and still
 * recovers) -- Save callers should treat a failure as NON-fatal (the file is already written). */
tp_status tp_model_compact_journal(tp_model *m, tp_error *err);

/* R5b-1 (plan S18 R): forward recovery metadata {timestamp, path, name} to the attached journal
 * (tp_journal_set_metadata) so a startup scan can list this journal's crashed project by path + name +
 * time. `timestamp` is a caller-supplied unix-seconds value (core stays deterministic -- it never calls
 * time()); `path`/`name` are UTF-8 and may be empty (untitled project -> path ""), NULL is treated as "".
 * A no-op (returns OK) when no journal is attached (recovery off / journal-less), mirroring
 * tp_model_compact_journal. Non-fatal to the caller: a write failure returns the status for the soft
 * channel (metadata is informational -- it must never fail an edit or Save). NULL model -> INVALID_ARGUMENT. */
tp_status tp_model_set_recovery_metadata(tp_model *m, int64_t timestamp, const char *path, const char *name,
                                         tp_error *err);

/* R5b-2 fix [0]: true iff `m` currently owns an attached recovery journal (m->journal != NULL). A
 * journal is attached ONLY after its initial CHECKPOINT durably wrote (tp_model_attach_journal sets
 * m->journal after tp_journal_init_checkpoint succeeds), so this answers "is the model's committed state
 * durably backed by a live journal right now?". The GUI adopt path uses it to gate the delete of an
 * adopted crash-recovery SOURCE: if the fresh live journal failed to attach (journal-less), the source is
 * the only durable copy of the recovered work and MUST NOT be deleted. NULL model -> false. */
bool tp_model_has_journal(const tp_model *m);

/* Rebuild a model from a journal's backing store after a process restart (§7.1/§7.2,
 * §22.3). Creates a journal over `io` (TAKES OWNERSHIP of io) keyed by `key`, replays
 * checkpoint + transaction records, and on a usable recovery returns a model (*out)
 * whose project is the last good committed snapshot, whose revision is that record's
 * revision, and which OWNS a journal (over the same `io`, index seeded with the
 * retained ids) ready to continue appending -- so a post-restart retry of an
 * acknowledged transaction id de-duplicates. A torn/corrupt tail is truncated away
 * before continuing (never guessed). *info (may be NULL) reports how replay
 * classified the store. When nothing is recoverable (empty / bad-header / stale-key /
 * torn-first-record) *out is NULL and info->status says why -- the caller falls back
 * to loading the project file. Free the recovery info's snapshot is handled internally. */
tp_status tp_model_recover(tp_journal_io io, tp_id128 key, tp_model **out, tp_journal_recovery *info, tp_error *err);

/* ---- transaction request/result JSON contract (task 3, C0-02 §3) --------- *
 * Versioned envelope; `schema` = 1 the only accepted version. Byte-stable canonical
 * encoding (keys ascend; the discriminator "schema"/"op" first; 2-space indent, LF,
 * trailing newline; integral numbers with PRId64 and no decimal point, fractional
 * "%.9g" -- identical to the tp_project writer; label/author sparse-omitted).
 * Unknown-field policy = REJECT at envelope/transaction/operation levels. */

/* Decode a request envelope JSON into a typed request. STRUCTURAL/envelope faults
 * fail fast (malformed JSON, bad schema/version, missing/typed field, bad 32-hex id,
 * a number outside the range-checked +/-2^53 converter, an unknown envelope/
 * transaction key). Per-op SHAPE faults (unknown op, unknown field, malformed *_id)
 * are NOT raised here -- tp_model_apply_json collects them in stable order. On OK
 * *out is a heap request (free with tp_txn_request_free); on fault returns non-OK,
 * *out = NULL, and fills `err`. */
tp_status tp_txn_request_decode(const char *json, tp_txn_request **out, tp_error *err);

/* Canonical byte-stable encode of a typed request. Returns a heap NUL-terminated
 * string (caller frees), or NULL on OOM / an INVALID-kind op. */
char *tp_txn_request_encode(const tp_txn_request *req);

/* Canonical byte-stable encode of a result (committed or rejected). Heap string
 * (caller frees), NULL on OOM. */
char *tp_txn_result_encode(const tp_txn_result *res);

/* The one JSON entry point (the "CLI batch JSON golden" path): decode `json` ->
 * validate-all (structural fail-fast; then, in tp_model_apply, idempotency ->
 * revision short-circuit -> per-op shape collect-all -> semantic apply on the clone)
 * -> atomic apply. Fills `*out` (committed or rejected; free with tp_txn_result_free)
 * and mutates the model atomically on commit. Returns the apply status. */
tp_status tp_model_apply_json(tp_model *m, const char *json, tp_txn_result *out, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_TRANSACTION_H */
