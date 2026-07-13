# 0015 — F2-05b GUI transaction cutover (b-i: mutations → journal-LESS transactions)

**Date:** 2026-07-13
**Status:** accepted (lead review pending)
**Decided by:** F2-05b-i implementation agent (delegated), lead review pending
**Implemented in:** `apps/gui/gui_project.c` (mutators → typed ops on a journal-less
`tp_model`; model-owns-project lifecycle), `apps/gui/gui_actions.c`/`.h` (end-of-frame
deferred-edit queue + rename-on-Enter deferral + op-reject surfacing), `apps/gui/gui_view_settings.c`,
`apps/gui/gui_view_chrome.c`, `apps/gui/gui_view_lists.c` (declare fns enqueue instead of
commit), `apps/gui/gui_selftest.c` (3 introduced UAF re-fetches), `apps/gui/main.c`
(`--parity` byte-parity dev seam), `scripts/check_boundaries.sh` (GUI rule R7). Master spec
§4.1–4.2, §7–8, §59. Plan §7 F2-05 (GUI half of tasks 1,2,4,5).

## Lead split: F2-05b = b-i (this) + b-ii (later)

The lead split the GUI cutover in two reviewable halves:

- **b-i (THIS):** every `gui_project_*` mutator builds typed `tp_operation`(s) and commits
  through a **journal-LESS** `tp_model` (F2-02 clone-swap; `journal==NULL`, `history==NULL`);
  the GUI's inline value-range validation authority moves into core (the ops); the
  pointer-invalidation UAF class is fixed; drag COALESCING (undo depth 1) is preserved; a GUI
  boundary rule is added. The **existing 32 MB snapshot undo (`gui_history.c`) is KEPT and
  UNCHANGED** — because the model is journal-less, snapshot-undo stays coherent (undo reloads a
  snapshot into a project and the model is re-wrapped around it).

- **b-ii (NOT done here, explicitly out of scope):** attach a live recovery journal to the GUI
  model; append-fail UX + crash recovery; replace the snapshot undo with the F2-03 diff history
  (undo→history); remove the 32 MB snapshot path; the D2 measurement. None of these were
  touched. `gui_history.c` is byte-for-byte the pre-cutover file.

## The model-owns-project lifecycle

