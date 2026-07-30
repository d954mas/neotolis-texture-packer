# UI/Session Simplification Plan (Track S)

**Status:** complete — P1..P8 landed on `codex/ui-session-architecture-refactor`
(`5d3a675`, `6db159a`, `032ba50`, `1a83549`, `d914b50`, `c2033ba`, `886f482`,
plus this docs commit). Each packet below carries a `Landed:` line recording the
commit and its deviations from the plan text above it. The plan text is kept as
written; deviations are recorded, not retconned.
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

**Landed:** `5d3a675` — 148/148 debug, 147/147 release (delta 0: the new
assertions landed inside existing ctests). The atlas-selection fallback item is
a deliberate BEHAVIOUR change, not only a fix: undo-deleting the selected atlas
now CLEARS the selection instead of retargeting it to index 0, which is the
`USA-24` "preserve or explicitly clear" rule (`gui_rows.c` reconcile; adoption
moved to the explicit `gui_view_adopt_default_atlas`). Deviations: transaction-id resolution
stays *below* the admission/pin gates — filling the terminal receipt on every
non-OK return made the move unnecessary, and hoisting it would have reserved an
ID for calls that never reach admission; the three CLI `NTPACKER_TEST_*` hooks
are gated by **per-binary** macros (`NTPACKER_CLI_INSPECT_FAULT_SEAM`,
`NTPACKER_CLI_PACK_ARENA_FAULT_SEAM`) rather than `TP_ENABLE_TEST_SEAMS`, so
production `ntpacker` has no environment-controlled behaviour at all.

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

**Landed:** `6db159a` — 149/149 debug, 148/148 release (+1: the
`tp_architecture_negative_unclassified_view` fixture, mutation-tested).
Deviations: role classification is a **declared file list with bidirectional
disk↔list guards**, not directory inference — adding or renaming an
unclassified `gui_view_*` TU now fails the check; intentional deletions live in
an `_arch_deleted_files` registry so `_arch_assert_absent` can fail closed; the
`gui_host_queue` carve-out was replaced by two direct invariants (struct-storage
scan + `gui_host_queue_*` containment sweep); **zero ratchet entries were
pruned** — no entry had both its guarded symbol and its file gone.

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

**Landed:** `032ba50` — 149/149 debug, 148/148 release (delta 0). Wire protocol
version 2. Five deviations: (1) `tp_pack_cancellable` was not left untouched —
`tp_pack_cancellable_observed` is reimplemented on top of the new
`tp_pack_produce_observed` seam (Export/CLI behaviour unchanged); (2) the frame
cap did not go away — `TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES` was kept and lowered
272 MiB → **80 MiB** (64 MiB project JSON + 16 MiB), and the stream cap stays
derived from it (+16 MiB); (3) `artifact_size` is kept on the wire as a **u64
validation input**, checked against `tp_fs_stat` rather than replaced by it;
(4) cancel relabelling was single-sited into `job_publish_response` instead of
being fixed per terminal path (this is what fixed cancelled-Pack reporting
`crashed`); (5) `tp_job_worker__test_*` had to be fenced behind
`TP_ENABLE_TEST_SEAMS` with the worker main TU compiled into the transport test
target, so the racy cancel test could be fixed in place as planned.

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

**Landed:** `1a83549` — 149/149 debug, 148/148 release (delta 0); net **−232**
production LOC (the originally recorded −496 counted a 179-line region MOVE into
`gui_actions_structural.c` as a deletion; the corrected figure counts production
sources only, moves excluded);
the five frozen oracle tests are byte-identical. Deviations: **three**
default-less switches remain, not two (payload write, snapshot read, and the
draft-value read all need `-Wswitch` exhaustiveness); the descriptor table is
**29 rows** (the "25 rows" first recorded here predated the four slice9 slots
being spelled out as their own rows); 11 forwarders were deleted rather than 6-8, but **four thin
submit-ingress functions were kept on purpose** — `gui_project_submit_text`,
`_atlas_settings`, `_sprite_settings`, `_animation_settings` — because they are
the family-level payload boundary the parity harness binds to, not per-field
pass-throughs. The one sanctioned behaviour change is the parity corpus moving
to `TP_STATUS_DUPLICATE_ID`; USA-12's named successor is the core retained-ID
test plus `test_retained_id_retry_commits_once_and_returns_duplicate_result`.

### P5 — S4': host owner boundary closure (no merge)

- Close the 7-8 `gui_project.c` → `binding.queue` bypass sites with thin
  `gui_host_binding_*` ingress functions (~40 LOC); the P2 rule enforces it.
- Undo/Redo/Save/SaveAs/invalidate move onto the host owner (binding), which
  owns admission per spec §6.1. Client stays per spec §9.
- `gui_session_client_is_attached()` predicate replaces the ~14
  null-probe borrows.
- Terminality poll-DTO cleanup inside the queue (observation is the authority).
- ctest delta: 0 (queue tests keep their entry points).

**Landed:** `d914b50` — 149/149 debug, 148/148 release (delta 0). Deviations:
the ingress cost **+244 LOC, not ~40** — re-homing Undo/Redo/Save/SaveAs/
invalidate onto the host owner needs nine command functions plus four
capability queries (`can_undo`/`can_redo`/`undo_depth`/`redo_depth`), because
the depth/capability reads borrow the session exactly like the commands do; the
P2 rule alone did not express this, so a new **`A2d single host command owner`
sweep** was added (`cmake/check_architecture_boundaries.cmake`) banning
`tp_session_undo|redo|save|save_as|invalidate_sources|can_undo|can_redo|
undo_depth|redo_depth` in every GUI TU except `gui_host_binding.c`. The
`is_attached()` predicate replaced 13 null-probe borrows (not ~14) and cut the
borrow-owner list 5 → 3.

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

