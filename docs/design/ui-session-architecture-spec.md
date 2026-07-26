# UI/Session Architecture Specification

**Status:** Approved for implementation
**Date:** 2026-07-26
**Scope:** Native GUI presentation architecture and live-session observation
**Authority:** Derived from `docs/ntpacker-master-spec.md`; the master spec
prevails on conflict.

## 1. Decision

The native GUI uses a **session-observing Supervising Controller**:

1. `tp_session` remains the only authoritative live writer.
2. The authoritative host has one admission thread. GUI actions, Dev API
   requests, worker completions, Undo/Redo, Save, and Refresh are admitted there.
3. Each GUI process owns one `gui_session_client`; each GUI view owns independent
   selection, navigation, and draft state.
4. GUI state is refreshed from one atomic session observation containing a
   consistent event high-water mark and immutable read state.
5. Simple views may read a frame-pinned immutable snapshot through a read-only
   query API. Derived, virtualized, identity-sensitive, or policy-bearing
   presentation uses explicit projections/reducers.
6. Views never mutate the session, perform filesystem/platform I/O, decide
   revision conflicts, or implement product validation/capability rules.
7. An uncommitted human gesture is explicit view-local draft state. Concurrent
   model change preserves the value and requires explicit Apply Mine or Discard.

At the product boundary this remains Ports and Adapters. Inside the GUI it uses
Passive View only where it buys a real testability or ownership seam. It does
not require a global immutable store, universal panel DTO graph, global
catch-all intent union, binding framework, actor runtime, or event sourcing.

## 2. Motivation and current delta

The core already provides:

- typed atomic transactions;
- optimistic `expected_revision`;
- idempotent transaction IDs;
- shared history and Undo/Redo;
- serialized synchronous admission;
- immutable retained project generations;
- bounded ordered events;
- owned session snapshots.

The production GUI still caches its snapshot and drops it at GUI-specific
mutation chokepoints. A direct session commit from Dev API would not invoke
those paths. The current event and snapshot APIs are also sampled under separate
locks, so naively composing them can show state whose event metadata was never
reduced.

The architectural delta is therefore:

- one atomic observe/resync API;
- one GUI session client;
- one exact per-view draft state machine;
- stable per-view identities;
- targeted presentation seams;
- eventual removal of GUI-owned source-runtime/business logic.

It is not a rewrite of `tp_session`.

## 3. Goals

- Show human and agent commits in every open view through the same ordered path.
- Guarantee that a GUI frame sees one consistent observation cut.
- Preserve user draft input across concurrent commits without hidden merge.
- Separate domain/runtime state from view, draft, platform, and GPU state.
- Keep trivial immediate-mode rendering direct and lightweight.
- Make complex presentation logic testable without `nt_ui`.
- Preserve stable selection/edit targets across external changes and resync.
- Prepare Dev API/MCP and multiple GUI views without implementing their deferred
  transports or windows in this phase.
- Preserve project schema v5, export formats, and typed operation vocabulary.

## 4. Non-goals

- No CRDT, last-writer-wins, field merge, automatic rebase, or automatic retry.
- No actor thread inside `tp_session`.
- No complete event sourcing.
- No generic dependency injection, reflection, observable properties, or
  middleware framework.
- No requirement to wrap every scalar shown by a view in a copied DTO.
- No visual redesign.
- No engine-submodule change.
- No compatibility layer for removed internal GUI APIs.
- No Dev API framing, controller authorization, process claim, or authority
  handoff implementation in this refactor. A minimal read-only
  controller-attached status port is in scope solely for identity-changing
  lifecycle guards.

## 5. Ownership

