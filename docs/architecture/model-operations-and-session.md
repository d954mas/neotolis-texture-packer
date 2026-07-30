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

`tp_project` owns the persistent graph described by
[project format v5](../formats/project-v5.md). `tp_model` wraps it with runtime
state:

- monotonic revision;
- semantic saved baseline and dirty state;
- bounded transaction-ID retention;
- optional semantic Undo/Redo history;
- optional best-effort recovery journal.

Revision starts at zero and increments exactly once for each committed
transaction or Undo/Redo transition. Save does not increment revision.

Dirty state is semantic identity compared with the last saved baseline, not
`revision != saved_revision`. A transaction that returns the graph to the saved
meaning becomes clean even though its revision remains higher.

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

Session mutation and observation publication run on one owning host thread.
Worker processes never mutate the session. Replacement is a prepare/drain/
commit lifecycle: an old job or observation cannot publish into a new session
instance generation.

## Observation and snapshots

Clients consume immutable snapshots and a pull-based observation token:

- current model snapshot;
- revision, dirty, history, recovery, and source-runtime state;
- current job state and terminal result;
- events after a caller's last observed sequence.

If the event window is no longer available, the client resynchronizes from an
owned snapshot. Borrowed observation data is valid only for the documented
observation lifetime; views do not retain pointers across a mutation or refresh.

The GUI client converts frame intents and explicit edit drafts into session
calls, then rebuilds presentation from the next observation. Drafts are UI
state, not a second hidden project copy.

The file CLI uses immutable load/apply-preview facilities for queries and dry
runs, and a short-lived writable session for saved-file mutations.