**Landed:** `c2033ba` — 149/149 debug, 148/148 release (delta 0). Deviations:
the planned cross-session test found that a token is a **counter cut, not a
session identity** — a token from a session that did work sits in a fresh
session's future and correctly forces resync, but two *untouched* sessions mint
equal **all-zero** tokens, so crossing one between them reads as "nothing
changed". That is documented, not patched: embedding a session id in the token
was rejected (wider DTO, extra comparison per poll) in favour of the host-owned
rule, which the paired GUI test now pins — `gui_session_client_prepare` observes
every candidate with `after == NULL`, so **attachment always observes from
scratch**. Deleting the snapshot `recovery_health` field also cleaned the
detached `tp_session_snapshot_load` path, which had been hand-seeding a
`recovery_degraded` notice on a snapshot that has no session behind it.

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

**Landed:** `886f482` — 151/151 debug, 150/150 release (+2 as planned, on the
149/148 base the earlier packets had already reached). Deviations: the split is
`_draft` **25** / `_refresh` **10** / `_job` **21** — **56** tests across the
three targets, plus the one test MOVED to `test_gui_view.c`, all bodies
byte-identical incl. the five frozen oracles (the "57" first recorded here
double-counted the moved test); **zero deletions** — no
duplicated integration fixture met the named-successor rule, because each facade
test exercises a path the pure reducer table cannot reach; USA tagging landed as
**15 full and 17 partial** coverage claims, each partial carrying its named
limit in the tag comment, enforced by a new `check_boundaries.sh` R22 grep gate
with self-tests. That tally has since been re-audited to **13 full / 18 partial
/ 1 build-gate-owned**: `USA-11`, `USA-12` and `USA-32` were overclaimed as full
and now name their limits, `USA-18` is full because spec §16 pre-authorizes its
reducer-level proof, and `USA-25` moved to R22's build-gate owner class — its
proof IS the checker, so no test can own it.

### P8 — S8: docs

- Spec: §13/USA-18 → single draft owner reality; USA-21 vs §12.4 — quote both,
  fix only if a real contradiction is demonstrated; file-based artifact
  handoff reconciled with master-spec §10.4 ("transient private handoff, not
  a cache"); structural-intent receipt-free contract; §12.4 already-green table.
- Old refactor plan: mark superseded items; this plan gets the ledger.

**Landed:** this commit — docs only, no ctest/boundary delta (151/151 debug,
150/150 release unchanged). Deviations: **USA-21 vs §12.4 is not a
contradiction** — §8.3/USA-21 answer "does an arriving revision-changing event
conflict an active draft" while §12.4 answers "what happens when the local user
triggers the outer action while a draft is active". Both texts are quoted in the
report; the only edit is one clarifying sentence under §12.4 naming the two
axes, since the local Undo/Redo trigger is blocked-with-choice and never reaches
the conflict path — USA-21's Undo/Redo half is exercised by an Undo/Redo
admitted from another view or controller. Two further amendments beyond the
packet text: master-spec §14.1 gains the worker mode (argv flag for worker mode,
then frame magic selects job vs build service), and §10.6 records wire v2 with
the path + u64 size terminal frame.

## Phase 2 ledger (post-review packets)

**S9 — merged fresh-review + Codex findings.** Landed: `c58d21a`. Artifact
path containment, cancelled-after-success orphan deletion, host-pid `req-`
keying, 2 GiB artifact cap, `elapsed_ms` on synthesized terminals, real
cancel tests (falsification-tested), DRAINING wedge, forced shutdown chain,
observation in the (now consecutive) shutdown budget, `--shot` rendered
counter, descriptor-row oracle sibling, per-site debt allowances, R22 bound
to owning tests + gate-owner class, seam fence verifies `#ifdef` placement.
Deviation: `submitted_revision` kept (spec-normative).

**S10 — cancel unification 9 forms → 5 + two reserves.** `tp_cancel_token`
is the only library cancel type (`tp_pack_cancel_poll` deleted, adapter
gone); cancellation is `TP_STATUS_CANCELLED` everywhere (`out_cancelled`
and the OK+NULL-result convention deleted — this supersedes the
`tp_pack_produce_observed(..., out_cancelled, ...)` signature in the P3
packet text above; the cancelled-pack-inside-export latent hole is closed
and asserted via `report.pack_failed`); `tp_cancel_source` carries the
latched reason (REQUESTED/CONTROL_LOST) replacing the worker's two bools;
five physical forms named in `tp_cancel.h` with their forcing boundaries.
Reserves: pending-map capacity derived from
`TP_SESSION_OBSERVATION_EVENT_CAPACITY`; atomic export publication
(`tp_fs_write_file_atomic`, three exporter sites, old file survives a
failed replace). Production net +124 (+94 is the named `tp_cancel_source`
module; the unification proper is −40).

**S13 — one checker launch mechanism + CMake target boilerplate.** Both boundary
gates are now ctests: `scripts/check_boundaries.sh` is registered as
`tp_boundaries_grep` (labelled `architecture`, skipped with a STATUS notice where
`bash` is absent) and its bespoke CI step is gone, so `ctest --preset <p>` is the
single entry point on all three platforms. The CMake checker's rule ids moved to
the `A` namespace (`A1a`..`A5`, `A6` for the job-owner seam fence) so `R1..R22`
means the grep gate and nothing else. `tp_add_test()` / `tp_add_gui_test()` own
the per-target boilerplate (executable, links, `-U_DLL`, warning/sanitizer flags,
include dirs, ctest registration); all 70 packer and 13 GUI test targets are
declared through them with test names byte-identical to before. The non-gating LOC
report (AGENTS.md Simplification Policy) stays visible through the existing
`report_loc_inventory` build target — `cmake --build --preset <p> --target
report_loc_inventory`.

**S14 — shipping-binary hygiene + public-surface trim.** New CMake option
`NTPACKER_GUI_DEV_SEAMS` (default OFF, same shape as `NTPACKER_GUI_SELFTEST`:
option + gated sources + gated call sites) takes the dev-only seams out of the
shipped GUI: `gui_shot.c` (`--shot`/`--shot-stale`/`--shot-packing`) and
`gui_bench.c` (`--bench-perf`) are conditional `target_sources`, `nt_fpng` is
linked only with them, and `gui_shot.h`/`gui_bench.h` carry `static inline`
no-op fallbacks so every `main()`/`frame()` call site folds away instead of
sprouting an `#ifdef`. Also gated: `main()`'s `--auto-pack` (`auto_pack_tick` +
its argv branch) and `--selftest-crash` branches, `gui_crash_selftest` (the
branch's sole callee), and `gui_pack_debug_force_busy` (the only writer of the
synthetic busy state). ~890 LOC and 56 KiB leave the release exe, which now
contains none of the strings `--shot`, `bench-perf`, `--auto-pack`,
`--selftest-crash`, `SHOT-BOUNDS`. `NTPACKER_GUI_SELFTEST=ON` implies the dev
seams (the selftest oracle drives `gui_pack_debug_force_busy`), so the two flags
cannot drift. `native-debug`/`native-tests-debug` set it ON; `native-release`
pins it OFF and CI's `perf-probes` job overrides with
`-DNTPACKER_GUI_DEV_SEAMS=ON` on the command line — a job-local flag, so
`release.yml`, which uses the bare preset, still ships a seam-free binary.
`NTPACKER_GUI_HEADLESS` untouched.

