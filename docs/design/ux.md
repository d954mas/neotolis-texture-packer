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