`s_proj` (the GUI's single `tp_project*`) becomes a **read view** of `tp_model_project(s_model)`.
A journal-less `tp_model` is created when a project is loaded/created and **owns** the project:

- `gui_project_init` / `_new`: `tp_project_create` + `seed_default_target` (lifecycle, direct —
  owns the exporter id; same as the CLI `do_new`) → `tp_model_wrap` (takes ownership) → refresh
  the `s_proj` view → snapshot baseline.
- `gui_project_open`: `tp_project_load` → `tp_model_wrap` → refresh → snapshot baseline.
- Each committed transaction clone-swaps: `m->project` becomes a NEW pointer, the OLD project is
  freed. The mutator refreshes `s_proj = tp_model_project(m)`, then calls `gui_project_touch(act)`
  — the UNCHANGED snapshot/dirty/coalesce choke point.
- **Undo/redo (snapshot path, kept):** `restore_from_buffer` loads the snapshot bytes into a new
  project and **re-wraps** the model around it (`tp_model_destroy` old + `tp_model_wrap` new). The
  model always wraps the live project, so a subsequent transaction clones the restored state.
- §5.5 id-promotion at save (`ensure_ids`→`tp_project_promote_ids`) is unchanged; dirty/stale
  semantics are byte-identical (still snapshot-memcmp vs the last-saved snapshot). The model's own
  revision/identity/dirty are **not consulted** by the GUI in b-i (snapshot dirty is authoritative);
  they exist only to drive the clone-swap.

Transaction id: a monotonic counter formatted as 32 lowercase hex (unique per commit; the model
persists across edits, so a fixed id would trip idempotency `duplicate_id`). `expected_revision =
tp_model_revision(m)` (single-threaded model edits — the pack worker never touches the model — so it
always matches; no `REVISION_CONFLICT`). Idempotency retention grows one 33-byte id per commit
(negligible vs the 32 MB undo; b-ii's journal-backed idstore supersedes it).

## Hazard 1 — POINTER INVALIDATION: architecture lever = **(B) defer to end-of-frame**

A commit frees the old project, so any `atlas`/`sprite`/`anim`/`target` pointer a `declare_*`
render fn holds ACROSS a commit dangles for the rest of the frame. Two safe patterns exist:

- **(A) re-fetch by index after every mutator** — local, but must be applied at ~30 sites and at
  every captured pointer (`a`, `ov`, `an`, `t`, `proj`), and silently regresses the instant a
  future widget is added after a commit without a re-fetch.
- **(B) defer all declare-fn model edits to the existing end-of-frame `apply_pending` queue.** No
  `declare_*`/render fn ever commits; edits are enqueued and committed at frame top (in
  `apply_pending`) where **no** live declare-fn pointer is held. **CHOSEN.**

Why B (the higher-leverage design, brief-recommended):

1. **Structurally collapses the entire UAF class.** Because nothing swaps mid-frame, every
   `a`/`ov`/`an`/`t` a declare fn fetches is valid for the *whole* frame (the model is swapped only
   between frames). The fix is uniform (no per-site re-fetch) and cannot regress by adding a widget.
2. **Uniform with the existing GUI.** Adds/removes/dialogs were ALREADY deferred via `s_pending_*`
   flags drained in `apply_pending`; the inline settings edits were the last holdouts. B makes the
   whole GUI mutation model uniform (everything commits in `apply_pending`).
3. **Natural coalescing home** (see Hazard 2).

Mechanism: a small growable **deferred-edit queue** in `gui_actions.c`. Each `declare_*` edit calls
a thin `gui_edit_*` enqueuer (capturing the atlas INDEX + typed args + copied strings — never a
pointer). `apply_pending` drains the queue at frame top by calling the self-contained
`gui_project_*` setters (which re-fetch by index/name internally). Rename-on-Enter is likewise
deferred (a `s_pending_commit_edit_enter` flag → `commit_active_edit(false)`).

**Same-frame read-after-write audit (the B caveat):** every deferred effect is visible only next
frame. Audited flows: (a) *add-then-select* (add atlas/anim) is ALREADY sequential inside
`apply_pending` (add returns the index, then select), so no cross-frame dependency; (b) a settings
widget reads the model value to seed itself — with deferral it reads a one-frame-stale value, which
self-corrects the next frame (the widget shows its OWN in-flight value during a drag — slider `v`,
focused text buffer — so there is no visible lag). No flow requires a same-frame read of its own
write, so pure B suffices (no A fallback was needed).

The 8 surveyed UAF sites and how each is now safe (all under B):

1. `gui_view_settings.c:840 declare_right_panel` — `a` passed live to 4 subsections. Subsections
   never commit now → `a` valid the whole frame.
2. `declare_atlas_settings(a)` — the 10 `a->field=…; touch_setting()` writes become
   `gui_edit_atlas_*(atlas, field, v)` enqueues (no write to `a`, no commit).
3. `declare_animation_editor(a)` — `an=&a->animations[…]` held across fps/playback/flip/frame edits,
   which are now enqueues; `an` is read-only for the frame.
4. `declare_export_targets(a)` — `t=&a->targets[ti]` held across target edits, now enqueues.
5. `declare_export_modal (chrome)` — `p`/`a`/`t` held across the target toggle, now an enqueue.
6. context-menu `CTX_TARGET (chrome)` — the toggle is an enqueue (was already low-risk).
7. `declare_left_panel` — inline `commit_atlas_rename`/`commit_anim_rename` on Enter now set the
   deferred-commit flag; `proj`/`a` are never dereferenced across a swap.
8. `gui_actions.c:204 current_anim()` — returns `&a->animations[s_sel_anim]` (fetch-by-index); its
   only holder `update_preview` runs during render with NO intervening swap (swaps are frame-top
   only), so it is safe by construction under B. Left fetch-by-index (already is).

Selection/edit state was already index/name-based (`s_sel_*`, `s_multi_sel[]`, `s_ctx_*`,
`s_rows[]`), so the swap cannot dangle it (unchanged).

## Hazard 2 — COALESCING (undo depth for a drag = 1)

`gui_history.c` (coalesce key = action tag, window = 0.30 s) is **UNCHANGED**. Each drained edit
calls the setter, which refreshes `s_proj` and calls `gui_project_touch(act)` — the same snapshot +
memcmp-dedup + coalesce path the pre-cutover inline mutation used. A drag emits one widget value per
frame → one enqueue per frame → one commit per frame → one `touch(GUI_ACT_SET_SETTING)` per frame.
Consecutive same-tag pushes within 0.30 s coalesce into a single undo entry. So **one committed
transaction per frame maps 1:1 to the pre-cutover one snapshot per frame, and undo depth for a drag
stays exactly 1** — proven by the selftest's undo-depth logging (a multi-frame slider drag adds one
undo step). Reducing transaction count per drag (buffer-till-drag-end) is a b-ii optimization (it
only matters once each commit is a journal append); it is NOT needed for b-i correctness.

## Validation moved to core

Value-RANGE authority is now core's on every commit (`tp_operation_validate` inside the ops:
`atlas.settings.set` knob ranges, `animation.settings.set` fps, sprite-override ranges) — the same
authority the F2-05a CLI cutover established. The GUI keeps ONLY widget parse+clamp (the input
fields clamp to the field domain, exactly as the CLI keeps parse) and two **client selection
policies** that core does not enforce and that preserve the exact editor UX:

