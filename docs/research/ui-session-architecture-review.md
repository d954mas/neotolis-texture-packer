# UI/Session Architecture Review Record

**Date:** 2026-07-26
**Status:** Non-normative review record
**Reviewed documents:**

- `docs/ntpacker-master-spec.md`
- `docs/design/ui-session-architecture-spec.md`
- `docs/plans/ui-session-architecture-refactor-plan.md`
- `docs/research/ui-session-architecture-comparison.md`

## 1. Review method

Three independent subagent reviews examined the initial proposal through
different lenses:

1. session concurrency, event ordering, command acknowledgement, and lifetime;
2. refactor sequencing, file ownership, deletion manifests, and verification;
3. comparison with alternative architectures and mature editor products.

The first review round did not approve the initial draft. The atomic observation
gap and undefined host/session lifetime were blockers. The plan and comparison
also received material request-changes findings. This record lists the accepted
criticism and the resulting corrections rather than preserving the superseded
draft.

## 2. Accepted architecture findings

| Finding | Severity | Correction |
|---|---|---|
| Events and snapshots sampled under separate locks can describe different session cuts. | Blocker | Specify one atomic `tp_session_observe`-style operation with a composite token, common high-water sequence, retained generation, snapshot scalars, and event/resync result. Add an injected race test at the old seam. |
| A submitted GUI command can wait forever when it is a semantic no-op or its echo is evicted. | Blocker | Treat synchronous `OK/no_change` as terminal; reconcile resync from the known submit result; reserve durable result lookup/replay for uncertain remote transport. |
| Transaction ID alone is insufficient to identify a draft's own commit. | High | Scope acknowledgement by `{transaction_id, origin_view_id, draft_instance_id}`. Every other revision-changing commit conflicts the active draft. |
| Dev API/workers retaining or calling raw `tp_session *` make replacement and shutdown unsafe. | Blocker | Assign one authoritative host admission thread. Other threads enqueue immutable generation-tagged requests/completions. Define stop-ingress, invalidate, drain/reject, cancel/join, detach, destroy order. |
| Worker completion and progress had no stale-result or flood policy. | High | Require exactly-once immutable completion envelopes tagged with session generation/request/input identity. Coalesce progress outside the bounded model event stream. |
| A model snapshot alone cannot atomically represent runtime/job/result payloads. | High | The observation retains matching immutable handles or separately versioned state with an explicit matching rule. |
| Save As could silently move a live MCP binding to another project identity. | High | Reject cross-identity Save As while a controller is attached unless a later explicit generation-changing rebind protocol is implemented. |
| Coalesced progress can change without advancing the model event cursor. | High | Replace the scalar event cursor with a composite observed token covering event/source/job/result generations; add an un-evented progress test and avoid project materialization for job-only changes. |
| Destroying the old session before candidate attachment contradicts fail-atomic replacement. | High | Require a detached candidate plus initial observation and all fallible resources before a non-fallible frame-boundary cutover; preparation failure leaves the old live session untouched. |
| Cancelling and joining an old job inside the cutover can block the GUI frame indefinitely. | High | Make drain asynchronous and keep pumping/rendering old state; join only after terminal confirmation, with bounded owned-worker termination escalation, then perform the short cutover. |
| Killing the existing builder child cannot release its parent `thrd_t` when source I/O/decode/export work is blocked in-process. | Blocker | Add mandatory R1c job-process isolation: all potentially unbounded job work moves behind a bounded worker protocol; the host polls non-blockingly and can terminate/reap the owned process tree before cutover. |

## 3. Accepted presentation findings