| State | Sole owner |
|---|---|
| Project, revision, semantic dirty baseline, history | `tp_model` |
| Session identity, admission, generations, event order, job handles | `tp_session` |
| Source scan/status/hash/refresh state | core source-runtime module coordinated by session |
| Pack/export jobs, freshness, result authority | core job/result modules coordinated by session |
| Composite observed token and current immutable observation | `gui_session_client` |
| Selection, filters, sort, collapse, camera, panel/modal state | one `gui_view_state` per view |
| Uncommitted field/text/gesture value | one `gui_edit_state` per view |
| Virtual rows, canvas layout, warnings/enabled summaries | targeted projections |
| Widget focus/hover/active mechanics | `nt_ui` |
| GPU textures, upload, residency | `gui_render_resources` |
| Dialogs, open/reveal URL/path, clipboard, app settings | `gui_platform` |
| Transport framing, authorization, controller identity | Dev API host adapter |

No GUI structure is a second project model.

## 6. Host and client topology

```text
GUI view actions                     MCP / Dev API
      |                                   |
      v                                   v
feature action queue              transport request queue
      |                                   |
      +-----------------+-----------------+
                        v
             authoritative host thread
             - validate/admit/commit
             - apply worker completion
             - publish ordered event/state
                        |
              +---------+---------+
              |                   |
              v                   v
      GUI session client       MCP mirror
      observe/resync           observe/resync
              |
              v
      pinned frame snapshot
      + local view/edit state
              |
              v
        immediate-mode views
```

### 6.1 Admission-thread rule

One host thread owns session lifetime and admission:

- in GUI-hosted mode it is the GUI host thread;
- in headless mode it is the MCP host loop;
- transport threads enqueue owned immutable requests and await structured
  results;
- workers enqueue owned immutable completions;
- transport and worker threads never call or retain raw `tp_session *`.

The existing session gate remains defensive synchronization and supports
focused core tests. It is not a lifetime-management substitute.

### 6.2 Shutdown and replacement

Replacement has a mandatory prepare phase:

1. construct a detached candidate session;
2. obtain its initial atomic observation and allocate every fallible attachment
   resource;
3. on any failure, destroy only the candidate and leave old ingress, session,
   observation, views, and generation unchanged.

Only after preparation succeeds does the owner enter a drain phase that may
span frames:

1. stop new external ingress and transition `OPEN -> DRAINING`;
2. reject new commands/job starts for the old session;
3. request cancellation of old session-owned jobs;
4. continue platform pumping, rendering the old pinned observation, and
   accepting only progress/terminal completions for the draining generation;
5. after terminal state is confirmed, join the already-terminal worker and
   transition `DRAINING -> READY_TO_CUTOVER`.

The job owner must provide enforceable terminalization: cooperative cancellation
first, then the platform-appropriate owned worker-process termination path after
its bounded deadline. Because blocking filesystem/decode work lives in that
process, a frame never joins an in-process job thread.

The following short frame-boundary cutover contains no blocking or fallible
work:

1. invalidate the old session-instance generation;
2. reject remaining old queued work and discard stale completions;
3. atomically attach the prepared candidate, its initial observation, and the
   new instance generation;
4. detach old view bindings and release old observations/snapshots;
5. destroy the old session;
6. transition to `OPEN` and resume ingress.

The candidate is not presented before the old job reaches terminal state.
Shutdown uses the same asynchronous drain and terminal-only join without
preparing or publishing a candidate.

Opening the same canonical saved identity never tries to acquire a second
writer lease and never destroys the old session to retry. In this refactor it
returns a structured already-open no-op/rejection. A future in-place reload or
lease-transfer command requires its own fail-atomic contract and tests.

### 6.3 Killable job boundary

Bounded replacement/shutdown requires a stronger boundary than cooperative
cancellation of an in-process C thread. Every session-owned job step that may
block indefinitely runs in one owned worker process:

- source traversal and current-read/stat;
- file read/write and image decode;
- Pack input construction and builder execution;
- Export preparation/publication work covered by the job contract.

The host sends immutable input over a bounded versioned protocol and polls
progress/process state without blocking. The worker never owns a session/model
pointer and never mutates project state. Any external publication follows the
existing staged/partial-result contract and is reported explicitly.

