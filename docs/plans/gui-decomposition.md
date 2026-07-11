# GUI decomposition plan — main.c → systems, logic/view split (2026-07-11, rev 2)

Owner directive: main.c (~5900 L, 354 statics) is too big — decompose into systems,
separate logic from visuals. Grounded in `docs/design/gui-code-review-2026-07-11.md`
(0 P0 / 1 P1 / 7 P2; async layer sound). Rev 2 incorporates two adversarial plan
reviews (feasibility + architecture): step order fixed (state before dev-seams),
P1 un-folded from the big move, eyeball verification replaced with a programmatic
draw-hash gate, shared widget kit given a home, function-level assignments added.

Status: an owner UI pass (density-2 / status pill / smart folder) lands as step 0
(in flight at writing). Line anchors are current-tree hints; steps bind to
`// #region` names + symbol names.

## 1. Goals / non-goals / honesty about the seam

Goals: main.c → shell core (~1500–2000 L: frame loop, init/shutdown, input glue,
wiring); systems in own TUs with narrow interfaces; behavior bit-identical on every
pure-move step; review's P1/P2 fixes folded in ONLY as dedicated, separately-gated
steps.

Non-goals: no engine changes, no tp_core changes, no new features, no visual changes
after step 0.

**Immediate-mode honesty (per review):** in an IM UI only *model-mutating* actions
separate cleanly (they already defer through the `s_pending_*` queue + `apply_pending`
pump — the seam exists today). Interaction/gesture logic (selection clicks, edit-start,
canvas pan/zoom arming) is inherent to the declare/input path and STAYS in view TUs /
the input controller. No Elm-style command bus — not worth it for a thin client
(AGENTS.md). The deliverable is judged against this, not against absolute purity.

## 2. Target architecture

    view TUs (declare-only)  →  actions (model/state mutation, zero Clay)  →  state
                             →  widgets/icons (shared render kit)          →  model modules
    shell main.c: frame loop, init/shutdown, input pre-pass, hooks to dev seams

New files:
- `gui_defs.h` — pure constants/macros, include-anywhere: `S()/Su()` (static inline
  over `extern float g_ui_scale`), `RGBA8`, `LAYER_*`, `BASE_*`, `FS_*`, `C_*` color
  consts, `g_*_base` style constants. No mutable state.
- `gui_state.h/.c` — shared mutable editor state: selection (`s_sel_*`), multi-select
  set, edit/rename state, menu/modal open flags + disclosure bits (`s_sec_*_open` —
  shared with selftest, can never be view-local), preview state, panel widths,
  `s_ctx`, `s_canvas`, `s_exe_dir`, ui ids (`s_id_*`), `s_row_tips`, status message +
  severity, the ~30 mutable scaled styles (written by `apply_ui_scale`, read
  everywhere). Includes ONLY model-module headers (gui_canvas.h for the `s_canvas`
  type) — never actions/rows/view headers.
- `gui_widgets.h/.c` — shared render kit + its rescale: `apply_ui_scale`, `ui_btn`,
  `ui_icon_btn`, `ui_row_icon`, `ui_label_fit`, `section_rule_label`, `tp_checkbox`,
  `render_rename_field` (used by lists AND settings), `truncate_to_width`,
  `left_row_text_w`, `right_panel_text_w`, `record_row_tip`, icon refs +
  `bind_icon_ref` (gui_icons folded here — one kit TU).
- `gui_actions.h/.c` — `apply_pending`, `do_*` (pack/pack_blocking/export/undo/redo/
  refresh), file dialogs + add files/folder, new/open/save/exit confirm flow,
  `commit_active_edit`/renames, animation ops, `reset_selection`/`clamp_selection`/
  `cancel_edit`/`preview_stop`/`atlas_name_valid`, `handle_canvas_input` + the
  canvas-mouse statics (`s_lmb_*`, `s_mmb_panning`, `s_press_*`, `s_pan_last_*`) —
  it is Clay-free (reads `g_nt_input` + `gui_canvas_*`) and is an input CONTROLLER,
  not a declare. **Carve-out (stays in shell):** the click-outside-commit bbox check
  and the blur-inputs detection in `frame()` (`nt_ui_get_bbox`/`nt_ui_input_any_focused`)
  — they set `s_pending_commit_edit`/`s_blur_inputs`; actions only consume the flags.
