# UI/Session Architecture Refactor Plan

**Status:** Approved for implementation
**Date:** 2026-07-26
**Target:** `docs/design/ui-session-architecture-spec.md`
**Normative source:** `docs/ntpacker-master-spec.md`

## 1. Outcome

Create a correct session-observing GUI foundation before MCP transport lands:

- one atomic session observation cut;
- one host-thread admission/lifetime owner;
- one GUI client for observe/resync/submit/replace;
- one killable worker-process boundary for bounded job cancellation/lifecycle;
- per-view stable selection and semantic draft conflicts;
- read-only frame-pinned snapshot access;
- targeted projections only for complex presentation;
- no mutation-specific GUI invalidation as a correctness mechanism.

The plan does not rewrite `tp_session` as an actor, does not introduce a global
GUI store, and does not require every panel to use a copied DTO.

## 2. Scope split

The original draft mixed four projects. They are now separate tracks.

### Track A — Required session-observed GUI foundation

Packets `R0` through `R5` are the focused refactor and may execute after the
existing branch correctness gates pass.

### Track B — Future targeted presentation cutovers

`PV-*` entries are follow-on planning targets, not executable packets or part of
Track A's Definition of Done. Each receives its own vertical-slice plan with
exact files, tests, gate, and deletion manifest after Track A exposes measured
seams. Simple immutable snapshot reads remain allowed.

### Track C — Separate source-runtime prerequisite

`SR-BASE` moves the existing file/folder runtime owner below GUI. It changes the
current roadmap dependency and requires its own executable plan plus updates to
`docs/ROADMAP.md` and `docs/plans/master-spec-implementation-plan.md` before
implementation.
Linked-atlas and watcher breadth remain in B1.

### Separate remediation plans

The following are required project work but are not hidden inside this
architecture plan:

- existing U-01a/U-02a Pack/freshness defects other than the job-liveness
  prerequisite isolated as R1c;
- platform/render cleanup;
- Unicode folding fallback removal;
- lossy scan-wrapper removal;
- full Dev API/MCP transport, authorization, and handoff.

They may block an affected slice but do not enlarge Track A.

## 3. Execution rules

1. Each packet has exclusive production-file/symbol ownership.
2. Packets sharing a file do not execute in parallel.
3. A new owner is proven before the old owner is removed.
4. The obsolete final caller and compatibility path are deleted in the same
   cutover packet.
5. Every packet has an exact deletion manifest.
6. No implementation commits during `nt_ui` declaration.
7. No worker/transport thread calls or retains raw `tp_session *`.
8. Project schema, operation schema, and export formats remain unchanged.
9. Engine submodule remains read-only.

## 4. Packet graph

```text
R0  normative mapping + boundary/deletion manifests
 |
R1a atomic model observation contract
 |
R1b coalesced job/runtime observation contract
 |
R1c killable job-process isolation
 |
R2a gui_session_client observe
 |
R2b host completion admission
 |
R2c GUI submit cutover + delete mutation invalidations
 |
R2d New/Open/replace/shutdown lifecycle cutover
 |
+--------------------+
|                    |
v                    v
R3 draft reducer     R4 stable per-view identity
|                    |
+----------+---------+
           v
R5 old pending/global identity deletion + focused foundation gate

Prerequisite seams for targeted presentation:

PLATFORM-SEAM --> PV-chrome/dialogs
RESULT-INDEX  --> PV-canvas
SR-BASE       --> PV-tree/list
R3/R4         --> PV-settings

Each targeted seam/slice requires a separate executable plan.
```

`R3` and `R4` may run in parallel only after assigning non-overlapping file
ownership. If both require `gui_state.*`, `gui_actions*`, or `main.c`, they run
serially.
Inside `R3`, packets execute strictly `R3a -> R3b -> R3c -> R3d`.

The serialized `R2a -> R2b -> R2c -> R2d` chain is intentional: `R2a` and `R2b`
both touch `main.c`, while lifecycle teardown in `R2d` depends on completion
admission from `R2b` and submit ownership from `R2c`.

## 5. Detailed packets

### R0 — Freeze contracts and manifests

**Goal:** Make architecture and deletion success mechanically testable.

**Owned files:**