Public-surface trim: `tp_diff.h`, `tp_name_map.h`, `tp_pack_read.h`, and
`tp_project_lease.h` moved `packer/include/tp_core/` → `packer/src/` (content
byte-identical, names kept — they are not `*_internal.h`, so the R18 registry
needs no row). Zero `apps/` includers each; the 33 include sites in
`packer/src`/`packer/tests` drop the `tp_core/` prefix and four test targets
(`name_map`, `pack_read`, `export_json`, `raw_ownership`) gained `PRIVATE_SRC`.
`tp_selector.h` (spec §5.4, future MCP/Dev API), `tp_cancel.h` (included by
public `tp_pack.h`/`tp_input.h`/`tp_scan.h`/`tp_export_run.h`) and
`tp_pack_result_cache.h` (owner decision pending) stay public.

Reserve landed: request-id exhaustion is now executable on both sides —
`TP_ENABLE_TEST_SEAMS`-gated `gui_host_queue__test_set_next_request_id` and
`tp_session_job_observation__test_set_next_request_id` position the monotonic
counter, and one Unity case each asserts the `UINT64_MAX` branch returns a
structured rejection without breaking the FSM (the queue stays OPEN, unstaged
and drainable and admits work again; the session publishes nothing, leaves the
refused job unreserved, and admits + observes it once ids are available).

**S15 — one string builder, one sanitization policy.** Three byte-similar JSON
writers became one. `packer/src/tp_sb.h` moved to `packer/include/tp_core/tp_sb.h`
(the CLI needs it and `apps/` must not reach into `packer/src`; it is not an
`*_internal.h`, so the R18 registry needs no row). `tp_project_write.c`'s shadow
`typedef struct tp_sb` plus its nine same-named statics are gone, and
`apps/cli/cli_out.c`'s `cli_sb` machinery with them — `cli_out.h`'s "revisited in
B3" note is discharged. The writer-specific state the project writer used to keep
inside its builder (`absolute_sources`, `path_context`) is now a two-field
`tp_write_ctx` passed beside `tp_sb`, so the shared builder stays one general
writer; its `too_large` flag is the shared `limit_exceeded`, and its allocation /
peak-capacity probes ride the shared `allocation_count` hook plus a final
`cap == peak` read.

Sanitization policy (owner decision): invalid UTF-8 in machine OUTPUT is replaced
with U+FFFD so an emitted document is always decodable, and that behavior — ported
verbatim from `cli_sb_json_str` — is now THE behavior of `tp_sb_json_string`. It
therefore covers the CLI `--json` payloads (unchanged), the json-neotolis exporter,
and the operation/transaction encoders. The persisted round-trip path is the
deliberate exception: every string `.ntpacker_project` emits is UTF-8-validated by
`tp_project_validate_canonical` before the first byte, so `tp_project_write.c`
routes them through a local `tp_emit_json_string()` that `NT_ASSERT`s validity
instead of laundering corruption into the user's file. The one string with no
upstream admission — a checkpoint's absolute source path, joined against an
OS-supplied base dir — is refused as a structured `OUT_OF_BOUNDS` there rather
than reaching the assert. Defold's `pb_string` keeps its own escaper: it writes
protobuf text with octal escapes, not JSON.

Two divergences were reconciled rather than preserved: the shared builder reads
`limit == 0` as "unlimited" (a documented `tp_encode_internal.h` contract) while
the project writer read it as a hard zero budget, so `tp_write_begin()` poisons
the builder up front for a zero cap; and the shared `oom`-vs-`limit_exceeded`
classification of a `SIZE_MAX`-scale length overflow differs from the old
`too_large`, in a branch unreachable under any real byte limit. `tp_error.h`'s
twin 43-case `tp_status_str` / `tp_status_id` switches collapsed into one
`TP_STATUS_LIST` X-macro (prose and machine token on one row, both switches
generated, still `default`-less so `-Wswitch` catches a new enum value); all 86
strings verified byte-identical against the pre-refactor header.

