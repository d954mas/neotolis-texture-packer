# GUI UX contract

**Status:** active implementation supplement

**Normative source:** [`../ntpacker-master-spec.md`](../ntpacker-master-spec.md)
(canvas, workspace, and interaction model: §61)

**Scope:** human-facing interaction and presentation only

This document does not define project schema, operation semantics, CLI/MCP
shapes, format capabilities, or roadmap order. Those belong to the master spec,
the executable format/CLI contracts, and the derived roadmap. If this document
conflicts with the master spec, the master spec wins.

## 1. Product interaction principles

1. **Explicit state.** The UI distinguishes project dirty state, preview
   freshness, source health, running work, and export loss. These are separate
   facts and must not be collapsed into one “changed” indicator.
2. **Explicit Pack.** Project edits and source watcher events mark the current
   preview stale; they do not start Pack automatically. The last completed
   preview remains visible until the user explicitly runs Pack.
3. **Predictable side effects.** Export, Extract Sprites, overwrite, install,
   and other file-writing actions show a preflight summary. Destructive or lossy
   actions have an explicit choice and a machine-equivalent dry-run path.
4. **Structured failure.** Bad input produces a visible structured error and
   leaves the application usable. A missing or corrupt source is never silently
   replaced with stale full-resolution pixels for Pack or Export.
5. **Capability parity, interface freedom.** GUI, CLI, MCP, and Dev API expose
   the same product capability through interface-appropriate shapes. There is
   no requirement that every GUI control map to one CLI flag.
6. **Mouse complete, keyboard efficient.** Every action is reachable by mouse.
   Common actions also have shortcuts, but shortcuts never hide the only route.

## 2. Workspace

The primary workspace remains a three-region desktop layout:

```text
project/atlas/source tree | pack or source canvas | contextual inspector
--------------------------------------------------------------------------
status, notices, job progress, connection and freshness
```

- The left region answers “what am I editing?”
- The center answers “what does the selected source/result look like?”
- The inspector answers “what can I change or run here?”
- The status/history surfaces answer “what happened, who changed it, and is the
  visible result current?”

Responsive contraction may collapse secondary panels, but it must not remove
the current project/atlas identity, primary action, error state, or freshness.

### 2.1 Two canvas levels

The center canvas has two levels and no third "page" level (normative model:
master spec §61.1):

- **Project level** presents each atlas as a group-card: name, health badges
  (fill %, outdated, not packed, warning), a visibility eye, and the last pack's
  page thumbnails. Boards hold freeform positions -- auto-placed when the atlas is
  created, never moved by the tool, re-gridded only by an explicit "Tidy up".
  Hiding an atlas leaves its spot empty; an "N hidden" chip restores it.
  Project-level batch actions include Pack stale (N) and Export all.
- **Atlas level** lays out all of one atlas's pages spatially at once. Page tabs
  are removed; double-clicking a page zooms the camera to it (a camera move, not a
  mode) down to texels; sprites are selectable here.

Breadcrumbs (Project > atlas) ascend. A single continuous canvas and a third page
level are rejected.

### 2.2 One project tree

The left region is one project tree with a clickable project root node; selecting
the root shows project settings and export rules in the inspector. The tree is
root -> atlases -> folders/sprites -> a per-atlas Animations node, replacing the
separate ATLASES / SPRITES / ANIMATIONS sections. The Ctrl+F filter is
project-wide and shows matches under their atlases. Cross-atlas drag works in the
tree; pages are packer output and are never drag targets.

### 2.3 Sorting is view state

Sorting is view state, not project data. Four keys -- name (default), size, file
mtime, and added_at -- each with two directions (re-clicking the active key flips
it), plus an independent "warning on top" checkbox that overlays any sort. A key
applies within each tree level. Atlas order and animation frame order stay manual;
filesystem-derived rows mirror disk and are never manually reordered.

### 2.4 Thumbnails and display modes

Thumbnails are the default, not a mode: sprite and atlas rows always show previews
(async thumbnail cache), and hover shows a large 1:1 preview. The sprite area has
list and grid display modes (a header toggle, view state). The atlas cards view is
the Project canvas level, not a panel mode.

Performance budgets are validated at the owner's real scale (30 atlases,
5000 sprites): sub-100 ms interactions, non-blocking refresh, virtualized lists,
and a bounded thumbnail cache.

## 3. Project and session state

### 3.1 Dirty and saved

Dirty state compares current semantic project state with the saved baseline.
Save does not create an Undoable model operation and does not change revision.
The History surface may show a non-Undoable Save checkpoint.