- atlas name validity/uniqueness (`atlas_name_valid` in `gui_actions.c`) — kept (core also rejects a
  duplicate atlas name; the GUI keeps its check for the specific editor message + keep-editing UX);
- animation name uniqueness (`gui_project_set_anim_id`) — **required** in the client because core has
  no animation-rename op at all (see below), so there is nothing in core to enforce it.

On a core reject the setter surfaces the structured status to the status-bar soft-error channel
(`gui_project_take_op_error`, drained in `poll_async`) and does NOT mutate (the clone-swap discards,
model byte-unchanged) — mirroring the F2-05a reject contract. In practice the widgets clamp to valid
ranges, so a reject never fires on normal input.

## The one mutator without an op (honest gap): animation rename

The F2-01 operation catalog has NO `ANIMATION_RENAME` op (the CLI has no `anim rename` verb either —
animations are name-keyed; the id/name split keeps the structural id stable). The GUI's animation
"Id" rename therefore CANNOT be expressed as an op in b-i. `gui_project_set_anim_id` keeps its
direct `an->name =` write (annotated `boundary-ok:`) — coherent because the model is journal-less
(the direct write mutates `m->project`; `touch` snapshots it; the next transaction clones it). This
is a pre-existing core-catalog gap, not a b-i regression; adding a core `ANIMATION_RENAME` op is
F2-01 scope and is left for a follow-up. Every OTHER `gui_project_*` mutator routes through an op.

## Byte-identity

Saved `.ntpacker_project` bytes for an equivalent edit sequence are byte-identical to pre-cutover:
the GUI's ops apply the identical fields the inline mutators did (the same core `tp_op_apply` the
byte-identity-proven F2-05a CLI cutover uses), the canonical writer is unchanged, created-entity ids
are random either way (not golden-pinned), and re-save is idempotent. Verified by a scripted
`--parity` dev seam (a fixed non-creating edit sequence on a fixed-id project) diffed against the
byte-identity-proven CLI oracle for the same logical edits.

## Boundary rule (R7, mirrors the CLI's R6)

`scripts/check_boundaries.sh` R7 bans, in the GUI mutation-surface files (`gui_project.c`,
`gui_view_settings.c`, `gui_view_lists.c`, `gui_view_chrome.c`, `gui_actions.c`), the inline
`tp_project_*` mutators the ops replaced (R7a), direct writes into the loaded project's arrays
(R7b), and writes through the `a`/`an`/`t`/`ov`/`proj` aliases into it (R7c) — with a self-test that
proves each detector fires on a seeded violation and does not false-positive on op-payload / alias-read
/ lifecycle forms. `gui_selftest.c` (a dev-seam test harness that drives internal states, like the
CLI tests) stays excluded, as it already is for R1–R3. `tp_project_atlas_seed_default_target`,
`tp_project_promote_ids`, and the annotated animation-rename are the sanctioned lifecycle/gap
exceptions.

## F2-05b-i FIX pass (adversarial-review corrections, 2026-07-13)