- `docs/ntpacker-master-spec.md`
- `docs/design/ui-session-architecture-spec.md`
- `docs/plans/ui-session-architecture-refactor-plan.md`
- new `cmake/check_architecture_boundaries.cmake`
- `apps/gui/CMakeLists.txt`
- `packer/CMakeLists.txt`

**Tasks:**

1. Pin the atomic observation, host-thread, draft, identity, and Save As rules.
2. Split read-only snapshot query API from session mutation/lifetime API.
3. Add boundary checks:
   - views cannot call session mutation/admission;
   - views cannot include filesystem scan, platform, or mutable-model APIs;
   - core cannot include GUI/transport/protocol formatting;
   - worker and transport modules cannot own raw session pointers.
4. Permit direct read-only snapshot queries from views.
5. Define which feature views require projections.
6. Create symbol/deletion manifests for R2c, R2d, R3/R5, and each `PV-*` slice.
7. Add one negative fixture per boundary category.

**Deletion manifest:** none; this packet adds normative and mechanical guards.

**Gate:** CI can reject the old dependency direction before production changes.

### R1a — Atomic session observation

**Goal:** Provide one linearizable event/snapshot observation cut.

**Owned production files:**

- `packer/include/tp_core/tp_session.h`
- new `packer/src/tp_session_observation.c`
- `packer/src/tp_session_internal.h`
- `packer/src/tp_session_snapshot_internal.h`
- `packer/CMakeLists.txt`
- new `packer/tests/test_session_observation.c`
- `packer/tests/CMakeLists.txt`

**Tasks:**

1. Add an owned `tp_session_observation` API with a composite observed token
   covering event, source-runtime, job-state, and result generations.
2. Under one gate:
   - fix cut event sequence;
   - copy retained events through the cut or mark resync;
   - retain the matching immutable project generation;
   - capture revision/admission/generations/identity/dirty/recovery scalars.
3. Materialize owned DTO data after unlock.
4. Return an allocation-free/no-project-materialization empty delta when no
   token component changed.
5. When only runtime/job/result generation changed, return its retained
   immutable state without materializing the project.
6. Define event capacity and resync behavior without a caller-sized partial
   range.
7. Include author/label in committed model event data.
8. Define `OK/no_change` as terminal without an event.
9. Preserve current `events_after()`/snapshot APIs only for non-observer callers
   that still need them; do not use them to assemble GUI correctness.

**Tests:**

- commit injected at former event-read/snapshot-capture seam;
- event window exact edge and overflow;
- future event-sequence component/session generation change;
- OOM before and after generation retention;
- no-event allocation/materialization counters;
- no-op result without event;
- author/label/transaction identity.

**Deletion manifest:** none; this packet creates the canonical observer port.

**Gate:** `USA-01` through `USA-05`.

### R1b — Runtime/job observable state

**Goal:** Define how worker-owned activity becomes safe host-observed state
without flooding the model-event ring.

**Owned production files:**

- `packer/include/tp_core/tp_session.h`
- `packer/src/tp_session_observation.c`
- new `packer/src/tp_session_job_observation.c`
- new `packer/src/tp_session_job_observation_internal.h`
- `packer/src/tp_session_internal.h`
- `packer/CMakeLists.txt`
- new `packer/tests/test_session_job_observation.c`
- `packer/tests/CMakeLists.txt`

**Tasks:**

1. Define immutable completion envelope:
   `{session_instance_generation, request_id, base_input_token, target_ids,
   terminal_result}`.
2. Define coalesced latest progress/job state with a generation.
3. Include that generation in the composite observation token and return its
   latest immutable state even when event sequence is unchanged.
4. Publish terminal/meaningful state changes without one event per progress
   tick.
5. Retain/pin result handles for an observation lifetime.
6. Reject superseded, cancelled, deleted-target, old-generation, duplicate, and
   post-close completions.
7. Keep protocol serialization outside the session gate.

**Tests:**

- Refresh/job A and B complete in reverse order;
- completion after target deletion;
- cancellation racing terminal completion;
- duplicate completion;
- completion after session replacement/close;
- progress burst does not overflow model event retention;
- progress changes with no event are visible on the next observation.

**Deletion manifest:** none; this additive packet extends the canonical
observation owner.

**Gate:** `USA-06`, `USA-07`, and `USA-30`.