**S16 — one deferred-intent queue.** The GUI stored "do this at the next
between-frame boundary" FOUR ways: 17 mutable extern globals in
`gui_actions.h`, 17 `s_actions.pending_*` fields, a `target_edit_intent[]`
array and an `animation_edit_intent[]` array. All four are now one
`gui_intent {kind; union payload;}` in one growable queue with one
`gui_actions__intent_push()`, one default-less `intent_execute()` switch
(-Wswitch exhaustiveness where ten independent `if(flag)` blocks used to be),
and one exhaustive `intent_payload_dispose()` that owns every heap payload.
`gui_actions_structural.c` became `gui_actions_intents.c` — the queue's sole
owner; `gui_actions__apply_structural_edits`, `__apply_file_dialogs`,
`__apply_pack_requests` and `__drain_edits` collapse into
`gui_actions__intent_drain(phase)`.

Drain order is preserved exactly and is now written down once: the
`gui_intent_kind` enum IS the order, declared as a transcription of the legacy
sequence (animation array → target array → dialog if-sequence → structural
if-sequence → refresh → pack if-sequence). The two families that were arrays
share one drain step each, because inside those arrays the legacy drain ran in
ENQUEUE order across kinds; every other kind was a single boolean slot, so kind
order is the legacy order and a repeat request coalesces into the one slot the
way setting a bool twice did. `gui_actions__rebase_deferred_edits` still bumps
`expected_revision` N→N+1 for those two families only.

Extern globals 17 → 0: every view writes through a `gui_request_*()` function
(`save`, `save_as`, `add_files`, `add_folder`, `add_atlas`, `refresh`, `pack`,
`export`, `remove_atlas`, `remove_source`, `preview_target` join the seven that
already existed). That is what makes the boundary enforceable, so the CMake
checker gains rule **A7 `VIEW_ACTION_STATE`**: no `s_pending_`/`s_actions`
token in any `gui_view_*.c`, zero debt paths, with a negative fixture
(`cmake/fixtures/architecture_boundaries/view_action_state`) and a
falsification test on both halves of the token set.

Modal-vs-intent split: `pending_lifecycle_request` and
`recovery_pending_row`/`_action` stayed OUT of the queue. They are not "run
this later" requests but modal FSM state — each is consumed and re-armed by its
own modal (the unsaved-changes confirm flow re-arms the lifecycle request
through `confirm_continue`; the recovery modal's row index is only meaningful
while its list is open), each carries a NONE/-1 sentinel, and neither was part
of the `clear_pending` reset set. Browse-target IS an intent: the dialog is the
executor, not the state.

ctest 152 → **153** debug / 151 → **152** release (+1 = the new negative
fixture). Deviation: production LOC went **+199**, not the −150..−250 the packet
projected. The mechanisms it replaced were terser than an explicit tagged union
because they were untyped: the queue costs one 24-line enum with its 18-line
order contract, a 45-line union, and a 20-case exhaustive destroy switch, and it
absorbs ~376 lines from five TUs into 507. Per AGENTS.md the measurement is
inventory, not a gate; the structural result (4 mechanisms → 1, 17 globals → 0,
10 sequential ifs → 1 exhaustive switch, one new enforceable checker rule) is
the deliverable.

**S17 — core field registry (master spec §6).** One scalar knob used to be
enumerated in parallel across seven translation units. `tp_op_catalog.c` now
owns one const table per field-presence SET family — atlas (10 rows), sprite
(11), animation (4), target (3) — each row `{mask bit, wire key, value type,
offsetof in the op payload, offsetof in the project record, clear token, group
label, reset value}`. `tp_op_field_rows(family, &count)` is public
(`tp_operation.h`): the machine-readable argument schema §6 requires of the
operation engine, indexable by a palette or MCP client. Three private walkers —
`tp_op__fields_apply` / `__fields_clear` / `__fields_match` — each carry exactly
ONE default-less `switch` over `tp_field_type`, so a new value type is a
`-Wswitch` error at every codec instead of a silent drop. NO function-pointer
columns.

Converted consumers: `tp_op_apply.c` (atlas + animation copy blocks,
`sprite_apply_set`, `sprite_apply_clear`, and target.set's scalar half → walker
calls, −94 LOC), `tp_op_encode.c` (four field emitters + the sprite clear-token
loop → one `push_fields`), `tp_txn_lower.c` (four JSON lowerers → one
`lower_fields` that derives the presence mask and enforces grouped arity),
`tp_txn_apply.c` (the ten-line `TP_SETTING_DIFF` macro → one `__fields_match`
call), `apps/cli` (`fill_knob` and `fill_anim_settings` → one shared
`cli_fill_registry_field`, and the "known: ..." hints are now GENERATED from the
rows, so the CLI cannot advertise a stale vocabulary). The closed per-op
vocabulary `k_fields[]` also stopped spelling the payload keys a second time: a
SET row carries its family and `tp_op_field_allowed` consults the registry.
`k_sprite_clear_fields` is gone — the `clear_token` column IS that vocabulary.

