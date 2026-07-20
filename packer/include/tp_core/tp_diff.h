#ifndef TP_CORE_TP_DIFF_H
#define TP_CORE_TP_DIFF_H

/*
 * Semantic diff / exact inverse (Undo) + redo replay over a committed transaction,
 * and a minimal in-memory undo/redo history (master spec §9-9.5, §59 items
 * 15-17). Builds on the transaction engine (tp_transaction.h): a committed
 * transaction captures ONE compact SEMANTIC DIFF -- the per-op before/after data +
 * ordering position needed to invert it -- NOT a full project snapshot. The exact
 * inverse restores the pre-transaction state byte-for-byte; redo re-applies it.
 *
 * INVERSE REPRESENTATION -- state-capture, not inverse-operations (docs/decisions/0012):
 * each op's diff records the touched entity's before/after DATA + position (the
 * decision 0012 state-capture shapes: CREATE=after entity+position,
 * REMOVE=before entity+position,
 * MOVE=from/to index, SET=before/after field values); a dedicated diff-apply
 * restores the data directly. This is byte-exact under the array-order-sensitive
 * serializer (which the append-only op catalog cannot invert positionally) and
 * handles a coarse atlas.remove (whole subtree) naturally. The ORACLE (test_diff.c)
 * proves A->forward->B->inverse->A' is byte-identical to A and equals the legacy
 * full-snapshot restore.
 *
 * The session is the shipping owner of this primitive: it admits transactions,
 * exposes Undo/Redo, and records best-effort recovery transitions. Frontends never maintain a second
 * snapshot history. History remains bounded in-memory state; journal recovery
 * reconstructs the recoverable model rather than serializing these allocations.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_transaction.h" /* tp_model (the history attaches to it) */

#ifdef __cplusplus
extern "C" {
#endif

/* History is deliberately bounded session state. A transaction whose semantic
 * diff cannot fit the single-record budget is rejected before
 * model publication: committing an edit without its Undo record is forbidden.
 * Older undo steps are evicted FIFO at the live transaction commit. */
#define TP_HISTORY_MAX_STEPS 256
#define TP_HISTORY_MAX_BYTES (64U * 1024U * 1024U)
#define TP_HISTORY_MAX_RECORD_BYTES (32U * 1024U * 1024U)

/* ---- attach an in-memory undo/redo history to a model -------------------- *
 * Off by default: a model with no history behaves EXACTLY like the transaction engine (a committed
 * transaction captures nothing). Enabling it makes each subsequently committed
 * transaction capture a compact semantic diff and push it as one undoable step; a
 * NEW transaction applied after an Undo discards the redo branch. History is
 * in-memory session state (not serialized as a history stack). Idempotent
 * (a no-op if already enabled). Returns TP_STATUS_OOM if the stack cannot allocate.
 * Owned by the model; freed by tp_model_destroy. */
tp_status tp_model_enable_history(tp_model *m);
bool tp_model_has_history(const tp_model *m);

/* ---- queries ------------------------------------------------------------- */
bool tp_model_can_undo(const tp_model *m);
bool tp_model_can_redo(const tp_model *m);
int tp_model_undo_depth(const tp_model *m); /* undoable steps below the cursor */
int tp_model_redo_depth(const tp_model *m); /* redoable steps above the cursor */

/* Label / author of the transaction the NEXT Undo would reverse (the top undoable
 * record), or NULL when there is none / it was sparse. Borrowed (valid until the
 * next mutation). For a future history DTO (F3). */
const char *tp_model_undo_label(const tp_model *m);
const char *tp_model_undo_author(const tp_model *m);

/* ---- enumerable history records (the F3 visible-history spine) ------------ *
 * One undo/redo edit record, borrowed for enumeration. The in-memory stack stays
 * the single authority: the session projects these plus its non-undoable markers
 * into the shared visible-History DTO. Strings are borrowed and valid only until
 * the next model mutation; the session copies them out at its own boundary. */
typedef struct tp_model_history_entry {
    int64_t revision;           /* the revision this transaction produced */
    const char *label;          /* borrowed; NULL when sparse */
    const char *author;         /* borrowed; NULL when sparse (A6 passthrough) */
    const char *transaction_id; /* borrowed 32-hex id; "" when unknown/sparse */
    bool undoable;              /* index < position: reversible by the next Undo */
} tp_model_history_entry;

/* Total edit records retained (undoable records + the redo branch). */
int tp_model_history_count(const tp_model *m);
/* Cursor: the number of undoable records. records[0..position) are undoable,
 * records[position..count) are the already-undone redo branch. */
int tp_model_history_position(const tp_model *m);
/* Fills *out for records[index] (0-based, oldest first). false for a NULL model,
 * no history, or an out-of-range index (out is zeroed, transaction_id ""). */
bool tp_model_history_entry_at(const tp_model *m, int index,
                               tp_model_history_entry *out);

/* ---- exact inverse (Undo) + redo replay ---------------------------------- *
 * Undo reverses the most recently committed (or redone) transaction via its
 * captured semantic diff; Redo re-applies the next transaction on the redo branch.
 * STAGE-THEN-COMMIT (reuses the clone/swap): the inverse is applied to a CLONE
 * of the live model and swapped in only on FULL success, so an allocation failure
 * mid-inverse ROLLS BACK -- the live model is BYTE-UNCHANGED, the revision and the
 * history cursor unchanged. Each Undo/Redo that succeeds bumps the revision by
 * exactly 1 (a new committed state; dirty is recomputed from semantic identity, so
 * an Undo back to the saved baseline is clean even at a higher revision).
 * A recovery transition encode/append/sync failure occurs after that commit,
 * leaves Undo/Redo successful, and makes later recovery recording unavailable.
 *
 * A corrupted/hostile diff (a stale or unknown entity id, an out-of-range position)
 * yields a STRUCTURED error (TP_STATUS_NOT_FOUND / TP_STATUS_OUT_OF_BOUNDS), never a
 * crash / UB; the model is left byte-unchanged. Nothing to undo/redo -> a structured
 * TP_STATUS_NOT_FOUND (callers gate on tp_model_can_undo/redo). NULL model or no
 * history -> TP_STATUS_INVALID_ARGUMENT. */
tp_status tp_model_undo(tp_model *m, tp_error *err);
tp_status tp_model_redo(tp_model *m, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_DIFF_H */
