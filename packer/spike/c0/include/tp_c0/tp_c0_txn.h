#ifndef TP_C0_TXN_H
#define TP_C0_TXN_H

/*
 * C0-02 tasks 3, 5, 6: the versioned transaction request/result JSON contract,
 * the revision precondition, and the before/after diff shapes.
 *
 * Master spec §7 (a transaction = transaction_id + expected_revision + label +
 * author + ordered operations; resolve selectors, validate the whole batch,
 * apply atomically, one revision, one Undo entry, one commit event), §7.2
 * (idempotent transaction ids in a retention window), §8 (expected==current ->
 * commit; expected<current -> revision_conflict; expected>current ->
 * invalid_revision), §9 (semantic diffs with before/after), §20.3 (a compact
 * project.apply(transaction) endpoint), §59 items 9-19, 52.
 *
 * This is a parser/encoder SKELETON: it round-trips the versioned JSON
 * byte-for-byte and pins the fault vocabulary + validation ordering; it does NOT
 * mutate a project model (no engine). Diffs are contract-shaped fixtures.
 */

#include <stdbool.h>
#include <stdint.h>

#include "tp_c0/tp_c0_error.h"
#include "tp_c0/tp_c0_op.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Current transaction JSON schema version (the only one this build accepts). */
#define TP_C0_TXN_SCHEMA 1

/* Spike caps (F2 uses dynamic storage). Kept small so a whole tp_c0_txn_result
 * -- which embeds per-op before/after diff snapshots -- stays a few hundred KB,
 * not stack-blowing megabytes. */
#define TP_C0_STR_CAP 160
#define TP_C0_MAX_FIELDS 16
#define TP_C0_MAX_OPS 32
#define TP_C0_MAX_ERRORS 32
#define TP_C0_MAX_ADDR 6 /* max echoed `*_id` addressing fields on a result op */

/* ---- typed field value (closed value vocabulary) ------------------------- */

/* An `*_id`/`key`/`name`/`exporter_id`/`out_path` value decodes as STR; validate
 * checks id-ness by key name. A JSON number decodes as INT only when it is
 * integral AND within the exactly-representable range +/-2^53 (9007199254740992);
 * outside that (or fractional/inf/NaN) it is NUM, so classification is UB-free and
 * byte-identical across OS. `frames`/`fields` are STR_ARRAY. */
typedef enum tp_c0_val_kind {
    TP_C0_VAL_INT = 0,
    TP_C0_VAL_NUM,
    TP_C0_VAL_BOOL,
    TP_C0_VAL_STR,
    TP_C0_VAL_STR_ARRAY
} tp_c0_val_kind;

typedef struct tp_c0_val {
    tp_c0_val_kind kind;
    int64_t ival; /* INT: integral value in +/-2^53, width-stable across OS */
    double nval;
    bool bval;
    char sval[TP_C0_STR_CAP];
    char **items; /* STR_ARRAY: heap array of heap strings (owned) */
    int item_count;
} tp_c0_val;

typedef struct tp_c0_field {
    char key[64];
    tp_c0_val val;
} tp_c0_field;

/* ---- one operation (request, canonical ID-only form) --------------------- */

typedef struct tp_c0_op {
    tp_c0_op_kind kind;                    /* INVALID if wire was unknown */
    char wire[64];                         /* raw "op" string as decoded */
    tp_c0_field fields[TP_C0_MAX_FIELDS];  /* canonical fields incl `*_id` addressing */
    int field_count;
    bool has_selector;                     /* an unresolved `selector` key was present */
} tp_c0_op;

/* ---- transaction request ------------------------------------------------- */

typedef struct tp_c0_txn_request {
    int schema;
    char id_hex[33];        /* 32 lowercase hex + NUL (128-bit idempotency token) */
    int64_t expected_revision;
    char label[TP_C0_STR_CAP];
    char author[TP_C0_STR_CAP];
    tp_c0_op ops[TP_C0_MAX_OPS];
    int op_count;
} tp_c0_txn_request;

/* Structural decode of a request envelope. Returns NULL and sets *detail (+ err
 * prose) on a structural/envelope fault (bad JSON, bad/absent schema, missing
 * required field, wrong type, too many ops). Per-op SEMANTIC faults (unknown op,
 * unknown field, malformed id, selector) are NOT raised here -- they are
 * collected in stable order by tp_c0_txn_validate. Caller frees with
 * tp_c0_txn_request_free. */