| Finding | Severity | Correction |
|---|---|---|
| Requiring Passive View, a copied DTO, and an intent union for every panel over-engineers a native immediate-mode GUI. | High | Adopt a session-observing Supervising Controller. Simple views may query the frame-pinned immutable snapshot; use projections/reducers only for derived, virtualized, identity-sensitive, policy-bearing, or draft state. |
| One universal Presenter or action union can become the next GUI god object. | High | Partition observation, submission, local edit state, projections, platform effects, and GPU ownership. Use feature-scoped typed action sinks. |
| The comparison used an unfair “MVP without observer” alternative. | Medium | Compare against session-observing MVP honestly; describe the adopted design as its partitioned, lightweight form. |
| VS Code, Blender, and Godot were being used to justify more than they establish. | Medium | Limit their lessons to shared authority/event echo, typed operations/Undo, and document ownership. They do not mandate reducers or DTOs for every view. |

## 4. Accepted plan findings

| Finding | Severity | Correction |
|---|---|---|
| The initial plan mixed session observation with old Pack/freshness defects, source runtime, and platform/render/Unicode work. | High | Keep the main plan focused on session-observed GUI foundations and targeted presentation cutovers. Route unrelated defects through separate remediation plans. |
| Source-runtime work conflicted with its existing roadmap ownership. | High | Make reordering an explicit roadmap/master-plan decision before implementation; otherwise defer tree/list completion to the existing B1 phase. |
| Observation alone would leave private GUI invalidation paths alive. | High | Add an explicit submit cutover packet and delete all mutation-specific snapshot drops. |
| New/Open/replace/shutdown was not an owned migration packet. | High | Add a fail-atomic lifecycle packet with generation invalidation and teardown ordering. |
| Draft conversion omitted non-widget lifecycle callers. | High | Cover Enter, blur, Escape, Save, Save As, Open, New, Undo, Redo, Pack, close, validation, OOM, and conflict paths. |
| Listing lifecycle triggers without defining their transitions still permits inconsistent data-loss and outer-action behavior. | High | Add a normative transition table, preflight-before-submit rule, explicit Apply/Discard/Cancel lifecycle gate, and abort-on-failed-prerequisite tests. |
| Parallel packets shared `main.c`, and lifecycle teardown did not depend on completion admission. | High | Serialize `R2a -> R2b -> R2c -> R2d`; make teardown consume both completion and submit ownership. |
| Abstract file/deletion manifests and pseudo-packets made the plan non-executable. | High | Name Track A files and symbols explicitly; mark Track B and SR-BASE as follow-on plans rather than pretending they are part of Track A's Definition of Done. |
| Save As safety had no minimal attached-controller seam while full controller transport was deferred. | High | Add a narrow injectable read-only attached-status port in R2d without implementing transport, claim, or authorization. |
| The proposed source cache could be misread as Pack/Export authority. | High | Require current-read verification and stale/external-change/read-failure tests in the future SR-BASE plan; last preview may remain visible only as visibly stale. |
| Stable multiple-view identity was coupled to the MCP-visible MVP. | Medium | Test view/draft identity in the state structures now; do not block the first vertical slice on shipping multiple windows. |
| A shared-runner wall-clock maximum would be flaky. | Medium | Gate deterministic work/allocation budgets in normal CI; keep sub-100 ms checks for controlled or calibrated environments. |

## 5. Remaining decisions before implementation

These are intentional gates, not unspecified behavior:

1. Approve or reject moving the minimum path file/folder source-runtime base
   ahead of roadmap phase B1.
2. Freeze the exact C structs and ownership rules during R0; the specification
   fixes semantics, not field spelling.
3. Decide which first complex GUI seam proves targeted projections after the
   settings draft vertical slice.
4. Keep cross-identity Save As rejected while MCP is attached until an explicit
   rebind protocol has its own specification and tests.

## 6. Review outcome

After applying the findings above, the proposal is deliberately narrower:

- the core session remains the only mutable project authority;
- observation becomes atomic rather than a composition of lock samples;
- the GUI becomes a partitioned observer/controller, not a second state store;
- local drafts are preserved through conflicts without automatic merge;
- host-thread and replacement lifetime are explicit;
- migration is deletion-driven and split from unrelated correctness work.

Final convergence outcome: **APPROVE** from all three review lenses. No
blocker/high design or plan finding remains. This approves the architecture
documents for implementation; it is not a claim that the refactor code already
exists.