An 18-agent adversarial review of the cutover (battery-green) found 9 defects — 4 real
correctness regressions. The "approach B structurally closes the UAF class" claim above was
INCOMPLETE: one edit site still committed mid-declare. All are fixed on top of the cutover
commit; no gate regressed.

### F1 — the LAST synchronous commit (a real UAF on "Add frames")

The migration deferred every declare-fn edit EXCEPT `add_selection_frames_to_anim` →
`gui_project_anim_add_frames`, which still committed SYNCHRONOUSLY from inside
`declare_animation_editor`. That declare invocation holds `an = &a->animations[s_sel_anim]` and
keeps dereferencing it AFTER the call (`an->frame_count`, the frame-row loop `an->frames[fi].name`)
— the commit clone-swaps + frees the project under `an`, so a plain "Add frames" click read freed
memory. Hazard-1 table row 3 was wrong: the anim editor was NOT enqueue-only.

**Fix:** route "Add frames" through the SAME deferred queue as every other panel edit. A new
`GEDIT_ANIM_ADD_FRAMES` enqueuer (`gui_edit_anim_add_frames`) captures the atlas index + anim
selector + a HEAP-COPIED array of the selected sprite keys; `apply_pending` drains it next frame
(no live pointer held) via `gui_project_anim_add_frames`. `add_selection_frames_to_anim` now builds
the sorted selection (read-only) and enqueues instead of committing. Re-audited: a project-wide grep
of the `gui_view_*.c` declare/render fns confirms this was the ONLY remaining synchronous commit —
every other mutation already routes through `gui_edit_*` or an `s_pending_*` flag. **Proof:** a new
`gui_selftest.c` case enqueues, asserts the frame_count is still 0 BEFORE the drain (a synchronous
commit would make it 2 → the assert fails), clears the live multi-selection, then drains and asserts
the 2 copied frames landed natural-sorted — so the UAF cannot regress silently.

### F2 — a target toggle truncated a long out_path to 255

`gui_edit_target` copied `t->out_path` into a fixed 256-byte queue slot (`e.s1[256]`, `snprintf`),
but `out_path` is stored as a heap `char*` up to `TP_PATH_MAX` (4096) and is reachable from a
loaded/CLI-authored project or the Browse dialog. A mere enable/disable / exporter-change click
silently truncated + persisted a corrupted export path. **Fix:** the queued `out_path` is now a
HEAP `edit_strdup` copy (freed after the drain via `edit_dispose`), carrying the full string with no
cap. **Audit of every `gui_edit_*` string arg:** `exporter_id` comes from the exporter registry
(short constant — safe); the sprite-name slot matches the GUI-wide 256-byte key assumption used
throughout the panels (`char[224]`/`char[256]`) and the setter re-fetches by key (a truncated key
no-ops, never corrupts) — only `out_path` could exceed its slot, and only it was heap-lifted.
**Proof:** a new selftest builds a 464-char out_path, toggles `enabled` through the deferred
`gui_edit_target`, drains, and asserts the stored path is byte-identical.

### F3 — `wrap_model` freed the open project BEFORE wrapping the replacement

`wrap_model` called `tp_model_destroy(s_model)` (freeing the current model+project) and nulled
`s_proj` BEFORE `tp_model_wrap(p)`, so an OOM in the wrap left `s_proj`/`s_model` NULL — the open
project lost and the next frame NULL-derefs (`declare_atlas_list` reads `proj->atlas_count`
unguarded). **Fix:** COMMIT-THEN-REPLACE (mirrors the F1-00 Save-As rollback) — wrap the replacement
into a TEMP first; only on success destroy the old model and swap it in; on wrap failure free `p`
and keep the CURRENT model+project intact. `wrap_model` now returns `bool`; `restore_from_buffer`
(undo/redo), `gui_project_new` (now returns `bool`), and `gui_project_open` check that return
instead of `!s_proj`. New/open surface a structured OOM error ("current project kept"); undo/redo
abort cleanly with the project intact. Covers open, new, and undo/redo restore.

### F4 — GUI max_size presets 8192/16384 silently failed (op validator capped at 4096)

