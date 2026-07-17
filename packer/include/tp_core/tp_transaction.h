#ifndef TP_CORE_TP_TRANSACTION_H
#define TP_CORE_TP_TRANSACTION_H

/*
 * Atomic typed transactions with monotonic revisions, semantic dirty tracking,
 * bounded idempotency, history, and journal acknowledgement. Apply clones the
 * project, validates and applies the complete batch, then publishes with one
 * allocation-free pointer swap. Rejection leaves the live model byte-unchanged.
 * cJSON remains private to the request parser.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Current transaction JSON schema version -- the only version this build accepts. */
#define TP_TXN_SCHEMA 1

/* Request bytes and operation count are checked before JSON materialization,
 * lowering, or model cloning. The byte count excludes an optional trailing NUL. */
#define TP_TXN_MAX_REQUEST_BYTES (1U * 1024U * 1024U)
#define TP_TXN_MAX_OPS 4096

/* A committed transaction ID remains duplicate-protected while it is one of
 * the newest IDs in this deterministic FIFO retention window. */
#define TP_TXN_RETAINED_ID_CAP 4096

/* ---- idempotency retention store ----------------------------------------- *
 * Keys are 32-lowercase-hex transaction IDs. Only committed IDs are recorded;
 * a failed record rejects the candidate transaction before publication. */
typedef struct tp_txn_idstore {
    void *ctx;
    bool (*contains)(void *ctx, const char *id_hex);
    tp_status (*record)(void *ctx, const char *id_hex, tp_error *err);
    void (*destroy)(void *ctx); /* frees ctx; may be NULL for a borrowed store */
} tp_txn_idstore;

/* The in-memory default retention store uses the bounded binary window above.
 * Its fixed backing arrays are allocated at creation; record/lookup/eviction do
 * not allocate. Free it through the destroy hook or tp_model_destroy. */
tp_txn_idstore *tp_txn_idstore_memory_create(void);

/* Opaque history owned by tp_history.c. */
struct tp_history;

struct tp_journal;
struct tp_side_effect_coordinator;

/* ---- the model-state wrapper --------------------------------------------- *
 * project + canonical revision + saved-baseline identity + idempotency store. The
 * revision is RUNTIME: it is never serialized to the project file (§414) and starts
 * at 0. The wrapper OWNS the project (the atomic swap frees and replaces it).
 * Opaque: the field layout is private to the owning TUs (tp_txn_apply.c,
 * tp_txn_parse.c, tp_history.c) via tp_txn_internal.h. */
typedef struct tp_model tp_model;

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

/* The attached recovery journal (borrowed; NULL if none is attached). */
tp_journal *tp_model_journal(tp_model *m);

/* The attached Undo/Redo history (borrowed; NULL if history is not enabled via
 * tp_model_enable_history). */
struct tp_history *tp_model_history(tp_model *m);

/* The canonical revision. Increments by exactly 1 per committed transaction or
 * acknowledged Undo/Redo history transition; Save does NOT change it (§420). */
int64_t tp_model_revision(const tp_model *m);

/* dirty = current semantic identity != saved-baseline identity (NOT derived from
 * revision). See tp_semantic_identity. */
bool tp_model_dirty(const tp_model *m);

/* Re-baselines the dirty anchor to the current identity WITHOUT changing the
 * revision (the "mark saved" that Save performs, §8/§420). */
void tp_model_mark_saved(tp_model *m);

/* ---- semantic-state identity --------------------------------------------- *
 * Deterministic, endian-stable 128-bit identity over the participating persistent
 * partition (see tp_semantic.c). Order-normalized for ID-keyed collections; an
 * animation's frames are order-semantic. Excludes all runtime fields, the project
 * path, and schema_version. NULL project -> a fixed empty identity. */
tp_id128 tp_semantic_identity(const tp_project *p);

/* ---- revision precondition ----------------------------------------------- *
 * expected == current -> OK; expected < current -> TP_STATUS_REVISION_CONFLICT
 * (stale; rebuild & retry); expected > current -> TP_STATUS_INVALID_REVISION. No
 * CRDT/merge (§8). Fills `err` prose on a mismatch. */
tp_status tp_revision_check(int64_t expected_revision, int64_t current_revision, tp_error *err);

/* ---- transaction request (typed) ----------------------------------------- *
 * A transaction = a 32-hex idempotency id + an expected_revision precondition +
 * optional label/author + an ordered batch of typed operations. The ops are
 * malloc-owned tp_operation values (free the whole request with tp_txn_request_free,
 * which frees each op's arms + the ops array + label/author). */
typedef struct tp_txn_request {
    int schema;                /* == TP_TXN_SCHEMA */
    char id_hex[33];           /* 32 lowercase hex + NUL (128-bit idempotency token) */
    int64_t expected_revision;
    char *label;               /* NULL when absent (sparse) */
    char *author;              /* NULL when absent (sparse) */
    tp_operation *ops;         /* dynamic; admitted up to TP_TXN_MAX_OPS */
    int op_count;
} tp_txn_request;