### R1c — Killable job-process isolation

**Goal:** Make bounded cancellation, replacement, and shutdown possible without
joining a potentially blocked in-process thread.

**Owned production files:**

- new `packer/include/tp_core/tp_job_worker.h`
- new `packer/src/tp_job_worker.c`
- new `packer/src/tp_job_worker_main.c`
- new `packer/src/tp_job_worker_proto.c`
- new `packer/src/tp_job_worker_internal.h`
- `packer/src/tp_job.c`
- `packer/src/tp_input.c`
- `packer/src/tp_scan.c`
- `packer/src/tp_image.c`
- `packer/src/tp_export_run.c`
- `packer/src/tp_proc_posix.c`
- `packer/src/tp_proc_win32.c`
- `packer/CMakeLists.txt`
- `apps/gui/main.c`
- new `packer/tests/test_job_worker_proto.c`
- `packer/tests/test_job_owner.c`
- `packer/tests/test_build_worker.c`
- `packer/tests/CMakeLists.txt`

**Tasks:**

1. Define a bounded versioned request/progress/result protocol for immutable
   Pack/Export job input.
2. Dispatch the private job-worker invocation before engine/window/session
   initialization.
3. Run source traversal/current-read, file I/O, decode, Pack input construction,
   builder execution, and Export job I/O in the owned process.
4. Keep project model, session admission, history, and result authority in the
   host.
5. Replace the in-process `thrd_t` job with non-blocking process
   poll/progress/result ownership.
6. Request cooperative cancellation first; kill the owned process tree after a
   bounded deadline; reap only after terminal process state.
7. Bound request/reply/progress sizes and fail closed on malformed/truncated
   protocol, child crash, timeout, or output overflow.
8. Preserve existing external side-effect guarantees: staged work or explicit
   partial-publication report, plus deterministic orphan-staging cleanup.

**Tests:**

- blocked/delayed stat, read, decode, write, and builder seams;
- cancellation before start, during each blocking class, and at terminal race;
- child crash, malformed/truncated/oversized reply, timeout, and tree kill;
- no model/revision/history mutation from worker;
- partial export publication remains explicit and recoverable;
- frame-poll work remains bounded while the worker is live.

**Deletion manifest:**

- `tp_live_job.thread`, `thrd_create`, and `job_join` from
  `packer/src/tp_job.c`;
- in-process `pack_job_main`/`export_job_main` blocking execution paths;
- any direct worker alias to live `tp_session`/`tp_model`.

**Gate:** every potentially unbounded Pack/Export job path is process-owned;
forced termination reaches structured terminal state and permits non-blocking
reap. This gate is mandatory before R2b/R2d and `USA-32`.

### R2a — Introduce GUI session client observation

**Goal:** Attach/pump/resync the current GUI without changing submit ownership.

**Owned production files:**

- new `apps/gui/gui_session_client.h`
- new `apps/gui/gui_session_client.c`
- `apps/gui/gui_project_internal.h`
- `apps/gui/main.c`
- `apps/gui/CMakeLists.txt`
- new `apps/gui/test_gui_session_client.c`

**Tasks:**

1. Move current GUI snapshot ownership, composite observed token, instance
   generation, and observation lifetime into `gui_session_client`.
2. Implement attach, observe, resync, frame pin, and detach.
3. Fan one observation batch out to registered view reducers.
4. Preserve old observation/composite token on failure.
5. Add a direct external `tp_session_apply()` test that becomes visible through
   observation.
6. Keep current submission and invalidation temporarily, but assert that
   observation independently reaches the same state.

**Temporary dual-path rule:** only observation determines the displayed frame.
Old invalidation may force earlier observation but cannot supply state.

**Deletion manifest:** none; mutation invalidation is removed by R2c after this
observer is proven.

**Gate:** external commits and Save are visible without wrapper-specific state
construction.

### R2b — Host completion admission

**Goal:** Route worker completions through the host thread.

**Owned production files:**

- new `apps/gui/gui_host_queue.h`
- new `apps/gui/gui_host_queue.c`
- `apps/gui/main.c`
- `apps/gui/gui_pack_jobs.c`
- `apps/gui/CMakeLists.txt`
- new `apps/gui/test_gui_host_queue.c`