- `gui_rows.h/.c` — `build_rows` + row storage, `multi_sel_*`, `nat_cmp`/
  `names_common_prefix` (+ sort scratch), `select_row_for_region`, `strip_ext`/
  `normalize_slashes`. (nat-sort is GUI-side policy sanctioned by ux.md §3.7b; parity
  holds because ordering bakes into stored animations[]. Watch-item only.)
- `gui_view_settings.c` — right panel + `PANEL_ROW_*` macros (verified panel-local).
- `gui_view_lists.c` — left panel (atlases/sprites/animations declares).
- `gui_view_canvas.c` — canvas card, strip (`strip_group_*`), preview window, stale
  overlay, message pill, empty state.
- `gui_view_chrome.c` — menubar (+`close_menubar_menus`), menus, context menu,
  tooltips (`declare_row_tooltips`), modals.
  One header per view TU, exposing ONLY its declare entry points.
- `gui_selftest.h/.c`, `gui_shot.h/.c` — dev seams. Headers expose only the hook
  prototypes `frame()`/`main()` call (`selftest_pre_frame/post_draw`, `run_selftest`;
  `gui_shot_parse_arg/tick/post_draw/apply_scale` — the shot mode has 4 touch points,
  budget the wiring). The TUs #include the same internal headers as view TUs. NO
  god-header `gui_internal.h`. Selftest externs are #ifdef-guarded; gui_selftest.c is
  an empty TU when the flag is off (legal).

## 3. Function-level assignment for the fan-out regions