The 'Max page size' dropdown offers 8192/16384 (with a "may not load on mobile" note treating them
as valid), but `tp_operation_validate` capped `max_size` at `TP_OP_MAX_PAGE_DIM` = a HARDCODED
4096, so those presets snapped back. **Investigation:** the root CMake
`add_compile_definitions(NT_BUILD_MAX_TEXTURE_SIZE=16384)` (a deliberate "universal packer" lift of
the engine's mobile-safe 4096 default) makes `TP_PACK_MAX_PAGE_DIM == NT_BUILD_MAX_TEXTURE_SIZE ==
16384` — the packer, the `.ntpack` format (source dims u16 ≤ 65535; UVs normalized u16), and the
engine builder ALL accept 16384 in this build. The op validator's hardcoded 4096 was the STALE
outlier (its own comment even claims to "mirror … the engine `NT_BUILD_MAX_TEXTURE_SIZE`"); 4096 is
only the engine-*load* mobile caveat the GUI already warns about, not a packer limit. **Fix:**
`TP_OP_MAX_PAGE_DIM` now DERIVES from `NT_BUILD_MAX_TEXTURE_SIZE` (`#ifndef` fallback 4096), so it
can never drift from the packer again. `cli_validate.c`'s `CLI_MAX_PAGE_DIM` (same stale hardcode,
same "== engine constant" claim) is aligned identically.

**CLI implication (F2-05a routed `set max_size` through the op validator):** the CLI `set` now
accepts 4097..16384 again — this RESTORES pre-cutover CLI behavior (the pre-cutover `set` wrote the
field directly with no 4096 cap and the packer accepted ≤16384), while `max_size=99999` stays
rejected (> 16384). **No golden changes** (goldens use ≤2048; no golden pins the range text). One
unit test that had encoded the bug (`test_operation.c::test_validate_knob_bounds_match_cli` asserted
`max_size=8000` rejected) was corrected to the reconciled behavior (8192 accepted, 99999 rejected);
`cli_mutate_family`'s `max_size=99999 → exit 2` is unaffected.

### F5 — a queue-OOM silently dropped an edit

On queue-realloc failure `edit_push` dropped the just-made edit with no signal, though the widget
already returned "committed" → the value visibly reverts next frame with no explanation. **Fix:**
`edit_push` returns `bool` and, on OOM, frees the edit's heap payload (no leak) and raises a
status-bar error (the same soft-error channel a core reject uses) so the drop is visible.

### F6 — unbounded idstore growth + O(n²) per-commit scan → REMOVED

The long-lived journal-less model stamped a unique txn id into `m->idstore` every commit (grows via
realloc, never evicted, O(n) `contains` scan per commit) → climbing memory + O(n²) CPU over a long
session (esp. one-commit-per-drag-frame). Idempotency is MOOT for the single-threaded interactive
GUI (unique monotonic ids, no retries — `contains` always missed). **Fix:** the GUI model now
carries NO idstore. `wrap_model` calls a new `drop_idstore` (mirrors `tp_model_destroy`'s idstore
cleanup) immediately after `tp_model_wrap`; the commit path already NULL-guards
`if (m->idstore && m->idstore->record)` (and the `contains` pre-check), so a NULL idstore simply
skips id recording. Confirmed nothing on the GUI path needs it: the GUI never attaches a journal
(the migration-on-attach `tp_txn_idstore_mem_view` is the only other reader and is unreachable). This
supersedes the "idempotency retention grows one 33-byte id per commit" line above — it now grows by
zero. Saved bytes are unaffected (the idstore is runtime-only, never serialized).

### F7 / F8 — cleanups

- **F7:** the three `GEDIT_ATLAS_INT/BOOL/FLOAT` kinds/drain-cases collapsed to one `GEDIT_ATLAS`
  (field in `i0`, ivalue in `i1`, fvalue in `f0`; bool == ivalue 0/1) — the tri-branch drift hazard
  is gone; the three typed `gui_edit_atlas_*` enqueuers stay for call-site clarity.
