# ntpacker GUI — full code review (2026-07-11)

Scope: `apps/gui/` (main.c 5867 L, gui_pack.c/h, gui_canvas.c/h, gui_project.c/h,
gui_history.c/h, gui_scan.c/h, build_packs.c, CMakeLists.txt). Reviewed against the
`packer/` contracts (tp_pack / tp_project / tp_export_run / tp_model). Engine read-only.
Extra scrutiny on the 8 commits `91c2532..d371303` (screenshot mode → async pack/export).

**Bottom line:** the newest and highest-risk code — the async worker layer (gui_pack.c) —
is genuinely well engineered. I found **no P0 and no threading race**. Findings are 1×P1
(silent list truncation) and a cluster of P2 robustness/consistency/perf items. Structural
debt is real but bounded: main.c is at the edge of what its region markers can carry, and
the ~900-line self-test that lives inside the shipping translation unit is the obvious cut.

---

## 1. Architecture walkthrough

**Shell shape.** One `nt_app_run(frame)` loop (main.c:5404). `main()` (5669) does the
engine/gfx/ui/resource bring-up, creates the project (`gui_project_init`), the canvas
(`gui_canvas_init`), and the pack session dir (`gui_pack_init`), then hands control to
`frame()`. Teardown (5841–5865) is strictly reverse: `gui_canvas_shutdown` →
`gui_pack_shutdown` (joins any live worker) → `gui_project_shutdown` → engine modules.

**Declare/walk split.** The UI is retained-mode Clay over `nt_ui`. Each frame:
`apply_pending()` (deferred side-effects) → input → `nt_ui_begin` → a tree of `declare_*`
functions that emit layout + read last-frame widget events → `nt_ui_end` → `nt_ui_walk`
(actual GL). Nothing mutates the model *during* the walk except through the project
wrappers; the touch-on-render self-test guard (phase 2) enforces this.

**Pending-actions model** (main.c:422–438, `apply_pending` 1575). Every user action that
opens an OS dialog or mutates the model sets an `s_pending_*` flag / index during the
declare pass; `apply_pending()` runs them at the *top of the next frame*, between frames.
This keeps dialogs and model edits out of the middle of a Clay tree. `poll_async()` runs
first inside `apply_pending` (1576) so a finished worker lands *before* this frame's canvas
pickup. Undo/redo and Pack-completion are the two mutation classes that bypass the flag
queue: undo/redo run inline in `handle_shortcuts` (still before the canvas bind), pack
results land through `poll_async`.

**Module contracts.**
- `gui_project` — owns exactly one `tp_project` + path + two dirty bits. Every mutation
  funnels through `gui_project_touch` (gui_project.c:162): serialize → memcmp-dedup →
  push pre-mutation snapshot to history → recompute `project_dirty`. `preview_stale` is set
  on every touch and cleared only by a matching pack (`mark_packed`). Undo/redo/new/open
  **replace the `s_proj` pointer** (destroy + reload buffer, gui_project.c:541/590/607), so
  no caller may cache a `tp_project*`/`tp_project_atlas*` across those calls — verified none
  do (all run before the declare pass re-fetches).
- `gui_history` — two snapshot stacks, 32 MB budget, 0.30 s tag-coalescing window,
  drop-oldest eviction. Clean.
- `gui_scan` — 32-entry LRU cache of recursive directory scans; entries are owned vectors
  of {rel, abs, size, mtime}. Callers copy strings out immediately (no aliasing).
- `gui_canvas` — borrows a `const tp_result*` (arena-owned by gui_pack). Dual/tri mode
  (SOURCE / ATLAS / ANIM). `set_result` re-drops + re-uploads page textures; all
  `c->result` derefs in the handler are NULL-guarded.
- `gui_pack` — the pack/export orchestration + the async worker. Blocking
  `gui_pack_atlas`/`gui_pack_export` are the deterministic path (self-test + `--shot`);
  interactive use goes async.

**Async job lifecycle (in words).** One in-flight op max, gated by the main-thread bool
`s_job_active`.
1. *Start* (`gui_pack_async_start` / `gui_pack_export_async_start`): on the main thread,
   assemble a **fully self-contained snapshot** — sprite descs with duped name/path
   strings, a duped atlas name (`name_owned`), a fresh result arena (pack) or a full
   project clone via `save_buffer→load_buffer` + duped `project_dir` (export), plus a
   serialized model snapshot `snap0` for honest-stale. `thrd_create` the worker; set
   `s_job_active = true`. Any failure before that frees everything and leaves
   `s_job_active` false.
