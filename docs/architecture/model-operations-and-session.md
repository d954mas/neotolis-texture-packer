# Model, operations, and session

**Status:** Current architecture.

The live project has one mutation path:

```text
client intent
  -> typed operation(s)
  -> transaction admission
  -> candidate clone + validation
  -> one non-fallible model publication
  -> event/history/recovery observation
```

## Canonical model

The authored `tp_project` aggregate owns the persistent graph described by
[project format v5](../formats/project-v5.md). `tp_model` is the private
transaction owner around that aggregate and runtime state:

- monotonic revision;
- semantic saved baseline and dirty state;
- one retained immutable format-catalog generation;
- bounded transaction-ID retention;
- optional semantic Undo/Redo history;
- optional best-effort recovery journal.

Revision starts at zero and increments exactly once for each committed
transaction or Undo/Redo transition. Save does not increment revision.

Dirty state is semantic identity compared with the last saved baseline, not
`revision != saved_revision`. A transaction that returns the graph to the saved
meaning becomes clean even though its revision remains higher.

Persistent entity deep-copy/free is defined once in the private
`tp_project_owned` module. Clone and semantic diff retain independent allocator
and fault-injection domains, but both supply those allocators to the same field
ownership implementation. Packed output is the distinct public `tp_result`
surface declared by `tp_pack_result.h`; it is not an authored model.

## Operation catalog

`tp_operation` is an append-only, closed mutation vocabulary. Operations target
stable structural IDs and belong to create, remove, move, or set classes. There
is no raw JSON field-patch escape hatch.

The shared catalog provides:

- stable wire token and effect class;
- primary target ID kind;
- labels and label templates;
- allowed keys;
- field type, range or enum, grouping, clear/inherit behavior;
- current CLI lowering where applicable.

The same registry drives field admission metadata, operation encoding/lowering,
CLI key parsing, and schema-driven future clients. Semantic history/diff
capture still has exhaustive operation-specific handling and must be updated
when the vocabulary grows. Cross-field and effective-value rules remain in the
central operation validator.

Reserved operation kinds are not current capabilities. In particular,
linked-source replacement and bulk animation-frame set are present as future
wire slots but have no ordinary CLI behavior today.

## Transactions

Transaction schema 1 carries:

- a 128-bit lowercase-hex idempotency ID;
- `expected_revision`;
- optional author/label;
- an ordered batch of typed operations.

Requests are bounded before JSON materialization or model cloning. The current
limits are 1 MiB, 4096 operations, and a 4096-ID deterministic retention
window.

Admission checks the revision precondition, lowers and validates the complete
batch, applies it to a private clone, verifies the resulting canonical graph,
and prepares history/recovery information. Publication is one
allocation-free pointer swap. Rejection leaves the live model unchanged.

An expected revision lower than current is a revision conflict; one higher than
current is invalid. There is no implicit merge or CRDT behavior.

## Semantic history

History records semantic before/after state for touched entities, not inverse
command scripts. Undo and Redo use the same validation/publication boundary and
increment revision.

The session presents one history surface combining:

- undoable committed edits;
- non-undoable successful Save checkpoints;
- non-undoable external source refresh markers.

Recovery restores project state, not the previous process's in-memory Undo/Redo
stack.

## Session ownership

`tp_session` owns the live model, canonical project identity/lease, history,
recovery binding, snapshots, event sequence, source-runtime generation, and at
most one concrete derived job.

The model/session catalog is an explicit dependency, not mutable process-global
state. Session creation/open APIs accept a catalog generation, and every owned
snapshot retains that same generation independently. Target create/set
operations require the referenced exporter to be available in that catalog;
project-file parsing still admits syntactically valid absent IDs, and project
validation reports them as `unknown_exporter`. Recovery replay validates the
durable graph without an availability check, so an absent target ID is
preserved and can revive when a later catalog contains it. Compatibility entry
points use the immutable native-only baseline explicitly.

Session mutation and view publication run on one owning host thread. Worker
processes never mutate the session. Replacement is a prepare/drain/commit
lifecycle: an old task cannot publish into a new session instance generation.

## Current view and snapshots

Live clients call `tp_session_update` once at their host boundary, then borrow
the current `tp_session_view` until the next non-const session call. That one
cut contains:

- current model snapshot;
- revision, dirty, history, recovery, and source-runtime state;
- current task progress and terminal metadata.

The heavy terminal payload transfers out of `update` exactly once. The session
retains only compact terminal metadata. Owned snapshots remain available for
real file, thread, or transport boundaries; the frame loop does not allocate
one per update.

Fallible view preparation happens before publication. In particular, a
successful Refresh prepares its replacement snapshot before replacing the
source projection or advancing generation, events, and history. If preparation
fails, the previous complete view remains current and the terminal task remains
available for a later `update` retry.

The GUI client converts typed requests and explicit edit drafts into operations
in its local `gui_project_operations` lowering module, submits them to the
session, then renders from the borrowed current view. `gui_actions_step` is the
one host-facing between-frame boundary in `gui_actions_driver.h`; view-facing
`gui_actions.h` exposes only typed inputs and passive FSM state. The actions
step calls the internal `gui_project_step`, which alone updates the active
session and receives the owned terminal payload; operation lowering,
persistence, recovery queries, and views never perform a second observation.
Typed step receipts report task admission and lifecycle completion without
exposing a pump sequence. A failed automatic Refresh admission is a structured
step error; it is never collapsed into an idle observation that could allow a
later Pack or Export to run against the old source projection. This is not a
session adapter or client mirror. Drafts are UI state, not a second hidden
project copy.

A mutable session call closes the current GUI observation cut immediately.
Snapshot and source accessors are unavailable until `gui_project_step`
publishes the next borrowed view. The actions controller therefore stops the
current drain at that boundary and retains every unconsumed typed input. Later
calls to the single public `gui_actions_step` resume those inputs only after a
fresh view has been published. Callers submit inputs and tick one controller;
they never remember or reconstruct an apply/update/poll sequence.

Several edits captured from the same published revision are also sequenced by
that controller. After an accepted one-revision transaction, it rebases only
the retained dependent edit intents from that exact cut; already-stale inputs
and larger foreign revision jumps remain stale and are rejected normally.
Save As preparation is bound to the current session instance and revalidates
the controller/identity guard at execution.

The file CLI uses immutable load/apply-preview facilities for queries and dry
runs, and a short-lived writable session for saved-file mutations.
