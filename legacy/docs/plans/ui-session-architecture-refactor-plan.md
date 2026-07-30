# UI/Session Architecture Refactor Plan

**Status:** R0-R5 implemented and verified; raw title-bar Close veto remains
blocked on a public engine seam. Six outcomes were subsequently amended by
Track S — see §0.
**Baseline:** `d9ff3ff`
**Normative source:** `docs/ntpacker-master-spec.md`
**Architecture contract:** `docs/design/ui-session-architecture-spec.md`

## 0. Superseded amendments (Track S)

Track A executed as written and this document is kept as its record; it is not
rewritten. `docs/plans/ui-session-simplification-plan.md` (Track S) then amended
six of its outcomes on the same branch, following an independent review of
`d9ff3ff..7dc7f00`. Where the two disagree, Track S wins. The left column
describes the Track A end state at `7dc7f00`, not a defect in the plan text.

| Track A end state (`7dc7f00`) | Track S amendment | Commit |
|---|---|---|
| Pending-submit map retained a copy of the transaction result so a lost response could be replayed | Replay half deleted. The map is a receipt map (`observation_status` + terminal) only; retry is a caller-supplied retained transaction ID answered by core `TP_STATUS_DUPLICATE_ID`. `USA-12` moved to the core retained-ID test | `1a83549` |
| Draft `SUBMITTING` carried a fabricated `base_revision + 1` submitted revision | Revision prediction removed. `submitted_revision` is filled from the receipt only, and `SUBMITTING` is entered only when the submit actually reaches the session (preflight first) | `5d3a675`, `1a83549` |
| §4 R2d closed lifecycle ownership; history/identity/source commands stayed wherever a session was borrowed | Undo/Redo/Save/SaveAs/invalidate and their capability/depth queries re-homed onto the host owner (`gui_host_binding`), enforced by a new `A2d single host command owner` checker sweep | `d914b50` |
| The boundary checker classified views by the `gui_view_*` filename prefix, and two of its rules were fully neutralized by their own exemptions | Classification is a declared file list with bidirectional disk↔list guards; `VIEW_PLATFORM`/`VIEW_MODEL_POLICY` demoted to a non-gating debt report; `_arch_assert_absent` fails closed on a missing guarded file | `6db159a` |
| The job worker returned the Pack artifact inside the response frame, under a 256 MiB blob cap | Wire protocol v2: the terminal Pack frame carries the per-request artifact path + `u64` size; the artifact moves by private file handoff and is deleted on adoption/cancel/failure/destroy (master spec §10.4, §10.6) | `032ba50` |
| §5 R3b lifts the draft lifecycle into "one `gui_draft_owner` per GUI view" | Production has exactly one draft owner by construction; the two-view conflict rule is a reducer-level property proven at struct level (spec §13, `USA-18`) | `1a83549`, `886f482` |

Track S explicitly did **not** change one thing this plan leaves open: the
nested build worker remains under the outer job worker, for Pack and for Export
alike (master spec §10.6).

## 1. Outcome

Track A installs one predictable data path:

```text
view intent
  -> host admission
  -> tp_session mutation
  -> atomic immutable observation
  -> view/edit reducer
  -> view-local state
  -> render
```

`tp_session` is the only live model writer. Views never mutate observed state,
perform domain validation, or retain a competing model truth. A facade may
derive a value but may not mirror another owner's state machine.

The required boundaries are:

- `gui_session_client`: attach, observe, resync, submit, frame pin;
- `gui_host_queue`: command/job admission and completion staging;
- `gui_host_binding`: replacement and shutdown lifecycle;
- worker process: immutable job input and killable blocking work;
- `gui_edit_state`: one pure draft/conflict state machine;
- concrete view owners: stable identity, navigation, and local draft instances.

This is not a global store, MVVM framework, universal DTO graph, actor rewrite,
or generic callback architecture. Simple immutable snapshot reads remain
allowed. A new abstraction requires a real ownership boundary or two production
consumers.

## 2. Execution rules

1. Execute Track A strictly as
   `R0 -> R1a -> R1b -> R1c -> R2a -> R2b -> R2c -> R2d
   -> R3a -> R3b -> R3c -> R3d -> R4 -> R5`.
2. Production files with overlapping responsibility are not edited in parallel.
3. Prove the new owner through public behavior before deleting the old path.
4. Delete the replaced path in the same packet. No fallback, dual-write,
   compatibility bridge, deprecated API, or dead code survives cutover.