Cancellation first sets the protocol token. After a bounded deadline, the host
terminates the owned process tree. Exit, signal/exception, malformed/truncated
reply, output-cap overflow, and forced termination map to structured terminal
job results. Orphan staging cleanup is deterministic and tested. The host can
therefore confirm terminal process state before any join/reap operation.

Cancellation and terminal completion linearize in the host owner. A cancel
accepted before that owner admits the terminal frame owns the `CANCELLED`
outcome even if the worker has already queued its reply; the reply still carries
explicit full/partial/uncertain publication metadata. Once the host admits the
terminal frame, a later cancel is rejected. No worker-side timing guess is a
second authority.

New/Open/Discard/Shutdown and fail-atomic session replacement use this owner.
No caller may preserve the old composite observation token, transaction
acknowledgement, draft target binding, or borrowed pointer across a replacement.

## 7. Atomic observation contract

Independent `events_after()` and `snapshot_create()` calls cannot define GUI
correctness because a commit may occur between their lock acquisitions.

The core therefore exposes one owned observation operation conceptually shaped
as:

```c
typedef struct tp_session_observation_token {
    uint64_t event_sequence;
    uint64_t source_runtime_generation;
    uint64_t recovery_health_generation;
    uint64_t recovery_owner_generation;
    uint64_t job_state_generation;
    uint64_t result_generation;
} tp_session_observation_token;

tp_status tp_session_observe(
    const tp_session *session,
    const tp_session_observation_token *after,
    tp_session_observation **out,
    tp_error *err);
```

The host tracks session-instance generation alongside this token. The exact
public spelling and generation set may differ. Its semantics may not: every
independently coalesced observable authority participates in change detection.

### 7.1 Single cut

Under one session gate, observation:

1. fixes the output composite token and `cut_sequence`;
2. determines whether events after the supplied event cursor remain retained;
3. copies retained events only through `cut_sequence`, or marks `resync`;
4. retains the immutable model generation current at the same cut;
5. captures revision, admission sequence, dirty/identity/recovery scalars, model
   and source generations, and current immutable runtime/job/result handles
   available at that cut;
6. releases the gate;
7. materializes owned DTO data outside the gate.

The observation owns every returned snapshot/handle lifetime. Borrowed aliases
must not outlive it.

Recovery health has two independently changing inputs: model durability health
and session-owned recovery attachment/requirement state. Both generations
participate in the token; the typed recovery scalar is copied directly into the
observation so a recovery-only change does not require project materialization.

### 7.2 Empty observation

If no component of the composite token changed, observation may return an empty
delta without cloning/materializing the project. The exact C contract is
`TP_STATUS_OK` with `*out == NULL`; the caller retains its prior observation and
token. Normal no-change frame polling must remain cheap.

If only a coalesced source/job/result generation changed, observation returns
the matching retained immutable state without materializing a new project
snapshot. Un-evented progress therefore cannot remain hidden behind an
unchanged event sequence.

### 7.3 Normal delta

For a retained range:

- events are complete and ordered through `cut_sequence`;
- any supplied model/runtime/job state describes that same cut;
- the client reduces events before making the new frame observation visible;
- composite token and observation swap together.

### 7.4 Resync

On event gap, future cursor, or session-instance change:

- return `resync=true` with a complete observation at one cut;
- replace prior observed state atomically;
- active drafts whose base revision differs become conservatively conflicted;
- exact known synchronous submit results resolve their draft state;
- unknown remote transaction outcomes require the later Dev API outcome-query or
  result-replay contract.

The client never invents missing event metadata from prose or pointer identity.

### 7.5 Failure

Allocation/materialization failure:

- does not advance the client composite token;
- does not replace the last valid observation;
- leaves session state unchanged;
- can be retried on the next frame;
- surfaces a structured degraded-presentation error.

`USA-02` must inject a commit at the former event-read/snapshot-capture seam and
prove the atomic API cannot lose it.

## 8. Event and observable-state contract

### 8.1 Model events

A committed model event contains:

- sequence and admission sequence;
- before/after revision;
- transaction ID;
- trusted author;
- semantic label;
- model/source generations;
- event kind.

Trusted author is assigned by the host (`human` or
`agent(<controller-instance-id>)`), never accepted from untrusted wire input.

A successful semantic no-op returns a terminal structured result but publishes
no committed change event.

### 8.2 Runtime and job observation

Progress must not flood the bounded model-event ring. Runtime/job owners expose
coalesced immutable current state and generations. Terminal or meaningfully
observable transitions may publish typed events; high-frequency progress is
read as latest state.

Worker completion is immutable and tagged with:

- session-instance generation;
- request/job ID;
- base model/source input token;
- target identities;
- terminal status/result handle.

Only the host admission thread may accept it, mutate runtime/result authority,
advance generation, and publish an event. It discards completions from an old
instance, superseded request, deleted target, or cancelled owner.

Runtime and result snapshots are either retained in the atomic observation
bundle or independently immutable with explicit generation-matching rules.
Borrowed mutable pointers are forbidden.

`tp_core` owns the observable slot and token generations. `tp_build` job code
exposes a constant-time, allocation-free projection callback through its
refcounted opaque job owner. The host-thread observation path pulls that
projection under the session gate, validates/adopts it, and retains the
type-erased owner as the result handle. Workers update only their private
atomics and immutable terminal payload; they never push into the session.
Observation must not depend on, include, or inspect `tp_build` private state.
The result receipt and GUI presentation slots share the same retained owner;
the final release destroys the Pack arena exactly once.

The accepted-result slot retains its own immutable completion envelope. Starting
request B changes the current job projection but does not relabel a retained
result from request A; an atomic observation can therefore contain B's progress
and A's explicitly tagged result without ambiguity. Worker-thread creation is
inside the session admission gate, and the job projection becomes visible only
after creation succeeds, so a failed start preserves the prior state, result,
token, and owners exactly.

### 8.3 Event impact

| Event/state change | Model draft conflict | Presentation effect |
|---|---:|---|
| Model commit from another draft/client | yes | model/history/freshness |
| Exact commit submitted by this draft instance | acknowledgement | model/history/freshness |
| Undo/Redo | yes | model/history/freshness |
| Save at same project identity | no | dirty/fingerprint/checkpoint |
| Source runtime change | no | source rows/thumbnails/freshness |
| Job/progress/result change | no | progress/preview |
| Session replacement/identity rebind | resync/applicability decision | replace all observed state |
| Discard | active target becomes non-applicable | terminal state |

### 8.4 Save As identity

Cross-identity Save As and first Save of an unsaved project change session
identity. While a controller is attached they are either:

- rejected until the controller disconnects; or
- executed by the later explicit rebind protocol with a typed identity-change
  event, new session-instance generation, and controller acknowledgement.

They must never appear to a bound controller as an ordinary same-identity
`SAVED` event.

The lifecycle owner queries a narrow host-owned `controller_attached` status
port before any fallible draft commit or file work. This is not controller
authorization or transport implementation; it makes the identity guard
testable before that transport lands.

## 9. GUI session client

Conceptually:

```c
typedef struct gui_session_client {
    tp_session *session;                  /* host-thread only */
    tp_session_observation *current;
    tp_session_observation_token observed;
    uint64_t session_instance_generation;
    gui_pending_submit_map pending;
} gui_session_client;
```

Pending submit identity includes:

```text
transaction_id
origin_view_id
draft_instance_id, when applicable
synchronous_terminal_result
```

Only an exact `{transaction_id, origin_view_id, draft_instance_id}` match
acknowledges that submitted draft. Any other revision-changing commit conflicts
active drafts, including another transaction from the same GUI process or a
different GUI view.

The client owns attach, observe, resync, submit, replace, and detach. All GUI
transaction adapters submit through it; none call `tp_session_apply()` directly
after cutover.

## 10. Frame contract

