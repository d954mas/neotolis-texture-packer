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

## F2-05b-ii-A FIX pass (adversarial-review corrections, 2026-07-13)

A 21-agent adversarial review of the coalescing cutover (battery-green) found 8 verified defects —
several SILENT DATA LOSS. All are fixed GUI-LAYER ONLY (`apps/gui/**`); `packer/src`, `external`, and
`packer/spike` were untouched (the tp_model/tp_diff/tp_operation core contracts are correct). Journal /
append-fail / recovery / D2 stay untouched (that is b-ii-B).

### The shared root cause — buffered gestures freeze the committed model

The coalescing buffer DEFERS the model write to the gesture boundary (`pending_offer` only stores
`s_pending_op` + sets `s_preview_stale`; it does NOT touch the model). So during a buffered gesture,
`gui_project_get()` / `a->field` / an override record read the **pre-gesture COMMITTED** value, frozen.
Every view that read the committed model expecting the in-flight edit was wrong. The **effective-value
reading discipline** is: a view/guide must use *pending-if-buffered-for-this-key else committed*. Fixes
#1/#2/#5 restore that; #3 restores the pre-cutover no-op suppression the read-only core can't do.

### #1 — stale committed-value no-op guards committed the WRONG value (mechanism: guard removal + #3)

`gui_view_settings.c` compared each widget value against the **committed** model
(`iv != a->padding`, `fv != a->pixels_per_unit`, slice9 `iv != cur`, and the two per-sprite override
numeric fields `iv != ov_margin` / `iv != ov_extrude`). During a buffered gesture the model is frozen,
so returning a control to its committed value SKIPPED the correcting enqueue while the pending buffer
kept the stale intermediate — the flush then committed the wrong value (e.g. type "40", correct to "4",
Enter → committed 40). **Fix: mechanism (a) — REMOVE the committed-value guards** so every changed
frame enqueues (coalesced, latest wins), paired with the #3 flush-time no-op suppression that drops a
gesture netting back to committed. Chosen over (b) effective-value peeks because (a) unifies #1 and #3
(one no-op check at the single flush choke point beats a peek at every guard site) and matches the
already-correct anim-setter pattern (`if (!s_pending_valid && an->fps == fps) return true;` — which only
skips when NOTHING is buffered, so it never froze anything and was left as-is). The margin/extrude
override numeric fields share the identical bug class and were fixed with the enumerated seven (their
`on` gate is kept; only the committed-value compare was dropped).

### #2 — Pivot X/Y shared a coalesce key + a stale view read-modify-write (component-keyed, mirror slice9)

`gui_project_set_sprite_origin` keyed both axes `field=-1`, and the view built each edit by re-reading
the OTHER component from the frozen committed model. X buffered, then Y (same key → no flush) replaced
`{x=new,y=old}` with `{x=old,y=new}` → X silently lost. **Fix: mirror slice9 exactly** — the origin
setter is now COMPONENT-keyed (`axis` 0/1 = a different key) and the read-modify-write moved INSIDE the
setter: editing Y flushes the buffered X first (X committed), then Y seeds the committed X and buffers
`{x=committedX,y=new}` — both survive. The public setter signature became
`gui_project_set_sprite_origin(atlas, sprite, int axis, float value)`; `gui_edit_sprite_origin` and the
view carry the axis; the `--parity` seam sets X then Y (was one both-axes call). **Byte-identity:** the
final override record is unchanged (`origin=(0.25,0.75)`), so the saved file is byte-identical (proven
by a stash-baseline `--parity` diff — the pre-fix and post-fix outputs differ only in random ids).

### #3 — no-op commit pushed a phantom undo step + dropped the redo branch (flush-time suppression)

