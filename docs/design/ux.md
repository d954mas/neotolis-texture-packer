# GUI UX contract

**Status:** active implementation supplement

**Normative source:** [`../ntpacker-master-spec.md`](../ntpacker-master-spec.md)

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
