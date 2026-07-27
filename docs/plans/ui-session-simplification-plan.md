# UI/Session Simplification Plan (Track S)

**Status:** approved for execution
**Baseline:** `7dc7f00` (branch `codex/ui-session-architecture-refactor`)
**Normative:** `docs/ntpacker-master-spec.md`, `docs/design/ui-session-architecture-spec.md`
**Provenance:** independent 6-agent critical review of `d9ff3ff..7dc7f00` (2026-07-27)
plus 3-agent adversarial review of this plan. Findings and amendments are
recorded in the review transcripts; this document is the executable result.

## Goals (measured, not aspirational)

- Fix every P0/P1 from the review (battery is green today and catches none).
- Net production LOC: −1.5k..−2k without losing any spec guarantee.
- One future editable field: ≤2 write-path edits (today 6-7).
- Scalar mutation: ≤5 GUI hops, one `tp_operation` build (today 6 hops, built twice).
- One boundary checker whose every rule actually fires; no exemption that
  neutralizes its own rule; no occurrence-count ratchets (AGENTS.md).
- Battery preset names are explicit: `native-tests-debug` (148 today) and
  `native-release` (147). Each packet states its expected ctest delta.

## Rules

1. One packet = one commit; full battery + boundaries before each commit.
2. Test deletion only with a named successor in the same commit.
3. The five oracle tests are frozen: `test_atlas_draft_maps_every_scalar_component`,
   `test_text_drafts_submit_exact_atlas_animation_sprite_and_target_ops`,
   the three `*_apply_mine_preserves_*` tests, and
   `test_real_client_parity_manifest_covers_every_shipped_mutation`.
   Any diff to them is a regression, not an update.