`#region` boundaries do NOT map 1:1 to TUs. Cherry-pick rule: a symbol assigned to an
earlier-step TU that sits inside a later region moves when its TU is created. Known
fan-outs (from the feasibility review; the executor re-derives the full table against
the current tree as packet task #1):

| Symbol(s) | From region | → TU |
|---|---|---|
| set_status*/severity | small helpers | state |
| multi_sel_*, nat_cmp, names_common_prefix (+`s_sel_sort_buf/ptr`) | small helpers / multi-select | rows |
| reset_selection, cancel_edit, clamp_selection, preview_stop, atlas_name_valid | small helpers | actions |
| truncate_to_width, left/right_panel_text_w, compute_panel_widths, record_row_tip | small helpers | widgets (compute_panel_widths: state ok) |
| select_row_for_region | canvas | rows |
| handle_canvas_input + mouse statics | canvas | actions |
| render_rename_field | left panel | widgets |
| close_menubar_menus | menu bar | chrome |
| s_row_tips storage / record_row_tip / declare_row_tooltips | mixed | state / widgets / chrome |

## 4. Steps (each = one packet, one commit; battery green before the next)

- **Step 0 — land the in-flight owner UI pass.** [in flight]
- **Step 1 — the un-static pass + kit: `gui_defs.h`, `gui_state.h/.c`,
  `gui_widgets.h/.c`.** Publishes the ~26 state externs + kit the reviews counted;
  relocates definitions. THE deliberately-wide step (both reviews: this must precede
  everything). Pure move; draw-hash gate.
- **Step 2 — logic layer: `gui_actions.c` + `gui_rows.c`.** Pure move ONLY (arrays
  stay fixed-size — pixel/hash-identical). Zero-Clay grep gate on both TUs. Includes
  the §3 cherry-picks from canvas/small-helpers regions.
- **Step 3 — dev seams: `gui_selftest.c` + `gui_shot.c`.** Now a genuine lift (state
  + actions externs already exist). Deep-reasoner packet (widest privileged surface:
  ~37 symbols, nt_ui_get_bbox/nt_ui_id usage, fb_width writes).
- **Step 4 — `gui_view_settings.c`.**
- **Step 5 — `gui_view_lists.c`.**
- **Step 6a — `gui_view_canvas.c`;  Step 6b — `gui_view_chrome.c`.** (split per
  review: 6a+6b together exceeded the size budget).
- **Step 7 — P1 fix as its own gated change:** growable `s_rows`/`s_multi_sel`/
  preview idxs **+ companions** `s_sel_sort_buf`/`s_sel_sort_ptr` +
  `names_common_prefix` signature. Realloc-keep-capacity (build_rows runs every
  frame — no per-frame malloc). Verified by a NEW large-N selftest phase (>4096
  sources, >512 frames), not by pixel-diff. Confirmed no ripple into
  UI_STATE_SLOTS/arena (vlist virtualizes; 520-sprite phase proves the pool math).
- **Step 8 — shell cleanup + P2 hardenings** (one-liners, in final locations):
  busy_block() incl. undo/redo; gui_pack_reset_shown(); refresh-during-pack keeps
  stale; dead BASE_TOOLBAR_H; MK_*/CTX_* orphan audit; >16-targets + 8KB add-files
  visible notices.
- **Step 9 — test net:** selftest phases for async-export / cancel /
  shutdown-while-busy / mutate-then-land-stays-stale; register selftest as ctest
  where GL allows (local preset at minimum); document manual TSan run.

Every packet includes: update `add_executable` in apps/gui/CMakeLists.txt (explicit
sub-step — the only place TUs get compiled), and headers for the new TU.

## 5. Per-step gate (run by the LEAD, not the moving agent)

1. Agent hand-off = moved code + compile check of BOTH presets only. No background
   waits inside agent runs; the lead runs the long battery (session lesson).
2. Packet records its base git SHA; agent re-derives all anchors against that tree.
3. Both presets zero warnings; ctest 13/13 × 2.
4. GUI selftest (isolated dir) green end-to-end.
5. **Draw-hash gate (pure-move steps):** hash the nt_ui draw-command list right
   before `nt_ui_walk` (or, if no stable iterator exists, hash the `--shot` PNG
   bytes) for a fixed scenario set {1920×1080@1.5, 1366×768@1.5 × normal/stale +
   empty project} — pre-step vs post-step hashes MUST match. Required hand-off
   artifact from the agent; lead re-runs. Steps with sanctioned deltas (0, 7, 8, 9)
   state their expected diff instead.
6. Zero-Clay grep == 0 on gui_actions.c/gui_rows.c; reverse gate on view TUs: no
   direct `tp_project_*` mutation calls bypassing gui_project wrappers / pending
   queue (read-only tp_* accessors are fine — list the allowed set in the packet).
7. Fresh-clone build.

## 6. Risks (updated)

- Step 1 is the honest "wide diff" (~40 un-statics + kit move). If it explodes past
  ~1500 moved + ~300 edited lines, split: defs+state first, widgets second.
- Include discipline: state → model headers only; widgets → defs+state; actions →
  state+rows+model; views → everything except each other; dev seams → same as views.
  No view↔view includes; render_rename_field lives in widgets exactly so lists and
  settings don't include each other.
- Movers may not "improve" code in flight — fold-ins live only in steps 7–9.
- Concurrent editing ban: one packet in apps/gui at a time, strictly serialized.
- CRLF/style: match existing 4-space style by example.

## 7. Execution packets

P-1: step 1 [deep-reasoner] · P-2: step 2 [deep-reasoner] · P-3: step 3
[deep-reasoner] · P-4: step 4 [fast-worker] · P-5: step 5 [fast-worker] ·
P-6a/P-6b: steps 6a/6b [fast-worker] · P-7: step 7 [deep-reasoner] · P-8: steps 8+9
[deep-reasoner]. Lead: battery + draw-hash verify + commit per packet.

## 8. Future-feature homes (so main.c doesn't re-bloat)

- Notices panel (audit C2 / ux.md region H): model in a new `gui_notices.c`
  (mirrors gui_history), declare in gui_view_chrome (panel) — pill already speaks
  its severity language.
- List search/filter (audit C4 / region D): filter string in gui_state, applied
  inside `build_rows` (gui_rows), field declared in gui_view_lists.
- Keyboard list navigation (I2): focus index in gui_state, key handling in the
  shell input pre-pass, highlight in view TUs.