5. Keep project/operation schemas and export formats unchanged.
6. Keep `external/neotolis-engine` read-only.
7. Worker/transport threads never call or retain raw `tp_session *`.
8. Invalid user input returns structured status/error data, never a crash.
9. Use Neotolis assertions for internal invariants; do not hide side effects in
   assertions.
10. Every packet runs its focused tests, native Debug build, architecture gates,
    and full native Debug `ctest`. R5 additionally runs native Release.
11. Tests live primarily at the owning layer. Use a small number of integration
    traces; do not repeat a field-by-trigger cross-product through every facade.
12. LOC is diagnostic. Split code only when ownership or dependency direction
    becomes clearer.

## 3. Ownership after Track A

| Authority | Sole owner | Consumers |
|---|---|---|
| Model, revision, history, dirty state | `tp_session` / core model | GUI, CLI, future MCP/Dev API |
| Atomic model/runtime/job observation | `tp_session_observe()` | `gui_session_client` |
| GUI observation and submit receipts | `gui_session_client` | project/actions/reducers |
| Command and job admission | `gui_host_queue` | `gui_host_binding` |
| Replacement/shutdown | `gui_host_binding` | project lifecycle facade |
| Blocking job execution | owned worker process | session job owner |
| Frame read state | pinned immutable observation/snapshot | views |
| Draft lifecycle and conflict state | `gui_edit_state` reducer plus concrete action/view owner | views and action integration |
| Selection and navigation identity | concrete view owner using stable structural IDs | rows/canvas/lists/settings views |

`tp_core` remains below `tp_build`. Core observation stores retained typed
job/result state through a narrow publication port and never depends on build
private data.

## 4. Completed packet ledger

Detailed pre-implementation prose is preserved in git history. This ledger is
the executable evidence for completed packets.

| Packet | Landed owner/contract | Deleted or forbidden path | Evidence | Commit |
|---|---|---|---|---|
| R0 | normative ownership map, read-only snapshot query boundary, architecture checker | view admission/I/O/platform calls; frontend dependency into core; async raw-session capture | positive and negative architecture fixtures | `39123eb` |
| R1a | one atomic owned observation and composite token | independent GUI correctness reads via `events_after + snapshot_create` | race, gap/resync, OOM retry, no-materialization tests | `3630b42` |
| R1b | coalesced typed job/result observation | GUI-private runtime truth and event-ring progress dependence | generation/result ownership, reverse completion, burst tests | `c638518` |
| R1c | owned worker-process protocol and non-blocking host pump | in-process job thread retaining raw session; blocking frame join | protocol caps/faults, cancellation, process-tree tests | `7865dbf` |
| R2a | frame-pinned `gui_session_client` observation | GUI snapshot cache and mutation-specific observation truth | external commit, Save, resync, frame pin, allocation faults | `dc8975c` |
| R2b | `gui_host_queue` admission/completion owner | direct GUI job start/poll/session access loops | stale generation, staged completion, reverse job traces | `8090a9b` |
| R2c | all GUI mutations through typed client receipts | direct GUI `tp_session_apply`, sequential transaction IDs, mutation refresh invalidation | parity, idempotency, exact identity/echo, create visibility | `578f17b` |
| R2d | fail-atomic prepare/drain/cutover in `gui_host_binding` | direct session replacement/destruction and job-wait teardown | New/Open/shutdown, active-job drain, candidate OOM | `c2c91a3` |
| R3a | pure `gui_edit_state` reducer and one draft owner per edited scalar | per-widget pending flags and duplicated draft transitions | reducer transition table, no-op, OOM, invalid input, Undo tests | `6b3d97c` |
| R3b | the same reducer for text, rename, and path drafts | field-specific text/rename compatibility routes | focused draft integration and structured validation tests | `d900fd9` |
| R3c | explicit grouped read-modify-write draft owners | broad pending queue, timer flush, whole-target setter, fallback paths | ordering, conflict, deletion, retry, and zero-symbol gates | `ac1bc74` |
| R3d | one action flow per outer ordering class | duplicate integration fixtures and implicit success on failed commit | action traces, two-view conflict, OOM/resync/shutdown acceptance; title-bar Close veto remains an upstream engine blocker | `d42dc99` |
| R4 | stable structural identity beside each concrete view owner | global index/reselect state and pointer/lifetime identity | removal-before-selection, frame move, external change, and OOM reconciliation tests | `9e5b1c3` |
| R5 | exact cutover gates and narrow identified submit surface | legacy rename APIs, broad target-path setter, ownerless result-destroy fallback, redundant tests | Debug/Release full suites, GUI self-test, parity, deletion manifest, independent critical review | this commit |