**Tasks:**

1. Add host state
   `OPEN -> DRAINING -> READY_TO_CUTOVER -> OPEN/CLOSED`.
2. Drain immutable completions before session observation.
3. Validate instance generation and supersession before applying completion.
4. Ensure workers never mutate GUI/session authority directly.
5. Integrate R1b coalesced observable state.
6. Keep platform/frame pumping during drain and join only after confirmed
   terminal state.
7. Provide the bounded owned-worker termination escalation used when
   cooperative cancellation reaches its R1c deadline.

**Deletion manifest:**

- `job_session()` from `apps/gui/gui_pack_jobs.c`;
- direct `tp_session_job_*`, `tp_session_pack_job_start`, and
  `tp_session_export_start` calls from `apps/gui/gui_pack_jobs.c`, replaced by
  the host-owned queue/admission port.

**Gate:** completion lifecycle tests pass with no raw session pointer in worker
payloads.

### R2c — Cut over all GUI submits

**Goal:** Make `gui_session_client_submit()` the only GUI transaction path and
delete mutation-specific model/Save invalidation.

**Owned production files:**

- `apps/gui/gui_session_adapter.h`
- `apps/gui/gui_session_adapter.c`
- `apps/gui/gui_session_client.h`
- `apps/gui/gui_session_client.c`
- `apps/gui/gui_project_mutations.c`
- `apps/gui/gui_project_pending.c`
- `apps/gui/gui_project.c`
- `apps/gui/gui_project_file.c`
- `apps/gui/gui_project_internal.h`
- `apps/gui/test_gui_session_adapter.c`
- `apps/gui/test_client_parity.c`

**Tasks:**

1. Generate full 128-bit transaction IDs through production RNG.
2. Populate semantic label and trusted `author="human"`.
3. Register `{transaction_id, origin_view_id, draft_instance_id}` before submit.
4. Submit every GUI typed transaction through the client.
5. Record synchronous terminal result, including `OK/no_change`.
6. Observe revision-changing success through common event echo.
7. Remove model/Save mutation-specific snapshot drops and direct
   `tp_session_apply()` calls from GUI adapters.
8. Keep source/runtime invalidation separate until `SR-BASE`.

**Deletion manifest:**

- direct GUI adapter `tp_session_apply`;
- model/Save snapshot-drop calls whose only purpose was local mutation;
- process-local sequential transaction ID generator.

**Gate:** `USA-08` through `USA-14`.

### R2d — Session lifecycle cutover

**Goal:** Make client ownership fail-atomic across New/Open/Save As/Discard/
Shutdown.

**Owned production files:**

- `apps/gui/gui_project_file.c`
- `apps/gui/gui_project.c`
- `apps/gui/main.c`
- `apps/gui/gui_session_client.h`
- `apps/gui/gui_session_client.c`
- `apps/gui/gui_host_queue.h`
- `apps/gui/gui_host_queue.c`
- new `apps/gui/gui_host_binding.h`
- new `apps/gui/gui_host_binding.c`
- `apps/gui/CMakeLists.txt`
- lifecycle tests in `apps/gui/test_gui_session_client.c`

**Tasks:**

1. Add `attach`, `detach`, and fail-atomic `replace_session`.
2. Prepare a detached candidate session, initial atomic observation, and all
   fallible attachment resources while the old session remains live.
3. On prepare failure, destroy only the candidate and preserve old ingress,
   session, observation, views, and generation.
4. After prepare succeeds, stop accepting new ingress by entering `DRAINING`;
   do not invalidate the old generation yet, and keep its observation renderable.
5. Request cancellation and continue pumping frames until terminal completion;
   use bounded worker-process termination escalation when required.
6. Join only after terminal confirmation, then enter `READY_TO_CUTOVER`.
7. In one short non-fallible cutover, invalidate the old instance generation,
   reject remaining old work, and atomically publish the prepared candidate/new
   generation.
8. Release old observations and destroy the old session only after publication.
9. Clear the old composite token and pending submit acknowledgement.
10. Reconcile or invalidate per-view drafts/selection explicitly.
11. Treat cross-identity Save As as explicit rebind; reject it while controller
   attached until later rebind protocol exists.