The title/status area shows at least:

- project path or `Unsaved project`;
- saved/modified;
- live integration connected/disconnected when Epic A is present;
- source errors count;
- Pack running/current/stale/not yet run.

### 3.2 History

Undo/Redo labels describe one semantic transaction, for example:

```text
Create enemy animations
Replace linked atlas with extracted folder
Set max page size
```

One external multi-operation transaction appears as one History entry and is
undone once. Snapshot-based history is only a migration oracle for the current
implementation; it is not the target UX contract.

### 3.3 Pack freshness

The canvas keeps the last completed result visible when it becomes stale.
Staleness is prominent but non-modal:

- `Current` — preview hash matches current Pack inputs;
- `Stale` — project/source inputs changed after this result;
- `Running` — a Pack job is computing from an immutable snapshot;
- `No result` — no usable completed result exists;
- `Failed` — latest Pack failed; the last older result may remain viewable and
  must retain its own stale/current identity.

Running Pack results never silently replace a newer user-selected result.

## 4. Sources

Source rows show type, display name/path, runtime health, and whether the source
is read-only. The target source model includes path files, path folders, and
linked atlases.

Watcher events refresh runtime status and thumbnails automatically. They do not
change revision, dirty state, or Undo history and do not start Pack. Manual
Refresh remains available and forces verification.

For a missing or corrupt source:

- keep persisted project metadata;
- show the error on the source and affected sprites;
- a last-good thumbnail may remain visible with a stale/error badge;
- Pack, Export, and Extract retry the current files and fail clearly if the
  source is still unavailable.

Linked-atlas regions are visibly read-only. Users may change source-level import
settings and format selection, but not edit a derived region in place.

## 5. Pack and export

The main Pack action is always explicit. Target preview and Export show the
selected format, data version/profile, adaptation notices, metadata losses, and
affected sprites before files are written.

Severity language:

- **Notice:** compatible adaptation, such as disabling unsupported transforms;
- **Warning/loss:** valid output is possible but runtime meaning or metadata is
  reduced;
- **Error:** no valid artifact can be produced.

Warnings do not disable Export by themselves. A blocking error does.

### 5.1 Export rules and preflight

Export rules (targets) are edited only at the project root; the atlas inspector
shows a compact participation block -- a per-target enable toggle plus an optional
path override -- not a second target editor (normative model: master spec §31,
§61.1). Ctrl+E opens preflight only and never edits targets. Preflight scope is
the current selection (nothing selected at Project level means all) and it shows
the dry-run file list, amber overwrites, per-target degradation notices, a
"Copy CLI command" action, and an optional "Export as ZIP" (§37.2).
Repeat-last-export is Ctrl+E then Enter.

## 6. Import and Extract Sprites

Import format selection is visible. Exact unique detection may preselect a
format, but the user can change it. Ambiguous input presents candidates and
never guesses.

`Extract Sprites` asks the user only for the output folder in its primary flow.
Before commit it presents:

- current linked atlas and selected format;
- output file count and paths;
- collisions, traversal or invalid-name errors;
- overwrite policy (default: fail);
- the project change that will replace the linked source with a folder source.

All files are staged before publication. The model changes only after output is
ready. Undo restores the linked source and metadata but does not delete published
files; the UI says this before commit.

## 7. Live AI integration

When Epic A is present, the GUI exposes:

- connection/authorization status;
- the external controller identity;
- Ask/Allow/Deny and explicit Replace Controller actions;
- Disconnect/Revoke;
- transaction author in History;
- ownership handoff/recovery progress without a second writable copy.

The GUI and AI edit the same live unsaved project. A hidden file copy or later
merge is not an acceptable implementation.

## 8. Current baseline and migration

The shipped GUI already provides project editing, worker-based Pack, canvas
inspection, settings, animations, export, manual Refresh, and snapshot Undo.
The following target behaviors are not implied to be implemented merely because
they are specified here:

- semantic transaction History and shared revision;
- watcher-backed runtime source state;
- canonical Pack hashes and result LRU;
- linked atlas sources and Extract Sprites;
- format selection/package diagnostics;
- live Dev API/MCP ownership and authorization.

Implementation status belongs in [`../ROADMAP.md`](../ROADMAP.md), not here.

## 9. Verification

Every UX packet must include:

- a core/contract test for behavior, not only a screenshot;
- GUI self-test or deterministic interaction coverage for the visible state;
- mouse-only and shortcut paths for primary actions;
- structured error/loss examples;
- confirmation that frontend code does not reimplement operation or capability
  rules;
