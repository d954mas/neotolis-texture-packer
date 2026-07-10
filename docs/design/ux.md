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
1. Toolbar **Export All** → for each enabled target, `tp_core` packs with
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