The R0-R2d audit additionally requires:

- no locally cached `duplicate_id` receipt;
- no ambient `last_submit`;
- no derived occupancy/readiness flags;
- canonical operation labels from the core catalog;
- no raw-arena result ownership fallback;
- bounded forced terminalization and safe process-group lifetime;
- real production-path tests for partial publication and delayed drain.

## 5. Active packets

### R3a — Pure draft reducer and atlas scalar fields

**Goal:** introduce one draft/conflict state machine with no widget or session
ownership.

**Owned production area:**

- new `apps/gui/gui_edit_state.h/.c`;
- atlas-setting edit/action/view code;
- CMake and architecture deletion gate.

**Implement:**

1. Concrete states: `IDLE`, `EDITING`, `SUBMITTING`, `CONFLICTED`.
2. `gui_edit_state` stores only stable lifecycle identity and revisions. The
   concrete `gui_atlas_draft` beside it stores the exact component and scalar
   value; there is no generic field/value union.
3. Reduce exact submit echo, foreign model event, no-op, validation/OOM,
   gap/resync, target deletion, Apply Mine, and Discard.
4. Convert the complete `CK_ATLAS_SETTING` family end to end.
5. Keep simple committed values as direct pinned-snapshot reads.
6. Apply the normative lifecycle table to the first converted draft family:
   failed gesture/Save/Pack prerequisites abort dependent actions; Undo/Redo
   preserve the draft; Open/New and in-app Exit require Apply and Continue,
   Discard and Continue, or Cancel.

**Verified owner/dependency cut:**

- the settings view emits one scalar intent and renders the reducer's effective
  value over the pinned snapshot;
- one concrete `gui_atlas_draft` combines the lifecycle state with the exact
  atlas component/value and is the only semantic uncommitted value;
- the action sink rebuilds one narrow masked atlas operation only at submit;
- after an exact successful atlas prerequisite, only remaining intents captured
  at that exact pre-submit revision advance with the local action batch; an
  already-stale intent is never rebased or retried;
- `gui_session_client` supplies exact view/draft/transaction receipts and the
  atomic observation stream;
- `tp_session` remains the only model, validation, revision, history, and
  mutation owner.

**Delete:**

- the `CK_ATLAS_SETTING` route from the broad pending operation;
- the atlas action-intent array and ready-to-submit pending operation;
- atlas scalar mirrors as semantic state. Engine-required text/caret scratch may
  remain only as presentation storage and must be refreshed from the reducer's
  effective value when it is not focused;
- the atlas use of revision-only acknowledgement/rebase. The shared
  `revision_after_owned_route()` helper remains until its R3c consumers are
  converted and is deleted with them.

**Gate:** exhaustive pure reducer table; one gesture is one transaction/Undo;
another transaction from the same GUI process conflicts unless view/draft ID is
an exact match; failed prerequisite submission cannot continue or retry its
dependent action; every session-replacing lifecycle request requires the
explicit draft decision.

### R3b — Text, rename, and path drafts

**Goal:** reuse the R3a reducer for text-shaped values without creating a field
framework.

**Owned production area:** the concrete GUI draft owner, relevant actions,
text widgets, list/settings/chrome views, main-loop edit routing, session
adapters, project mutation/pending callers, the Save-As preflight seam, and the
R3 reducer/action tests.

**Implement:**

1. Lift the R3a lifecycle into one `gui_draft_owner` per GUI view. It contains
   exactly one `gui_edit_state`, a closed family discriminator, and the two
   concrete payloads needed so far: atlas scalar and text. Do not add a value
   variant, field registry, callbacks, vtable, or global UI store.
2. Convert exactly four current text semantics: atlas name, animation name,
   sprite rename, and target `out_path`. The text payload stores its exact
   domain kind, canonical structural identity, and one owned UTF-8 value.
3. Permit only one active draft by construction. One observation reducer and
   one receipt/conflict/resync path serve both concrete families.
4. Preserve the user's text on conflict, validation failure, OOM, resync, and
   target deletion. Apply Mine resolves the stable identity again against the
   newest pinned snapshot and never performs automatic retry or merge.