- visual regression evidence when layout or drawing changes.

Historical audits and visual proposals under `docs/design/` remain evidence,
not active requirements, unless revalidated against this document, the master
spec, and current code.

## Visual token reference (from ui-redesign-2026-07-11)

Salvaged from `docs/design/ui-redesign-2026-07-11.md` (deleted 2026-07-20; git
history has the full redesign rationale + competitor comparison). **These tokens
are what the shipped GUI currently encodes** — the concrete color/spacing/type/
icon vocabulary the native `ntpacker-gui` (`nt_ui`/Clay) was built against. Phase
U (§61) may revise them; until it does, they are the reference. Constants live in
`apps/gui/main.c` (palette/layout) unless noted.

### Palette tokens (exact RGB)

Clay colors are `{r,g,b,255}`; button `bg_tint` uses `RGBA8(r,g,b)` — author every
tint through it, never a hand-packed `0xFF..` literal (the hand-packed accent was
the root of the amber/blue-swap bug).

| Token | Role | RGB |
|---|---|---|
| `bg` | window base | 17, 18, 23 |
| `canvas` | canvas well (darkest surface) | 11, 12, 15 |
| `panel` | side panels (mid surface) | 28, 31, 40 |
| `header` | section-header fill (**lighter than panel** so headers advance) | 40, 45, 57 |
| `input` | field well (recessed, darker than panel) | 21, 23, 30 |
| `hover` | row/btn hover | 46, 52, 66 |
| `sel` | selected-row fill (desaturated blue — reads as selection, not a button) | 48, 74, 120 |
| `border` | 1px separators | 52, 58, 72 |
| `border-strong` | focus ring / active input | 86, 132, 204 |
| `accent` | **primary action** (one bright saturated blue; clearly ≠ `sel`) | 64, 140, 214 |
| `accent-press` | primary pressed | 48, 112, 182 |
| `warn` | **stale / warning** (amber — distinct hue from every blue) | 228, 158, 92 |
| `success` | up-to-date / exported (green, notices) | 104, 186, 124 |
| `danger` | remove / error (red, destructive) | 214, 96, 96 |
| `text-strong` | values, active row | 230, 234, 242 |
| `text` | rows, body | 196, 204, 216 |
| `text-dim` | labels, captions | 140, 148, 164 |
| `text-faint` | hints, stale stats, disabled | 98, 104, 120 |
| `link` | hyperlink | 110, 170, 245 |

Retire the separate dim title color; section titles get weight from **uppercase +
tracking + a 3px `accent` left-rule**, panel/atlas titles use `text-strong`.

### Spacing scale (via `S()`)

4px base. `SP_XS S(4)`, `SP_SM S(6)`, `SP_MD S(8)`, `SP_LG S(12)`, `SP_XL S(16)`.
Rules: icon↔label gap `SP_SM`; row inner padding `SP_MD` horizontal; section gap
`SP_LG`; panel padding `SP_MD`. `BASE_ROW_H 27`, strip/header `34/26` heights stay.

### Type scale / tiers (single Slug vector font — weight comes from size + color + case)

| Style | px (base) | Color tier | Use |
|---|---|---|---|
| `title` | 16 | text-strong | panel/atlas title |
| `section` | 13 | text-dim, **UPPERCASE, +8% tracking** | ATLASES / SPRITES / Region / Export targets |
| `body` | 15 | text | button labels, values, editable fields |
| `row` | 15 | text | list rows |
| `caption` | 13 | text-dim | secondary/meta |
| `hint` | 20 | text-faint | empty-state canvas hint |
| `tag` | 13 | on-accent/on-warn | chips, badges |

Section headers are the hierarchy backbone: quiet (dim, small, uppercase) but
structured by the accent rule. Do **not** make them big/bright.

### Status severity color language (§2.8)

The bottom status bar carries the shared severity language (the future problems
panel — §61/UX-B.6 — inherits it): a leading severity icon + tint — info
`text-dim`+`info`, success `success`+`check-circle`, warning `warn`+`alert-triangle`,
error `danger`+`octagon-alert`. Errors must not be overwritten/decolored by the
next info write. Stale stats keep `text-faint` (they describe the last pack).
Stale-state is the one place amber is mandatory: amber Pack button + amber
"outdated — press Pack" chip + `alert-triangle` icon + dim the canvas ~12% with a
corner "outdated" tag.

### Icon system — Lucide inventory + bake pipeline

