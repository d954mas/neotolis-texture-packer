#ifndef TP_CORE_TP_DIFF_H
#define TP_CORE_TP_DIFF_H

/*
 * Semantic diff / exact inverse (Undo) + redo replay over a committed transaction,
 * and a minimal in-memory undo/redo history (F2-03, master spec §9-9.5, §59 items
 * 15-17). Builds on the F2-02 transaction engine (tp_transaction.h): a committed
 * transaction captures ONE compact SEMANTIC DIFF -- the per-op before/after data +
 * ordering position needed to invert it -- NOT a full project snapshot. The exact
 * inverse restores the pre-transaction state byte-for-byte; redo re-applies it.
 *
 * INVERSE REPRESENTATION -- state-capture, not inverse-operations (docs/decisions/0012):
 * each op's diff records the touched entity's before/after DATA + position (the
 * C0-02 §6 shapes: CREATE=after entity+position, REMOVE=before entity+position,
 * MOVE=from/to index, SET=before/after field values); a dedicated diff-apply
 * restores the data directly. This is byte-exact under the array-order-sensitive
 * serializer (which the append-only op catalog cannot invert positionally) and
 * handles a coarse atlas.remove (whole subtree) naturally. The ORACLE (test_diff.c)
 * proves A->forward->B->inverse->A' is byte-identical to A and equals the legacy
 * full-snapshot restore.
 *
 * HONEST SCOPE (the F1-03/F2-01/F2-02 lesson -- see docs/decisions/0012): F2-03
 * builds and CORE-TESTS the diff/inverse ENGINE + a minimal history primitive. It
 * does NOT wire the GUI Undo/Redo shortcuts, save-checkpoint visibility, or
 * ownership (that SESSION/GUI behavior is F3-02), does NOT route the shipping
 * frontends (F2-05), and does NOT persist history across reopen/crash (F2-04
 * journal). The legacy GUI snapshot stack (apps/gui/gui_history.c) is left in place
 * untouched (not deleted, not rewired); the oracle reproduces its full-snapshot
 * restore via tp_project_save_buffer/tp_project_load_buffer as the comparison oracle.
 */

#include <stdbool.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_transaction.h" /* tp_model (the history attaches to it) */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- attach an in-memory undo/redo history to a model -------------------- *
 * Off by default: a model with no history behaves EXACTLY like F2-02 (a committed
 * transaction captures nothing). Enabling it makes each subsequently committed
 * transaction capture a compact semantic diff and push it as one undoable step; a
 * NEW transaction applied after an Undo discards the redo branch. History is
 * in-memory session state (not serialized, not crash-durable -- F2-04). Idempotent
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

/* ---- exact inverse (Undo) + redo replay ---------------------------------- *
 * Undo reverses the most recently committed (or redone) transaction via its
 * captured semantic diff; Redo re-applies the next transaction on the redo branch.
 * STAGE-THEN-COMMIT (reuses the F2-02 clone/swap): the inverse is applied to a CLONE
 * of the live model and swapped in only on FULL success, so an allocation failure
 * mid-inverse ROLLS BACK -- the live model is BYTE-UNCHANGED, the revision and the
 * history cursor unchanged. Each Undo/Redo that succeeds bumps the revision by
 * exactly 1 (a new committed state; dirty is recomputed from semantic identity, so
 * an Undo back to the saved baseline is clean even at a higher revision).
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
