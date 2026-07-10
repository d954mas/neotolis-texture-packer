#ifndef NTPACKER_GUI_HISTORY_H
#define NTPACKER_GUI_HISTORY_H

/* Snapshot-based undo/redo (ux.md §3.3c). A pure byte-stack over serialized project
 * snapshots (tp_project_save_buffer): the GUI's `touch` choke point pushes the
 * PRE-mutation snapshot here; undo/redo swap snapshots back into the live model
 * (done in gui_project). This module owns no model.
 *
 * Guards (in adoption order):
 *   - memcmp dedup: the CALLER only pushes when the model bytes actually changed.
 *   - coalescing: a push with the same non-zero action_tag within
 *     GUI_HISTORY_COALESCE_SECS of the previous push (same gesture / same-frame
 *     batch) folds into the existing top -- one undo entry per gesture -- but still
 *     clears the redo stack.
 *   - byte-budget ring (~32MB): oldest undo entries are dropped once the total
 *     exceeds the budget, so small projects get deep history and huge ones shallow.
 * Redo is cleared on any new push. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gui_history_init(void);
void gui_history_reset(void); /* drop both stacks (new / open / save-baseline) */
void gui_history_shutdown(void);

/* Push a PRE-mutation snapshot (bytes are copied). `action_tag` + `now` (seconds)
 * drive coalescing; pass action_tag 0 to force a distinct entry. */
void gui_history_push(const char *buf, size_t len, uint32_t action_tag, double now);

bool gui_history_can_undo(void);
bool gui_history_can_redo(void);

/* Undo: copies (cur_buf,cur_len) onto the redo stack, then pops the top undo
 * snapshot into out + out_len (malloc'd; caller frees). Returns false with
 * *out=NULL when there is nothing to undo. Redo mirrors it. */
bool gui_history_undo(const char *cur_buf, size_t cur_len, char **out, size_t *out_len);
bool gui_history_redo(const char *cur_buf, size_t cur_len, char **out, size_t *out_len);

int gui_history_undo_depth(void);
int gui_history_redo_depth(void);
size_t gui_history_bytes(void); /* total snapshot bytes held (both stacks) */

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_HISTORY_H */
