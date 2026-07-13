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