5. Keep engine-required caret/text memory as presentation-only scratch. It is
   synchronized from the active draft and writes changes immediately back
   through the action ingress; it is not another semantic owner.
6. Offer Apply Mine, Discard, and Copy only where Copy has real meaning.
7. Every text submit uses an exact identified terminal receipt. Target path
   submits only `TP_TF_OUT_PATH` and never round-trips sibling fields.
8. Test one representative action for each ordering:
   commit-first, choice-first, and preflight-first.
9. Abort every outer action when its draft prerequisite fails. Save As performs
   its read-only path/identity/controller feasibility preflight before draft
   submission; no continuation/token framework is introduced.

**Delete:**

- `s_edit_kind`, `s_edit_atlas`, `s_edit_anim`, `s_edit_sprite`, and
  `s_edit_buf` as semantic state;
- `edit_atlas_*`, `edit_anim_*`, and `edit_sprite_*` action-state mirrors;
- `s_pending_commit_edit`, `s_pending_commit_edit_enter`,
  `commit_active_edit`, `commit_sprite_rename`, and the force-cancel path that
  loses an invalid draft;
- `TARGET_INTENT_OUT_PATH`, its heap queue/drain route,
  `CK_TARGET_OUTPATH`, and the target-path use of
  `gui_project_pending_offer`;
- the per-target path array as semantic state. A single active engine input
  scratch may remain only as presentation storage;
- converted GUI callers and implementations of the old revision-only rename
  and path mutation routes. Their replacements are narrow identified-submit
  endpoints with exact terminal receipts;
- selection-change `cancel_edit()` paths that silently discard text.

Stable widget identity remains owned by R4. R3b binds the draft to stable domain
identity and must not pull the broader row/widget identity cutover forward.

**Gate:** one active draft, no text loss, no outer action continues on an older
committed state, all four text kinds build the exact operation/mask, and no old
inline/path pending symbols remain.

### R3c — Grouped read-modify-write drafts and final legacy cutover

**Goal:** convert grouped components and remove the old pending architecture.

**Owned production area:** project pending/mutation/file modules, action modules,
settings/list widgets, state, main loop, selftest, and focused integration tests.

**Implement:**

1. Extend the existing closed draft owner with concrete sprite and animation
   payloads. Store stable target identity, exact component, typed value, and
   captured revision only; do not add a generic field registry/value variant.
2. Convert sprite origin, Slice-9, sprite overrides, animation FPS/playback,
   and per-axis flip. Origin `{x,y}` and Slice-9 `{L,R,T,B}` are the only true
   grouped read-modify-write operations in the current core contract.
3. On submit or explicit Apply Mine, rebuild untouched origin/Slice-9 siblings
   once from the newest atomic observation. Every other converted component
   submits its exact independent operation mask; Flip H never resends Flip V
   and vice versa.
4. Keep animation frame add/remove/move and narrow target enabled/exporter
   requests as structural/discrete between-frame intents. Delete the unused
   broad whole-target compatibility path instead of creating another draft.
5. Test sibling change, source/animation deletion, foreign-view conflict,
   exact component masks, and one gesture -> one Undo step.

**Delete completely:**

- `gui_project_pending.c` and its CMake registration;
- `pending_valid`, `pending_key`, `pending_op`, `pending_time`,
  `pending_expected_revision`, `pending_preview_stale_before`;
- `gui_project_flush_pending`, `gui_project_pending_route`,
  `gui_project_pending_offer`, `gui_project_peek_pending_slice9`,
  `gui_project_flush_elapsed`, `gui_project_pending_discard`;
- `gui_project_tick`, the pending-owner clock, timer fallback,
  `gui_project_flush_error`, and `gui_actions__flush_failed`;
- sprite scalar intent storage and animation FPS/playback/flip queue arms;
- `TARGET_INTENT_FULL`, `gui_edit_target`, the broad whole-target setter, heap
  target payload, superseded field buffers, and compatibility tests/drivers.

**Gate:** no stale sibling overwrite, no failed prerequisite continuation, and
zero legacy pending symbols.

### R3d — Draft integration acceptance

R3d adds no production abstraction and delays no deletion.

Prove:

- one gesture -> one transaction -> one Undo step;
- one flow per outer-action ordering class;
- validation, OOM, resync, target deletion, and two-view conflict;
- raw close/shutdown never converts a failed draft commit into silent success.

