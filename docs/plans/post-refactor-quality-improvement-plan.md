# Post-refactor quality improvement plan

Status: proposed execution plan, 2026-07-19

Normative source: `docs/ntpacker-master-spec.md`

This plan follows the simplification/refactor review. It fixes concrete
correctness and contract defects first, then simplifies recovery around the
owner-approved best-effort policy. It does not reopen the completed
decomposition merely to reduce LOC.

## 1. Locked owner decisions

1. Neotolis owner assertions (`NT_ASSERT`, and `NT_BUILD_ASSERT` at the builder
   boundary) remain enabled in both Debug and Release. `NT_ASSERT_MODE=OFF` and
   libc `assert()` are not acceptable substitutes for required invariants.
2. Recovery is additional insurance, not a database and not a model commit
   gate. An edit, Undo, or Redo remains committed if recovery append/sync fails.
3. The normal recovery representation remains the compact ordered version-4
   TXN/HISTORY diff log. Do not take a periodic full-project snapshot.
4. With a healthy backend, process-crash and power-loss RPO is at most 5
   seconds. Losing a few final edits is acceptable; losing minutes or hours is
   not.
5. Recovery failure is sticky and visible. Stop recording later dependent
   diffs until a fresh checkpoint re-establishes the chain; continue editing.
6. Start with the existing synchronous append **and durability sync**, moved
   after the irreversible model commit. This is simpler and stronger than a
   5-second RPO. Split append/sync or add a queue/timer only if the checked-in
   large-project Release benchmark demonstrates a user-visible latency problem.

## 2. Evidence and current baseline

The repository now contains
`examples/projects/large-synthetic.ntpacker_project`: 100 atlases, 1,000 file
source memberships, 100 targets, and no duplicated image assets. Its generator
and `tp_large_project_contract` pin canonical bytes, schema, structural counts,
source tags, target paths, and resolvable relative assets.

`tp_bench_foundation --project` runs the production mutation, journal,
snapshot, history, and recovery paths over that committed file and reports
p50/p95/p99/max. A short Windows Debug sample (three measured iterations) gave:

| Scenario | p50 | p99/max | Observation |
|---|---:|---:|---|
| one history-enabled normal transaction | 0.521 ms | 0.529 ms | shipped model/history baseline |
| same transaction with current file journal | 1.198 ms | 1.220 ms | 364-byte append; current per-record durability behavior |
| session snapshot | 4.244 ms | 5.202 ms | 1,000 sources |
| recovery of checkpoint + 5 edits | 28.034 ms | 29.086 ms | startup/recovery path, not per-edit UI latency |

These numbers are diagnostic, not release thresholds. Release builds and all
three supported operating systems still need sampling. The first discarded
generator design also showed that constructing this graph through a 1,200-op
history-aware transaction (and then as 100 smaller transactions) exceeded 120
seconds in Debug. That is a separate large-batch performance hotspot, not a
reason to weaken transaction correctness.

The checked project exposed a Save-As identity defect not present in synthetic
in-memory fixtures: saving relative sources into another directory reloads
successfully, but semantic identity can differ because equivalent paths with
`..` are not reduced to one lexical identity form.

## 3. Actionable findings

