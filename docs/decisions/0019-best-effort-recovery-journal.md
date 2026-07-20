# 0019 — Best-effort recovery journal, independent of model commit

**Status:** accepted
**Date:** 2026-07-19 (recorded 2026-07-20)
**Scope:** local recovery journal/checkpoint recording for GUI/MCP/Dev-API live sessions
**Supersedes:** the acknowledgement-before-visibility gate, the rollback-on-append-failure
contract, and the full-snapshot-per-transaction journal payload described in
[0013](0013-f2-04-recovery-journal.md) and [0015](0015-f2-05b-gui-transaction-journal-cutover.md)

## Context

The original F2-04 journal (0013) made durable append part of the commit itself: a
transaction was not "committed" until its record was durably written, and an append or
sync failure rolled the transaction back (clone discarded, live model byte-unchanged,
`TP_STATUS_JOURNAL_FAILED`). 0015 (F2-05b-ii-B) shipped that contract into the GUI: a full
disk fails the commit inside `tp_model_apply`, and every dependent client operation
(Save, Save As, Undo, Redo, Pack, Export) inherited an abort-on-append-failure cascade
through their shared flush points. Each committed transaction also journaled a full
post-commit project snapshot rather than a compact diff.

The owner reviewed this coupling on 2026-07-19 and rejected it: recovery is a
best-effort safety net, not a precondition for editing or a second source of commit
atomicity. Tying visible model state to local disk I/O health means a transient
recovery-journal failure (full disk, permission error, slow storage) can block or revert
work that the in-memory model already completed correctly. The master spec (§7.1,
§22.3) and `docs/ROADMAP.md` (lines 76–84) were updated the same day to record the new
policy; this record backfills it as a numbered decision so it is not only reachable
through prose in those two documents.

## Decision

Recovery is a bounded, best-effort version-4 diff journal, and its I/O is **independent**
of the model commit:

- A transaction's irreversible commit point is full validation, atomic apply to the
  authoritative session, and a new revision/history position — exactly as before, but
  now explicitly **not** conditioned on any recovery I/O outcome.
- The compact TXN or HISTORY journal record is attempted **after** the model commit
  point; it may be written before or after the ordered committed event/result is
  emitted, but it can never gate, delay past the required barrier, or reject that
  commit.
- Journal allocation, encoding, admission, append, or durability-sync failure **never**
  rejects or rolls back a committed transaction. The edit stays committed and visible
  (revision, event, and Undo/Redo position all remain published).
- On such a failure, recovery enters a **sticky degraded state** and surfaces a
  persistent structured `recovery_degraded` notice (first cause, last durable
  revision/time, sticky/cleared transition). While degraded, recovery stops recording
  later dependent diffs — it does not guess or partially reconstruct — until it is
  re-established from a fresh checkpoint.
- The only thing that heals degradation is a **fresh checkpoint**: a successful Save
  publishes the project independently of recovery health, then attempts to replace the
  journal with one fresh checkpoint; checkpoint failure preserves the old evidence and
  the notice instead of clearing it. Save itself is never blocked by recovery failure.
- Healthy-backend RPO target: under a healthy process/power-loss scenario, recovery
  loses at most 5 seconds of edits. This is a target for the healthy path, not a
  guarantee once degraded.
- The journal payload stays **compact v4 TXN/HISTORY diffs**, not a full project
  snapshot per transaction. There is no periodic full-project snapshot; an unsupported
  or oversized transition marks recovery degraded and waits for an explicit
  Save/reattach checkpoint instead of taking a surprise full snapshot.

This explicitly supersedes, from 0013 and 0015:

- the **acknowledgement-before-visibility gate** ("a transaction is not committed until
  its journal record is durable") — 0013 §2 step 5, §7.1 as originally specified;
- the **rollback-on-append-failure contract**, including `TP_STATUS_JOURNAL_FAILED`
  rejecting the transaction inside `tp_model_apply`, and the dependent
  abort-on-append-failure cascade across Save/Save-As/Undo/Redo/Pack/Export described
  in 0015 (F2-05b-ii-B, "Append-fail UX");
- the **full-snapshot-per-transaction journal payload** described in 0013 §1/§D2
  ("payload = full post-commit project snapshot", "KEEP the full-snapshot payload for
  v1").

## Consequences

Master spec §7.1 and §22.3 were updated 2026-07-19 to state the new unified commit/recovery
contract and the compact-diff-only journal policy. The implementation landed the same
window and merged to `main` in PR #3 (merge `590ef83`): live edits and semantic history
now commit independently of journal I/O, recovery reports a structured sticky health
state instead of failing the commit, and Save heals degradation with a fresh checkpoint.
`docs/decisions/0013-f2-04-recovery-journal.md` and
`docs/decisions/0015-f2-05b-gui-transaction-journal-cutover.md` are annotated as
partially superseded by this record; their bodies remain as historical implementation
context and are not rewritten.