Wire/JSON is byte-identical and every golden file is untouched: the canonical
encoder sorts keys ascending (`emit_object`), so push order was never a wire
contract; the JSON lowering keeps its row order because order fixes WHICH value
fault is reported first, and the grouped-arity messages ("origin_x and origin_y
must be provided together", "slice9_l/r/t/b must be provided together") are
regenerated verbatim from the `group` column. Every CLI usage message is
byte-identical, verified by hand against the pre-change forms.

Stayed hand-written, by design: all four `tp_op_validate_*` families (ranges are
cross-field — they run against EFFECTIVE values folded from atlas + op, iterate
the atlas's sprites, and carry genuinely bespoke prose such as "extrude > 0
requires shape RECT", "ov_allow_rotate = %d must be 0 (force no-rotate) or -1
(inherit)", and the raw-vs-effective message pairs with different field
attribution — so a range column would be dead data); target.set's string swap in
apply (stage-then-commit dup must precede any mutation, and no function-pointer
column is allowed); `cli_mutate_sprite.c` (the `inherit` sentinel, CSV group
parsing, and a `rename` that lowers to a DIFFERENT op); `cli_mutate_target.c`
(its CLI keys `exporter`/`out` are not the wire keys); `anim set playback` (an
enum parse accepting index OR mode name); and `tp_project_parse.c` /
`tp_project_write.c` (schema v5 is pinned by byte-contract tests and the §6
requirement is on the operation engine, not the file codec). The GUI
`k_draft_rows` table stays separate as specified — it already references the same
mask constants.

Battery: 153/153 debug, 152/152 release, zero warnings from our targets (the 13
in a clean build are pre-existing `cgltf.h` deprecations in the vendored engine
dep), `check_boundaries.sh` clean, standalone CMake checker clean, zero
golden-data files changed. Deviation: production LOC went **+53**, not the
−200..−350 the packet projected — the same shape as S16. The parallel
enumerations removed ~165 lines; the typed mechanism that replaces them costs
~220 (65 table + 100 walkers + 46 lines of new PUBLIC schema surface that §6
requires and that replaced nothing). Per AGENTS.md the measurement is inventory,
not a gate; the deliverable is that a new field is now one table row plus a GUI
row plus a widget, where it used to be seven edits in seven files.

**S18 — `tp_pack_result_cache` wired into the GUI Pack path.** Owner decision:
«подключить». The module was fully built and tested with ZERO production callers;
the GUI meanwhile kept up to 64 Pack results resident forever, one per atlas, each
pinning a whole page arena through its session receipt. Native results now live in
one session-lifetime store behind `gui_pack`: the atlas on screen is the store's
PINNED active result, every other packed atlas is an INACTIVE entry in a
byte-budget LRU, switching away demotes instead of dropping, and switching back is
a store hit that never repacks.

**(a) Budget.** Master spec §10.4 names none — "concrete budgets and compression
details are implementation policy" — so `GUI_PACK_RESULT_BUDGET_BYTES` in
`gui_pack.c` is that policy and the only place it is written down: **256 MiB**,
measured in RAW RGBA8 page bytes. At the owner's calibration scale (30 atlases /
5000 sprites, §61.1) a 2048² page is 16 MiB, so the budget keeps ~16 recently
visited atlas pages warm — nobody working across a handful of atlases ever
repacks — while a pathological 4096² page (64 MiB) still leaves room for four.
Retaining everything, the pre-S18 behaviour, is 30× that at the same scale with no
ceiling. The true resident ceiling is budget + the active pin + the
highest-sequence entry, both exempt from eviction by the store contract
(decision 0004).

**(b) What is cached: the retained result owner, not bytes, not thumbnails.**
This is the packet's one real design decision, because the module as built stores
the serialized `.ntpack` artifact and ADOPTS a raw `tp_arena` — and the GUI has
neither. The host deletes the worker artifact the moment it inflates it (§10.6,
packet P3/S9: "transient private handoff, not a cache"), and the Pack arena
belongs to the refcounted `tp_live_job` receipt, not to the frontend. Feeding the
module its serialized shape would have meant retaining the artifact bytes on every
job (a memory regression in a lifecycle S9 deliberately tightened, plus 2× the
page bytes for the active result) *and* would still not have given the store an
arena it may destroy. So `tp_pack_result_cache` gained a second, additive entry
point instead: **`tp_pack_result_cache_store_retained(...,  result,
retained_bytes, pin_owner, pin_release, err)`**, plus
`tp_pack_result_cache_forget(hash)`. A retained-pin entry keeps its decompressed
`tp_result` for its whole life and is released ONLY through the caller's hook —
the GUI passes `tp_session_job_result::_owner` and a hook that calls
`tp_session_job_result_destroy`, so the Pack arena is still destroyed exactly once
and the store never touches an arena it does not own. Serialized and retained
entries mix freely in one store and one budget; every existing serialized
behaviour (inflate on hit, corrupt-entry containment, zero-budget max-sequence
exemption) is unchanged and its eight original cases are untouched. The trade the
retained flavour makes is a larger inactive footprint (decompressed, not the
§10.4 compressed/serialized representation) for zero restore work and STABLE
result pointers — switching back hands back literally the same `tp_result *`.
**Thumbnails (§10.4/§52.3/§61.1) are explicitly NOT in this packet**: cheap
downscaled page thumbnails need new downscale/render code and a second budget
class, and belong in the Project-overview canvas packet that will consume them.
Recorded as follow-up, not silently dropped.

**(c) Freshness.** Restore is freshness-NEUTRAL by construction: residency touches
no input token, no `pack_input_hash`, and not the `preview_stale` bit, and the
`tp_session_pack_job_result` freshness verdict is consumed at poll time before
publication, never re-read from a slot. A result that was out of date when the
atlas left the screen is still out of date when it comes back, pinned by
`test_restored_pack_result_keeps_its_freshness_verdict`. Freshness is still the
project-wide `preview_stale` boolean; retiring it for hash-keyed per-atlas
current/stale is master-spec-implementation-plan item 5 and stays out of scope.