2. *Run* (`pack_worker` / `export_worker`): reads only job-owned memory + `s_work_dir`
   (static, init-only). Writes outputs, then a single **release-store** on `s_job.state`.
3. *Poll* (`gui_pack_poll`, once/frame from `poll_async`): **acquire-load** `state`; if
   done, `thrd_join`, then on the main thread apply the slot swap (pack) or read the report
   (export), free all job memory, `memset(&s_job)`, `s_job_active = false`. Cancel is a
   relaxed flag: the non-interruptible worker runs to completion and its result is
   discarded on landing.
4. *Shutdown* (`gui_pack_shutdown`): if busy, set cancel + join + free (covers the window
   X-button, which bypasses the interactive busy-guard).

The acquire/release pair is correct: every non-atomic output field the worker writes
happens-before the release-store, and the main thread reads them only after the
acquire-load returns "done". Fields the main thread reads *while the worker runs*
(`kind`, `start_ms`) are written before `thrd_create` and never touched by the worker;
progress/cancel use atomics. `s_slots[]` is main-thread-only (the worker writes its own
arena; the swap into a slot happens in poll on the main thread). **No shared mutable state
between worker and main except the atomics + the release-guarded output block.**

---

## 2. Findings (ranked)

### P1 — silent list/selection/preview truncation caps
**CONFIRMED.** main.c:1733 & 1756 (`build_rows` caps at `MAX_ROWS = 4096`), 1003/1007
(`update_preview` static `idxs[512]`), 404 (`MAX_MULTI_SEL = 4096`).
The **pack/export path has no such cap** — `assemble()`/`desc_vec` (gui_pack.c:98,173) grow
unbounded, so an atlas with >4096 sprites packs and exports *all* of them correctly. But the
left-panel list stops at row 4096 with no status/badge, so those sprites are
**invisible and un-selectable** (no rename, no per-region override, no multi-select, not in
the anim preview beyond 512 frames). A user with a large sprite folder silently loses the
ability to inspect/edit part of a correct atlas.
*Trigger:* add a folder with >4096 images (or one folder producing >4096 recursive files).
*Fix:* raise/remove the caps or, minimally, surface a status warning when `build_rows` /
`update_preview` truncates ("showing first 4096 of N — list truncated"). If kept, make the
truncation loud, not silent.

### P2 — stale flag can be wrongly cleared by a Refresh during an in-flight pack
**CONFIRMED (edge case).** `model_changed_since` (gui_pack.c:381) compares only the
*serialized model* to `snap0`. `do_refresh` marks stale on **disk** change without touching
the model (main.c:1391; by design — sources are paths). If a Refresh lands while an async
pack is running, its `mark_stale` is later overwritten: the pack lands, the model still
equals `snap0`, `poll_async` calls `mark_packed` (main.c:1468) and clears stale — even
though the on-disk sprites changed and the just-landed result reflects the *pre-refresh*
disk. Preview reads "up to date" while actually stale.
*Fix:* have `gui_project_mark_stale` set a "disk dirtied since pack-start" latch that
`poll_async` respects, or don't clear stale if a refresh happened after the active pack
started.

### P2 — undo/redo not guarded against async-busy (inconsistent with new/open/exit)
**CONFIRMED safe, but inconsistent.** `request_new/open/exit` refuse while
`gui_pack_async_busy()` (main.c:1206/1223/1237). `do_undo`/`do_redo` (1266/1278) do not —
they run inline in `handle_shortcuts` and call `gui_pack_clear(-1)`. This is *not* a race:
`gui_pack_clear` only frees `s_slots[]`; the worker uses its own `s_job.arena`/snapshot and
`s_proj` is irrelevant to it. The observable effect is confusing: undo during an in-flight
pack, then the pack lands and repopulates the slot with a result for the *pre-undo* model,
which `model_changed_since` then flags stale. *Fix:* either guard undo/redo the same way, or
document the intent (and accept the transient).

