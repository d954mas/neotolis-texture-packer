# Architecture overview

**Status:** Current architecture.

Neotolis Texture Packer is one native application with a shared core and thin
clients:

```text
saved project / live operator
              |
   file CLI / agent / native GUI
              |
     typed operations + session
              |
 project model / history / recovery
              |
 source scan + pack/export jobs
              |
       neotolis-engine builder
```

## Ownership layers

- `packer/tp_core` owns the project model, canonical identity, validation,
  operation catalog, transactions, session, snapshots, persistence, history,
  recovery, source scanning, immutable Export IR, native format descriptors,
  the immutable format catalog and strict package discovery/diagnostics,
  artifact planning/publication, and pack-result metadata.
- `packer/tp_build` owns fallible builder execution, worker-process transport,
  packing, parse-back, and export orchestration.
- `apps/cli` parses saved-file commands and renders human or versioned JSON
  responses. It does not own live-session policy.
- `apps/agent` owns JSON-lines framing, request correlation, and one new
  headless session. It adapts the shared operation, observation, history, and
  recovery APIs; it owns no second mutable project.
- `apps/gui` owns native presentation, drafts, intent scheduling, and the
  process host around one shared session. It does not implement a second project
  mutation engine.
- `apps/common` owns shared application-data paths, recovery key, and catalog
  startup, plus JSON syntax and policy-file admission used by agent mode.
- `external/neotolis-engine` is a read-only dependency. The packer uses public
  engine APIs and shared format headers.

Clients never bypass core validation, canonical naming, transaction,
revision/dirty, persistence, or Undo rules.

The native GUI has one explicit control loop:

```text
view request / typed draft
          |
   gui_actions_step
          |
 internal gui_project_step
          |
 tp_session_update + FSM transition
          |
 newly borrowed session view + typed receipts
```

Views never pump the session, poll jobs, cancel a job directly, or assemble
lifecycle phases. `gui_actions.h` contains only typed input and passive state;
the host-only `gui_actions_driver.h` exposes the one between-frame actions
step, and `gui_actions_dev.h` isolates blocking test/dev adapters. The project
step is an internal primitive callable only by the actions controller, enforced
by architecture gates.
New/Open/Exit confirmation is one tagged actions state (`idle`,
`resolve-draft`, `resolve-dirty`, or `open-dialog`) plus a typed user choice
for resolve phases. The Open file picker is the terminal input of its own
exclusive phase, so ordinary queued inputs cannot run between confirmation and
the selected/cancelled dialog terminal. Views read passive state and submit a
choice; they do not coordinate modal flags or carry the continuation
themselves. Startup recovery likewise exposes
one passive `idle`/`choose`/`resolving` state and typed choices, not a public
modal flag or mailbox. Startup, shutdown, and blocking test/dev adapters enter
through action-controller helpers and use the same step, rather than driving a
second host loop.
Any mutable session call closes the current borrowed observation cut. The
controller retains unconsumed typed inputs, publishes a fresh cut through the
project step, and resumes them on a later tick; callers still use only
`request -> gui_actions_step -> view`.

## Current and target boundaries

The current product has a file-oriented CLI, a native live GUI, and the first
agent-mode packet over the in-process live-headless session shape. The agent
can create a new unsaved session, inspect its snapshot/history, transact,
Undo/Redo, and close with recovery preservation. It has no saved-project
binding, Save/job commands, GUI attachment, IPC, or handoff yet. The supported
wire is [agent v1](../formats/agent-v1.md); the full target is in
[automation](../spec/automation.md) and
[the approved v1 design](../spec/agent-mode-v1.md).

Current format execution combines the fixed built-in native exporters with
strict API-v1 Lua packages over Export IR v1 and one common artifact
planner/publisher. Startup builds an immutable owned catalog from the
executable-relative `formats/` root, validates Lua candidates in an isolated
compiler, and atomically installs the complete eligible generation or the
native fallback. Admission captures exact package bytes for the outer worker;
only that worker reaches the Lua 5.5 kernel. There is no production runtime C
registration surface. GUI Reload Formats builds a complete candidate through
the same scanner/compiler, drains catalog-dependent work, and atomically swaps
only an eligible generation. Import and linked-atlas ingestion remain outside
this slice. The current runtime and bundled-format contract is documented in
[`../spec/format-ecosystem.md`](../spec/format-ecosystem.md).

## Primary durable contracts

- [Project format v5](../formats/project-v5.md)
- [Model, operations, and session](model-operations-and-session.md)
- [Persistence and recovery](persistence-and-recovery.md)
- [Jobs, pack results, and cache](jobs-pack-and-cache.md)
- [Sources and raster ingress](sources-and-raster.md)
- [Engine and client boundaries](engine-and-client-boundaries.md)

Plans, research logs, review history, and old implementation decisions are not
runtime authority. Code and executable tests prove the implemented state; the
current and target product specs define intended behavior.