The pre-cutover `gui_project_touch` memcmp-dedup that suppressed no-op commits is gone, and the
read-only core commit path pushes an undo record + discards the redo branch UNCONDITIONALLY. So a
net-zero gesture (drag anim FPS 24→30→24 and release with a redo branch present) dropped the redo
branch AND left a phantom undo step. **Fix (GUI-side): `pending_is_noop()` before `commit_txn_now`
inside `gui_project_flush_pending`** — when the buffered op would not change the committed model,
DISCARD it (no history push, no redo drop, no dirty flip). Covers the coalescable op kinds:
`TP_OP_ATLAS_SETTINGS_SET` (each masked knob vs the atlas field), `TP_OP_ANIMATION_SETTINGS_SET`
(masked fields vs the anim), and `TP_OP_SPRITE_OVERRIDE_SET` (masked fields vs the current override
record; an ABSENT record == all-default/inherit, the same seed the setters use). Only the MASKED fields
are compared (an unmasked field is untouched by the op, so it can't differ). Structural ops never
buffer, so they always commit — a rename-to-same-name phantom stays a pre-existing minor edge (not
expanded here). This unifies #1: the guard removal enqueues the corrected value, and a net-zero gesture
is dropped here.

### #4 — `--parity` read a freed target pointer across a flush (flush-before-read + copy)

`main.c` bound `a = tp_project_get_atlas(gui_project_get(), 0)` while a slice9 edit was still buffered,
then `gui_project_set_target(.., a->targets[0].exporter_id, ..)` — set_target's first act
(`gui_project_flush_pending`) committed the slice9, clone-swapped `m->project`, freed the old project,
and `a->targets[0].exporter_id` DANGLED before `dupstr` read it. **Fix:** flush the pending buffer
FIRST, re-get `a` from the now-stable project, COPY `exporter_id` into a local buffer, THEN call
set_target (whose internal flush is now a no-op). **Audit:** every production view edit routes through
the deferred `gui_edit_*` queue, which copies the atlas INDEX + heap strings and re-fetches by
index/name in the setter — no production path passes a project-owned pointer across a flush. The direct
`--parity`/selftest call sequences are the only exceptions; both are now flush-before-read.

### #5 — canvas slice9 guides lagged during typing (effective-value peek)

`main.c` fed the canvas slice9 guide lines from the COMMITTED record, which freezes mid-gesture, so the
documented "typing in the Region panel moves the lines this same frame" regressed. **Fix:** a read-only
`gui_project_peek_pending_slice9(atlas, sprite_key, out_lrtb[4])` returns the BUFFERED slice9 (the
pending op already seeds all four components from the record, so it is the full effective value) when a
slice9 gesture is buffered for this atlas+sprite; the guide feed prefers it, else reads the committed
record. **On-canvas PIVOT marker:** it is drawn from the LAST PACK result (`s->pivot`), NOT a live
project feed (there is no `sel_origin` equivalent to `sel_slice9`), so buffered origin edits do not
regress it — it already only updates on repack, unchanged by the cutover. No fix needed there.

### #7 — `is_dirty` hashed the whole project every frame (cached bool)

`gui_project_is_dirty` called `tp_model_dirty`, which walks the whole project (`tp_semantic_identity`)
per call; the menu-bar dot polls it ~60x/s. **Fix:** a cached `s_dirty_cache` recomputed ONLY at the
identity-change choke points — `gui_project_touch` (every committed mutation + the direct anim rename),
undo, redo, and `mark_saved` (`promote_and_baseline` for init/new/open, and `save_as`). `is_dirty`
returns the bool. A buffered (uncommitted) gesture is not in the identity, so buffering never touches
the cache. The existing dirty/clean/undo-to-saved selftest asserts still hold.

### #8 — TARGET_CREATE default-target build triplicated + buffer-size mismatch (shared helper)

`add_atlas` and `add_target` each hand-built the default json-neotolis target op with an `out_path[576]`
buffer (vs the core seed's `path[512]`). **Fix:** one GUI-local `fill_default_target_op(op, atlas_id,
name)` helper (single 576-byte buffer, mints the id, fills exporter/out_path/enabled) used by both.
Saved bytes stay byte-identical (same fields, same size; the core seed for init/new is unchanged and
only sees short auto-names well under 512). The atlas name has no length cap in the GUI, but "out/"+name
overflowing 576 was already unreachable in practice — consolidating removes the divergence hazard.

### #6 — unbounded undo history: KNOWN LIMITATION + follow-up (NOT fixed here)

`wrap_model` enables the F2-03 diff history, and the core `tp_history` **never evicts** — undo memory is
unbounded over a marathon session (the old 32 MB snapshot ring is gone). The stored records are SEMANTIC
DIFFS (far smaller than the retired snapshots), so practical growth is modest, but it is unbounded.
Bounding it needs a CORE `tp_history` budget/oldest-eviction API — out of this GUI-only packet's scope
AND the core is read-only here. **Follow-up packet:** add a core `tp_history` memory-budget +
oldest-eviction API, then the GUI opts in at `wrap_model`. Do not touch core until then.

### Second-round re-review (fix-diff `5d0bb8a..ae82edc`): one more correctness fix + one follow-up

A focused re-review of the fix diff surfaced one CONFIRMED correctness defect and one PLAUSIBLE
cleanup/fragility finding.

**Browse-target UAF (`do_browse_target_at`) — FIXED (`gui_project_set_target` sink-defensive).**
`do_browse_target_at` (and the Export-dialog toggle path) reads `t->exporter_id` — a pointer INTO the
live project — and passes it into `gui_project_set_target`, whose FIRST act is
`gui_project_flush_pending()`. If a coalescable gesture is buffered (e.g. a Padding/Slice9/Pivot field
edited then defocused by clicking a panel button, so no gesture-commit fired), that flush commits it and
the clone-swap **frees the very project the pointer addressed** before `dupstr(exporter_id)` reads it — a
use-after-free that corrupts the saved `exporter_id` and breaks GUI/CLI byte-parity. This is a
base-cutover defect (the setter's flush-first ordering is what makes the caller's pointer stale); the #4
pass fixed only the `--parity` harness, leaving the real UI path exposed. **Fix:** dup `exporter_id` /
`out_path` into locals at the TOP of `gui_project_set_target`, BEFORE the flush, then transfer ownership
to the op (`commit_txn_now` frees the arms on every path). Fixing at the sink inoculates EVERY caller
(the two production paths plus four selftest sites that also pass project-owned pointers), not just the
one the reviewer flagged. Regression `#9` in `ntpacker_gui_selftest` reproduces the exact ordering
(buffer a real slice9 op, then `set_target` with a project-owned `exporter_id`) — clean under CI ASan and
asserts the exporter value survives the intervening free.

**`pending_is_noop` lockstep-mirror hazard — FOLLOW-UP (not a present bug).** `pending_is_noop`
hand-mirrors each masked field of the three coalescable op kinds; it is COMPLETE today, but adding a new
masked field to a handled op kind without extending this function would misclassify a single-field edit
as a no-op and SILENTLY DISCARD it (data loss). The robust fix is to derive the no-op verdict from the
core semantic diff (an empty diff == no-op) instead of re-listing fields GUI-side. Deferred to a separate
hardening packet (needs a core diff/identity probe); a MAINTENANCE warning now sits atop `pending_is_noop`
so the coupling is not silent. (A `main.c` slice9 copy-loop duplication candidate was REFUTED — both
branches are correct and identical in effect.)

### Third-round re-review (fix²-diff `ae82edc..36ca2db`): the sibling sink `gui_project_remove_animation`

The completeness angle ("did the sink-local `set_target` fix leave any SIBLING flush-first setter
exposed?") surfaced one more instance of the SAME class. `gui_project_remove_animation` flushes pending
first, then reads its caller-supplied `id` via `find_anim_by_name`; its production caller (the
do-remove-animation handler) passes `a->animations[i].name` — a pointer INTO the live project. If a
coalescable gesture is buffered (e.g. the user edits an animation's FPS/playback/flip, then clicks Remove
within the window without an intervening gesture-commit), the setter's flush clone-swaps and frees that
project, dangling `id` before the lookup. **Fix:** dup `id` into a local BEFORE the flush (single `free`
at the end; `an->id` used to build the op is re-resolved from the post-flush project). Regression `#10`
reproduces the ordering (buffer a real slice9 op, then remove an animation by a project-owned name).

An exhaustive audit of EVERY flush-first string-taking wrapper against EVERY call site (production +
selftest) confirms these two — `set_target` and `remove_animation` — are the ONLY sinks reachable with a
project-owned pointer today: `create_animation` receives `snprintf` copies in `s_sel_sort_buf`;
`set_atlas_name` / `set_sprite_rename` / `set_anim_id` receive the static UI buffers `s_edit_*`;
`add_source` receives path locals; the coalescable setters and `anim_add_frames` receive drain-queue
copies (`e->s0` / `e->keys`). A CONVENTION comment atop the mutation-wrappers region now records the
dup-before-flush rule so a future wrapper cannot silently reintroduce the class.

### Carried-forward limitation

Animation rename is still NOT undoable (`gui_project_set_anim_id`'s direct `an->name =` write; the F2-01
catalog has no `ANIMATION_RENAME` op). Unchanged by this fix pass — documented above under the cutover.

### Verified (both presets)

0 warnings (native-debug + native-release, cgltf filtered); `ctest` 79/79 (debug) + 78/78 (release);
`ntpacker_gui_selftest` PASS — extended with one regression assert per correctness fix: #1
revert-to-committed commits nothing / revert-to-new commits the final value; #2 origin two-component
edit survives both; #3 net-zero gesture = no phantom undo step AND redo branch intact + still restores;
#4 flush-before-read commits the target with no dangling exporter read; #5 the peek returns the buffered
slice9 while buffered and false after flush; #9 browse-target UAF — `set_target` dups the caller's
`exporter_id` before its flush so the value survives the intervening clone-swap/free; #10 sibling-sink
UAF — `remove_animation` dups its `id` before its flush so the animation is removed with no dangling
lookup. `check_boundaries.sh` = boundaries OK. `--parity` output is
byte-identical (id-normalized) to the pre-fix baseline — no saved bytes changed. No file under
`packer/src` / `external` / `packer/spike` changed; journal / append-fail / recovery / D2 untouched.

## F2-05b-ii-B — GUI LIVE recovery journal + append-fail UX + crash recovery + D2 measurement (2026-07-14)

The FINAL F2 packet flips the GUI model's journal from NULL to **LIVE** (F2-04) and adds the durability
UX + the owner's D2 numbers. Implemented GUI-layer ONLY — `apps/gui/gui_project.c`/`.h` (attach at the
clean baseline + append-fail message + crash recovery adopt + clean-exit reset + the enable/notice API +
a selftest dev seam), `apps/gui/main.c` (enable recovery at the interactive baseline + surface the
notice), `apps/gui/gui_selftest.c` + `apps/gui/CMakeLists.txt` (append-fail + recovery round-trip
regressions), and `packer/tests/tp_bench_journal.c` + `packer/tests/CMakeLists.txt` (the D2 bench). **No
file under `packer/src` / `external` / `packer/spike` was touched** — the F2-04 journal core is used
exactly as shipped. b-ii-A's tasks (undo→F2-03 history, coalescing, snapshot retirement, identity dirty)
are DONE and untouched here.

### Attach point + keying/recovery-slot design (the KEY DECISION — implemented, no core change)

There is **no** in-place re-key / detach / swap seam in the core (verified: `m->journal` is attach-once,
owned, freed at `tp_model_destroy`). So the memory-note "swap `m->journal` on Save-As" is **not
achievable** in place. Implemented the brief-recommended alternative: a **DETERMINISTIC SESSION-RECOVERY
SLOT**, not a path-keyed journal.

- **Attach point:** `wrap_model` (the one place every installed model is minted — `init` / `new` /
  `open`). After `tp_model_enable_history`, the OLD model is destroyed FIRST (closing its slot file
  handle), then a FRESH file-backed journal is attached to the new model via `attach_recovery_journal`.
  Attaching *after* the old model is gone is REQUIRED: the file io opens `r+b`, so two live handles to
  the same slot during the commit-then-replace window would race. The slot is `remove()`d immediately
  before the fresh open so it never ACCUMULATES across New/Open (the file io's `ensure_header` only
  writes a header when the store is empty — an un-reset slot would otherwise append a second checkpoint
  after the old records and grow unbounded). Net: **one slot, always exactly the currently-open
  project's fresh checkpoint.**
- **Keying:** a fixed build/app-stable 128-bit key (`recovery_key()`, bytes `"ntpk_recovery_01"`). It is
  the additional guard (beyond the journal header's magic + format-version) that makes a foreign /
  old-format sidecar `STALE_KEY`-reject on recover instead of misapply. Bump it on any journal-format
  change so old slots self-invalidate to a clean fresh init.
- **Save / Save-As do NOT touch the journal** (no re-key, no reset). `gui_project_save_as` writes the
  `.ntpacker_project` and `tp_model_mark_saved`s the model; the live model keeps recording to the same
  slot. A crash after a Save still recovers the correct latest committed state (the user re-saves; the
  recovered model is `recovered_unsaved` → dirty → prompts Save). This sidesteps the missing detach seam
  ENTIRELY and needs **zero core change** — so no core seam was escalated.
- **The slot is a SIDECAR:** it never enters `.ntpacker_project` serialization → saved bytes are
  byte-identical (proven below). Recovery-enabled ONLY in the interactive `main.c` path; the headless
  selftest, `--parity`, and the batch/headless main path leave it DISABLED (empty `s_recovery_path` →
  journal-LESS, exactly the pre-B behavior) so they stay deterministic + file-free.
- **Degraded durability is never a crash:** on any create/attach failure the editor keeps running
  journal-less and raises a soft status ("Recovery journal unavailable … editing continues without crash
  recovery"). The next New/Open re-tries the slot.
- **KNOWN LIMITATION (documented, accepted, not escalated): concurrent editor instances.** A single
  deterministic slot is the owner's explicit "deterministic temp path, not random session id"
  requirement, which inherently trades multi-instance isolation for deterministic recovery. Two GUI
  instances sharing the slot would garble the SIDECAR only — never the project file, never a crash: the
  worst case is a corrupt/foreign sidecar, which `tp_model_recover` classifies UB-cleanly
  (`BAD_HEADER` / `CORRUPT` / `STALE_KEY`) and falls back to a fresh init. Acceptable for the
  single-instance interactive editor (the real use case). A future hardening could add a slot lock or a
  per-instance suffix; that needs no core change and is out of this packet's scope.

### Append-fail UX (task 5)

A recovery-journal append failure (full disk) already fails the commit INSIDE `tp_model_apply`: the gate
`tp_project_save_buffer(clone)` + `tp_journal_append_txn` rolls the store back to its prior length and
returns `TP_STATUS_JOURNAL_FAILED`, the clone is discarded, and the live model + revision + `s_proj` are
BYTE-UNCHANGED (the tx id stays retryable). `commit_txn_now` already returns false on that; the only
GUI change is to surface a clear, actionable status for `TP_STATUS_JOURNAL_FAILED` ("Could not journal
the edit — disk full? Your change was not applied.") instead of the internal gate prose. No false
"saved"/clean state is ever shown (the model did not change). **Selftest J1** (`gui_project__test_attach_
memory_journal` + `tp_journal_io_memory__fail_next_writes(io,1)`): arm the next append to fail, commit a
real structural edit → commit REJECTED, the soft-error surfaced, the semantic identity + atlas name
BYTE-UNCHANGED, dirty unchanged, and the very next edit commits normally (editor still live once the
one-shot fault clears).

### Crash recovery at startup + clean-exit reset (task 6)

`gui_project_init` runs `try_adopt_recovered()` BEFORE creating a fresh model: with recovery enabled it
opens the slot and calls `tp_model_recover` (takes ownership of the io on every path — never leaked;
reads the slot UB-cleanly). On a usable recovery it adopts the rebuilt model as `s_model` (drops the
idstore — the journal is the idempotency authority; enables a fresh F2-03 history starting AT the
recovered state; sets path="" so the recovered work is untitled and needs a deliberate Save As; keeps it
DIRTY via F2-04 C5 `recovered_unsaved`) and raises a one-shot "Recovered unsaved changes …" notice
(`gui_project_take_recovery_notice`, surfaced by `main.c`). Empty / bad-header / stale-key / torn-first
→ returns false → normal fresh init. **Clean-exit reset:** `gui_project_shutdown` deletes the slot after
destroying the model (which closes the file handle first). Only a CRASH — which never reaches shutdown —
leaves the slot on disk. This makes clean quit / X-close / discard-and-exit all leave NOTHING to recover
(no spurious "recovered" prompt on the next launch); recovery fires ONLY for a real crash / power loss.
**Selftest J3** (isolated build-dir slot, both ways): a committed edit → simulated crash (tear down with
recovery disabled so the slot survives) → re-init RECOVERS it (notice raised, dirty, the edited name
restored); then a clean shutdown (recovery enabled) DELETES the slot → the next init is a fresh CLEAN
project with NO spurious recovery notice.

### D2 MEASUREMENT (task 7 — measure only; owner's snapshot-vs-diff data)

With the journal live, every commit serializes the WHOLE committed project and appends it as one
full-SNAPSHOT record (F2-04 v1 payload; payload format UNCHANGED). `packer/tests/tp_bench_journal`
(a dev exe, NOT a ctest — mirrors `tp_bench_clone`'s fixtures so the numbers are comparable to the P-01
arena bench) isolates the per-commit append cost = `tp_project_save_buffer` + `tp_journal_append_txn`, on
both the memory io and the real file io. **native-release numbers:**

| project | serialized snapshot | ms/commit (mem) | ms/commit (file) | bytes/commit | growth (N edits) |
|---|---|---|---|---|---|
| NORMAL (3 atlas, ~17 KB) | 17,141 B | 0.151 | 0.162 | 17,190 | 32.8 MB / 2000 |
| HUGE (100 atlas × 1000 sprites, ~16.5 MB) | 17,356,035 B | 244.68 | 244.70 | 17,356,084 | 827.6 MB / 50 |

`bytes/commit == serialized_len + 49` exactly (49 = TXN framing: 4 len + 1 type + 32 id + 8 rev + 4 crc),
confirming one full snapshot per commit.

**Snapshot-vs-diff read (for the owner to decide):**
- **NORMAL is a non-issue.** 0.15 ms and ~17 KB per commit is negligible; with b-ii-A's gesture
  coalescing (one commit per gesture, not per frame) even a marathon session is trivial. Full-snapshot
  journaling is fine here and simplest.
- **HUGE is the cliff.** 245 ms/commit would freeze the UI for a quarter-second on EVERY edit, and 17 MB
  written per edit → 828 MB over a 50-edit session. **The memory-io and file-io numbers are essentially
  identical (244.68 vs 244.70 ms)** → the cost is DOMINATED by serialization (`tp_project_save_buffer`),
  NOT the disk write. So a faster disk / async flush does not help; only reducing WHAT is serialized
  does. The F2-03 history already computes a compact per-op semantic DIFF per commit — a diff-based
  journal (append the op's inverse/forward diff instead of a whole-project snapshot, checkpointing the
  full state only periodically) would cut both ms and bytes per commit by orders of magnitude on large
  projects while keeping small projects unchanged. **My recommendation:** ship the full-snapshot journal
  now (correct, simple, sidecar, byte-safe — this packet), and take diff-based journaling as the owner's
  D2 follow-up gated on a real large-project threshold (it changes the F2-04 payload, so it is a core
  packet, not a GUI one). Gesture coalescing is what makes even the snapshot form survivable today;
  without it a HUGE-project drag would append a 17 MB snapshot per frame.

### Verified (both presets)

0 warnings (native-debug + native-release, cgltf filtered); `ctest` 79/79 (debug) + 78/78 (release);
`ntpacker_gui_selftest` PASS — extended with **J1** (append-fail: commit rejected, identity + name
byte-unchanged, dirty unchanged, editor recovers) and **J3** (crash-recovery round-trip BOTH ways:
recover-after-crash restores the edit + dirty + notice; clean-exit deletes the slot → no spurious
recovery). `check_boundaries.sh` = boundaries OK (`gui_selftest.c` stays the boundary-excluded dev seam;
the `packer/src` include is scoped to the selftest build only, so the shipped GUI never sees it).
**Saved-bytes byte-parity:** the GUI `--parity` output is byte-identical (id-normalized; same 3237 B) to
the f6a5225 baseline — the journal is a sidecar and no serialization path changed. Goldens green in
`ctest` (the sidecar never enters `.ntpacker_project`). No file under `packer/src` / `external` /
`packer/spike` changed; no core seam was needed or added.

## F2-05b-ii-B FIX pass (adversarial-review corrections, 2026-07-14)

A high-effort adversarial review of `190b06c` found 4 CONFIRMED correctness bugs (2 made crash-recovery
UNUSABLE, 1 was silent data loss) + 3 cleanups. All fixed GUI-layer only (`apps/gui/**`) + the D2 bench;
no `packer/src` / `external` / `packer/spike` file touched; no core seam needed. Saved bytes stay
byte-identical (id-normalized; same 3237 B).

### [0]+[2] — recovered model was uneditable → recover the STATE, rebuild a FRESH model + FRESH journal

Two bugs made a recovered project **uneditable**: [0] `s_txn_seq` (the `%032llx` txn-id counter) restarts
at 0 each launch, but the recovered journal's retained-id index already held the crashed session's ids
`0..K-1`, so the first post-recovery commits collided as `DUPLICATE_ID`; [2] `tp_model_recover` can hand
back a usable model whose journal is **POISONED** (mid-stream corruption, F2-04 C2/C3), so every append
failed with the misleading "disk full" message. The prior `try_adopt_recovered` adopted that recovered
journal directly.

**Fix (one redesign closes both):** `try_adopt_recovered` now recovers only the **STATE** — it
`tp_project_clone`s the recovered project off `rm`, `tp_model_destroy`s `rm` (dropping the old/poisoned
journal + closing the slot handle), then `wrap_model`s the clone so a **FRESH journal** is attached at the
reset slot. A fresh journal has an **empty, non-poisoned** retained-id index → [0] can't collide (even
with `s_txn_seq==0`) and [2]'s poison is gone. The clone is on the PUBLIC surface (`tp_project_clone` +
`tp_model_project`), so **no core seam** was needed. The recovered content is unsaved work ahead of any
file, so it is flagged dirty via the model's own `recovered_unsaved` field (F2-04 C5, the designed
"dirty vs the project file" mechanism) — `is_dirty()` reads true with no spurious extra commit, and a Save
re-baselines + clears it. **The regression that hid [0]: J3 now EDITS after recovery and asserts the edit
COMMITS** (`edited_after_recovery`), and **J4 builds a real mid-stream-corrupt slot** (ckpt + 3 txns,
corrupt the middle txn via a byte-exact frame-walk), recovers the last good record (`poison_v1`), and
asserts a post-recovery edit COMMITS — proving the adopted journal is fresh, not the poisoned one.

### [3] — Save dropped the last gesture on a journal failure (SILENT DATA LOSS) → flush reports + Save aborts

`gui_project_save_as` flushed the pending gesture, but if that flush's commit failed with
`TP_STATUS_JOURNAL_FAILED` the buffered op was discarded, yet save proceeded to `tp_project_save` +
`tp_model_mark_saved` → a file WITHOUT the edit + a false "saved"/clean title. **Fix:**
`gui_project_flush_pending` now returns `bool` (false iff a buffered gesture existed and its commit
FAILED). `gui_project_save`/`save_as` **ABORT** on a false flush (no `tp_project_save`, no `mark_saved`;
surface the reason; the model stays dirty) so a journal-failed flush is never a false "saved". Audited the
other flush-before-action callers: **undo/redo** also abort on a false flush (they must not revert a
DIFFERENT/older step after losing the in-flight edit); **new/open/exit-confirm** discard the outgoing
project anyway and surface the op-error via the status bar; **pack/export** write atlas outputs, not the
`.ntpacker_project`, so there is no false-clean-project there. **Regression J2:** buffer a gesture, arm
append-fail, Save → Save returns non-OK, the model is NOT marked clean, and NO file is written.

### [1] — a 2nd instance adopted the 1st's LIVE session / could truncate its journal → single-instance LOCK

The fixed key + one deterministic slot meant a 2nd concurrent editor's `try_adopt_recovered` opened the
1st's VALID same-key **live** journal, presented the 1st's in-progress project as "Recovered unsaved
changes", and its New/Open `remove()`+reopen could truncate the 1st's live journal. **No `.ntpacker_
project` data loss** (confirmed: the sidecar is never inside the project file and the project file is
never touched by any recovery path), but wrong/confusing recovery + a clobbered live recovery journal —
the earlier ADR's "garbled sidecar → fresh fallback" **understated** this.

**Fix (implemented, not deferred): a single-instance advisory LOCK on `<slot>.lock`** (GUI-layer, not
core; Windows exclusive-share `CreateFile` + `DELETE_ON_CLOSE`, POSIX `flock(LOCK_EX|LOCK_NB)`; both
auto-release on process death, so a crash never leaves the slot locked). `gui_project_enable_recovery`
acquires it; recovery is ACTIVE only when the slot is configured AND the lock is held (`recovery_active()`
gates BOTH the recover-adopt and the attach). A 2nd instance that cannot acquire the lock runs
**journal-less** and **never touches the slot** (no adopt, no attach, no `remove`), and raises a one-shot
"Another window is open — crash recovery is off for this one" notice. The lock is released at
`gui_project_shutdown` (after the clean-exit slot delete). **Regression J5:** a foreign lock simulates the
1st instance; enabling recovery here leaves recovery INACTIVE, raises the busy notice, and the 2nd
instance never creates the slot journal. Residual: the lock is a companion `.lock` file (advisory,
single-host); it does not coordinate across network filesystems — acceptable for a desktop editor and
noted as such.

### Cleanups

- **[4]** `attach_journal_io(m, io, err)` is now the ONE owner of the journal create → null-check →
  attach → destroy-on-fail ownership dance, called by both `attach_recovery_journal` and the append-fail
  test seam (a future contract change can't leak/double-free one copy).
- **[5]** `s_recovery_path` is now `GUI_RECOVERY_PATH_MAX` (1200) ≥ every caller buffer (main.c 1152,
  selftest 1200) — no silent slot-path truncation for a long exe path.
- **[6]** `tp_bench_journal` no longer double-serializes to print the snapshot size — it derives it from
  the append loop's own buffer (was a throwaway ~17 MB serialize on HUGE).

### Verified (both presets)

0 warnings (native-debug + native-release, cgltf filtered); `ctest` 79/79 (debug) + 78/78 (release);
`ntpacker_gui_selftest` PASS — extended with **J2** (save aborts on a journal-failed flush; not marked
clean; no file written), **J3 edit-after-recovery** (the gap that hid [0]: a post-recovery edit commits),
**J4** (recover from a mid-stream-corrupt slot → last good state + a post-recovery edit commits), **J5**
(a 2nd instance without the lock skips recovery + never touches the slot). `check_boundaries.sh` =
boundaries OK. `--parity` id-normalized byte-identical (same 3237 B) to the f6a5225 baseline — no saved
bytes changed. No file under `packer/src` / `external` / `packer/spike` changed; no core seam.

## F2-05b-ii-B FIX² — propagate the flush-failure contract to ALL callers (2026-07-14)

`e3effef` made `gui_project_flush_pending()` return `bool` and handled save/save_as/undo/redo, but the
re-review found the SAME root cause open in the OTHER callers: they ignored the false return, so a
journal-failed flush silently dropped the buffered gesture and the caller proceeded as if the model were
clean/current. **Uniform rule applied:** `flush_pending()==false` means the buffered gesture existed and
its commit FAILED (the edit is gone AND the model is not what the caller expects), so every caller that
then persists / discards / packs / switches MUST surface the (already-set) op-error and ABORT. GUI-only;
no core. (Preserving the dropped edit across a journal failure is a separate larger change — the op arms
are already freed by `commit_txn_now` — noted as a possible follow-up, not attempted here.)

- **[0] the unsaved-changes dirty gate (`gui_actions.c` `request_new`/`request_exit`/`request_open`).** Each
  did `flush_pending(); if (is_dirty()) prompt; else discard/quit/switch;` — a journal-failed flush dropped
  the only buffered change, `is_dirty` read clean, and the project was **silently discarded / the app quit
  / the project switched with no prompt**. Fixed via a shared `flush_failed()` helper (drains the op-error
  → status bar, returns true on failure): `if (flush_failed()) return;` BEFORE the `is_dirty` check in all
  three, so a failed flush aborts the destructive action and tells the user.
- **[1] pack / export (`do_pack_blocking`/`do_pack`/`do_export`).** A failed flush dropped the edit, then
  pack/export ran on the stale model and reported success. Fixed with the same `flush_failed()` guard —
  abort, no stale pack/export, no false success.
- **[3] the structural mutation wrappers (`gui_project.c`).** Each flushes a buffered gesture then commits a
  structural op; on a TRANSIENT journal failure the flush dropped the gesture but the structural op could
  still journal + commit — pairing a silent loss with an unrelated change. Fixed with a uniform
  `if (!gui_project_flush_pending()) return <fail>;` guard at the top of **every** structural wrapper:
  add_atlas / remove_atlas / add_source_kind / remove_source / set_atlas_name / set_sprite_rename /
  add_target / remove_target / **set_target** / create_animation / remove_animation / set_anim_id /
  anim_add_frames / anim_remove_frame / anim_move_frame. (`set_target` and `remove_animation` free their
  pre-flush string dups on the abort path; `set_target` was NOT in the review's enumerated list but is the
  same class, so it was included for completeness.) The op-error is already surfaced by the failed commit.
- **Audited FINE as-is (confirmed, no "proceed as clean" decision after them):** `pending_route` and
  `gui_project_flush_elapsed` (internal coalescing flushes — on failure the different-key/streamed gesture
  is dropped with the op-error surfaced and the caller only BUFFERS a new uncommitted edit), and the
  `s_gesture_commit` gesture-boundary flush in `apply_pending` (drops the gesture with the op-error
  surfaced via `poll_async`; no persist/discard follows). Each carries an explicit `(void)` + a fix2 note.

### [2] recovery redesign — accepted degraded-durability edge (DOCUMENTED, not "fixed")

`try_adopt_recovered` `tp_model_destroy`s `rm` (which `remove()`s the crashed-session journal) and then
`wrap_model` attaches a FRESH journal; if that fresh create/checkpoint fails (disk pressure exactly at
recovery time), the recovered work is **in memory only** — no on-disk journal — so a SECOND crash before
the user Saves would lose it. **Accepted as a degraded-durability edge, not code-changed**, because: the
old journal was potentially POISONED (the very reason we rebuild), so preserving it is not valuable; the
recovered state is already **dirty + prompts Save**; and a fresh-journal failure raises the
degraded-durability notice ("Recovery journal unavailable … editing continues without crash recovery")
telling the user to Save. So the only loss window is a second consecutive crash-before-Save on a failing
disk. Implementing "confirm the fresh journal before removing the old" would re-introduce the two-open-
handles / slot-accumulation hazards the redesign deliberately avoids, risking the [0]/[2] correctness just
fixed — so it is documented here rather than implemented.

### Verified (both presets)

0 warnings (native-debug + native-release, cgltf filtered); `ctest` 79/79 (debug) + 78/78 (release);
`ntpacker_gui_selftest` PASS — extended with **J6** (a structural wrapper aborts on a journal-failed flush;
neither the gesture nor the op lands; op-error surfaced), **J7** (a pack aborts on a journal-failed flush;
no stale result produced), **J8** (`request_new` aborts on a journal-failed flush; the project + its path
are kept, NOT silently discarded — detected via the retained project path). `check_boundaries.sh` =
boundaries OK. `--parity` id-normalized byte-identical (same 3237 B) to the f6a5225 baseline — no saved
bytes changed. No file under `packer/src` / `external` / `packer/spike` changed; no core seam.

## F2-05b-ii-B FIX³ — the abort is now correctly interpreted by every caller (2026-07-14)

fix2's abort-guards were correct, but the re-review found the abort was MISINTERPRETED one level up — the
same `act-as-clean-despite-failed-flush` class, moved to the callers. Both bugs are narrow (need a
disk-full journal AND a still-buffered gesture when the action fires) but real. GUI-only; no core; saved
bytes byte-identical.

### [0] void remove wrappers couldn't signal their abort → false "Removed" + a bad Ctrl+Z

`gui_project_remove_source` / `remove_atlas` / `remove_target` / `remove_animation` were **void**, so
fix2's `if (!flush_pending()) return;` abort was invisible to the deferred handlers in `apply_pending`
(gui_actions.c), which ran `reset_selection()` / `clamp_selection()` / `preview_stop()` + a
"Removed X (Ctrl+Z to undo)" message UNCONDITIONALLY. On a journal-failed flush the item was NOT removed
but the handler still reset selection and showed "Removed" — a false success, and the offered Ctrl+Z would
undo an earlier UNRELATED committed edit. **Fix:** the four wrappers now **return bool** (true iff the
removal actually committed — false on the flush-abort, an invalid index, or a commit reject). Each deferred
handler runs its side-effects + message **only when the wrapper returned true**; for remove_anim the
`preview_stop()` + `s_sel_anim`/`s_sel_anim_frame` reset are likewise guarded on the real removal
(preview_stop only resets flags → safe to run after the removal, no project deref). `remove_animation`
keeps its dup-before-flush `id` + free-on-abort. Every other caller (gui_selftest.c) compiles unchanged
(bool ignorable). **Regression J9:** arm append-fail + a buffered gesture → `remove_atlas` returns false
and the atlas count is unchanged; the healthy-journal path returns true and removes.

### [1] anim rename misreported a journal failure as a name collision + trapped the editor

`gui_project_set_anim_id` returns false for BOTH a name collision AND (since fix2) a journal-failed flush.
`commit_anim_rename` showed "Animation 'X' already exists." + kept the inline editor open;
`commit_active_edit`'s anim branch showed "Animation name must be unique.". So a disk-full failure while
renaming to a valid UNIQUE name was reported as a duplicate and the editor was trapped, the user retrying
names that all "fail the same way". **Fix:** disambiguate with `gui_project_anim_id_exists(atlas, name)` at
BOTH sites — a genuine collision (name exists on another animation) keeps the collision message + keep-
editing behavior; otherwise (not a collision → a journal/other failure) do NOT claim uniqueness: dismiss
the editor (`cancel_edit`) like the success path and let `poll_async` surface the real op-error
("...could not be committed (disk full?)..."). **Regression J10:** a genuine duplicate → `set_anim_id`
false AND `anim_id_exists` true (collision); a journal-failed flush on a unique name → `set_anim_id` false
but `anim_id_exists` FALSE (not a collision). **Atlas/sprite rename confirmed FINE:** `commit_active_edit`'s
inlined atlas branch pre-checks via `atlas_name_valid` and, on a `set_atlas_name` false (flush-fail), shows
no success message and `cancel_edit()`s unconditionally → no false success, no wrong-message trap;
`commit_sprite_rename` guards its success message + `cancel_edit()`s (empty clears the override).
(fix4 note: the anim disambiguation described here was itself still wrong — see the fix4 section below,
which replaces the whole per-caller heuristic with flush-first-at-entry.)

### [2] cleanup — shared neutral flush-failure wording

`flush_failed()` (gui_actions.c) and `gui_project_save_as` (gui_project.c) open-coded "flush failed → drain
op-error → fallback → surface" with divergent wording, and `flush_failed()`'s fallback said "could not be
**saved**" though it also gates Pack/Export. **Fix:** one shared `gui_project_flush_error(out, cap)` (fills
the drained op-error, else a NEUTRAL fallback "Your last edit could not be committed (disk full?) — resolve
it and try again." that fits save + pack + the gate); both `flush_failed()` and `save_as` call it. (The
review's other two cleanup candidates were refuted — skipped.)

### Completeness re-scan (every deferred handler + every caller of a now-abortable wrapper)

- **Fixed:** the four void-remove deferred handlers (remove_source/atlas/target/anim) and the two anim-
  rename sites (above).
- **Also guarded (minor):** `main.c`'s Delete-key `gui_project_anim_remove_frame` cleared `s_sel_anim_frame`
  UNCONDITIONALLY — now guarded on the real removal (a benign selection deselect, not a false success, but
  closed for completeness).
- **Confirmed SAFE, no change:** the ADD deferred handlers (`add_atlas`/`add_target`/`add_anim`/
  `create_animation_from_selection`) already guard success + Ctrl+Z on `idx >= 0`; `drain_edits`'s
  `(void)`-ignored coalescable setters + anim-frame + target edits show no message and run no side-effect
  after the call (the op-error surfaces via `poll_async`); `commit_active_edit` (atlas) + `commit_sprite_rename`
  guard their success message + always `cancel_edit()`; `do_browse_target_at`
  guards its "Output path" message on `set_target` true. **Noted:** `do_add_files` multi-file add — the
  first file's add can abort on the buffered-gesture flush (the gesture drops, then the flush is clear for
  the rest); the summary counts only the files that actually added (honest count, op-error surfaced) — no
  false success, left as-is.

### Verified (both presets)

0 warnings (native-debug + native-release, cgltf filtered); `ctest` 79/79 (debug) + 78/78 (release);
`ntpacker_gui_selftest` PASS — J1-J8 stay green; **J9** (a bool remove wrapper returns false on a journal-
failed flush + the item is still present; the success case removes) and **J10** (the `anim_id_exists`
disambiguation: a real duplicate is a collision, a journal-failed flush on a unique name is NOT) added.
`check_boundaries.sh` = boundaries OK. `--parity` id-normalized byte-identical (same 3237 B) to the
f6a5225 baseline — no saved bytes changed. No file under `packer/src` / `external` / `packer/spike`
changed; no core seam.

## F2-05b-ii-B FIX⁴ — DEFINITIVE closure: flush-first at every interactive entry (2026-07-14)

Each of the last three rounds found ONE more caller that misread the overloaded return of a flush-
internally operation. **Root cause:** the operations flush the buffered gesture INTERNALLY and collapse
(flush-failed vs domain-outcome) into ONE overloaded bool, so every caller re-derives — and the heuristics
keep being wrong. **Terminal fix (the pattern the dirty-gate / pack / save already used):** call
`flush_failed()` FIRST at the interactive action entry, so the operation's own internal flush is a
guaranteed no-op and its return is DOMAIN-ONLY (empty-history / name-collision / removed / …). GUI-only;
no core; saved bytes byte-identical.

### [0]+[1] anim inline rename — flush-first, revert the heuristic

fix3's `gui_project_anim_id_exists()` discriminator was WRONG: it returns true for the anim's OWN
unchanged name, so pressing Enter without changing the name while the journal fails was misreported
"Animation name must be unique." + trapped the editor ([0]); and its non-collision else-branch
`cancel_edit()`d SILENTLY (a regression — pre-fix3 always warned) ([1]). **Fix:** `commit_active_edit`
calls `flush_failed()` FIRST (before the kind switch) — on a journal-failed flush it surfaces the neutral
error and aborts (keeping the editor open unless `force`). After a successful flush, the anim branch is
the simple pre-fix3 `if (!set_anim_id) { "Animation name must be unique."; if (force) cancel_edit; return; }`
— `set_anim_id` is a DIRECT write (never a journal append), so post-flush its only false is a genuine
collision, the warning is correct again and never silent. The `anim_id_exists` heuristic is GONE from the
rename path. The same entry flush-first covers the atlas + sprite branches (their rename ops DO journal; on
that op's own append failure they return false → no success message + the op-error surfaces via
`poll_async` — no false success, no wrong message).

### [2] do_undo / do_redo — flush-first

`gui_project_undo()/redo()` return false for BOTH a journal-failed flush AND an empty history, and
`do_undo`/`do_redo` showed "Nothing to undo/redo." on false. **Fix:** `if (flush_failed()) return;` at the
top of both. Verified in the core that `tp_model_undo`/`redo` do NOT append to the journal (they clone +
replay a diff, `tp_history.c`), so after a successful entry flush the only false is `NOT_FOUND` (empty
history) → "Nothing to undo/redo." is correct again.

### [3]+[4] dead code + ADR reference

`commit_anim_rename` and `commit_atlas_rename` were DEAD (no callers — the live Enter/blur path is
`commit_active_edit`, which inlines atlas+anim and delegates only sprite). **Deleted both functions + their
`gui_actions.h` declarations + fixed the stale start-edit comment.** Kept `commit_sprite_rename` (called by
`commit_active_edit`). The fix3 ADR references to the dead `commit_atlas_rename` as the "safe path" are
corrected to name `commit_active_edit`'s inlined atlas branch.

### MANDATORY entry-point audit — the class is CLOSED (every entry is (a) flush-first or (b) domain-only)

| entry point | flush-internally op it drives | classification |
|---|---|---|
| `request_new` / `request_open` / `request_exit` (dirty gate) | pending flush | **(a)** `flush_failed()` first (fix2) |
| `do_pack` / `do_pack_blocking` / `do_export` | pending flush | **(a)** `flush_failed()` first (fix2) |
| `gui_project_save` / `save_as` (via `do_save`/`do_save_as`) | pending flush + save | **(a)** flush-first + ABORT on fail (e3effef); callers check `== OK` |
| `do_undo` / `do_redo` | pending flush + undo/redo | **(a)** `flush_failed()` first (fix4); post-flush false = empty-history |
| `commit_active_edit` (atlas / sprite / anim) | pending flush + rename op | **(a)** `flush_failed()` first (fix4); domain-only after |
| `commit_sprite_rename` (only caller: `commit_active_edit`) | `set_sprite_rename` (rename op) | **(b)** guards success on true; entry already flushed; op-fail → poll_async |
| `do_browse_target_at` / `do_browse_target` | `set_target` | **(b)** shows "Output path" ONLY on true; no false success (op-error via poll_async) |
| `do_add_files` / `do_add_folder` | `add_source_kind` (returns status) | **(b)** checks `GUI_ADD_*`; FAILED → error, honest count (multi-file partial noted) |
| `do_refresh` | — (rescans disk + `mark_stale`; no mutation op) | **N/A** — no journal-fail interpretation |
| apply_pending: add_atlas / add_target / add_anim / create_anim | add ops (return index) | **(b)** guard success + Ctrl+Z on `idx >= 0` |
| apply_pending: remove_source / remove_atlas / remove_target / remove_anim | remove ops (return bool) | **(b)** guard side-effects + "Removed"/Ctrl+Z on the bool (fix3) |
| apply_pending: `drain_edits` coalescable setters / anim-frame / target | setters (buffer) / frame / target ops | **(b)** `(void)`-ignored, no message/side-effect after; op-error via poll_async |
| apply_pending: `s_pending_commit_edit` → `commit_active_edit` | (see above) | **(a)** (fix4) |
| apply_pending: `s_gesture_commit` boundary flush | pending flush | **audited** — surfaces op-error, no proceed-as-clean decision after (fix2) |
| `pending_route` / `gui_project_flush_elapsed` (internal coalescing) | pending flush | **audited** — drop-with-op-error then only BUFFER a new edit; no persist/discard (fix2) |
| `main.c` handle_shortcuts: Delete-frame | `anim_remove_frame` (bool) | **(b)** clears selection only on true (fix3) |
| `main.c` handle_shortcuts / `gui_view_chrome` buttons: undo/redo | → `do_undo`/`do_redo` | **(a)** delegate to the guarded entries |

No **(c)** (misinterpreting) entry remains → the journal-failed-flush class is closed.

### Verified (both presets)

0 warnings (native-debug + native-release, cgltf filtered); `ctest` 79/79 (debug) + 78/78 (release);
`ntpacker_gui_selftest` PASS — J1-J10 stay green; **J11** (anim rename: renaming to the OWN name is a no-op
SUCCESS though `anim_id_exists` is true → the heuristic was invalid; a journal-failed flush is caught at the
flush-first entry) and **J12** (`do_undo` after a journal-failed flush surfaces the disk-full error, NOT
"Nothing to undo.") added. `check_boundaries.sh` = boundaries OK. `--parity` id-normalized byte-identical
(same 3237 B) to the f6a5225 baseline. No file under `packer/src` / `external` / `packer/spike` changed; no
core seam.