### P2 — `s_shown_result` holds an indeterminate pointer after `gui_pack_clear`
**CONFIRMED (theoretical UB, practically safe).** After `gui_pack_clear(-1)` frees an
arena (undo/redo/new/open), `s_shown_result` (main.c:384) still holds the freed address.
The next line that touches it, `if (want != s_shown_result)` (5579), *reads that
indeterminate pointer value* (comparison only, never a deref) — UB per C11 6.2.4, harmless
on every real target. The design is otherwise safe: poll always runs before the bind, the
bind calls `gui_canvas_set_result(NULL)` which nulls `c->result` before any walk, and all
handler derefs are NULL-guarded. *Fix:* reset `s_shown_result = NULL` alongside every
`gui_pack_clear(-1)` (or expose a `gui_pack_notify_cleared`) to keep the invariant explicit
and non-UB. Cheap insurance against a future reorder of the frame body.

### P2 — X-button close during a long pack blocks process exit
**CONFIRMED (documented tradeoff).** `gui_pack_shutdown` (gui_pack.c:773) joins the
non-interruptible worker. If a large concave pack is running when the window closes, the
window is already gone but the process lingers until the pack finishes. Acknowledged in the
code comment. *Fix (optional):* a cooperative cancel check inside `tp_pack` would need an
engine change (out of scope) — for now, note it in the release limitations.

### P2 — `>16` export targets export but are invisible/uneditable
**CONFIRMED.** `gui_project_add_target` has no cap; the settings panel (main.c:4103,
`shown = min(count, GUI_MAX_TARGETS=16)`) and the Export dialog (3398, `ti < GUI_MAX_TARGETS`)
only render the first 16, but `gui_pack_export_async_start` iterates **all**
`a->target_count` (gui_pack.c:515,524). A 17th target exports silently with no UI to see or
disable it. No OOB (the `s_dd_target_open[16]`/`s_nb_target_path[16]` arrays are guarded by
`shown`). Unusual, latent. *Fix:* cap `gui_project_add_target` at `GUI_MAX_TARGETS`, or make
those UI arrays dynamic.

### P2 — `do_add_files` 8 KB buffer truncates very large multi-selects
**CONFIRMED.** main.c:1117 copies the `|`-joined dialog result into `char buf[8192]`; a
selection whose joined paths exceed 8191 bytes truncates mid-path, and the final partial
path resolves to a missing file (shown as a missing row, non-fatal — no crash). *Fix:*
size from the returned string length, or parse the tinyfd result in place.

### P2 — `gui_scan` 32-directory cache thrashes for many-folder projects
**CONFIRMED (perf).** `GUI_SCAN_CACHE_CAP = 32` (gui_scan.c:17), round-robin eviction. A
project referencing >32 folders re-runs full recursive directory walks every frame in
`build_rows`/`fp_collect` for evicted dirs → per-frame disk I/O jank. No correctness bug
(each scan result is fully consumed before the next `gui_scan_get` can evict it — verified
in `assemble` and `build_rows`; no held pointer survives an eviction). *Fix:* raise the cap
or cache by directory count/age.

### P2 — export clone completeness depends on tp_export_run reading nothing beyond the serialized model
**PLAUSIBLE — verify on the packer side.** `gui_pack_export_async_start` clones the project
with `tp_project_save_buffer → tp_project_load_buffer` and re-sets `project_dir`
(gui_pack.c:573–595). From the GUI side this is complete: the `tp_project` struct has no
runtime-only fields besides `project_dir` (handled), and `save_buffer` is the same
round-trip-stable primitive undo/redo rely on. The residual risk is packer-internal: if
`tp_export_run` ever reads project state that `save_buffer` omits when at default (sparse
serialization) or a path form that differs pre-relativize, the clone would diverge. Worth a
one-line confirmation that `tp_export_run` consumes only the serialized model + `project_dir`
+ the caller-supplied descs. *Evidence I could not close from the GUI:* tp_export_run's
internal field reads (out of scope).

**Not bugs (verified, worth recording):**
- No live-model aliasing in the pack job. `tp_pack_settings` (tp_pack.h:86) has exactly
  three pointer fields — `atlas_name`, `work_dir`, `sprites` — and all three are overridden
  with owned/static copies after `tp_project_atlas_to_settings` (gui_pack.c:458–461); the
  `name_owned` dup happens on the async path (445/459) before `thrd_create`. The blocking
  path aliases the live name but is synchronous, so that is safe.