Store key is the canonical `pack_input_hash`, which is what makes a later
Undo/Redo probe (`_contains`) find the exact result. A repack under a NEW hash
leaves the previous entry in the LRU on purpose — that IS the warm Undo/Redo
cache, and the budget bounds it. Two edges are handled explicitly: a NIL hash
(core could not read a source) would be a shared key, so such a result is filed
under the atlas' own stable ID instead; and because the hash is content-addressed,
two atlases whose result would be byte-identical legitimately share one entry, so
a slot only forgets an entry no other live slot is still bound to.

Presentation: `gui_pack`'s per-atlas slots stopped owning result memory (they are
now the atlas→cache-key binding plus the version consumers watch), and the
canonical sprite index moved from per-slot to ONE index following the one resident
result — an atlas switch rebuilds it once instead of every atlas carrying its own.
No view changed, no call site changed, and no observation coupling was added: the
store, the budget and residency are invisible above `gui_pack.h`. The one contract
a caller must know is documented there — an EVICTED result reads as "not packed"
(NULL, version 0) exactly like one that was never packed, which is the §10.4 cache
miss: the preview is out of date, the user runs Pack, nothing auto-packs.

Battery: 153/153 debug, 152/152 release (delta 0 — the new cases are Unity cases
inside `tp_pack_result_cache` and `tp_gui_canonical_identity`), zero warnings from
every TU that includes the changed headers, `check_boundaries.sh` clean,
standalone CMake checker clean, five frozen oracles byte-identical. Production LOC
**+391** (+124 store, +50 public header, +217 GUI), tests +345. New cases: five in
`test_pack_result_cache.c` (demote keeps the pin and re-hands the same pointer;
LRU eviction releases the owner EXACTLY once and destroy does not re-release, via
the release-counting pattern `test_session_job_observation` uses; re-store and
forget each release the superseded pin once; a rejected store leaves the pin with
the caller; retained and serialized entries share one budget) and three in
`test_gui_canonical_identity.c` (switch away → still held, inactive, budget-charged;
switch back → identical result pointer with no Pack started; evicted → reads as
unpacked and still never auto-packs; plus the freshness case above). Exactly-once
release is asserted at the store level, where releases can be counted — a GUI test
cannot fabricate a session receipt, so it asserts the eviction and the miss
presentation instead.

Deviation: the packet was framed as wiring the module in unchanged; it needed the
additive retained-pin entry point above, because the module's serialized-plus-raw-
arena contract is unreachable from a frontend. The alternative (plumbing the
worker artifact bytes out of `tp_job.c` and retaining them per job) was rejected
for the memory and lifecycle reasons stated in (b); if a memory serializer for
`tp_result` ever lands, an inactive GUI entry can migrate to the serialized
flavour with no change above `gui_pack`.

**S19 — the last environment variable leaves the shipping binary.** Owner
decision: headless is a CI *build* concern, not a runtime one, so the
`NTPACKER_GUI_HEADLESS` environment variable is **deleted**. This supersedes the
P1 packet line "`NTPACKER_GUI_HEADLESS` is a deployment flag and stays" and the
S14 note "`NTPACKER_GUI_HEADLESS` untouched" above. New CMake option
`NTPACKER_GUI_HEADLESS_CI` (default OFF, same shape as `NTPACKER_GUI_DEV_SEAMS`:
option + compile definition on `ntpacker-gui`) turns the four former `getenv`
sites into `#ifdef`s: `gui_selftest.c` (jump straight to the GL-independent
phase 16, skipping visual phases 1-15 — xvfb+llvmpipe never brings the engine's
materials/shaders/font atlas to "ready"), `gui_crash.c` ×2 (`gui_crash_install`
and the `--selftest-crash` dev seam — ASan/UBSan install their OWN
SIGSEGV/SIGABRT handlers on CI and overriding them would mask sanitizer
reports), `gui_log_file.c` (no writable app-data dir needed), and `gui_bench.c`
(`--bench-perf` frame timing has nothing to measure without GL). `<stdlib.h>`
dropped from `gui_log_file.c`/`gui_bench.c` — `getenv` was its only user there.

The base preset pins the flag OFF (S20: a reused build dir must not keep a stray
ON in the CMake cache), so a local `native-tests-debug` selftest keeps the hard
visual phases on a real GPU; CI passes `-DNTPACKER_GUI_HEADLESS_CI=ON` on the
configure line of the two jobs that need it (`gui-selftest`, and `perf-probes`
alongside its existing `-DNTPACKER_GUI_DEV_SEAMS=ON`) — job-local overrides, so
`release.yml`, which uses the bare preset, is unaffected. Both `NTPACKER_GUI_HEADLESS`
env lines are gone from `ci.yml`.

Battery: 153/153 debug (visual phases confirmed still running locally),
152/152 release, zero warnings from our targets with the flag both OFF and ON,
`check_boundaries.sh` clean, standalone CMake checker clean. A byte scan of the
release `ntpacker-gui.exe` finds zero occurrences of the string
`NTPACKER_GUI_HEADLESS`, and the CI combo (`native-tests-debug` +
`-DNTPACKER_GUI_HEADLESS_CI=ON`) was configured into a scratch build dir and its
`ntpacker_gui_selftest` run green, proving the CI path without CI. The shipped
`ntpacker-gui` now has **zero** environment-controlled behaviour, matching
`ntpacker` (S-P1, per-binary seam macros).

**S20 — Codex round-2 review triage ("merge with fixes" on `be71372..72fbec3`).**
Three independent verification agents re-derived every finding from code before
anything was fixed. Confirmed and fixed (four concurrent zones):

- *Resident switch use-after-free (P1, `gui_pack.c`).* `authoritative()` can
  evict — and free — the outgoing resident mid-switch; an index-build OOM then
  left `s_resident_result` dangling for the fast path. The resident is now
  cleared BEFORE the store can evict, so every exit leaves the new resident or
  none. New one-shot seam `gui_pack__test_fail_next_ref_index_build` + a
  derived-budget test that makes the switch itself do the eviction (a fixed tiny
  budget frees the victim at publish, never during the switch — measured sizes
  are the only deterministic construction). Red-before/green-after verified.
