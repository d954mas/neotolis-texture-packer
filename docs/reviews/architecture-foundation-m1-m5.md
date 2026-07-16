# Architecture foundation M1-M5 completion review

Date: 2026-07-16

Branch: `impl/master-spec`

Normative source: `docs/ntpacker-master-spec.md`

Execution plan: `docs/plans/architecture-foundation-plan.md`

Status: **local gates GREEN**. Exact-SHA terminal CI is a handoff gate whose
current result is recorded on the PR and in the task handoff.

## Scope

This review covers the ownership foundation M1-M5. M0 evidence is retained in
`architecture-foundation-m0-baseline.md`. The work deliberately does not add an
actor/mailbox, COW model, arena-backed mutable model, generic manager/context,
event bus, MCP transport, format packages, or a second mutable source of truth.

The v1 idempotency contract is `duplicate_id` for a retained committed
transaction ID. Durable replay of the original result is optional and must be
advertised explicitly by a future transport surface.

## Implementation commits

| Commit | Purpose |
|---|---|
| `f7d353a` | M0 contract repair and measurement integrity |
| `d78e10e` | Initial M1-M5 ownership foundation |
| `f03c715` | Portable CI corrections |
| `cf9a6b8`, `0785f26` | Isolated, genuinely dirty POSIX recovery fixtures |
| `92016e1` | Canonical GUI identity and stable deferred intent capture |
| `c98fe8e` | Core-owned composite derived-job freshness token |
| `57ba946` | Exact, fail-closed client job capabilities |
| final closure (this commit) | Cross-client parity, deletion gate, selftest/benchmark oracle repair and durable review evidence |

## Requirement and evidence matrix

| Package | Result | Primary executable evidence |
|---|---|---|
| M1 | `tp_session` solely owns the mutable model; snapshots are immutable and owned; events and admission remain synchronous and serialized | `tp_session`, `tp_job_owner`, boundary R8/R10 |
| M2 | GUI mutations cross thin stable-ID adapters; rows are cached by model/source generations; row override lookup is `O(rows + overrides)` | `tp_gui_session_adapter`, `tp_gui_canonical_identity`, `tp_bench_gui_rows`, boundary R6/R7/R8/R11/R12/R14 |
| M3 | Journal parsing is bounded before materialization; retained IDs, payload ownership, OOM/corruption and recovery budgets are executable | `tp_journal`, `tp_bench_foundation`, journal fault corpus |
| M4 | Recovery store/live/claim and project lease are core-owned and GUI-independent; process/lock/cleanup cases are executable | `tp_recovery_store`, `tp_project_lease`, boundary R9/R10 |
| M5 | GUI/CLI share operation/session rules; a direct live-headless harness exercises Pack and Export; unsupported async Inspect/Validate jobs return typed capability results | `tp_client_parity`, CLI mutation families, boundary R6-R15 |

The closed per-family cutover record is
`architecture-foundation-migration-ledger.md`.

## Owner boundaries

- `tp_model` owns mutation, validation, revision, dirty identity, history and
  retained transaction IDs.
- `tp_session` owns orchestration, identity/path persistence state, admission,
  runtime generations, events and concrete job handles.
- Pack/Export algorithms, recovery codecs, filesystem/lock backends and protocol
  formatting remain outside `tp_session`.
- Frontends capture intent and map typed results. They do not retain mutable
  project/model aliases or duplicate business rules.

## Superseded paths deleted

- Mutable GUI project/model read authority and snapshot Undo path.
- Shipping name-only sprite lookup/selection and queued collection-index
  authority. Selftest-only name helpers remain test oracles.
- Name-only animation-frame payloads and pending-name Pack fallback.
- GUI-owned recovery slot/claim/lock state machine and GUI worker authority.
- Per-frame filesystem probing and quadratic row-to-override lookup.
- GUI `s_refresh_epoch` / `s_pack_start_refresh_epoch`, model-only
  `model_changed_since`, `model_generation_at_start`, and preview
  `s_preview_ver` freshness paths.
- Generic `TP_CLIENT_CAPABILITY_LIVE_JOBS` and the weak snapshot-input seam
  test. Pack job and Export command are exact capabilities; async Inspect and
  Validate jobs are explicitly not implemented.

## Review convergence

Independent correctness, architecture and performance reviews were run for each
corrective slice. Initial reviews were intentionally not GREEN:

1. Canonical GUI identity review found stale-revision laundering and delayed
   atlas/animation/target indices. Stable IDs plus captured revisions replaced
   those paths; re-review was GREEN.
2. Freshness review found target preview still using model-only generation and
   duplicate equality logic. Preview now consumes the same core token and core
   owns equality; stale preview publication is rejected; re-review was GREEN.
3. Capability review found generic `LIVE_JOBS` over-promised and used a blanket
   AVAILABLE branch. The exact fail-closed matrix and real Pack/Export harness
   replaced it; re-review was GREEN.

Performance review found no new hot-path allocation or filesystem work. The
composite token is a 16-byte POD and comparisons are `O(1)`. Test Pack/Export
artifacts are isolated and removed by portable fixture cleanup.

## Local gates

- Native debug and release builds: GREEN.
- Native debug CTest: **94/94 GREEN**.
- Native release CTest: **94/94 GREEN**.
- GUI selftest preset: GREEN (full lifecycle/pack/export/recovery scenario).
- `scripts/check_boundaries.sh`: GREEN, including seeded detector selftests.
- `git diff --check`: GREEN.
- Reference row rebuild after M2: 1 source / 9,999 children / 4,096 overrides
  produces 10,000 rows with 17,090 probes, 14,096 linear units and zero row
  allocations. Timing is host-dependent; the structural counters are the gate.
- Foundation benchmark: NORMAL/LARGE/HUGE and Save+compact all GREEN. The
  checkpoint oracle compares recovery with the self-contained checkpoint
  encoding, while ordinary reload remains compared with ordinary Save.
- Sanitizer and exact-SHA cross-platform results are CI handoff gates and are
  recorded on the PR/task handoff rather than pinned as transient status here.

## Routed post-foundation work

- **P-UNDO: TRIGGERED.** M0 measured roughly 17.36 MB per HUGE Undo/Redo durable
  append and p95 around 1.5-1.65 s. Compact history acknowledgement is a named
  post-foundation packet; it does not invalidate M0-M5 ownership completion.
- **A-FOLLOW:** starts only when a second real live host requires authority
  handoff/fencing.
- Full semantic `pack_input_hash`, result ordering/cache and byte-budget LRU
  remain F3 product work. Foundation claims only the current composite
  `{model_generation, source_generation}` freshness token.
- Async Inspect/Validate session jobs are intentionally `NOT_IMPLEMENTED`;
  existing synchronous snapshot Inspect/Validate contracts remain available.

## Questions for the owner

1. Should P-UNDO be scheduled as the next engineering packet, or should product
   roadmap work continue first despite the measured HUGE history cost?
2. When a second live host is selected, which host pair should drive A-FOLLOW
   handoff tests (GUI+MCP or GUI+Dev API)?