| ID | Priority | Finding | Primary area | Required outcome |
|---|---|---|---|---|
| Q-01 | P0 | libc `assert()` disappears in Release | `packer/src/tp_project_generation.c` | replace with `NT_ASSERT`; exercise both build types |
| R-01 | P1 | journal durability is currently a transaction/Undo/Redo gate, contrary to the owner decision | `tp_txn_apply.c`, history, session/recovery APIs | publish model commit independently; sticky degraded recovery |
| R-02 | P1 | a mid-stream corrupt journal can be marked adoptable; resolving the recovered prefix can finalize/delete the original | `tp_recovery_scan.c`, claim/resolution lifecycle | recover only the valid prefix, preserve the original, require explicit discard after corruption |
| R-03 | P1 | current comments/tests promise every acknowledged edit and retained ID survives restart | all journal/transaction/session public headers, tests, README, ROADMAP and derived plans | replace every active promise with post-commit recording and the healthy RPO; leave no contradictory executable contract |
| CLI-01 | P1 | CLI `add` stores every source as `folder`, including image files | `apps/cli/cli_mutate_source.c` | default auto-classifies an existing file/directory; a missing/unstatable path requires explicit `--kind file|folder` or fails structurally; test saved JSON and inspect output |
| API-01 | P1 | snapshot path-resolution failures return a raw status without populating `tp_error` context | `tp_session_snapshot_query.c` | every failing public query returns structured context |
| CLI-02 | P1 | “no usable images” / “no enabled targets” is prose only, not a structured notice | `apps/cli/cli_pack.c`, CLI report schema | stable notice ID plus context in JSON and equivalent human text |
| CLI-03 | P1 | mutation verbs reject `--dry-run` although the repository contract requires predictable dry-run for destructive/lossy changes | `apps/cli/main.c`, mutation adapter | apply to a detached candidate, report operations/notices, never save or create files |
| CLI-04 | P1 | `help --json` and `--help --json` emit human text | `apps/cli/main.c`, CLI schemas | valid versioned JSON on stdout for every explicit JSON request |
| PATH-01 | P1 | Save As can change semantic identity for equivalent relative paths containing `..` | project path/semantic/save modules | one canonical lexical identity; Save As and reload preserve semantic identity |
| GUI-01 | P2 | canvas sets `buffers_ready` even if one or more created handles are invalid | `apps/gui/gui_canvas_resources.c` | readiness derives from every required handle; partial construction is safely destroyed/retried |
| TEST-01 | P2 | client parity manifest grants broad COMMON coverage flags without one executable oracle per claimed dimension | `apps/gui/client_parity_manifest.*`, parity tests | coverage is evidence-derived; unsupported cells remain unset and fail the required-coverage audit |
| DOC-01 | P2 | CLI exit code 8 exists in code but help/README/report docs still say 8 is reserved or omit it | `cli_exit.h`, `main.c`, README, CLI report | one frozen table everywhere, including typed file I/O exit 8 |
| BUILD-01 | P2 | `cli_json_check` is the only first-party CLI helper without `nt_set_warning_flags` | `apps/cli/CMakeLists.txt` | all first-party targets use repository warning flags |
| PERF-01 | P2 | large multi-operation transactions are disproportionately slow in Debug | transaction validation/apply/result/history path | profile first; optimize only a demonstrated repeated cost without splitting atomic batches |
| ARCH-01 | P2 | immutable snapshot queries reach through mutable project lookup helpers | snapshot query/project lookup seam | introduce/use const lookup ownership; no mutable alias from immutable DTO API |

## 4. Execution waves

### Wave 0 — contract and measurement foundation

Status: implemented in the current change set.

- Update master spec §7.1, §7.2, §9.3, §22.1, §22.3, §52.5, and decision item
  19 to the best-effort recovery contract.
- Add the committed large project, deterministic generator, contract test,
  LF attributes, packability smoke, example catalog, and release packaging.
- Add `tp_bench_foundation --project`, the journal-backed transaction scenario,
  and p99/max reporting.
- Mark older durable-acknowledgement statements in ROADMAP and the derived
  implementation plan as superseded.

Gate: generator is byte-deterministic across checkout EOL settings; project
loads with all 1,000 assets resolvable; one atlas decodes and dry-run packs both
before and after release staging; benchmark reports `PATH-01` checkpoint
identity drift explicitly and checks replay against the exact checkpoint bytes
rather than silently presenting it as live-identity equality.

### Wave 1 — small independent correctness fixes

Implement Q-01, CLI-01, API-01, CLI-02, CLI-04, GUI-01, DOC-01, and BUILD-01 in
small commits. Each item gets a failing regression first and may be reviewed and
reverted independently.

Required tests:

- Release test that executes the generation retain/release invariant path with
  `NT_ASSERT` active.
- CLI e2e: add an image and a directory, save, inspect with `--json`, and assert
  `file`/`folder` tags; missing path without `--kind` rejects, while explicit
  `--kind file|folder` persists exactly that offline classification.
- CLI JSON tests for both help spellings and all “skipped” pack outcomes.
- snapshot-query unit tests for invalid arguments, missing IDs, path-too-long,
  and path-base failures with non-empty structured error context.
- GPU handle failure injection or a pure readiness helper test; no draw/destroy
  call may receive an invalid “ready” resource.

Gate: targeted Debug and Release tests pass; `git diff --check`; no engine edit.

