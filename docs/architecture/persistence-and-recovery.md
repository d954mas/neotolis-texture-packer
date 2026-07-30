# Persistence and recovery

**Status:** Current architecture.

Project-file publication and crash recovery are related but independent
authorities:

- a project Save publishes canonical `.ntpacker_project` bytes;
- a recovery journal records a best-effort crash-recoverable prefix of live
  state.

A recovery failure never rolls back an already committed model transition or an
already published project file.

## Canonical writer lease

A writable session acquires an OS-backed exclusive lease for the canonical
project identity. Equivalent path aliases share the same lease domain.

- Another live writer receives `project_live`.
- Read-only CLI inspect/validate operations do not require the writer lease.
- Saved-file mutations respect the lease.
- Stale lock files are not mistaken for live owners.
- Save As acquires and transfers authority without a window in which two writers
  own the same destination.

The lease protects writer ownership; it is not stored in the project file.

## Save publication

Before publication, the writer serializes and validates a complete canonical v5
candidate. It does not partially rewrite an invalid project.

Save uses a temporary sibling and atomic destination replacement. The returned
save result distinguishes:

- not published: caller retains the previous authoritative file;
- published and durable;
- published but storage durability uncertain.

`file_durability_uncertain` is a successful publication notice. The published
file is authoritative; the save receipt returns its canonical target path and
fingerprint, not a copy of the canonical bytes. Clients must not retry as if
the write never occurred.

After publication the session updates project identity, the semantic saved
baseline, recovery metadata/checkpoint, and the visible Save marker in the
defined order. Save does not increment model revision.

## Recovery journal

The journal is an append-only, versioned, checksummed sidecar with bounded file,
record, payload, operation, and retained-ID limits. It contains a canonical
checkpoint plus later transaction or Undo/Redo transitions.

Live model publication is the primary commit. A journal append or sync failure:

1. leaves the model transition committed;
2. returns or records `journal_failed`/`recovery_degraded`;
3. makes recovery authority sticky-degraded until a fresh checkpoint succeeds.

Recovery parsing is safe on arbitrary, short, torn, corrupt, or oversized
bytes. A valid prefix can be recovered while a torn tail is ignored; corruption
and version/key mismatches remain explicit statuses.

## Recovery store lifecycle

Recovery is optional and host-attached. Creating or opening a `tp_session` does
not create a journal slot. A host that enables recovery supplies a validated
recovery root and explicitly attaches a private journal slot plus an ownership
lock and metadata identifying the project path, canonical key, fingerprint,
and recovery token.

The current GUI enables this policy under its application-data recovery
directory. That store is distinct from the application scratch/work root used
for transient Pack and Export artifacts. The file CLI does not attach recovery.
After a cross-identity Save As, the successful save receipt may set
`recovery_rebind_required`; the host then attaches a fresh slot for the new
identity.

Startup recovery:

1. scans within bounded file and byte budgets;
2. ignores currently live slots;
3. ranks orphan candidates deterministically;
4. claims one candidate through its lock domain;
5. materializes a detached recovered session;
6. requires an explicit resolution.

Resolution may save as a new destination, save over the original only when the
lease and exact fingerprint still agree, or discard the orphan. Recovery never
silently overwrites a changed original.

Unreadable, corrupt, incompatible, busy, and cleanup-failed candidates remain
structured diagnostics. A cleanup failure preserves a discoverable candidate
instead of pretending it was removed.

## Restored state

The recovered project is dirty. The recovered semantic state and transaction
idempotency prefix are restored; the previous process's visible Undo/Redo stack
is not.

External source state, preview cache, GPU resources, and client drafts are
runtime state and are rebuilt rather than journaled.