- *Cache retained the whole live job (P1, `tp_job.c`).* The retained pin held
  `project_json`, the worker-process object (with the encoded request embedding
  the project JSON a second time) and the exited child's OS handles — all
  invisible to the 256 MiB page-byte budget. New `tp_session_job_result_compact`
  frees them at GUI publication; the pinned receipt now retains only the arena
  the budget counts. The review's "serialized inactive representation" was NOT
  adopted — S18's ledger entry already records why retaining artifact bytes was
  rejected; compaction captures the uncounted memory without reversing that.
- *Prepared-candidate session leak on force-close (P1, `gui_session_client.c`).*
  `cancel_prepared` released the observation but not `prepared->session` — the
  only pointer to the candidate after `begin_replace` takes ownership. Cancel
  now owns the whole bundle; the drain-failure double-destroy in
  `gui_host_binding.c` is deleted. First binding-level `force_close` test.
- *Export SET atomicity (P1, `tp_export_*`).* S10 was per-FILE only; a mid-set
  failure left new pages under an old `.tpatlas`. Now per-target two-phase
  publish: a thread-local stage redirect inside `tp_fs_write_file_atomic` sends
  direct children of the output dir into a `.tp-stage-*` sibling (exporters see
  byte-identical inputs — repointing `out_path_base` would change the `.tpatlas`
  `file:` ref, which walks the REAL tree for `game.project`), then a preflighted
  promote loop replaces the enumerated set. Divergence between the declared and
  produced set is now a structured error in both directions. Pinning test:
  mid-set failure keeps every previously published byte. Residual window: a
  `tp_fs_replace` failing inside the promote loop (error names the file); a
  crash can orphan one recognizable `.tp-stage-*` dir (no reaper — follow-up).
- *Worker path buffers (P1-by-contract, `tp_job_worker_main.c`).* Three fixed
  512/544-byte buffers capped work_dir far below the `TP_IDENTITY_PATH_MAX`
  contract the host and proto admit (the host does NOT pre-reject — verified);
  Pack failed where Export worked. Buffers now sized from the contract; the
  comment states the true end-to-end bound (reply path ≤ 4095). Deep-path test
  red-before/green-after; test-harness `fopen` → `tp_fs_fopen` (long-path safe).
- *Orphaned artifact on early adoption failure (P3, `tp_job.c`).* The path is
  stored the moment containment trusts it, so the size-gate failure cleans file
  and request dir; the strdup-OOM branch deletes through the wire path. New
  seam + zero-req-dirs test.
- *Gates (P2, all four sub-fixes).* Bash-less machines now get a fail-closed
  red `tp_boundaries_grep` stub instead of silent non-registration; the
  `#if defined(TP_ENABLE_TEST_SEAMS)` fence pattern is end-anchored (compound
  conditions rejected, negative fixture proves the anchor is load-bearing); a
  `USA-nn` tag is credited only with an in-file `RUN_TEST`/call site (seeded
  self-test: a defined-but-unregistered function now fails R22); the base
  preset pins `NTPACKER_GUI_HEADLESS_CI=OFF` (CI's job-local `-D` wins,
  proven empirically).

Refuted, no change: the field registry "layout leak" (`op_off`/`rec_off`
describe already-public structs; `op_off` is load-bearing for the CLI's
registry-driven parser) and the evicted-slot stale version as a defect (lazy
retire is bounded to ≤1 frame by the frame loop; a 2-line `contains` hardening
was added as polish anyway). Knowingly deferred, recorded not fixed: undo/redo
does not reselect a cached result by CURRENT input hash — identical to pre-S18
behaviour, scoped out by the S18 entry above; its honest completion is the
per-atlas freshness work (master-spec item 5), whose off-UI-thread constraint a
quick undo-time `tp_session_pack_input_hash` probe (a full synchronous decode)
would violate.

**S21 — fresh-review round 3 (`be71372..690bd42`: three zone reviewers, two
adversarial verifiers).** No confirmed P0/P1. The headline GUI claim — canvas
use-after-free via a preview-driven cache eviction — was REFUTED as a live bug
(every interactive atlas switch stops the player, and a demoted entry is the
LRU *survivor*, not the victim), but the protection was ordering luck over an
unstated invariant, and the one reachable divergence state (`intent_add_atlas`)
flipped cache residency twice per frame. Fixed in three zones:

- *A read stops mutating residency (gui).* New `tp_pack_result_cache_peek`
  (no select, no demotion, no LRU touch, no eviction) + `gui_pack_result_peek`;
  the animation player observes through it, so two consumers can no longer
  fight over the single active pin frame after frame. `intent_add_atlas` stops
  the player like every other switch path; a result that was playing and then
  disappeared stops the player with a warning instead of silently re-arming
  the pack hint; `gui_pack.h` now states the real lifetime rule (the mutating
  read, and when its pointer dies). Publish's index-OOM branch reports
  `TP_STATUS_OOM` instead of a PACK_FAIL carrying OK; the action layer owns an
  exit shutdown (queued intent payloads + the preview frame map); the `--shot`
  pack latch is a success latch like its selection sibling.