### Wave 2 — CLI behavioral contracts and parity evidence

Implement CLI-03 and TEST-01.

Mutation dry-run is one-shot and file-oriented. Existing-project verbs load the
project, resolve and apply the exact typed transaction to a private
session/candidate, collect the same validation/notices/result, and destroy the
candidate without Save. `new --dry-run` builds and validates the new candidate,
preflights destination existence exactly as live `new`, but never publishes it.
It must not acquire a writable destination lease, create a journal, change a
project file, or create output paths.

The mutation JSON contract advances version if needed and contains at least:
`schema`, `command`, `dry_run`, `would_change`, `operation_count`,
`revision_before`, `revision_after`, affected/generated structural IDs, and a
structured `notices` array. Generated IDs describe that candidate only and need
not repeat across separate runs. Success and semantic no-op exit 0
(`would_change` distinguishes them); validation/selector/usage failures use the
same exit and structured error as live execution. Save/recovery notices are
absent because neither subsystem runs. Pin `new`, one normal edit, no-op,
validation rejection, selector miss, explicit/generated IDs, and destination
already exists.

Replace parity’s blanket `COMMON` claims with explicit evidence rows. A test
must fail if a manifest bit has no named executable oracle. Do not manufacture
notices/errors for every operation merely to fill a matrix; the required matrix
must reflect meaningful per-family behavior.

Gate: before/after fingerprints and directory listings prove every mutation
dry-run is side-effect free; normal mutation bytes remain unchanged; all JSON
payloads parse through the real checker.

### Wave 3 — recovery simplification/refactor

#### 3A. Separate commit from recording

- Move journal byte/admission failure out of transaction prevalidation.
- Attempt to stage/encode the compact recovery record while the candidate
  exists, but treat recovery-only allocation, codec, or admission failure as
  degradation rather than transaction rejection. Separately stage every
  fallible external side effect. Publish the authoritative
  model/revision/history as the irreversible commit point, complete any required
  infallible/idempotent post-commit side-effect publication, then attempt the
  synchronous journal append+durability sync. The attempt may happen before the
  ordered event/result but cannot change its committed outcome.
- A failed recovery allocation/encode/admission/append/sync never rolls back
  the published state. Atomically set
  `recovery_degraded`, preserve the first structured cause, emit the stable
  `recovery_degraded` state notice (first cause, last durable revision/time,
  sticky/cleared transition), and keep it visible until recovery is healthy.
- Once degraded, do not append later diffs whose base is no longer known. Do not
  repeatedly hit the failing backend on every keystroke.
- Retained transaction IDs remain authoritative in live memory. Journal indexing
  covers only the durable/recoverable prefix.

Side effects must not use recovery as their atomicity mechanism. All fallible
preparation precedes model commit; post-commit publication is infallible or
idempotently recoverable and completes before success is reported. Recovery is
not compensation for either side.

#### 3B. Measure before adding batching

- First run the history-enabled normal and journal-backed transaction benchmark
  after 3A in native-release on Windows/Linux/macOS. If synchronous per-record
  sync has no repeated user-visible stall, keep it and stop: every successful
  record is already durable, so no idle timer is necessary.
- Expose internal health plus last durable revision/time for GUI and future Dev
  API use. Transport-specific JSON mapping may remain deferred; the core notice
  vocabulary from 3A may not.
- Only if measurements reject synchronous sync, write a separate reviewed
  batching design: split append from durability, name a runtime timer/pump or
  writer owner that wakes while the user is idle, and make the durability
  barrier **complete** within 5 seconds of the oldest undurable commit. It may
  not depend on another edit, Save, or shutdown.
- That optional path uses an injected monotonic clock/fake backend and a bounded
  single writer. Never test the deadline with sleeps.

#### 3C. Lifecycle and corrupt recovery

- Save publishes the project through its existing independent atomic/durable
  contract even if recovery is degraded. It then attempts one fresh recovery
  checkpoint. Success clears degradation and resumes diff recording; failure
  preserves old evidence and the sticky notice but does not fail Save.
- The initial synchronous design needs no shutdown recovery flush: every
  successful record is already durable. Only the optional queued/batched design
  adds a bounded drain/flush mechanism whose timeout and ownership must be
  specified in that separate design. This plan does not claim the
  still-in-process Pack worker itself has bounded join time (H0.3-H0.5).