Raw title-bar Close requires a public engine close-request/veto seam. The
engine submodule remains read-only: that seam must ship through an upstream
engine issue/PR before this R3d acceptance item can pass.

Remove integration fixtures that duplicate the pure reducer table.

### R4 — Stable identity at concrete view owners

**Goal:** remove cross-frame index authority without adding a global view store.

**Owned production area:** existing GUI state, rows/canvas/list/settings views,
their actions, main loop, and canonical-identity tests.

**Implement:**

1. Store atlas/source/key/animation/target identities canonically.
2. Keep indices and row IDs projection-generation-local.
3. Keep each concrete view's state beside its existing owner and reconcile it
   from observation/resync.
4. Define one explicit parent fallback or clear behavior.
5. Add a shared selection helper only if two production views need identical
   reconciliation.

**Delete completely:**

- cross-frame authority of `s_sel_atlas`, `s_sel_src`, `s_sel_child`,
  `s_sel_anim`, `s_sel_anim_frame`;
- `s_sel_anchor_row`, `s_focus_view`, `s_sel_abs`, `s_sel_missing` as retained
  identity;
- `s_reselect_pending`, `s_reselect_source_id`, `s_reselect_key`,
  `s_reselect_atlas_id`;
- pointer/lifetime identity used as a retained selection key.

**Gate:** external structural change never selects or edits the wrong entity.
R3 already owns the two-view draft-conflict matrix; do not repeat it here.

### R5 — Focused foundation hardening

1. Run deletion checks for direct GUI submit, local model/Save invalidation,
   legacy pending storage, index-authoritative identity, and raw session capture.
2. Run parity, event-gap, OOM, no-op, lifecycle, process, draft, and stable-ID
   acceptance.
3. Run native Debug and Release builds/full suites.
4. Remove redundant tests and all temporary test seams that bypass production
   behavior.
5. Update roadmap evidence only after every gate passes.

R5 adds no production abstraction.

## 6. Follow-on work outside Track A

These are separate vertical slices and do not expand R3-R5:

- **PLATFORM-SEAM:** dialogs, clipboard, path/URL launch, app settings;
- **RESULT-INDEX:** extend and consolidate the existing canonical `gui_pack`
  index for rows/settings/canvas; never create a parallel index;
- **SR-BASE:** move existing path file/folder scan, status, fingerprint, refresh
  diff, and immutable runtime snapshot below GUI;
- **PV-settings:** direct snapshot + draft reducer; derive only real policy
  summaries;
- **PV-tree/list:** runtime snapshot + virtualized stable-ID rows;
- **PV-canvas:** existing result index + pinned result handles;
- **PV-chrome/dialogs:** view layout only; controller/platform owns effects.

MCP/Dev API transport, authorization/claim/handoff, linked atlases, watchers,
and broad format work remain roadmap work. They consume the same typed
session semantics but are not hidden in Track A.

## 7. Verification matrix

| Risk | Required evidence |
|---|---|
| Snapshot contains unseen commit | injected observation seam race |
| Gap loses an own echo | exact receipt plus resync reduction |
| Duplicate retry mutates twice or reports stale revision | retained-ID core retry test |
| OOM advances observation token | allocation fault and retry |
| Progress floods the event ring | 100 real admissions, fixed event sequence |
| Older job completion replaces newer result | B terminal before late A |
| New/Open destroys old state on prepare failure | fail-atomic candidate test |
| Replacement blocks a frame | real delayed child and bounded terminalization |
| Forced worker modifies files then fails | filesystem plus partial/uncertain result |
| POSIX destroy signals a reused PGID | post-reap lifetime test |
| Agent overwrites active input | scalar/text/grouped conflict tests |
| Apply Mine restores stale siblings | grouped component test |
| Structural edit selects wrong entity | stable-ID reconciliation |
| Bound controller follows Save As | identity-rebind rejection |
| Old architecture survives | zero-symbol/dependency gates |

## 8. Definition of done

Track A is complete only when:

- the one-way flow in section 1 is the production correctness path;
- `tp_session` is the only model/mutation truth;
- client, queue, binding, reducer, and concrete views each own one named state;
- every GUI mutation returns/uses its exact typed receipt;
- drafts and selection use stable view-local identity;
- legacy pending, index authority, fallback ownership, and compatibility paths
  are deleted;
- Debug/Release, full `ctest`, parity, architecture, fault, lifecycle, and
  process gates pass;
- an independent critical review has no unresolved P0/P1 findings.