void tp_txn_request_free(tp_txn_request *req);

/* ---- transaction result -------------------------------------------------- *
 * Committed: echoes each op's wire + addressing ids + the new revision. Rejected:
 * the collected errors (op_index -1 = envelope/revision level) + the UNCHANGED
 * revision. No serialized diff is exposed. */
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
    bool no_change;            /* accepted semantic no-op; no revision/history/journal */
    int64_t revision;          /* new if committed; unchanged if rejected */
    tp_txn_result_op *ops;     /* committed: echoed ops (dynamic) */
    int op_count;
    tp_txn_error *errors;      /* rejected: collected errors (dynamic) */
    int error_count;
} tp_txn_result;

void tp_txn_result_free(tp_txn_result *res);

/* ---- apply --------------------------------------------------------------- *
 * After request admission: duplicate check -> revision check -> clone -> ordered
 * validation/apply -> history and side-effect preparation -> acknowledgement gate
 * (durable when journal-backed) -> allocation-free publish. Any rejection or
 * allocation failure discards the candidate and leaves the live model unchanged.
 *
 * Returns TP_STATUS_OK on commit, else the first/short-circuit status; `*out` (if
 * non-NULL) is always filled (committed or rejected) and must be freed with
 * tp_txn_result_free. Never aborts on caller data. */
tp_status tp_model_apply(tp_model *m, const tp_txn_request *req, tp_txn_result *out, tp_error *err);

/* ---- side-effect coordinator --------------------------------------------- *
 * Optional hooks stage before acknowledgement, publish after its gate, and
 * abort on rollback. Full Extract binding remains separate. */
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

/* ---- model <-> journal glue ---------------------------------------------- *
 * Attach an OWNED recovery journal to `m` (tp_model_destroy frees it). Writes an
 * initial CHECKPOINT capturing the current committed project state + revision so the
 * journal is self-sufficient for recovery. Fails INVALID_ARGUMENT if a journal is
 * already attached; on a checkpoint-write failure returns the journal status and does
 * NOT attach (the caller still owns `j`). Once attached, every committed transaction
 * appends to the journal before it is acknowledged (§7.1) and the journal's retained-id
 * index answers idempotency (§7.2). */
tp_status tp_model_attach_journal(tp_model *m, struct tp_journal *j, tp_error *err);

/* After a durable Save, compact to one checkpoint containing the current state,
 * revision, and retained IDs. This bounds replay while preserving idempotency.
 * With no journal this is a no-op. Pre-truncate failure preserves the old log;
 * replacement failure after truncate poisons the journal so later edits fail
 * closed. Neither failure rolls back the already completed Save. */
tp_status tp_model_compact_journal(tp_model *m, tp_error *err);

/* Record recovery metadata on the attached journal. The caller supplies time;
 * core remains deterministic. Missing strings become empty and no journal is a
 * no-op. Durable failures are returned so the host can retire stale recovery
 * authority without rolling back an already completed edit or Save. */
tp_status tp_model_set_recovery_metadata(tp_model *m, int64_t timestamp, const char *path, const char *name,
                                         tp_error *err);
tp_status tp_model_set_recovery_metadata_ex(tp_model *m, int64_t timestamp, const char *path, const char *name,
                                            const tp_id128 *file_fingerprint, tp_error *err);

/* True only after the journal's initial checkpoint is durable. Recovery-source
 * deletion must be gated on this; otherwise it may be the only durable copy.
 * NULL model -> false. */
bool tp_model_has_journal(const tp_model *m);

/* Stop using and destroy the attached recovery journal, if any. The backing store is
 * not deleted: storage ownership belongs to the caller that created the journal I/O.
 * Used when recovery authority can no longer be kept current; subsequent edits remain
 * valid but run without crash recovery. NULL-safe. */
void tp_model_detach_journal(tp_model *m);

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

/* ---- transaction request/result JSON contract --------------------------- *
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

/* Length-aware request decode for untrusted/buffered input. `json[0..json_len)`
 * need not be NUL-terminated. Rejects more than TP_TXN_MAX_REQUEST_BYTES before
 * invoking cJSON and rejects more than TP_TXN_MAX_OPS before lowering/materializing
 * the typed operation array. The legacy C-string entry point above applies the
 * same limits and delegates here. */
tp_status tp_txn_request_decode_n(const char *json, size_t json_len, tp_txn_request **out, tp_error *err);

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

/* Length-aware twin of tp_model_apply_json. Applies the same pre-materialization
 * byte/op limits as tp_txn_request_decode_n; over-limit input leaves model state,
 * revision, history and idempotency state unchanged and returns a structured
 * TP_STATUS_OUT_OF_BOUNDS result. */
tp_status tp_model_apply_json_n(tp_model *m, const char *json, size_t json_len,
                                tp_txn_result *out, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_TRANSACTION_H */