12. Provide a minimal injectable controller-attached status port for the
    identity guard; do not implement transport, claim, or authorization.
13. Reject/no-op Open of the current canonical identity without acquiring a
    second lease or tearing down the live session.

**Deletion manifest:**

- direct production destruction/replacement of `s_project.session`;
- pointer-identity-based cache binding;
- lifecycle paths retaining old snapshot/composite token.

**Gate:** `USA-06`, `USA-29`, `USA-32`, fail-atomic Open/New/replace tests, and
same-identity already-open lease test.

### R3a — Pure draft reducer and the atlas-settings scalar family

**Goal:** Establish the state machine independently of widget/session code.

**Owned production files:**

- new `apps/gui/gui_edit_state.h`
- new `apps/gui/gui_edit_state.c`
- `apps/gui/gui_view_settings.c`
- `apps/gui/gui_actions_edits.c`
- `apps/gui/CMakeLists.txt`
- new `apps/gui/test_gui_edit_state.c`

**Tasks:**

1. Implement `IDLE/EDITING/SUBMITTING/CONFLICTED`.
2. Store stable target, exact component, value, base revision, view ID, draft
   instance ID, and submitted transaction ID.
3. Reduce model event, no-op result, validation/OOM, gap/resync, Apply Mine,
   Discard, and target deletion.
4. Convert every `CK_ATLAS_SETTING` field/gesture end to end.
5. Prove a different transaction from the same GUI client still conflicts.
6. Encode the complete normative lifecycle table before converting later
   families.

**Deletion manifest:** the complete `CK_ATLAS_SETTING` pending route inside
`gui_project_set_atlas_setting`; every atlas scalar presentation reads only the
committed snapshot plus its own exact draft value.

**Gate:** state table is exhaustive and no atlas-settings scalar uses the old
broad pending operation.

### R3b — Text, rename, and path drafts

**Owned production files:**

- `apps/gui/gui_actions_edits.c`
- `apps/gui/gui_project_pending.c`
- `apps/gui/gui_project_mutations.c`
- `apps/gui/gui_view_settings.c`
- `apps/gui/gui_actions_dialogs.c`
- `apps/gui/main.c`
- `apps/gui/test_gui_edit_state.c`

**Tasks:**

1. Convert inline rename, text fields, and target path.
2. Preserve user buffer visually during conflict.
3. Provide Apply Mine, Copy where meaningful, and Discard.
4. Cover Enter, pointer release, blur, Escape, Save, Save As, Pack, Export,
   Undo, Redo, Open, New, project Discard, Close, validation/OOM, and resync
   exactly as the normative lifecycle table specifies.
5. Prove every dependent outer action aborts when its draft prerequisite fails.

**Deletion manifest:** all converted text/rename/path callers of
`gui_project_flush_pending`/`gui_project_pending_offer`; final storage and
unconverted grouped callers remain owned by R3c/R3d.

**Gate:** no text is lost on revision conflict, validation, OOM, or resync.

### R3c — Grouped read-modify-write drafts

**Owned production files:**

- `apps/gui/gui_project_pending.c`
- `apps/gui/gui_project_mutations.c`
- `apps/gui/gui_actions_edits.c`
- `apps/gui/gui_view_settings.c`
- `apps/gui/main.c`
- `apps/gui/test_gui_edit_state.c`

**Tasks:**

1. Store only the exact edited component.
2. Rebuild untouched siblings from the newest snapshot on Apply Mine.
3. Convert each grouped edit family.
4. Test target deletion and sibling changes by another client.

**Deletion manifest:** remaining grouped `gui_project_pending_offer` and
`gui_project_peek_pending_*` callers; storage is deleted by R3d.

**Gate:** `USA-15` through `USA-23` and `USA-31`, especially no stale sibling
overwrite or continued outer action after failed submit.

### R3d — Delete old pending architecture

**Owned production files:**

- `apps/gui/gui_project_pending.c`
- `apps/gui/gui_project_internal.h`
- `apps/gui/gui_project.c`
- `apps/gui/gui_project_file.c`
- `apps/gui/gui_actions.c`
- `apps/gui/gui_actions.h`
- `apps/gui/gui_actions_dialogs.c`
- `apps/gui/gui_actions_edits.c`
- `apps/gui/gui_actions_internal.h`
- `apps/gui/gui_actions_preview.c`
- `apps/gui/gui_project.h`
- `apps/gui/gui_project_mutations.c`
- `apps/gui/main.c`
- `apps/gui/gui_selftest.c`
- `apps/gui/test_gui_action_trace.c`
- `apps/gui/test_gui_canonical_identity.c`
- `apps/gui/CMakeLists.txt`