- *`tp_error` owns its path by value (core).* `file_io.path` is a fixed
  256-byte array, so the latent compact/response-free aliasing and the
  worker's freshness-path strdup/OOM-fallback/free plumbing vanish
  structurally (net deletion; the "No heap" comment on the type is true
  again). Export preflight matches the staging dir against the listed set
  BEFORE the first rename — an unlisted leftover no longer reports "nothing
  published" over a fully republished set. Staging dirs are claimed by an
  atomic exclusive create (`tp_fs_create_dir_exclusive`) — a leftover or
  foreign name is genuinely never adopted, with no check-then-create window. A
  not-stageable output raises `TP_NOTICE_FIELD_SET_ATOMICITY` /
  `TP_NOTICE_REASON_PATH_NOT_STAGEABLE` instead of degrading silently.
- *Gates fail closed harder (tests/cli).* R22 strips block/line comments and
  `#if 0` regions before crediting a `RUN_TEST` call site (self-test seeds
  every stripped form; the dead-call-graph case is a documented known limit)
  and the corpus now sees `apps/*/*_test.c`. The CLI output-OOM fault covers
  every payload builder again via a `TP_SB_ALLOC_FAULT_SEAM` hook compiled
  only into the fault binary's CLI TUs (+4 contract ctests for the previously
  uninjectable branches). USA-25's gate owner carries `partial:` naming the
  allowlisted residue. The wire-offset proto tests discover their field by
  diffing two encodings instead of hardcoding offsets. The host-queue fault
  seam resets in `tearDown`. R1/R2/R3 gained the seeded-violation self-tests
  every other rule already had. Duplicate `tp_obj_key` copies deleted.

Battery: debug 159/159, release 158/158 (clean builds), boundaries OK.

**S22 — fresh-review round 4 (review of S21 itself, `690bd42..879b161`).** Three
fresh zone reviewers; no P0, one P1 (gates), the rest P2. Fixed:

- *R22 comment stripping is string-literal-aware (the P1).* The S21 stripper
  read `/*` inside a string literal (`"%s/*"` — live at two corpus sites) as a
  comment opener and swallowed ~50 lines of live code including `#else`/
  `#endif`, desyncing the dead-region depth. No credit was lost at HEAD
  (verified by an old-vs-new mirror over all 91 corpus files — zero drift), but
  one moved helper away from going red on good code or silently dropping
  owners. The stripper now tracks string/char-literal state (escapes handled;
  literal state is per-line, block-comment state still crosses lines); `#elif
  0` opens a dead region like `#if 0`; three new seeded fixtures pin all of it.
  New R0 corpus guard: `app_srcs` must be non-empty and contain both frontend
  `main.c` sentinels, so an over-matching exclusion can no longer drain every
  apps/-scoped rule silently.
- *Truthful export failure wording + fail-closed leftover scan (core).* Once an
  output bypassed staging (already published per-file, notice raised), later
  preflight failures no longer claim "existing outputs are untouched" — the
  message counts the bypassed outputs. An unreadable staging dir (open failure
  or mid-scan error) now fails the preflight instead of promoting an unverified
  set. Byte-exact (case-sensitive) name matching is now contract text.
- *Producer-side UTF-8 truncation repair (core).* `tp_error_set*` trim a
  trailing incomplete UTF-8 sequence after snprintf/vsnprintf truncation (msg
  and path), so a truncated diagnostic can no longer make the wire validator
  reject the whole worker payload. Outer wire bounds for the two error paths
  now state the real `TP_FILE_IO_PATH_MAX - 1` limit (and the previously
  missing outer check for `error_path_len` was added).
- *Parity vocabulary (core).* `client_parity_replay.c` gained `set_atomicity` /
  `path_not_stageable`, restoring the closed-vocabulary claim vs `cli_pack.c`.
- *Preview OOM retry restored (gui).* Under S21's peek, an index-build OOM
  cached an EMPTY frame map as valid, killing the documented retry. The rebuild
  now bails without caching when the canonical index is unavailable; a genuine
  zero-resolve with the index present still caches. `gui_actions_shutdown` also
  releases the refresh fingerprint and disarms the player (test-state bleed),
  and the headline peek test gained the positive control it lacked (its
  vacuous-pass mode was demonstrated directly before fixing).
- *Test hygiene.* Two S21 proto-test leaks freed (successful decode before the
  corrupting re-decode); the save-io remap assertion pins the public path via a
  deliberately non-canonical input (red-before proven by disabling the remap);
  the one seam-macro pair collapsed to `TP_SB_ALLOC_FAULT_SEAM` alone; the four
  output-OOM ctests assert the exact golden payload instead of a prefix regex.

Known limits recorded, not fixed: `#if 1 ... #else` dead branches and non-zero
`#elif` conditions still read as live (documented in the rule); the unreadable-
staging path has no portable test (Windows cannot make the generated dir
unopenable); fix-4's parity tokens have no reachable emitter yet.

Battery: debug 159/159, release 158/158 (clean builds), boundaries OK.

## Decision records

- **Escape after a deferred gesture commit is accepted as-is.** `frame()` runs
  `apply_pending()` at the between-frame ingress boundary (`main.c`), which
  submits a draft whose gesture already committed, and only later reaches the
  Escape handler. An Escape pressed in the SAME frame as the gesture commit
  therefore discards nothing: by the time it is read there is no active draft.
  This is consistent with spec §12.4, whose Escape row describes an *active*
  draft, and it is deliberate — moving Escape ahead of `apply_pending` would let
  a key press race a transaction that is already in flight, which is exactly the
  stranded-`SUBMITTING` class of bug P1 removed. Recorded, not changed.

## Deferred (explicitly out of Track S)

- Unifying the four cancel representations (host flag / pipe byte /
  `tp_cancel_token` / `out_cancelled`) — best follow-up packet.
- ~~Export partial-output staging (`tp_fs_replace` per export file).~~ Landed in
  S20 (per-target staged set publish).
- Pending-map capacity trim after replay deletion.
- In-process builder behind an upstream fallible builder API (master spec
  already names the exit condition).