- **F8:** the three copies of the by-name animation scan (`anim_id_exists`, `remove_animation`,
  `set_anim_id`'s rename-clash) now share one `find_anim_by_name(atlas, name)` helper.

### DEFERRED to b-ii — transaction-level drag coalescing (documented carry)

A slider drag commits ONE journal-less transaction PER FRAME. Undo coalesces these to a single step
(Hazard 2, unchanged), but the TRANSACTIONS themselves do not coalesce. For b-i this is fully
acceptable: F6 removed the only per-commit cost that scaled (the idstore), and each commit's project
clone is bounded (the arena is a later perf pass). **Transaction-level coalescing — debouncing a
drag to ONE commit at drag-end — is deferred to b-ii, where the LIVE journal makes it NECESSARY**
(else the journal floods with one full-snapshot record per drag frame). b-ii owns this: it is not a
b-i correctness issue, only a b-ii durability/volume one. The per-edit full-project clone on the
interactive path (review finding, PLAUSIBLE) is likewise a b-ii arena-perf pass, not a b-i defect.

## F2-05b-ii-A — GUI Undo/Redo → F2-03 diff history + gesture-scoped coalescing (2026-07-13)

The lead split b-ii into **A (this)** and **B**. A cuts GUI Undo/Redo over to the F2-03 diff history,
adds transaction-level coalescing, retires the 32 MB snapshot stack, and moves dirty to identity —
**with the journal still NULL** (B adds the live journal + append-fail + recovery + D2, all UNTOUCHED
here). Implemented in `apps/gui/gui_project.c` (undo/redo + pending buffer + dirty), `.h`,
`apps/gui/gui_actions.c`/`.h` (gesture-commit flag + boundary flushes), `apps/gui/gui_view_settings.c`
(per-widget gesture-end wiring), `apps/gui/main.c` (gated fallback + blur flush), `apps/gui/gui_selftest.c`;
`apps/gui/gui_history.c`/`.h` **deleted**.

### Undo/Redo through the F2-03 diff history

`tp_model_enable_history` is called in `wrap_model` (every installed model). Each committed
transaction captures one semantic diff = one undoable step. `gui_project_undo/redo` call
`tp_model_undo/redo`; because those clone-swap `m->project`, the GUI refreshes `s_proj =
tp_model_project(m)` (and callers re-resolve by index/name) exactly as `commit_txn_now` does. Redo-
branch-drop on a new edit is `tp_history_push_reserved`'s job. The **32 MB snapshot stack
(`gui_history.c`) and the per-touch full serialize are RETIRED**; `gui_project_touch` now only sets
preview-stale + bumps the model-version counter. Dirty is **identity-derived** (`tp_model_dirty`);
`gui_project_save_as` calls `tp_model_mark_saved` after a successful write (post-relativization
baseline), and `init/new/open` promote §5.5 ids then `mark_saved` (a fresh/opened project is clean).
Undo back to the saved revision reads clean by identity even at a higher revision. Never two live
histories.

### Transaction-level coalescing (the crux) — field-precise key

The F2-03 history has NO built-in coalescing (one commit = one undo step), so a raw drag would revert
one tick at a time. A **single pending-transaction buffer** holds ONE coalescable edit keyed by
`(edit kind + atlas index + EXACT target: field / component / sprite-key / anim)`. A same-key edit
replaces the pending value (latest wins); a **different-key** edit FLUSHES the pending first, then
buffers the new one. Structural ops (add/remove/rename/frames/target) are non-coalescable: they flush
pending, then commit immediately. **Field-precise keying is REQUIRED for correctness** — slice9 is a
read-modify-write (it seeds all four components from the record), so keying it BY COMPONENT makes a
different-component edit flush the prior component BEFORE the second RMW read, so the second seeds from
the committed value and neither component is lost. (Selftest proves L=11,R=22 both survive; a tag-only
key would commit `[0,22,0,0]` and drop L.)

### Flush trigger = GESTURE-SCOPED, not a timer (owner decision)

The buffer commits on the WIDGET's gesture boundary, decided by nt_ui, not by guessing with a clock:

| widget kind | commit trigger (raises `gui_request_gesture_commit`) |
|---|---|
| slider (pointer drag) | `nt_ui_query_interaction(ctx,id).released_now` — buffer the live value each frame, commit on release |
| text / numeric field | Enter (`nt_ui_input_text` `out_submitted`); out-of-panel blur via the existing `s_blur_inputs` synthesis (main.c) — keystrokes BUFFER (coalesce), never commit per-keystroke |
| dropdown / checkbox / preset | the discrete pick/toggle is one edit — commit immediately |

`apply_pending` flushes the pending buffer AFTER `drain_edits` folds in the frame's final value, so one
interaction = one committed transaction = one undo step. The 0.30 s time window survives ONLY as a
**gated fallback** (`gui_project_flush_elapsed`, called from main.c only when no pointer is held and no
input is focused) for a value stream that never gets a gesture edge; it can never split a live gesture.
An intra-panel field→field switch (no Enter, no out-of-panel blur) is committed by the next different-
key edit's flush or a boundary — the value is always safe in the buffer (never in only the widget's
text buffer), so it is never lost; only the just-blurred field's on-screen value may lag one commit.