**Tasks:**

1. Map every old `gui_project_flush_pending`, `pending_valid`, `pending_op`, and
   `peek_pending_*` use to an explicit reducer transition.
2. Delete old pending storage and fallback timer semantics not retained by the
   new contract.
3. Retain one gesture → one transaction/Undo.

**Deletion manifest:**

- `pending_valid`, `pending_key`, `pending_op`, `pending_time`,
  `pending_expected_revision`, and `pending_preview_stale_before` from
  `gui_project_state`;
- `gui_project_flush_pending`, `gui_project_pending_route`,
  `gui_project_pending_offer`, and every `gui_project_peek_pending_*`;
- `gui_project_pending.c` source registration in `apps/gui/CMakeLists.txt`;
- obsolete timer-coalescing callers and tests.

**Gate:** forbidden-symbol scan finds no old pending architecture.

### R4 — Stable per-view state

**Goal:** Replace cross-frame index authority without requiring additional
shipping windows.

**Owned production files:**

- new `apps/gui/gui_view_state.h`
- new `apps/gui/gui_view_state.c`
- `apps/gui/gui_state.h`
- `apps/gui/gui_state.c`
- `apps/gui/gui_rows.c`
- `apps/gui/gui_canvas.c`
- `apps/gui/CMakeLists.txt`
- `apps/gui/test_gui_canonical_identity.c`

**Tasks:**

1. Store atlas/source/key/animation/target identities canonically.
2. Keep indices and row IDs projection-generation-local.
3. Reconcile every registered view on observation/resync.
4. Define one explicit parent fallback or clear behavior.
5. Add a two-view test: View A commit conflicts View B draft.
6. Do not make extra GUI windows a completion gate.

**Deletion manifest:**

- cross-frame authority of `s_sel_atlas`, `s_sel_src`, `s_sel_child`,
  `s_sel_anim`, and `s_sel_anim_frame`;
- old reselect/index fallback fields superseded by canonical view identity;
- pointer/lifetime identity used as a retained selection key.

**Gate:** `USA-18`, `USA-24`; no wrong-entity selection after external structural
change.

### R5 — Focused foundation hardening

**Goal:** Prove Track A is complete and contains one observation/submit/lifecycle
architecture.

**Tasks:**

1. Run absence checks for direct GUI submit, local model/Save invalidation, old
   pending storage, index-authoritative edit identity, and raw session pointers
   in worker/transport payloads.
2. Run session/client parity, event gap, OOM, no-op, lifecycle, concurrency,
   draft, and stable-selection tests.
3. Run native Debug build and focused tests; then native Release test suite.
4. Update roadmap evidence only after gates pass.

**Owned files:**

- `cmake/check_architecture_boundaries.cmake`
- `apps/gui/CMakeLists.txt`
- `packer/CMakeLists.txt`
- tests introduced by R1a through R4
- `docs/ROADMAP.md`
- `docs/plans/master-spec-implementation-plan.md`

**Deletion manifest:** any temporary dual-path assertion or compatibility
instrumentation introduced by R2a.

**Gate:** focused acceptance items 1–8 in the spec.

## 6. Follow-on presentation planning targets

These are independent vertical slices, not a mandatory universal DTO rewrite
and not executable packets in this plan. Each future plan must name exact owned
files, tests, a deletion manifest, and an acceptance gate.

### PLATFORM-SEAM

Extract dialogs, clipboard, path/URL launch, and app settings behind
`gui_platform`. Must precede `PV-chrome/dialogs`.

### RESULT-INDEX

Create one canonical packed-result lookup shared by rows, settings, and canvas.
Must precede `PV-canvas`.

### SR-BASE