- No leaks / double-frees across the gui_pack error and cancel paths, or `thrd_create`
  failure (each frees dv + name_owned + snap0 + arena and does **not** set `s_job_active`,
  so no orphan thread/join).
- Repack correctly frees the previous slot arena (gui_pack.c:656) and the previous GPU page
  textures (`gui_canvas_upload_pages` → `drop_pages`, gui_canvas.c:300).
- Selection re-clamping after model mutation is handled (`clamp_selection` +
  `reset_selection` after undo/redo/new/open; anim selection re-validated each frame at
  main.c:5546–5553).
- Scaling discipline is consistent: no raw-literal `CLAY_SIZING_FIXED` — every metric flows
  through `S()`/`Su()` (spot-checked).

---

## 3. Structural recommendation

**Verdict: split main.c — but the highest-value cut is the self-test, not the panels.**
The `// #region` markers still make the file navigable, but 5867 lines in one TU (with a
~900-line test harness compiled into the shipping binary behind a flag) is past the point
where extraction pays for itself. Concrete cut list (all are self-contained region blocks
with narrow shared surface — mostly the `s_*` statics + the `declare_*`/`ui_*` helpers):

| New file | Moves (regions / lines) | ~LOC | Why |
|---|---|---|---|
| `gui_selftest.c` | self-test (4346–5219) + `selftest_*` post-draw hooks | ~900 | Test-only code out of the shipping shell; biggest single win. Guarded by `NTPACKER_GUI_SELFTEST` already. |
| `gui_panel_settings.c` | right panel (3520–4344): `declare_atlas_settings`/`_region_settings`/`_animation_editor`/`_export_targets` + `row_*`/`ui_*_field` | ~800 | Densest state machine; self-contained. |
| `gui_panel_lists.c` | left panel (2289–2665) + multi-select/nat-sort (595–718) + `select_sprite_row` | ~600 | Selection gesture logic. |
| `gui_chrome.c` | status bar + menus + tooltips + modals (3186–3518) + menu bar (2158–2287) | ~470 | Chrome, few deps. |
| `gui_shot.c` | screenshot mode (5221–5331) | ~110 | Dev seam. |

That leaves ~2000 lines in main.c (frame loop, init/shutdown, pending model, file/undo/pack
actions, canvas region) — a reasonable shell core. The shared statics would move to a small
`gui_app.h`/`gui_state.h` internal header; this is the main friction and the reason to do it
deliberately, not mechanically.

**Dead-code / cleanup inventory:**
- `BASE_TOOLBAR_H` (main.c:96) — **dead**, defined but never referenced (no toolbar row;
  the canvas action strip replaced it). Remove.
- Verify the `MK_*` menu-key enum (main.c:357–363) has no orphaned values after the icon
  redesign, and the `enum { CTX_* }` kinds are all reachable.
- The `s_rows` static is `MAX_ROWS(4096) × sizeof(sprite_row ≈ 928 B) ≈ 3.8 MB` of BSS
  (dominant non-arena static; total BSS incl. the 24 MB UI arena ≈ 28 MB). Fine for a
  desktop tool, but it is the same 4096 cap as finding P1 — resolving P1 by making rows a
  growable vector also removes this static.

**Top-5 refactors by value:**
1. Extract `gui_selftest.c` (removes ~900 test-only lines from the shell; enables wiring the
   self-test as a real headless target — see §4).
2. Fix P1's silent truncation (rows/multi-sel/frames) — a real user-facing correctness win,
   and it motivates making `s_rows`/`s_multi_sel` dynamic.
3. Introduce a tiny `gui_pack_reset_shown()` (or reset `s_shown_result` in the clear
   callers) to make the canvas-borrow invariant explicit and kill the P2 indeterminate-
   pointer read.
4. Centralize the "async-busy blocks destructive ops" rule (P2): today it is copy-pasted in
   3 request_* functions and *missing* from undo/redo — one `busy_block(action_name)` helper.
5. Split the right settings panel into `gui_panel_settings.c` — it is the densest region and
   the one most likely to keep growing (per-region overrides, targets, anim editor).

---

## 4. Test-coverage assessment