### The complete flush-boundary set (a missed flush = a lost edit)

`gui_project_flush_pending` runs before EACH of: **save / save-as** (in `gui_project_save_as`),
**new / open** (`pending_discard` — the buffered edit belongs to the outgoing project),
**undo / redo** (in `gui_project_undo/redo`), **pack / export** (`do_pack` / `do_pack_blocking` /
`do_export`), and **before every dirty GATE** — `request_new` / `request_open` / `request_exit` flush
BEFORE `gui_project_is_dirty()`, else a buffered edit leaves `tp_model_dirty` false and would be
silently discarded on the confirm. (The `MODAL_SAVE` re-check is covered because `do_save` flushes; the
cosmetic menu-bar dirty dot and the selftest read is_dirty read-only and are deliberately NOT flushed —
flushing every render frame would defeat coalescing.)

### Documented DIVERGENCE from b-i (intentional, correctness-driven)

b-i's coalescing key was the **action tag** (`GUI_ACT_SET_SETTING`), so two DISTINCT knobs edited within
0.30 s (e.g. padding then margin) merged into ONE undo step. A's **field-precise key** makes each
distinct field its OWN undo step (padding then margin = TWO steps — selftest proves `undo delta == 2`).
This is a refinement REQUIRED to prevent the RMW lost-edit above. The tested guarantee **dragging ONE
control = ONE undo step is preserved** (selftest: 8 same-key ticks → `undo delta == 1`, final value
wins, one undo reverts the whole drag).

### Direct (non-op) mutations vs the diff history

The diff history captures OPS only; a direct mutation is invisible to it. Two were surfaced by the
cutover (b-i's snapshot undo masked them) and FIXED here by routing through ops:
- **add_atlas** now commits `ATLAS_CREATE` + `TARGET_CREATE` as ONE transaction (was: create op +
  direct `seed_default_target`) — so undo removes the whole atlas and redo restores its seeded target,
  and the target's id is minted non-nil (the old nil-until-save id broke a raw `save_buffer`).
- **add_target** now commits a `TARGET_CREATE` op (was: direct `seed_default_target`) — so Undo removes
  exactly that target instead of reverting the WRONG prior edit.
  Both mirror the CLI's `target add` op; saved bytes stay byte-identical (random ids, same fields).
  `seed_default_target` remains the sanctioned direct lifecycle seed for `init/new` (fresh history, ids
  promoted by `promote_and_baseline`).

**Remaining honest gap — animation rename is NOT undoable.** `gui_project_set_anim_id` stays a direct
`an->name =` write because the F2-01 catalog has NO `ANIMATION_RENAME` op (a remove+recreate would
reorder the array and break byte-identity). With the diff history this rename produces no undo step, so
Ctrl+Z after it reverts the prior edit. This is a pre-existing catalog gap (not introduced here);
restoring undo needs a core `ANIMATION_RENAME` op (F2-01 scope).

### Verified (both presets)

0 warnings (native-debug + native-release, cgltf filtered); `ctest` 78/78 both presets; `gui_selftest`
PASS (extended: undo/redo-through-history restore + clean-at-saved-baseline; one-control drag = ONE
undo step; two distinct knobs = TWO steps; flush-before-is_dirty preserves a buffered edit; slice9 two-
component RMW loses neither); `check_boundaries.sh` = boundaries OK; GUI `--parity` deterministic on a
fixed-id project AND byte-identical to the CLI for the same logical edits. Journal / append-fail /
recovery / D2 remain UNTOUCHED (b-ii-B).