tp_c0_txn_request *tp_c0_txn_request_decode(const char *json, tp_c0_detail *detail, tp_error *err);
void tp_c0_txn_request_free(tp_c0_txn_request *req);

/* Canonical byte-stable encode ("op"/"schema" first, then ascending keys, 2-space
 * indent, LF, trailing newline). Returns a heap string (caller frees) or NULL on
 * OOM / if any op still carries an unresolved selector (detail=selector_unresolved:
 * canonical form is ID-only). */
char *tp_c0_txn_request_encode(const tp_c0_txn_request *req, tp_c0_detail *detail);

/* ---- before/after diff (task 6) ------------------------------------------ */

/* A committed operation's reverse-apply record. CREATE: has_after + position.
 * REMOVE: has_before + position. MOVE: has_indices. SET: has_before + has_after. */
typedef struct tp_c0_diff {
    tp_c0_op_class cls;
    bool has_before, has_after, has_indices, has_position;
    tp_c0_field before[TP_C0_MAX_FIELDS];
    int before_count;
    tp_c0_field after[TP_C0_MAX_FIELDS];
    int after_count;
    int before_index, after_index; /* MOVE */
    int position;                  /* CREATE/REMOVE ordering position */
} tp_c0_diff;

/* ---- transaction result -------------------------------------------------- */

typedef struct tp_c0_txn_error {
    int op_index; /* -1 = envelope-level (revision/schema/id); >=0 = operation index */
    tp_c0_detail code;
    char message[TP_C0_STR_CAP];
} tp_c0_txn_error;

typedef struct tp_c0_result_op {
    char wire[64];
    tp_c0_field addr[TP_C0_MAX_ADDR]; /* echoed `*_id` addressing fields */
    int addr_count;
    tp_c0_diff diff;
} tp_c0_result_op;

typedef struct tp_c0_txn_result {
    int schema;
    char txn_id_hex[33];
    bool committed;    /* true: committed (ops+diffs); false: rejected (errors) */
    int64_t revision;  /* new revision if committed; unchanged if rejected */
    tp_c0_result_op ops[TP_C0_MAX_OPS];
    int op_count;
    tp_c0_txn_error errors[TP_C0_MAX_ERRORS];
    int error_count;
} tp_c0_txn_result;

tp_c0_txn_result *tp_c0_txn_result_decode(const char *json, tp_c0_detail *detail, tp_error *err);
char *tp_c0_txn_result_encode(const tp_c0_txn_result *res, tp_c0_detail *detail);
void tp_c0_txn_result_free(tp_c0_txn_result *res);

/* ---- revision precondition (task 5) -------------------------------------- */

/* expected == current -> OK; expected < current -> revision_conflict;
 * expected > current -> invalid_revision. Checked against the WHOLE batch before
 * any application; a mismatch rejects the batch on its own (spec §8). */
tp_c0_detail tp_c0_revision_check(int64_t expected_revision, int64_t current_revision, tp_error *err);

/* ---- idempotency retention set (task 3 / §7.2) --------------------------- */

#define TP_C0_IDSET_CAP 256

typedef struct tp_c0_txn_idset {
    char ids[TP_C0_IDSET_CAP][33];
    int count;
} tp_c0_txn_idset;

/* Record a txn id in the retention set. Returns txn_duplicate_id (without adding)
 * if it is already present; txn_bad_id if not 32 lowercase hex; OK otherwise. */
tp_c0_detail tp_c0_txn_idset_add(tp_c0_txn_idset *set, const char *id_hex, tp_error *err);

/* ---- full-batch validation (task 5, ties tasks 1/2 together) ------------- */

/* Validate a decoded request against `current_revision` and the live `entities`
 * table (for selector/id-reference checks), producing a result:
 *   1. revision precondition (short-circuit: a mismatch rejects alone);
 *   2. per-op semantic checks -- unknown op, unknown field, malformed `*_id`,
 *      unresolved/ambiguous selector -- COLLECTED in stable order (op_index asc,
 *      then the op's field order) before any apply.
 * On success `*out` is a committed stub (revision incremented, no diffs computed:
 * diffs are engine work). Never aborts. */
tp_c0_detail tp_c0_txn_validate(const tp_c0_txn_request *req, int64_t current_revision,
                                const tp_c0_entity_ref *entities, int entity_count, tp_c0_txn_result *out,
                                tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_TXN_H */