4. Dropped from the original draft after adversarial review (do not resurrect):
   in-process builder for Pack (Export shares `tp_pack_cancellable`; nested
   build worker also owns ASCII staging, atomic publication, 20 ms cancel);
   separate `argv[1]` for the job worker (dispatch is already by frame magic);
   `instance_nonce`/`model_generation` token fields (host tracks instance
   generation per spec §7; S1's event fix covers the model path);
   queue/binding struct merge (kills 15 tests + a live struct invariant);
   commands on `gui_session_client` (violates spec §9; they go to the host owner);
   deleting the CMake checker (loses 60/68 enforcement points and 2 CI legs);
   full-file artifact digest (1-3 s; size+magic+stat suffices for our own file);
   staging keyed to host pid (inverts the self-healing reaper);
   removing the 1-frame Pack start latency (structural per spec §10).

## Packet order and content

### P1 — S0a: GUI draft/receipt correctness

- `gui_view_settings.c:896` uninitialized `inactive_scratch`: init and collapse
  the two-buffer branch (one active draft by construction).
- Stuck-`SUBMITTING` fix, preflight-first shape: `submit_draft` builds the
  operation and performs all pre-validation BEFORE `gui_edit_commit`; the FSM
  enters `SUBMITTING` only when `gui_session_client_submit` is actually invoked.
  Receipt postcondition in the client stays as defence-in-depth: move
  transaction-id resolution above the admission/pin gates, fill terminal on
  every return (incl. `DUPLICATE_ID`, capacity, replay-copy failure); consumer
  treats empty `transaction_id` as an explicit no-receipt branch.
  Structural intents (9 adapter functions without `out_terminal`) are
  receipt-free by contract — documented, not patched.
- `open_preview_ref`: submit-first instead of `cancel_edit()`.
- Sibling-field blur inside the settings panel submits the active draft
  (spec §12.4 Blur row); other §12.4 rows get an explicit already-green note.
- Atlas selection fallback: index-0 → nil + "delete selected atlas" test.
- `--shot`: split `gui_shot_tick` — dead-stick + frame-6 pack go pre-pin
  (beside `gui_bench_tick`), frame-10 select/preview stays post-rows.
- `tp_session_save_detached_recovery` publishes an event (token moves).
- Rejected terminal releases the Pack arena (`release_payload` hook) +
  assert in the cancel-race test.
- `gui_host_queue`: `cancel_queued` cleared only on successful admission;
  bounded shutdown retry loop with rate-limited logging.
- `NT_ASSERT`+dead-`if` contradictions (2 sites): keep exactly one.
- Guard the three CLI `NTPACKER_TEST_*` env hooks behind `TP_ENABLE_TEST_SEAMS`;
  `NTPACKER_GUI_HEADLESS` is a deployment flag and stays.
- ctest delta: +1..+2 (new fallback/cancel-race assertions).

### P2 — S6': boundary checker fixed in place (before write-path churn)

Keep `cmake/check_architecture_boundaries.cmake` and its fixtures (3-platform,
ctest-wired). Fix what the review proved broken:

- Role/directory classification replaces the `gui_view_*` filename prefix
  (the empirical bypass: a non-`gui_view_*` file with violations passed clean).
- `_arch_assert_absent` asserts the guarded file still exists (fail-open fix).
- `VIEW_MODEL_POLICY` / `VIEW_PLATFORM` (fully neutralized by their own
  exemptions): demote to a non-gating debt report per AGENTS.md; exemptions
  that remain get a rationale comment, never an occurrence count.
- Remove the `gui_host_queue` async carve-out by expressing the real rule.
- New rules: `borrow_active_session` only in owner files;
  `gui_host_queue_*` symbols only in `gui_host_binding.c`/`gui_host_queue.c`
  (prepares P5). `tp_session_apply`/observe/submit/lifecycle owner sweeps
  already exist — verified live, kept.
- Prune only ratchet entries whose guarded symbol AND file are both gone.
- ctest delta: 0 (rules change, registrations stay).

### P3 — S1: job transport (includes the transport-side S0 items)

- Worker: `tp_pack_produce_observed(..., out_path, out_cancelled, ...)` sibling
  entry — skip the dead back-parse (`result` is only null-checked today) and
  the duplicate disk read in `read_artifact`. `tp_pack_cancellable` untouched
  (Export/CLI read result fields).
- Artifact by path: per-request private subdir under `work_dir`
  (fixes native/preview and two-instance name collisions); response carries
  the exact path + size; artifact blob leaves the wire; the UTF-8 name table
  STAYS on the wire (load-bearing: rename-during-pack must not relabel
  sprites — add that test). Codec keeps variable-length frames; the
  `MAX_ARTIFACT_BYTES` cap and its stream-cap derivation go; recompute the
  two hard-coded byte offsets in `test_bad_magic_version_and_oversized_fields_fail_closed`.
- Host: budgeted chunked file read in the pump (16 MiB/frame), then one
  `tp_pack_read_memory`; validation = size + magic + `tp_fs_stat`
  (`staged_artifact_ok` pattern), no digest. Host deletes the artifact after
  adoption; per-request dirs reaped via the existing worker-pid reaper.
- Cancelled Pack emits a valid terminal frame (`run_pack` returns
  `TP_STATUS_CANCELLED` when cancelled with no result).
- `transport_failed` folds into the terminal state for Pack AND Export.
- Cancel grace 250 ms → 1000 ms; worker polls the cancel byte in the decode
  observer between images.
- Win32 pipes: GUID suffix, `ERROR_PIPE_BUSY` retry, overlapped only for
  owned-tree spawns.
- Tests: FRESH e2e, cancel linearization via `TP_TEST_JOB_WORKER_BLOCK_MS`
  (fix the racy `test_host_cancel_owns_process_terminal_result` instead of
  adding a twin: add `../src/tp_job_worker_main.c` + seams to that target),
  empty-export-as-skipped, rename-during-pack. Promote selftest phase 9's
  async==blocking equivalence assertion into `run_selftest()` (no GL needed;
  closes the CI headless gap).
- ctest delta: 0 (Unity cases inside existing ctests).

### P4 — S3: GUI write path

- One `tp_operation` build per mutation: `gui_project_submit_*` passes the
  built op through; delete the rebuild in `gui_session_adapter`.
- Delete the genuinely thin forwarders (6-8 fns, ~10 production call sites);
  `gui_project_mutations.c` keeps its real logic (visible-index resolution,
  sibling read-modify-write, batch plan, invalidate) — it is the concrete
  validation owner, not a forwarder file. No logic moves into the adapter
  (parity harness must stay view-free).
- Pure-data descriptor table `{family, kind, op_kind, field_mask, value_type,
  component}` replacing the dispatch switch families; exactly two
  default-less switches remain (payload write, snapshot read) so `-Wswitch`
  exhaustiveness survives. No function-pointer columns. One begin-or-continue.
- Delete: fabricated `revision+1`; replay half of the pending map
  (`transaction_result_copy`, replay branch, its seam) — retry contract
  becomes `TP_STATUS_DUPLICATE_ID` + terminal receipt, USA-12 moves to the
  core retained-ID test (named successor); duplicate
  `gui_project_job_completion` re-box; 170-line structural region →
  `gui_actions_structural.c`.
- Selftest retarget: ~17 real edits (mind the `#define` alias block at
  `gui_selftest.c:654-667`).
- ctest delta: 0. Frozen oracle tests must not change.

### P5 — S4': host owner boundary closure (no merge)

- Close the 7-8 `gui_project.c` → `binding.queue` bypass sites with thin
  `gui_host_binding_*` ingress functions (~40 LOC); the P2 rule enforces it.
- Undo/Redo/Save/SaveAs/invalidate move onto the host owner (binding), which
  owns admission per spec §6.1. Client stays per spec §9.
- `gui_session_client_is_attached()` predicate replaces the ~14
  null-probe borrows.
- Terminality poll-DTO cleanup inside the queue (observation is the authority).
- ctest delta: 0 (queue tests keep their entry points).

### P6 — S5': observation cleanup

- Delete `tp_session_snapshot_recovery_available`/`_recovery_health_query`
  and the snapshot `recovery_health` field (zero production callers; the
  observation is the single fresh source). Update `test_session.c:2002-2011`.
- `tp_session_recovery_health.generation` composed from BOTH counters
  (model + session owner) in `tp_session`.
- Single request-id reservation site.
- `tp_session_job_attach_internal` / `..._begin_internal` KEPT (they carry
  USA-07 reverse/superseded coverage unreachable via `start_internal`), but
  declarations move behind `TP_ENABLE_TEST_SEAMS` in an internal header +
  boundary rule that no production TU references them.
- New test: observing session B with session A's token forces resync
  (proves the existing host-side instance-generation check).
- ctest delta: 0.

### P7 — S7: test consolidation

- Split `test_gui_action_trace.c` → `_draft` (28) / `_refresh` (10) /
  `_job` (job+lifecycle, 20) + shared fixture `.c/.h`; distinct
  `TP_GUI_TRACE_TEST_DIR` per target; each `main()` keeps worker dispatch.
  Update the two literal-path lists in the checker.
- Move `test_canvas_buffer_readiness_requires_every_gpu_handle` to
  `test_gui_view.c`.
- `/* USA-nn */` tags above owning tests + grep gate (every id 01-32 present).
- Delete only: duplicated integration fixtures with a named pure-reducer
  successor. The manifest self-test and cross-product oracles stay (rule 3).
- ctest delta: +2 (148→150 debug, 147→149 release).

### P8 — S8: docs

- Spec: §13/USA-18 → single draft owner reality; USA-21 vs §12.4 — quote both,
  fix only if a real contradiction is demonstrated; file-based artifact
  handoff reconciled with master-spec §10.4 ("transient private handoff, not
  a cache"); structural-intent receipt-free contract; §12.4 already-green table.
- Old refactor plan: mark superseded items; this plan gets the ledger.

## Deferred (explicitly out of Track S)

- Unifying the four cancel representations (host flag / pipe byte /
  `tp_cancel_token` / `out_cancelled`) — best follow-up packet.
- Export partial-output staging (`tp_fs_replace` per export file).
- Pending-map capacity trim after replay deletion.
- In-process builder behind an upstream fallible builder API (master spec
  already names the exit condition).