```text
1. Drain platform/Dev API ingress into owned host queues.
2. Admit ready commands and eligible worker completions.
3. Obtain one atomic session observation.
4. Reduce events/resync into every registered view/edit state.
5. Pin the observation for the whole frame.
6. Build only required derived projections.
7. Declare views; collect feature-specific semantic actions.
8. Submit post-view actions through gui_session_client.
9. Optionally observe own synchronous completion before present.
```

No commit, observation swap, or session replacement may invalidate data used by
the frame currently being declared.

## 11. Presentation boundary

The GUI uses a hybrid selected for immediate-mode C:

### 11.1 Direct immutable reads are allowed

A simple view may consume:

- frame-pinned `const tp_session_snapshot *` through a read-only snapshot-query
  header, frozen as `tp_core/tp_session_snapshot_query.h`;
- `const gui_view_state *`;
- `const gui_edit_state *`;
- a narrow feature action sink.

The read-only query header exposes immutable DTOs and operations that only
inspect a caller-pinned snapshot. Some queries may fill caller-owned buffers or
allocate an owned serialized value. It must not expose session
creation/destruction, snapshot creation/destruction, mutation-preview,
admission, command, or job-start functions.

Visual layout, text truncation, color, widget mechanics, and direct display of a
snapshot scalar may remain in the view.

### 11.2 Projection/reducer is required when

- data is virtualized or indexed;
- stable IDs must be reconciled with rows;
- a value is derived from multiple authorities;
- enable/warning/error state reflects product policy;
- lifetime or cache ownership is non-trivial;
- the same presentation behavior needs headless unit tests;
- the view would otherwise scan, decode, mutate, or call platform APIs.

Tree rows, selection mapping, canvas layout/result lookup, conflict state,
source health, freshness, history summaries, and job state require such seams.

### 11.3 Forbidden view behavior

Views must not:

- call `tp_session_apply`, Undo/Redo/Save/Discard, Pack/Export admission, or
  source invalidation;
- perform filesystem traversal, source classification, decode, or hashing;
- open native dialogs or launch OS processes directly;
- change revision/cursor/draft conflict policy;
- retain mutable model/session aliases;
- use visual indices as cross-frame identity;
- reproduce core validation, naming, freshness, or capability rules.

No global catch-all action union is mandated. Feature action payloads in C17
must have explicit by-value/owned arms, destruction, capacity, overflow/OOM,
and frame-lifetime contracts.

## 12. Draft conflict state

### 12.1 States

```text
IDLE
EDITING(base_revision, target, field, value, draft_instance_id)
SUBMITTING(transaction_id, submitted_revision, target, field, value)
CONFLICTED(current_revision, target_status, target, field, value)
```

### 12.2 Representation

A draft stores stable target identity, exact field/component, user value, base
revision, view ID, and draft instance ID.

It must not store:

- a visual row/index;
- mutable model/session pointer;
- copied broad domain object;
- ready-to-retry broad operation;
- sibling values from an old snapshot.

Grouped operations rebuild untouched siblings from the newest snapshot on
explicit Apply Mine.

### 12.3 Transitions

- `IDLE -> EDITING` on gesture start.
- `EDITING -> SUBMITTING` on one explicit gesture commit.
- `EDITING -> IDLE` on cancel or net-zero gesture.
- Active draft -> `CONFLICTED` on any revision-changing event except the exact
  transaction submitted by that draft instance.
- `SUBMITTING -> IDLE` on matching committed event.
- `SUBMITTING -> IDLE` on synchronous `OK/no_change`.
- Validation/OOM rejection preserves value and returns to `EDITING` or
  `CONFLICTED` according to current revision.
- Gap/resync with different revision conservatively produces `CONFLICTED`
  unless a known synchronous terminal result resolves the exact draft.
- `CONFLICTED -> SUBMITTING` only through explicit Apply Mine.
- `CONFLICTED -> IDLE` through Discard.
- If target was deleted/non-applicable, Apply Mine is disabled; textual Copy and
  Discard remain.
