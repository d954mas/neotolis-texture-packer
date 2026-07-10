# UX Design — Neotolis Texture Packer

Design doc for the tool's user experience: the GUI (`ntpacker-gui`), the CLI
(`ntpacker`), and the project file (`<name>.ntpacker_project`) that binds them.
Recommendation-first; decisions and rationale trace to `docs/research/SUMMARY.md`
(esp. §5a/§5c/§5h) and the competitor studies in `docs/research/`. Widget
mappings are checked against the real `external/neotolis-engine/engine/ui/*.h`
headers and `examples/ui_showcase/main.c`.

Guiding invariant (AGENTS.md): **one op layer (`tp_core`), two equal clients**.
Nothing in this doc may put a capability in one frontend that the other can't
reach — every capability is a project field, edited by the GUI and executed by
the CLI. The GUI is an editor of `.ntpacker_project`; the CLI runs it.

---

## 1. Product principles

Seven principles. Every screen, flag, and default below is derived from these.

1. **The project file is the single source of truth.** The GUI is a visual
   editor for `<name>.ntpacker_project`; the CLI executes that same file. There
   is no hidden GUI-only or CLI-only state. Saving the GUI and running the CLI on
   the saved file produce **byte-identical output** (ROADMAP Phase 6 acceptance).
   This is TexturePacker's "GUI = project editor, CLI = project runner" model
   (`texturepacker.md` Lessons #1), and the AGENTS tool-parity invariant.

2. **The live preview is the real artifact, not an approximation.** The center
   canvas renders the session `.ntpack` (written to the tool cache) through the
   normal engine pipeline — `nt_resource → nt_atlas → GPU pages`, drawn with the
   sprite renderer (`SUMMARY.md §5g.4`). What the user sees on screen is literally
   the atlas that ships. No second "preview packer" to drift out of sync.

3. **Folder-driven inputs, live-linked.** You add **folders**, not files. Folders
   are stored as relative paths, rescanned on open, and (Phase 8) watched.
   Sub-folder names become sprite-name prefixes. Per-file adds are the exception,
   not the rule. This is the load-bearing workflow across TexturePacker "smart
   folders", Unity "packables", and gdx-tpgui directory inputs
   (`unity-raylib.md`: removing folder support caused user backlash).

4. **Never silently wrong — pack per target, down to what each format can hold.**
   The project stores the full desired feature set; each export target packs with
   `project ∩ target capabilities` (`SUMMARY.md §5h`). Exporting to a format that
   can't represent a flip or a polygon is **not an error the user must fix** — the
   target automatically uses the best it can, and only genuine metadata loss
   (dropped pivot, flattened polygon) raises a **non-blocking notice**. Hard errors
   are reserved for true impossibilities (a sprite larger than the target's max
   page).

5. **Feedback is non-modal and always visible.** Notices, warnings, and packing
   stats live in a persistent panel, never a dialog that interrupts work. A dialog
   appears only for a genuine decision (unsaved-changes, relink a missing folder).
   Export is blocked **only** on hard errors. This is TexturePacker's
   warnings-not-modals contract (`texturepacker.md` UI walkthrough, Lessons #9).

6. **CLI ≡ GUI, field for field.** Every GUI control is one project field with one
   CLI override flag (CLI-overrides-project semantics). If the GUI can compute it,
   the CLI can too — anything else is a layering bug (`oss-and-architecture.md`).
   This is enforced structurally by keeping all state in `tp_core`.

7. **Portable, diffable, deterministic — safe to commit and to run on every
   build.** Relative paths normalized to `/`; an integer schema `version` separate
   from app version, with chained migrations; sparse storage (never write
   defaults); sorted keys, LF, trailing newline. Outputs are byte-identical on
   re-run, so a content-hash no-op ("up to date") is reliable and the packer is
   safe to wire unconditionally into a build (`SUMMARY.md §5a`, §5f; against
   free-tex-packer's absolute-path / no-migration defects).

---

## 2. GUI layout

Single fixed-layout window (no docking, no floating panels over the canvas —
rTexPacker's lesson: floating panels occluding the canvas is a known weakness,
`unity-raylib.md` Anti-lessons). Three regions around a hero canvas, with a menu
bar on top and a notices strip at the bottom. Built with the engine's Clay-based
`nt_ui` over `nt_app_run` (same shell as `examples/ui_showcase/main.c`).

### 2.1 Text wireframe — a multi-atlas project open

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ File   Edit   View   Help                              game.ntpacker_project  ●unsaved │  (A) menu bar
├──────────────────────────────────────────────────────────────────────────────────────┤
│ [New] [Open] [Save]   │  [+ Folder] [+ Files]   │  [Pack]  [Export All]   ⟳ auto-pack☑ │  (B) toolbar
├───────────────┬──────────────────────────────────────────────────────┬─────────────────┤
│ ATLASES       │  ATLAS PAGE — "hero"                    page 1 / 3     │ SETTINGS · hero │  (C)(D)(F)
│ ┌───────────┐ │ ┌──────────────────────────────────────────────────┐ │ ▾ Basic         │
│ │ hero    ◀ │ │ │                                                  │ │ Algorithm [MaxR▼]│
│ │ ui        │ │ │        (page texture @ zoom, checkerboard)       │ │ Max size  [2048]│
│ │ fx        │ │ │        selected sprite highlighted               │ │ Padding   [ 2 ] │
│ └───────────┘ │ │                                                  │ │ Trim      [Trim▼]│
│ [+ Atlas] [⋯] │ │                                                  │ │ ▾ Advanced      │
│               │ └──────────────────────────────────────────────────┘ │ Extrude   [ 0 ] │
│ SPRITES  🔍   │  Overlays: ☑outline ☐trim ☐pivot   Zoom [-][100%][+][fit] Rotation  [☑] │
│ ┌───────────┐ │  128 sprites · 3 pages · 2048² · 71% filled · packed 34ms  Multipack [☑] │
│ │▾📁 enemies│ │                                                      │ ...             │
│ │ ▾📁 tank  │ │                                                      ├─────────────────┤
│ │   walk_01 │ │                                                      │ EXPORT TARGETS  │  (G)
│ │   walk_02⚠│ │                                                      │ ☑ json-neotolis │
│ │ ▸📁 slime │ │                                                      │ ☑ defold        │
│ │▸📁 ui     │ │                                                      │ ☑ .ntpack (auto)│
│ └───────────┘ │                                                      │ [+ Target] [⋯]  │
├───────────────┴──────────────────────────────────────────────────────┴─────────────────┤
│ NOTICES (2)   ⚠ defold · hero: pivot dropped on 3 sprites (format has no pivot)  ⓘ …  ▾ │  (H)
└──────────────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Regions and widget mapping

| # | Region | Purpose | `nt_ui` widget(s) | Notes / gaps |
|---|--------|---------|-------------------|--------------|
| A | Menu bar | File/Edit/View/Help | Row of `nt_ui_button` triggers + `nt_ui_menu` (`nt_ui_menu_begin`/`item`/`submenu`) per menu; dirty marker via `nt_ui_label` | **Minor gap:** `nt_ui_menu` is cursor/right-click-anchored (context menu). A click-driven top menu bar works by setting the menu's `st.open=true` + `anchor_x/y` from the button's bbox — buildable, slightly against the grain. |
| B | Toolbar | New/Open/Save, Add folder/files, Pack, Export All, auto-pack toggle | `nt_ui_button` (icon+label via `_begin`/`_end` content), `nt_ui_toggle` for auto-pack | Icons come from the app's own baked UI atlas. |
| C | Atlas list | Select / add / remove / rename atlases in the project | `nt_ui_vlist` (one row per atlas) inside `nt_ui_scroll`; active row = accent fill; per-row `nt_ui_menu` for rename/duplicate/delete; `nt_ui_button` "+ Atlas" | Recommended over a top **tab strip**: `nt_ui_tabbar` tabs are single-click ids with no per-tab close/reorder affordance (see §2.3 gap 6). A left list carries per-row controls cleanly and scales to many atlases (gdx-tpgui "packs" model, `spine-libgdx.md`). |
| D | Sprite tree | Folders + sprites of the **selected** atlas, live-linked | Flattened indented `nt_ui_vlist`; per-row chevron `nt_ui_button` toggles game-owned expand/collapse; folder/sprite icon via `nt_ui_image`; filter box via `nt_ui_input` | **PRIMARY GAP:** there is **no tree widget** — `nt_ui_vlist` is a flat fixed-extent virtualized list. Model the tree game-side: keep expansion state, compute the visible flat row set each frame, feed it to the vlist with per-row indent. Standard immediate-mode tree; see §2.3 gap 1. |
| E | Preview canvas | Render the current atlas page at zoom/pan with overlays | `nt_ui_custom` (CUSTOM element) whose handler draws the page texture (sprite renderer) + overlays (shape renderer) at a game-owned zoom/pan; selection via `nt_ui_events` hit-test on the canvas | **PRIMARY GAP:** no built-in pan/zoom canvas. Assemble it: the CUSTOM handler already gets a `world_mat4`; multiply in game-owned zoom+pan. Overlays (region outlines / trim rects / pivot markers) are shape-renderer draws in the same handler. `ui_showcase` proves CUSTOM handlers (radial widgets). See §2.3 gap 2. |
| E' | Page bar | Page switcher, zoom, overlay toggles, live stats | `nt_ui_tabbar` HORIZONTAL or prev/next `nt_ui_button` + `nt_ui_label` for pages; `nt_ui_button` ± / fit for zoom; `nt_ui_checkbox` per overlay; `nt_ui_label` for stats | Multipack pages as tabs above/below the canvas mirrors TexturePacker. |
| F | Settings panel | Per-atlas settings, grouped Basic / Advanced | `nt_ui_scroll` column of: `nt_ui_dropdown` (algorithm, trim mode, size constraint), `nt_ui_input` numeric or `nt_ui_slider_int` (max size, padding, extrude), `nt_ui_checkbox`/`nt_ui_toggle` (rotation, multipack, force-square); section headers via `nt_ui_button` toggling a game-owned `bool` that gates the block; `nt_ui_tooltip` on every control | **Minor gap:** no accordion/collapsible-group widget — build "Basic"/"Advanced" disclosure from a header button + conditional block. Progressive disclosure per TexturePacker. |
| G | Export targets | Enable/configure per-target exporters + output paths | `nt_ui_vlist`/rows of `nt_ui_checkbox` (enable) + `nt_ui_dropdown` (exporter id) + `nt_ui_input` (out path/prefix); capability-driven greying via `enabled=false`; add/remove `nt_ui_button` | Two user-facing targets `json-neotolis`, `defold`; `.ntpack` always produced (`SUMMARY.md §6 Q5`). Greying = TexturePacker capability-flag gating. |
| H | Notices panel | Non-blocking notices/warnings/errors, collapsible | `nt_ui_scroll`/`nt_ui_vlist` of rows (`nt_ui_image` severity icon + `nt_ui_label`); header toggles expand/collapse; click a row → focus the offending sprite/target | Never a modal (principle 5). Severity color: notice/warning/error. |

Cross-cutting widgets: `nt_ui_modal` for unsaved-changes confirm / relink-missing
/ About; `nt_ui_progress` during an async pack; `nt_ui_tooltip` throughout.

### 2.3 Engine-widget gaps (GUI-phase risks)

Ordered by risk. "Primary" items are real build work Phase 6 must budget; the rest
are minor glue.

1. **Tree view — no widget (PRIMARY).** Sprite folders need an expandable tree;
   `nt_ui_vlist` is flat. Mitigation: game owns a folder model + per-node expanded
   bit; each frame flatten visible nodes → feed `nt_ui_vlist` with an indent level
   per row and a chevron `nt_ui_button`. Well-trodden immediate-mode pattern, but
   it is code we write, not a widget we call.

2. **Zoom/pan preview canvas — no widget (PRIMARY).** Center preview must be a
   `nt_ui_custom` element with game-managed zoom/pan and shape-renderer overlays.
   The CUSTOM path exists and is proven (`ui_showcase` radials), but the
   pan/zoom/selection math and overlay drawing are ours to build.

3. **OS-native file/folder dialogs — none in engine (PRIMARY, external dep).**
   "Add folder" is the load-bearing input flow and there is no engine dialog. Must
   vendor `tinyfiledialogs` (or Win32/GTK/Cocoa) as ROADMAP Phase 6 already flags.
   Do **not** hand-roll an in-canvas file browser (rTexPacker's explicit lesson).

4. **OS file-drop onto the window — verify.** Drag-and-drop of folders onto the
   window is the nicest add-input affordance (TexturePacker, gdx-tpgui). Confirm
   whether the engine window/input layer surfaces a GLFW drop callback; if not,
   the Add-folder dialog is the fallback and drop is a post-v1 nicety.

5. **Menu bar — minor.** `nt_ui_menu` is context-menu-first; a top menu bar needs
   the open+anchor glue in gap-note (A).

6. **Closable / reorderable atlas tabs — minor.** `nt_ui_tabbar` tabs are single
   click ids: no per-tab close "×" or drag-reorder. Resolution: use the left atlas
   **list** (region C) with a per-row `⋯` menu instead of top tabs; if tabs are
   still wanted for atlas switching, keep add/remove out of the tab and in a
   separate control.

7. **Collapsible sections / accordion — minor.** No group-disclosure widget; build
   Basic/Advanced from a header button + conditional block (region F).

8. **Draggable split-pane dividers — minor / nice-to-have.** No splitter widget;
   panel widths are fixed Clay proportions. A resizable divider = a thin element +
   `nt_ui_events` drag → game-owned width. Ship fixed proportions for v1.

9. **Numeric stepper / color picker — minor.** No spinner or color widget. Numeric
   fields = `nt_ui_input` (numeric filter) or `nt_ui_slider_int`; background/preview
   color = preset swatch `nt_ui_button`s. Compose from what exists.

10. **Docking — intentionally absent.** Not a gap: single fixed window is the
    correct model (rTexPacker existence proof, `unity-raylib.md` Lessons #1). Noted
    so no one tries to add it.

### 2.4 Canvas rendering — exactly what is drawn (owner Q&A 2026-07-10)

The canvas draws the **real packed page texture** (principle 2), so baked
transforms are inherently visible: a rotated/flipped sprite *is* rotated/flipped
in the page pixels. On top of the pixels, overlay draws (shape renderer, all
data straight from `tp_result`):

- **☑ outline** — each region's true placement silhouette in page space. For
  RECT shapes that's the frame rect; for CONVEX/CONCAVE shapes it is the **actual
  hull polygon** (`tp_sprite.verts` mapped through the D4 `transform` to page
  coords) — the user sees exactly the concave outline the packer nested, not a
  bounding box.
- **☐ trim** — trimmed rect vs. original source bounds.
- **☐ pivot** — pivot markers (may sit outside the frame).
- **Transform indication** — hover/selection readout shows the sprite name +
  size + explicit transform decode ("rot 90°", "flip H", "rot 90° + flip V", or
  "—"); sprite-tree rows carry a compact badge (↻ / ⇋) for any non-identity
  transform so rotated/flipped sprites are findable without hovering the canvas.
- **Selection sync** — click a region on the canvas ⇄ selects the row in the
  sprite tree, and vice versa; selected region gets an accent outline.

Multiple atlases: one atlas at a time on the canvas — the selected row in the
atlas list (region C) drives the sprite tree, canvas pages, settings, and stats.
Each atlas is an independent object in the project (own sources, settings,
animations); pages within the selected atlas switch via the page bar (E').

---

## 3. Key flows

Each flow names the frontend actions and the `tp_core` call behind them.
`tp_pack(settings, arena) -> tp_result` runs on a **worker thread** in the GUI
(ROADMAP Phase 6); `nt_ui_progress` shows during; the preview swaps when it
returns. Repacks are **debounced** (~150–300 ms) and **committed on release** for
sliders (the `nt_ui_slider_*` header documents the `released_now` commit pattern)
so a drag does not repack every frame.

### 3.1 First run / new project
1. Launch `ntpacker-gui` with no file → empty state: toolbar + an empty-project
   hint in the canvas ("Add a folder to start"), one default atlas `atlas1`.
2. `File → New` (or the hint's Add-folder button) → asks for a save location only
   at first save (project stays in-memory until then; paths relativize once a save
   path exists).
3. Result: an in-memory `.ntpacker_project` (schema `version:1`) with one empty
   atlas and no sources.

### 3.2 Add folder → auto pack → preview
1. Toolbar `+ Folder` → OS-native folder dialog (gap 3) → the chosen path is
   stored **relative to the project file**, added to the selected atlas's
   `sources`.
2. `tp_core` scans the folder (recursive, honoring `ignore` globs), builds the
   sprite tree; sub-folder names become name prefixes.
3. If **auto-pack** is on (default), a pack job kicks off immediately; else the
   user presses **Pack**. `tp_pack` runs → writes session `.ntpack` → the canvas
   loads and renders page 1.
4. The sprite tree (region D) fills; the stats line shows sprite/page/size/fill.

### 3.3 Tweak a setting → live repack
1. Change a control in Settings (e.g. Padding via `nt_ui_slider_int`, or Algorithm
   via `nt_ui_dropdown`). The change writes the atlas's project field.
2. On **release** (sliders) or immediately (dropdown/checkbox), a debounced pack
   job runs for the affected atlas only. Cached source decode/trim is reused
   (`SUMMARY.md §5f` — decoded+trimmed images cached by file hash), so only layout
   re-runs.
3. Canvas + stats + notices refresh. Project is marked dirty (● in the menu bar).

### 3.3b Pack/Export button state & staleness (owner feedback 2026-07-10)

Two independent dirty bits, surfaced differently:

- **Project dirty** (unsaved to disk): ● next to the project name in the menu
  bar (§2.1 A). Cleared by Save.
- **Preview stale** (sources/settings changed since the last successful pack):
  any model mutation — add/remove image or folder, any setting change — sets it.
  Cleared only by a successful `tp_pack` run.

Surfacing the stale bit:
1. **Pack button state**: normal when preview is current; **accent/highlighted
   with a "stale" badge** when a repack is needed; a spinner/progress state while
   packing. Tooltip (`nt_ui_tooltip`) always explains: current → "Atlas is up to
   date (packed 34 ms ago)"; stale → "Sources or settings changed — press to
   repack"; packing → "Packing…".
2. **Canvas watermark**: while stale, the canvas dims slightly and shows a
   corner tag "outdated — press Pack" so the on-screen atlas is never mistaken
   for the current settings' result. (With auto-pack ON the stale window is a
   debounce tick, so the tag barely flashes.)
3. **Export honors staleness**: Export All on a stale preview first repacks
   (per-target anyway, §3.5) — it never writes files from a stale layout; its
   tooltip says what it will write and where.
4. Semantics recap (tooltips carry this): **Pack** = repack now with current
   project settings and refresh the preview (session `.ntpack` only, no files
   exported). **Export All** = pack per enabled target (∩ capabilities) and
   write that target's files to its output path.

### 3.3c Undo/redo (owner decision 2026-07-10: required)

Snapshot-based history, not command objects. The deterministic project
serializer (tp_project save-to-buffer) makes snapshots trivially correct and
cheap (projects are KBs):

- Every model mutation already funnels through one choke point (the GUI's
  `touch` wrapper) — it pushes the PRE-mutation serialized snapshot onto the
  undo stack and clears the redo stack. Undo = deserialize snapshot back into
  the live model (GUI selection re-clamped); redo = mirror stack.
- **Coalescing:** continuous gestures (slider drags, text typing) snapshot once
  per gesture (on begin/commit), not per frame/keystroke.
- Depth ~100 entries (ring); Ctrl+Z / Ctrl+Y (+ Ctrl+Shift+Z alias), Edit menu
  Undo/Redo with greyed state and action names later (v1: plain Undo/Redo).
- **Scale guard (owner challenge: 10 atlases × 1000 objects).** First, the
  model keeps SOURCES + sparse overrides, not per-object entries — folder-fed
  atlases stay tiny regardless of sprite count. The pathological case
  (thousands of per-file sources and overrides) is ~1-2 MB of JSON. Guards,
  in order of adoption: (1) skip-if-identical (byte-stable serialization →
  memcmp dedup); (2) snapshots stored compressed (miniz is already in the
  engine deps; JSON compresses 10-20×, so worst case ≈ 50-150 KB/step);
  (3) history budget counted in BYTES (e.g. 32 MB ring), not steps — small
  projects get deep history, huge ones shallower. Escalation only if real
  projects still hurt: per-atlas snapshots (mutations are atlas-local; full
  snapshot only for cross-atlas ops). Not built until measurements demand it.
- Scope: undo covers the PROJECT MODEL only — never disk files, never the
  packed preview directly (a restored model recomputes `preview_stale`;
  `project_dirty` recomputes by comparing the restored snapshot to the
  last-saved snapshot, so undoing back to the saved state clears the ● marker).

### 3.3d Keyboard shortcuts (v1 set, owner-requested)

Standard desktop bindings, all routed through the same actions as the menus
(no hidden key-only behavior): Ctrl+N New, Ctrl+O Open…, **Ctrl+S Save**,
Ctrl+Shift+S Save As…, Ctrl+Z Undo, Ctrl+Y / Ctrl+Shift+Z Redo, F5 Refresh
(rescan sources), Ctrl+P Pack, Ctrl+E Export All, Esc closes open menus/modals
(already live). Shortcuts shown next to their menu items. Text-input focus
swallows keys first (no accidental global actions while typing).

### 3.3e Mouse-complete access (owner rule 2026-07-10)

**No keyboard-only actions, ever.** Every hotkey is an accelerator for an
action that also exists as a menu item, toolbar button, or context-menu
entry. Right-click opens a context menu wherever a row/object has actions
(the engine's `nt_ui_menu` is natively cursor-anchored — this is its home
turf):

- Atlas row: Rename, Remove (later: Duplicate).
- Sprite/source row: Rename, Remove; multi-selection adds
  "Create animation from selection" (§3.7b).
- Animation row: Rename, Remove (later: Duplicate).
- Canvas (later phases): overlay toggles, zoom fit/100%.
Context-menu entries trigger the SAME code paths as menus/hotkeys (inline
rename editor, etc.) — no parallel behaviors.

### 3.3f Invalid / unsupported setting combinations (owner question 2026-07-10)

The engine builder ABORTS (`NT_BUILD_ASSERT`) on bad input and has no error-text
API, so the tool enforces safety in layers — a crash is never an acceptable
outcome, at any layer:

1. **Core (`tp_pack`) validates everything** before the builder runs and returns
   a status + human-readable message (`tp_pack.c validate_settings`). This is
   the safety net for hand-edited project files and the CLI. Known constraints
   (from `nt_builder.h`):
   - `extrude > 0` requires `shape == RECT` — the packer reserves space for the
     silhouette envelope, not an extrude band around the trim rect. Non-RECT
     shapes must use `padding` instead.
   - `max_vertices` ∈ [1..16] (engine hard cap; default 8).
   - `max_size` ∈ [1..16384] (`NT_BUILD_MAX_TEXTURE_SIZE`, raised from the
     engine's mobile-safe 4096 via a build-wide define — owner ruling
     2026-07-10; format-exact per plan §2.5 up to ~32K, memory is the real
     ceiling: an RGBA 16384² page is 1 GiB). Default stays 2048; values over
     4096 get an inline info line "may not load on mobile GPUs / stock engine
     runtime".
   - `alpha_threshold` ∈ [0..255]; `padding/margin/extrude` ≥ 0;
     `pixels_per_unit` > 0 and finite.
   - Sprite names unique and non-empty; files must exist.
   - Slice-9 is NOT an error case: the engine auto-forces that sprite to
     RECT + no-rotate (documented in `nt_builder.h`); the GUI surfaces this as
     an info line in the region params, never a warning.
2. **GUI makes invalid states unreachable.** The settings panel (region F)
   never offers a combination the core would reject:
   - Numeric fields clamp to their valid range at input time (spinner/slider
     bounds = the ranges above).
   - **Dependent controls disable, values persist**: with `shape != RECT` the
     Extrude control is greyed out with inline text "Extrude requires Rect
     shape — use Padding for polygon modes". The project KEEPS the stored
     extrude value (switching shape back restores it); the GUI passes
     `extrude = 0` to `tp_pack` while the control is disabled. Same pattern for
     any future dependent knob.
   - A disabled-with-reason control is the standard pattern; hiding controls or
     silently zeroing stored values is forbidden (owner must see what exists
     and why it's off).
3. **If the core still rejects** (hand-edited project, future skew between GUI
   and core): Pack fails softly — status-bar error with the core's message,
   preview keeps the last good atlas, stale badge stays on. Never a dialog loop,
   never a crash.
4. **CLI**: same core message on stderr, non-zero exit (§4.4).

### 3.3g Per-region packing overrides (owner ruling 2026-07-10)

The settings model is two-level, mirroring the engine: **atlas settings +
optional per-region overrides** (`nt_atlas_sprite_opts_t`: shape, allow_rotate,
max_vertices, margin, extrude — engine encodes 0 = inherit). The Region panel
carries a "Packing overrides" subsection where every control follows the
Default/override pattern: first entry "Default (inherited: <atlas value>)",
then explicit values. §3.3f applies per sprite (extrude override requires that
sprite's EFFECTIVE shape = Rect). Slice-9 shows shape/rotate as
overridden-by-slice9 (disabled, info). Project schema stores overrides
sparsely (absent = inherit). Engine limitation, backlog candidate: an explicit
override to 0 for margin/extrude/max_vertices is unrepresentable (0 means
inherit in the builder API).

### 3.4 Configure export targets
1. In Export Targets (region G), toggle a target on/off (`nt_ui_checkbox`), pick
   its exporter id (`nt_ui_dropdown`: `json-neotolis` / `defold`), set its output
   path/prefix (`nt_ui_input`).
2. Controls the target's format **cannot represent** are greyed
   (`enabled=false`) — e.g. selecting `defold` greys D4-flip-only options; this is
   informational, the pack still down-packs automatically (principle 4).
3. `.ntpack` is always present as an auto target (can't be removed; it is the
   interchange used for preview and the always-shipped engine artifact).

### 3.5 Export all

**Amendment (owner ruling 2026-07-10): Export ALWAYS opens an Export dialog
first** — never silently writes to configured paths. The dialog is a front-end
over the same target model (region G): per atlas, one row per target with
enabled checkbox, exporter id, and an editable out path (browse via save
dialog, relativized to the project); edits write back through the touch choke
point (dirty + undo). Footer: "N targets enabled across M atlases", [Export] /
[Cancel] (Esc). Empty state links to adding a target in region G. Triggers:
strip button, File > Export…, Ctrl+E.

1. Toolbar **Export All** → (after dialog confirm) for each enabled target, `tp_core` packs with
   `project ∩ target capabilities` (targets with identical effective settings
   share one pack run) and writes the target's files under its output path.
2. Progress via `nt_ui_progress`; per-target results and metadata-loss notices
   land in the notices panel. **Output-overlaps-input safety check**: refuse to
   write into a directory that contains source files, with a clear error
   (gdx-tpgui's safeguard, `spine-libgdx.md`).
3. On success: a transient "Exported N targets" notice; on hard error (doesn't
   fit, missing exporter/template): a red notice and that target is skipped, others
   still export.

### 3.6 Reopen a project
1. `File → Open` a `.ntpacker_project`. Loader refuses a **newer** schema with a
   clear message; runs chained migrations for older (`SUMMARY.md §5a`).
2. All source folders are **rescanned** on open (never trust a stale file list);
   new art added on disk since last save appears automatically (live-linked
   folders).
3. Absolute paths (if any snuck in) are accepted on load, **relativized on next
   save**, with a one-line notice.

### 3.7 Missing-files handling
Principle: missing inputs are **visible but never fatal** to the pack (contrast
free-tex-packer, whose absolute paths silently break on another machine).
1. **Missing folder source** (path gone): a warning notice + the folder row offers
   **Relink** (folder dialog → repoint) or **Remove**. Packing continues with the
   remaining sources.
2. **Missing pinned file**: warning notice; the sprite row shows a "missing" badge
   (⚠ in the wireframe); its per-sprite overrides are **kept**, not deleted (so a
   restored file re-binds — TexturePacker keeps `individualSpriteSettings`).
3. **Override referencing an absent sprite name**: kept sparse in `sprites[]`, shown
   as an orphaned-override warning; never auto-purged.
4. **Empty atlas** (all inputs gone): the atlas packs to nothing → a warning, not a
   crash. CLI: warnings to stderr, still exit 0 if anything packed; non-zero only if
   a *requested* atlas has zero usable input and produces no output.

### 3.7b Animations — assemble, edit, preview (owner requirement 2026-07-10)

Model follows Defold's atlas exactly: an atlas holds named flipbook animations;
every single image is implicitly also a 1-frame animation (Defold sprites
always reference an animation id — upstream `basic.collection` uses both
`"box_small_128"` (single frame) and `"BoxFlip"` (flipbook) as
`default_animation`).

- **Manual assembly is the primary UX** (owner: "как в Defold"): an
  ANIMATIONS block for the selected atlas — list + "+ Animation"; selecting
  one opens its editor: id (inline rename), ordered frame list (add frames
  from the atlas's sprites via multi-select picker, reorder ↑/↓, remove),
  fps (numeric), playback (dropdown of the Defold modes), flip_h/flip_v.
- **NO auto-grouping, NO suggestions — FINAL owner ruling 2026-07-10**
  (supersedes the interim suggestion-row idea from earlier the same day).
  Rationale: the `icon_1`/`icon_2` false-positive case, plus
  `docs/research/animation-grouping.md` — every explicit-assembly tool
  (Defold, Godot, Unity, Aseprite) ships ZERO name-based detection; the one
  default-on auto-grouper (TexturePacker) maintains a KB article on turning
  it off. Export emits ONLY explicit `animations[]` from the project.
  tp_normalize's numeric-suffix grouping is REMOVED from the export path
  (animations packet deletes it; a common-prefix/natural-sort helper
  survives only to power the selection gesture below).
  Instead, ASSEMBLY IS MADE FAST:
  (a) multi-select frames in the sprite list (Ctrl/Shift-click) →
  **"Create animation from selection"** — id derived from the common
  prefix, frames ordered by natural numeric sort (walk_2 before walk_10);
  (b) animation editor's "Add frames…" multi-select picker appends in
  natural order; reorder ↑/↓; remove;
  (c) invariant: exported region names NEVER strip numeric suffixes — the
  libGDX/Phaser runtime-convention path keeps working alongside explicit
  animations.
- **Playback enum pinned to Defold's set** (tp_core enum + GUI dropdown +
  Phase 5 .tpatlas mapping): once_forward (default 0), loop_forward,
  once_backward, loop_backward, once_pingpong, loop_pingpong, none.
- **Animation preview player** in the canvas: selecting an animation plays
  it at its fps with its playback mode — play/pause, frame step, current
  frame indicator ("3/10"), honors flip_h/v. Pre-pack it plays from source
  images (decode cache); post-#282 the same player runs on packed regions,
  which also visually validates trims/pivots in motion.
- Defold verification: upstream extension examples already play `.tpatlas`
  animations in stock Defold (see above); OUR generated `.tpatlas` is proven
  the same way in the Phase 5 demo (bob-built in CI; acceptance: "walk
  animation plays").

### 3.8 Later phases

**Pivot editing (post-v1).** Select a sprite in the canvas (`nt_ui_events`
hit-test) → toggle **Edit pivot** → drag a pivot marker on the canvas
(custom-draw handler reads/writes a game-owned normalized pivot, off-frame allowed
per the model in `SUMMARY.md §5d`); numeric X/Y via `nt_ui_input` (numeric). On
release, write into the sparse `sprites[]` override. **Make the edit mode
explicit** — a visible "Editing pivot · Esc to finish" banner — rather than a
hidden key-state (rTexPacker's modal-key-state is the "mixed" lesson, §5).

**Watch mode (Phase 8).** GUI: the toolbar **auto-pack** toggle, when combined
with the FS watcher, repacks on any change to a live folder (debounce + coalesce);
preview and notices update. CLI: `--watch` reuses the same watcher and pack job.

---

## 4. CLI UX (`ntpacker`)

Headless, CI-first. One console binary `ntpacker`; the windowed sibling is
`ntpacker-gui` (`SUMMARY.md §6 Q7`). All persistent state is the project file; the
CLI does argv + disk I/O only and calls the same `tp_core` the GUI does — so CLI
output byte-matches the library test and the GUI (ROADMAP Phase 4 acceptance).

> **Naming note (resolved, 2026-07-10).** **`pack`** is the canonical verb
> (ecosystem norm — rTexPacker/gdx/texpack; ROADMAP deliverable); **`export`**
> is accepted as an alias.

### 4.1 Verbs

```
ntpacker pack <project.ntpacker_project> [<project2> …] [flags]   # primary (alias: export)
ntpacker watch <project.ntpacker_project> [flags]                # = pack --watch
ntpacker info  <project.ntpacker_project>                        # list atlases + targets, no output
ntpacker --version | --help
```

`pack` with no flags packs every atlas to every enabled target — the whole CI
story is one line. Multiple project files batch (gdx-tpgui `--batch`,
TexturePacker multi-`.tps`).

### 4.2 Flags (mirror project fields; CLI overrides project)

| Flag | Effect |
|------|--------|
| `--atlas <name>` (repeatable) | Pack only these atlases (default: all). |
| `--target <id>` (repeatable) | Export only these targets: `json-neotolis`, `defold`, `ntpack` (default: all enabled). |
| `--out-dir <dir>` | Override the output root; per-target paths resolve under it (default: per-target paths in the project, relative to the project file). |
| `--watch` | Watch live folders; repack on change (debounced, coalesced). `Ctrl-C` exits 0. |
| `--force` | Ignore the content-hash cache; always repack + rewrite (crunch model / TexturePacker `--force-publish`). |
| `--save [<path>]` | Write the effective (project + CLI overrides) settings back to the project (TexturePacker `--save`). Enables scripted project edits. |
| `--verbose` / `-v` | Per-atlas, per-target, and metadata-loss detail. |
| `--quiet` / `-q` | Errors only; suppress progress + notices. |
| `--version`, `--help` | Standard. |

Stable identifiers everywhere (`json-neotolis`, never `"JSON (Neotolis)"`) — 
free-tex-packer's display-string-as-id was a documented CLI wart
(`oss-and-architecture.md`).

### 4.3 Output conventions

- **Streams:** human progress + notices → **stderr**; stdout reserved (kept clean
  for future machine-readable summary / redirection). Errors → stderr.
- **Progress** (default, not `--quiet`): one line per atlas → target, e.g.
  `hero → defold: 3 pages, 128 sprites (34ms)`; `--verbose` adds per-sprite /
  effective-settings detail.
- **Notices** carry a severity prefix and never, by themselves, change the exit
  code:
  - `notice:` genuine metadata loss under a target (dropped pivot, polygon → rect)
    — the §5h informational case.
  - `warning:` missing input, near-limit page usage, name collision resolved,
    absolute path relativized.
  - `error:` hard failure (below).
- **"Up to date"**: with the content-hash cache and no `--force`, an unchanged
  input set prints `hero: up to date` and does no work — safe to call every build.

### 4.4 Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success, including "up to date" and packs that emitted only notices/warnings. |
| `1` | Usage / argument error (unknown flag, missing project, unknown atlas/target id). |
| `2` | Pack/export failure: a sprite exceeds the target's max page, missing exporter/template, project schema newer than the tool, or all requested input missing / I/O error. |

Standard make/CI contract (TexturePacker, gdx-tpgui). Errors are actionable and
name the atlas + sprite.

### 4.5 CI recipes

```bash
# Build step — safe to run unconditionally (content-hash no-op when unchanged):
ntpacker pack game.ntpacker_project

# Clean rebuild job — bypass the cache:
ntpacker pack game.ntpacker_project --force

# Only the Defold target, into a build output tree:
ntpacker pack game.ntpacker_project --target defold --out-dir build/atlases

# Fail the build if packing fails (exit code gates the pipeline):
ntpacker pack game.ntpacker_project || exit 1

# Dev loop — repack on save, like a dev server:
ntpacker watch game.ntpacker_project           # or: ntpacker pack … --watch
```

Free, no license/EULA/floating-seat friction to script around — a deliberate
advantage over TexturePacker's CI licensing pain (`texturepacker.md` Lessons #10).

---

## 5. What makes packers pleasant vs painful

Distilled from the research into concrete do/don't rules for us. Sources in
parentheses.

### Do (adopt)

- **Live-linked smart folders + rescan-on-open** — folders are the input, not
  files; new art is auto-picked-up (TexturePacker, Unity packables, gdx-tpgui).
- **Live preview that is the real artifact** — instant repack in the GUI, preview =
  the session `.ntpack` (TexturePacker live repack; `SUMMARY.md §5g`).
- **Non-modal notices; block only on hard errors** — warnings persist in a panel;
  export blocked only by true impossibilities (TexturePacker warnings-not-modals).
- **Pack per target down to capability, never error on "unsupported"** — our §5h
  twist on TexturePacker/free-tex-packer capability flags: gate/grey controls
  **and** auto-down-pack, so exporting to a limited format is zero user action and
  always correct.
- **Relative, versioned, migrated, sparse project file** — portable across
  machines/VCS; schema `version` separate from app version; never store defaults
  (TexturePacker `.tps` relativized paths; against free-tex-packer).
- **Settings ≡ CLI flags ≡ project fields, 1:1** — one mental model, provable
  parity (rTexPacker settings-map-to-flags; AGENTS parity invariant).
- **Content-hash no-op with `--force` escape hatch** — makes "run on every build"
  free (TexturePacker `smartUpdate`, crunch).
- **Input↔region cross-highlight + hover shows region size** — hovering a sprite in
  the tree highlights its rect on the page and vice-versa (gdx-tpgui).
- **Output-overlaps-input safety check** — refuse to clobber source folders
  (gdx-tpgui).
- **OS-native file dialogs; don't hand-roll browsing** (rTexPacker).
- **Single-key overlay toggles** for outlines/trim/pivot — inspection without an
  inspector (rTexPacker).

### Don't (avoid)

- **Absolute paths in the project** — free-tex-packer's worst defect; projects
  die on another checkout path. Relative always; accept absolute on load, warn,
  relativize on save.
- **Store display strings as identifiers** — free-tex-packer forced a
  display-name→id mapping table in its CLI. Store stable ids; render labels only
  in the UI.
- **Conflate app version with schema version / skip migrations** — free-tex-packer
  silently reset renamed options. Integer schema version + chained migrations.
- **Duplicate pack/export logic between GUI and CLI** — free-tex-packer's GUI
  didn't depend on its core, so they drifted. One `tp_core`; frontends are thin
  (AGENTS invariant).
- **Hidden modal key-states for editing** — rTexPacker's "mixed" verdict: edit
  modes are fine, but make the active mode explicit (banner + Esc), not an
  invisible global key-state.
- **Floating panels over the canvas** — rTexPacker occlusion complaint; use fixed
  docked panels.
- **CLI that can't use a custom exporter because the template isn't in the
  project** — free-tex-packer's gap. Store user-template paths project-relative so
  they travel (relevant when Phase 7 mustache templates land).
- **Ship without a watch mode** — every surveyed OSS packer lacks it; it's our
  cheapest differentiator (`spine-libgdx.md`, `oss-and-architecture.md`).

---

## Open UX questions for the owner

1. **CLI verb** — resolved (owner, 2026-07-10): `pack` (this doc, ROADMAP) vs
   `export` (task packet)? **`pack` is canonical; `export` ships as a supported
   alias.**
2. **Atlas switcher** — resolved (owner, 2026-07-10): left **list** (per-row
   controls, scales) vs top **tab strip** (closer to TexturePacker but
   `nt_ui_tabbar` can't carry a per-tab close/reorder)? **Left vlist confirmed.**
3. **stdout contract** — resolved (owner, 2026-07-10): reserve stdout for a
   future machine-readable (`--json`) summary now, or send progress there?
   **Confirmed: stdout stays reserved for the future `--json` summary; progress
   goes to stderr (§4.3).**
4. **OS file-drop** — resolved (owner, 2026-07-10): is drag-drop onto the
   window a v1 target, or is the Add-folder dialog sufficient for MVP?
   **Deferred post-v1 — the Add-folder dialog suffices for v1; whether the
   engine surfaces the GLFW drop callback still needs verification before any
   post-v1 drop-support work starts.**
5. **Panel resize** — resolved (owner, 2026-07-10): fixed proportions for v1
   (recommended) or invest in a draggable splitter early? **Fixed proportions
   ship in v1; a draggable splitter is deferred to later.**