**What the net actually covers.** The GUI has **no ctest** — the only automated coverage is
the compile-time `NTPACKER_GUI_SELFTEST` option (CMakeLists.txt:61), which is **OFF by
default and not registered with `add_test`**. It runs *only* if someone configures
`-DNTPACKER_GUI_SELFTEST=ON` and launches the exe with a live GL context/display (it renders
real frames). CI (`ctest`) runs the `packer/` suite but never the GUI. Sanitizer flags *are*
applied to the target (`nt_set_sanitizer_flags`), so a manual TSan/ASan self-test run would
catch the async races/leaks — but nothing automates it.

When it does run, the self-test is genuinely strong: add/dedupe sources, decode+upload,
save/open + save_buffer/load_buffer round-trips, atlas rename + undo/redo, region-rename
override, refresh diff cycle, real blocking `tp_pack` of the demo atlases with assertions,
`gui_pack_export` with on-disk file checks, a 520-sprite + Cyrillic stress (packs, rows, name
round-trip, UI-state-pool no-overflow), effective-extrude and per-region RECT override,
overflow asserts across a scale matrix, the touch-on-render guard (memcmp baseline — proves
no control writes the model without input), and a pixel-level outline probe. Notably it
**does** cover async↔blocking pack equivalence (phase 9, main.c:5124) and the busy-strip
overflow via `gui_pack_debug_force_busy` (phase 10).

**Riskiest paths vs coverage — top missing checks:**
1. **Async export is never exercised** (only blocking `gui_pack_export`). The
   `export_worker`, the `save_buffer→load_buffer` clone + `project_dir` fix-up, `mkdirs`,
   and per-atlas OK/fail counting are untested. This is the least-tested new code.
2. **Cancel path** — `gui_pack_async_cancel` → poll returning `*_CANCELLED` and the
   arena-discard-on-cancel (gui_pack.c:653) is never hit; phase 9 waits for `!busy` first.
3. **Shutdown-while-busy** — `gui_pack_shutdown`'s join+free of a live worker is never
   driven (the app always quits idle in the self-test).
4. **`model_changed_since` stale logic** — no test mutates the model between async start and
   land, so neither the "stays stale" branch nor the P2 refresh-during-pack interaction is
   covered.
5. **Pack landing for a non-selected atlas** (start atlas 0, switch to atlas 1 before it
   lands) — the slot-swap-vs-`s_shown_result` bind is untested for that ordering.
6. **`thrd_create` failure** and **large-N truncation** (P1: >4096 rows, >512 frames,
   >16 targets) — no boundary tests.
7. **No TSan run is automated.** Given this is threaded code, a headless async self-test
   under ThreadSanitizer is the single most valuable addition.

**Recommendation:** register a headless self-test target as a ctest (it needs a GL context —
gate it on CI runners that have one, or add a `--headless` no-render mode that still drives
`gui_pack_*` directly), extend phase 9/10 to cover async export + cancel + shutdown-while-busy
+ a mutate-then-land stale check, and run at least one self-test configuration under TSan.

---

## Summary

- **Counts:** P0 = 0, P1 = 1, P2 = 7 (+1 PLAUSIBLE pending a packer-side confirmation).
- **3 worst:**
  1. **P1** — silent truncation at 4096 rows / 512 frames / 4096 multi-select: sprites that
     pack and export fine become invisible and un-editable, with no user signal (main.c:1733,
     1003, 404).
  2. **P2** — a Refresh during an in-flight pack gets its stale flag wrongly cleared when the
     pack lands (`model_changed_since` tracks only the model, not disk) (gui_pack.c:381,
     main.c:1468).
  3. **P2 cluster** — destructive-op consistency: undo/redo skip the async-busy guard that
     new/open/exit enforce, and `s_shown_result` reads an indeterminate pointer after
     `gui_pack_clear` (both safe today, both one-line hardening) (main.c:1266/1278, 5579).
- **Structural verdict:** split main.c, but lead with extracting the ~900-line self-test out
  of the shipping TU; then the right settings panel and the left lists. Remove dead
  `BASE_TOOLBAR_H`. The async worker layer itself is sound and does not need rework.
- **Biggest gap:** the async layer has real self-test coverage for the happy path but it is
  **not in CI**, and cancel / async-export / shutdown-while-busy / TSan are untested.