Move current path-file/path-folder scan, status, fingerprint, refresh diff, and
immutable runtime snapshot below GUI. Remove GUI recursive scan/fingerprint
ownership. Update roadmap/master-plan dependencies first. Linked atlases,
watchers, and broader cache policy remain B1 extensions of the same owner.
The core cache is only a hint: Pack/Export must perform authoritative
current-read verification. Its future plan must test an external change without
watcher notification, stale generation, read/decode failure, and preservation
of the last preview as visibly stale rather than silently using it as build
input.

### PV-settings

Use frame snapshot + edit reducer directly for simple fields; extract only
enabled/warning/product-policy summaries. Depends on R3/R4.

### PV-tree/list

Use runtime snapshot plus virtualized stable-ID row projection. It consumes the
runtime owner installed by SR-BASE; SR-BASE alone deletes `gui_scan` and GUI
source fingerprint/diff ownership. Depends on SR-BASE.

### PV-canvas

Use canonical result index and pinned result handles for layout/selection.
Depends on RESULT-INDEX.

### PV-chrome/dialogs

Keep layout/widget mechanics in views; move platform effects and lifecycle
decisions to controller/platform seams. Depends on PLATFORM-SEAM and R2d.

Each future plan deletes its direct
session/filesystem/platform/business-rule caller in the same landing.

## 7. Source-runtime roadmap decision

Before `SR-BASE`, update the execution documents to state:

- only the existing path-file/path-folder runtime foundation moves earlier;
- it provides one generation-keyed snapshot/cache used by GUI and Pack input
  planning;
- the cache is never build authority: Pack/Export re-read and verify current
  source bytes before accepting input;
- external change without watcher, stale generation, read/decode failure, and
  last-preview-visible-but-not-buildable have explicit tests;
- B1 adds linked-atlas sources, watchers, companion discovery, and remaining
  refresh breadth;
- no parallel GUI runtime survives;
- U-02's synchronous scan workaround is removed, not retained as fallback.

If this dependency change is rejected, `PV-tree/list` and full GUI architecture
follow-on remain blocked until B1; Track A still lands independently.

## 8. Verification matrix

| Risk | Required evidence |
|---|---|
| Snapshot contains unseen commit | injected former seam race |
| Gap loses own echo | synchronous result + resync transition |
| No-op waits forever | `OK/no_change` reducer test |
| Same client masks foreign view commit | exact view/draft identity test |
| Cursor advances after OOM | observation allocation faults |
| New/Open destroys live client state | fail-atomic replacement tests |
| Completion publishes into new session | instance-generation rejection |
| Replacement blocks a frame on worker join | delayed-worker drain/pump test |
| Progress overflows event ring | burst/coalescing test |
| Progress changes without event and remains hidden | composite-token observation test |
| Agent overwrites active input | scalar/text/grouped conflict tests |
| Apply Mine restores sibling values | grouped RMW matrix |
| Structural edit selects wrong entity | stable-ID reconciliation tests |
| Bound MCP silently follows Save As | identity-rebind rejection test |
| Old architecture survives | per-packet symbol/include deletion checks |

## 9. Performance gates

- No-event observation performs no project materialization.
- Runtime/job/result-only observation performs no project materialization.
- One event batch materializes at most one project generation.
- No UI-thread filesystem traversal/decode/hash.
- Virtualized paths have deterministic work-count/allocation gates.
- Owner-scale latency is measured in a controlled or calibrated environment.
- Shared-runner wall-clock maximum alone is not a CI correctness gate.
- Benchmark setup fails if the intended folder/large/packed path was not
  exercised.

## 10. Definition of done

Track A is complete when:

- atomic observation is the GUI correctness path;
- every GUI mutation submits through the client;
- lifecycle replacement has one owner;
- per-view draft and stable identity behavior is verified;
- old model/Save invalidation and pending-operation architecture are deleted;
- `USA-01` through `USA-24` and `USA-29` through `USA-32` pass where
  applicable;
- boundary, parity, fault, native Debug, and native Release gates pass.

Follow-on plans must schedule `SR-BASE`, the required `PV-*`/PLATFORM/RESULT
slices, and `USA-25` through `USA-28`. They are not silently included in Track
A's completion claim.

Dev API transport, authorization, controller claim, authority handoff, recovery
mirror promotion, live presence feed, and additional windows remain later
roadmap work. The minimal controller-attached status seam in R2d exists only to
enforce identity safety and is not a transport/claim implementation.