- A second race returns to `CONFLICTED`; no retry loop exists.

### 12.4 Lifecycle transition table

An outer action first performs non-mutating preflight. A known rejection, such
as cross-identity Save As while a controller is attached, leaves the draft and
model untouched. It must not commit the draft and then reject the outer action.

| Trigger | Draft transition | Outer-action result |
|---|---|---|
| Enter or pointer release | submit once; success/no-change -> `IDLE`; failure preserves value in `EDITING`/`CONFLICTED` | gesture ends only on terminal success |
| Blur | same submit rule as Enter | dependent semantic action runs only after success; focus may move but failed draft remains presented |
| Escape | explicit local Discard -> `IDLE` | consumed; no model/history change |
| Save | submit active draft first | Save runs only after terminal success/no-change; failure aborts Save |
| Save As | preflight identity/controller/path feasibility, then submit active draft | any preflight or submit failure aborts Save As with draft preserved |
| Pack or Export | submit active draft first | job starts only after terminal success/no-change |
| Undo or Redo | preserve active draft | command is blocked with explicit Apply/Discard choice; it is never silently combined with a draft commit |
| Open, New, project Discard, or Close | show one Apply and Continue / Discard and Continue / Cancel decision when a draft exists | Apply continues only after successful submit; Discard drops the draft; Cancel aborts the outer action |
| Validation, OOM, or revision rejection during a prerequisite submit | preserve value; choose `EDITING` or `CONFLICTED` from current revision | abort the initiating outer action |
| Event gap/resync | unchanged base may continue; changed revision -> `CONFLICTED`; known exact terminal result may resolve | no implicit retry or outer action |
| Session replacement | resolve through the Open/New/Discard/Close decision before cutover | no draft crosses session-instance generation |

No old `flush_pending` caller may survive without one exact row in this table.

## 13. Stable per-view identity

Canonical retained state uses structural identity:

- atlas/source/animation/target IDs;
- source key;
- future stable object IDs.

Indices and virtual row IDs exist only inside one projection generation.

After observation replacement:

1. resolve canonical selection/edit target;
2. preserve it when still present;
3. use one documented parent fallback or clear it;
4. never select a different object occupying an old index.

One session event batch is reduced into every registered view, visible or not.
Commit from View A conflicts an active draft in View B.

## 14. Source-runtime boundary

Filesystem scan, classification, fingerprint baseline, refresh diff, semantic
source hash, and full verification belong below GUI.

The architecture refactor establishes the observation/projection seam. Moving
the current file/folder runtime base earlier than B1 requires an explicit
ROADMAP/master-plan dependency update. Linked-atlas, watcher, and broader B1
behavior remain in B1.

Until that cutover, a tree/list view cannot claim full passive-boundary
completion. No duplicate second runtime is introduced as an interim solution.

## 15. Failure and performance

- Observation failure never advances the composite token.
- Revision conflict never modifies project state.
- Worker completion after replacement/close is discarded by instance
  generation.
- Transport response serialization never runs under session gate.
- No UI-thread filesystem traversal, decode, hash, or blocking job join.
- One observation batch retains/materializes at most one model generation.
- No-event polling performs no project clone.
- A job/source/result-only observation does not materialize a project clone.
- Projections are generation-keyed and virtualized where required.
- Deterministic work/allocation limits are CI gates.
- Wall-clock interaction budgets use controlled benchmark environments or
  calibrated thresholds; CI does not fail solely on an unstable shared-runner
  maximum.

## 16. Verification requirements

### Observation and host

- `USA-01`: atomic observation returns events and immutable state from one cut.
- `USA-02`: an injected commit at the former event/snapshot seam is neither lost
  nor displayed before its reducer metadata.
- `USA-03`: event gap produces complete resync without duplicate/lost commit.
- `USA-04`: observation OOM preserves old composite token and observation.
- `USA-05`: no-event observation performs no project materialization.
- `USA-06`: host shutdown order rejects completion/query/apply after generation
  invalidation and prevents session UAF.