- Process-crash tests prove recovery reaches the latest durable watermark.
  Power-loss tests use the backend fault seam to discard the undurable tail.
- Scan ranks a mid-stream-corrupt file as a damaged-prefix candidate, never as a
  clean adoptable journal. Recovery may preview/save the valid prefix, but the
  original journal bytes are preserved until explicit discard/cleanup. Hold the
  claim lock only during an active attempt; release it on cancel, keep, or
  failure and reacquire on retry so the candidate remains discoverable.
- Keep version-4 framing and compact TXN/HISTORY records. No timer-driven full
  checkpoint, no unsupported-record full-checkpoint fallback, and no persistent
  full Undo stack. An unsupported/oversized record degrades recovery until
  explicit Save/reattach can establish a fresh checkpoint.

Gate:

- recovery allocation/encode/admission/append/sync failure after an edit: edit,
  revision, history, and event remain; recovery becomes degraded; a second edit
  does not attempt a dependent append;
- synchronous design: every successful append is immediately recoverable;
  optional batched design only: idle fake-clock crossing proves the barrier
  completes by 5 seconds without another edit/Save/shutdown;
- corrupted middle record followed by valid bytes: only the prefix is offered,
  original bytes remain byte-identical and discoverable after cancel, relaunch,
  and save failure;
- Save/recovery-close fault matrix has no silent slot deletion; synchronous
  recovery has no close-time flush, and any optional queue adds only its bound;
- existing v4 clean/torn-tail/mixed TXN/HISTORY fixtures remain readable.

### Wave 4 — path identity and architecture seams

Implement PATH-01 and ARCH-01 after recovery behavior is stable because both
touch identity/recovery or snapshot consumers.

- Define one lexical absolute source identity routine that normalizes separators,
  dot segments, drive/UNC rules, and case according to the existing identity
  contract without filesystem existence requirements.
- Pin Open → Save As to another directory → reload → same semantic identity,
  same resolved physical sources, and expected canonical bytes.
- Move const source/atlas lookup to a read-only project query owner and make
  snapshot queries use only const pointers.

Gate: path/Save/recovery tests pass on Windows, Linux, and macOS; no realpath
dependency and no change to live source spellings merely to satisfy the hash.

### Wave 5 — large-batch performance, not speculative architecture

- Use the 3B results to close the journal design before this wave. Profile
  PERF-01 separately with large atomic batches. Optimize proven repeated
  validation, cloning,
  result, or history work; do not split one requested atomic batch or add a
  transaction-size limit to conceal it.

Timing remains advisory across heterogeneous CI machines; correctness, sample
count, failure count, byte accounting, and RPO are hard gates.

## 5. Explicit follow-up phases, not refactor regressions

The following are real remaining product work but should not be mixed into the
correctness/recovery patch series:

- stale/current preview hash and result LRU (F3.4-F3.5);
- canonical raster coverage such as EXIF orientation, 16-bit/ICC policy, and
  additional codecs such as WebP (H0/B0 policy work);
- private fallible builder process and crash/cancel/full-disk containment
  (H0.3-H0.5);
- canonical Import/Export IRs, format registry/packages, MCP, and Dev API.

These need their existing roadmap phases and acceptance criteria. They are not
reasons to expand the journal refactor.

## 6. Final verification and completion definition

Run, at minimum:

```bash
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug --output-on-failure

cmake --preset native-release
cmake --build --preset native-release
ctest --preset native-release --output-on-failure

build/_cmake/native-release/packer/tests/tp_bench_foundation \
  --project examples/projects/large-synthetic.ntpacker_project \
  build/benchmark-scratch 20 100
```

The improvement program is complete only when:

- every finding in §3 is fixed or explicitly moved to a named follow-up with
  owner approval;
- all required assertions execute in Debug and Release;
- every CLI `--json` path emits exactly one valid structured payload;
- mutation dry-run is demonstrably side-effect free;
- synchronous recovery makes each successful record durable, or an
  explicitly-triggered batched design meets the idle fake-clock 5-second RPO;
  degraded recovery never blocks or rolls back editing;
- corrupt recovery never silently deletes the only evidence/source journal;
- the large checked project ships in release archives and its contract stays
  green;
- Debug/Release and three-platform CI are green, with no changes under
  `external/neotolis-engine/`.
