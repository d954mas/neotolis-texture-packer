# Canvas, workspace, and interaction

**Status:** Target contract; current GUI does not yet implement the complete
workspace model described here.

Current interaction invariants remain:

- Pack is explicit; no auto-pack on edit or source refresh.
- Editing uses explicit drafts and commits typed transactions.
- Selection, filters, scroll, panels, camera, and preview playback are view
  state.
- The current GUI observes the shared session/history/project; future
  automation clients must observe that same authority.
- Committed transactions identify their author as `human` or
  `agent(<external-controller identity>)`; Undo/Redo semantics are identical
  for either author.

The target below preserves the approved durable canvas/workspace direction
without claiming it is present in canonical v5.

## Two canvas levels

There are exactly two canvas levels:

1. **Project** shows one group-card per atlas, including health, visibility, and
   bounded page thumbnails. Batch Pack/Export actions live here.
2. **Atlas** shows all pages of one atlas spatially. Double-clicking a page moves
   the camera; it does not enter a third mode.

Breadcrumbs ascend from Atlas to Project. Double-clicking an atlas card
descends. A single continuous cross-atlas canvas and a third page level are
rejected.

## Unified project tree

The left panel is one tree:

```text
project
  atlas
    folders and sprites
    animations
  project notes
```

The project root is selectable and owns project settings/export rules.
Project-wide filtering searches every atlas and note. Cross-atlas drag uses
model operations; generated pages are never drag targets.

Sorting is view state. Name, packed size, live mtime, and `added_at` views do
not rewrite model order. Manual atlas order and animation frame order remain
undoable model operations. Filesystem-derived rows mirror disk rather than
gaining manual persistent order.

The target post-v5 source record adds a write-once `added_at` stamp set when the
source is added. It is never refreshed from the filesystem. It supports
recently-added sorting and a new-source badge without persisting churning
`mtime` values. This field is not backported to canonical v5 and requires an
explicit newer-schema migration.

Lists are virtualized and thumbnails come from bounded async/cache-backed
presentation state. A 30-atlas / 5000-sprite bench fixture validates sub-100 ms
ordinary interactions, non-blocking refresh, virtualized lists, and a bounded
thumbnail cache.

## Settings provenance and operation surfaces

For every inherited setting the target core exposes both the effective value
and its origin. Clients use that information consistently:

- an overridden field has a non-default marker;
- a modified-only view is derived from provenance;
- reverting means a typed clear/`clear_override` operation at the level that
  supplied the value, not assigning the currently inherited value;
- validation runs on effective values.

Any new persisted default layer requires a project schema newer than v5 and an
explicit migration. The presentation contract does not authorize silently
adding fields to canonical v5.

The operation catalog supplies stable labels, label templates, argument types,
ranges/enums, and clear/inherit behavior. The target command palette indexes
that catalog rather than maintaining a second command vocabulary in the GUI.
Named Undo uses the same operation/transaction labels.

## Export interaction

Future project-level export rules are edited at the project root. Atlas
inspectors show participation and optional per-atlas path override.

Export preflight is read-only. It reports:

- exact scope;
- dry-run artifact list;
- overwrite conflicts;
- effective settings and capability losses;
- a reproducible CLI command;
- optional archive delivery.

Opening preflight never edits target configuration. Canonical v5 continues to
use per-atlas targets until a newer schema and migration land.

## Visible history and diagnostics

The History surface shows semantic transactions from human and automation
clients in one order. Each entry has its transaction label and author. One
multi-operation transaction is one entry and one Undo.

A successful Save may appear as a non-undoable checkpoint. Failed Save creates
no checkpoint. Source refresh/runtime markers may appear as non-undoable
session information; History is not a persistent audit log.

Structured notices feed a persistent Problems/status surface. A problem retains
enough typed target information to navigate to the affected project object,
canvas item, or field. Pack freshness, project dirty state, source health,
running work, and export loss remain separate facts rather than one generic
“changed” indicator.

## Workspace model

A future project schema adds authored workspace state:

- atlas board positions;
- atlas display visibility;
- project- and atlas-parented text notes.

This is model state: changes use typed operations, enter semantic history, dirty
the project, and synchronize through automation.

Board positions are freeform. Creation chooses an initial free position, but
the tool does not continuously reflow:

- hiding an atlas leaves its space;
- Pack growth wraps previews inside the board instead of pushing neighbors;
- overlap is legal and does not change atlas data;
- Tidy is an explicit user-invoked operation.

Notes are text model objects, not a drawing layer. A note has content, parent,
position, and one per-object style. Atlas-parented notes move/hide/delete with
the atlas; project notes use project-canvas coordinates. Reparenting is an
explicit drag/operation. Notes live only on Project canvas until a stable
sprite-canvas anchor exists.

Exact workspace field names, note style tokens/palette, and operation wire
schemas remain open release contracts.

## View state versus project state

App-level settings outside the project contain:

- window geometry and recent projects;
- panel layout and list/grid mode;
- sort/filter presentation;
- current canvas level, camera, zoom, and overlays.

They never dirty the project or enter version control.

The future workspace section is the deliberate exception: board positions,
notes, and authored atlas visibility are synchronized project state. It
requires a project version newer than 5 and an explicit migration.

## Rejected directions

Do not introduce without new product approval:

- automatic Pack on edit or watcher change;
- a third page canvas level;
- a single continuous cross-atlas page flow;
- per-operation AI confirmation dialogs;
- manual persistent reordering of filesystem-derived rows;
- continuously persisted filesystem timestamps;
- target editing inside Export preflight;
- a freeform drawing layer disguised as project notes.