- `USA-07`: reverse-order Refresh/job completions accept only the current
  eligible request.
- `USA-30`: a coalesced progress generation change with no model event is
  visible on the next observation without project materialization.
- `USA-32`: a delayed/blocking worker keeps replacement in `DRAINING` while the
  frame pump remains responsive; candidate publication and old-session destroy
  occur only after terminal state and a non-blocking join.

### Client parity and acknowledgement

- `USA-08`: equal GUI/headless typed transactions produce equal
  snapshot/event/history/revision/result.
- `USA-09`: direct external session commit is visible at the next observation
  without GUI wrapper invalidation.
- `USA-10`: own revision-changing commit updates presentation through its echo.
- `USA-11`: `OK/no_change` terminates its draft without waiting for an event.
- `USA-12`: lost remote response plus retained-ID retry commits once.
- `USA-13`: 100-operation agent transaction is one revision/event/history/Undo
  entry.
- `USA-14`: trusted author/label reach every observing client.

### Drafts and views

- `USA-15`: table tests cover every draft transition and invalid action.
- `USA-16`: agent commit during numeric, text, rename, and grouped edit preserves
  the draft and conflicts explicitly.
- `USA-17`: same-field and different-field commits use the same v1 rule.
- `USA-18`: commit from View A conflicts active draft in View B.
- `USA-19`: target deletion disables Apply Mine without losing copyable text.
- `USA-20`: Apply Mine never restores stale grouped sibling fields.
- `USA-21`: Save/source/job state does not conflict a draft; Undo/Redo does.
- `USA-22`: gap/resync and repeated race have deterministic transitions.
- `USA-23`: one gesture produces at most one transaction and Undo entry.
- `USA-24`: external insert/remove/reorder preserves or explicitly clears stable
  selection.
- `USA-31`: table tests cover every lifecycle trigger and prove an outer action
  cannot continue after failed draft prerequisite or known preflight rejection.

### Boundaries and scale

- `USA-25`: boundary checks reject mutation, filesystem, platform, and business
  policy in views.
- `USA-26`: targeted reducers/projections run without `nt_ui`.
- `USA-27`: production GUI contains no duplicate source-runtime or freshness
  implementation after the corresponding roadmap cutover.
- `USA-28`: owner-scale work-count/allocation and controlled latency gates pass.
- `USA-29`: cross-identity Save As cannot silently move a bound controller.

## 17. Acceptance gate

The focused session-observation/draft phase is complete when:

1. atomic observe/resync exists and is used by the GUI;
2. all GUI transactions submit through `gui_session_client`;
3. New/Open/replace/shutdown use its lifecycle owner;
4. wrapper-specific model/Save snapshot drops are deleted;
5. semantic drafts and stable selection are per-view and fully tested;
6. external commits are visible and cannot silently overwrite a human draft;
7. read-only snapshot access is separated from session mutation APIs;
8. boundary and parity gates pass.

Full GUI architecture completion additionally requires the source-runtime
cutover and targeted view slices defined by the execution plan. Dev API
transport, authorization, handoff, and additional windows remain explicitly
deferred.

## 18. Edge coverage

Covered explicitly:

- commit between event query and snapshot capture;
- event ring overflow and lost own echo;
- synchronous semantic no-op;
- own client versus exact originating draft;
- two GUI views;
- target deletion and grouped stale siblings;
- Save without revision change;
- Save As identity change with attached controller;
- reverse worker completion and completion after close;
- delayed worker drain during replacement/shutdown;
- session replacement and observation OOM;
- progress-event flooding;
- un-evented coalesced progress;
- stable selection after structural changes.

## 19. Related material

- `docs/ntpacker-master-spec.md` §§4, 7–10, 16–23, 61
- `docs/design/ux.md`
- `docs/plans/ui-session-architecture-refactor-plan.md`
- `docs/research/ui-session-architecture-comparison.md`