**Vehicle:** `nt_ui_image` — an atlas-region image with `color_packed` **tint**,
composed as a child inside `nt_ui_button_begin/_end` or standalone in a row. Icons
are baked **monochrome white-on-alpha once** and tinted at render to any text
tier/accent/warn — one sprite serves every state. No new engine widget.

**Bake pipeline.** Add icons to `ntpacker_ui_atlas` in `apps/gui/build_packs.c`
(alongside `_white`) — that atlas is `shape=RECT`, `allow_transform=false` (icons
must never rotate/flip-pack), `LINEAR` min/mag, premultiplied. Codegen emits
`ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS__PACK` etc.; resolve at runtime like the
white region (`nt_atlas_find_region` → `nt_atlas_ref_idx` → an
`nt_atlas_region_ref_t` per icon). Because `build_packs` runs with WORKING_DIRECTORY
= engine root, pass the ntpacker icons dir as a **new `argv[3]`** from the
`add_custom_command` (`= ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons`) and join
absolute in `build_packs.c`.

**Grid & scale survival.** Author on a **24px grid, 2px stroke**; export each
master PNG at **48px (2×)**; render into logical boxes `S(16)` (strip/menubar/
row-action), `S(14)` (row type icons), `S(12)` (chevrons). One 48px master
downsamples crisply via LINEAR at scale 1.0/1.5 and is ~native at 2.0. Hero
empty-state icon: separate 96px master rendered `S(48)`. **Avoid 1px hairlines**
(they vanish on downscale) — 2px stroke minimum. Style: stroke/outline, 2px,
rounded joins, monochrome (Lucide house style); filled variants only for status
dots. **Source:** vendored Lucide (MIT/ISC), pre-rasterized to PNG, with
`apps/gui/assets/icons/{*.svg,*.png}`, a `regen_icons.py`, and an `ATTRIBUTION.md`
checked in (commit the PNGs so the build needs no Python).

Inventory (Lucide glyph → placement → tint):

| Area | Icon | Placement | Tint |
|---|---|---|---|
| Strip | `layout-grid` | Pack button | text-strong / on-warn when stale |
| Strip | `alert-triangle` | prepended on Pack + chip when stale | warn |
| Strip | `download` | Export | text |
| Strip | `refresh-cw` | Refresh | text-dim |
| Strip | `chevron-left`/`chevron-right` | page ◄/► | text-dim |
| Strip | `minus`/`plus`/`scan`/`maximize-2` | zoom −/+, 100%, Fit | text-dim |
| Left | `layers` | atlas rows | text / text-strong sel |
| Left | `folder`/`folder-open` | folder source + expand state | warn-ish (smart) |
| Left | `image` | sprite/file leaf rows | text-dim |
| Left | `film` | animation rows | text-dim |
| Left | `file-plus`/`folder-plus`/`plus` | +Files / +Folder / +Atlas·Animation·Target | text |
| Left | `x` | remove × on source rows | text-dim → danger hover |
| Right | `chevron-down`/`chevron-right` | section chevron | text-dim |
| Right | `check` | checkbox tick | accent |
| Right | `link`/`external-link` | out-path browse; About repo | link |
| Canvas | `square-dashed`/`crop`/`crosshair` | overlay toggles outline/trim/pivot | text-dim / accent when on |
| Status | `info`/`check-circle`/`alert-triangle`/`octagon-alert` | status-bar + notices severity | dim/success/warn/danger |
| Empty | `folder-plus` (96px) | empty-state hero | text-faint |

Icon-only buttons **require** a tooltip (`nt_ui_tooltip`). Menubar top-level
(File/Edit/View/Help) stays text; icons go on menu *items* only, optional.

### Responsive breakpoints (logical px, post-scale)

Deterministic, three breakpoints, no reflow/wrapping (the strip must never wrap):

| Logical width | Strip | Panels | Labels |
|---|---|---|---|
| **≥ 1180** (wide) | Pack/Export = icon+label; others icon-only | left 300 / right 300 fixed | full label column (`PANEL_LABEL_W` 116) |
| **900–1180** (mid) | Pack/Export icon+label; hide zoom-% label; chip → icon+2 words | right label column clamps to ~92 | ellipsize values |
| **< 900** (narrow) | all strip buttons icon-only (tooltip carries the label) | side panels shrink to min (left/right 240) before canvas gives up | label column ~80, ellipsize |

Rules: icon-only buttons always keep a tooltip; never wrap the strip to two lines
(drop labels instead); the menubar project title ellipsizes rather than pushing
menus.
