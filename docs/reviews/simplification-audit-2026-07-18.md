# Simplification audit — 2026-07-18

## Scope and baseline

Аудит выполнен после canonical foundation commit `c672ff4`, до добавления новых
Import/Export IR, registry, MCP и Dev API функций. Нормативный источник —
`docs/ntpacker-master-spec.md`; engine submodule не изменялся. Исходный baseline:
clean tracked tree, отдельный пользовательский untracked `.serena/`, build green,
84/84 tests green.

Цель прохода — убрать доказанное дублирование и сделать сложность видимой,
не меняя product contracts. Новый feature work в этот проход не входит.

## Responsibility map

| Module | Owned responsibilities | Audit decision |
|---|---|---|
| `tp_fs_internal.c` | strict UTF-8 path admission; Win32/POSIX file and directory backend; checked I/O, sync and publication primitives | Один правильный owner, но 3 подответственности в одном TU. Сначала устранён frontend policy duplicate; platform/TU split остаётся mechanical follow-up. |
| `tp_history_codec.c` | wire primitives; project/diff record codec; decoded-record validation; atomic replay adapter | Codec и replay orchestration смешаны. Разделять только отдельным contract-preserving packet с byte-for-byte v1 goldens. |
| `tp_project_identity.c` | pack-domain checks; structural ID assignment/uniqueness; canonical source/reference/graph gate | Pack-domain не относится к identity. Будущий split: constraints owner + ID owner + canonical graph validator. |
| `tp_json_internal.c` | raw JSON admission and duplicate-key rejection | Связная ответственность; оставить. |
| `tp_image.c` | bounded file ingress, RGBA8 decode, allocator-matched release | Связная ответственность; оставить. |
| `tp_utf8.c` | strict scalar width and byte/C-string validation | Связная ответственность; оставить. |
| `apps/common/nt_utf8_argv.c` | Windows process/environment UTF-16 ingress and argv ownership | Правильный client-ingress owner. Не переносить CLI parsing в core. |
| `apps/common/nt_utf8_fs.c` | CRT-local `fopen/remove/rename` adapters | После F-01 не владеет decoding, long-path, namespace или Win32 error policy. |

Отдельный legacy hotspot — `tp_project.c` (3387 LOC): path algorithms, mutable
CRUD, deterministic writer/save publication, JSON schema/parser и pack bridge.
Его физическое разделение необходимо, но не должно совмещаться с изменением Save
semantics или canonical-v5 schema.

## Findings and classification

| ID | Severity | Classification | Finding | Result |
|---|---|---|---|---|
| F-01 | high | auto-fixable after CRT review | `apps/common/nt_utf8_fs.c` repeated core UTF-8→UTF-16, long-path and namespace policy; implementations had already drifted at the 32767-character bound | Fixed in `c508eea`. Core exports only caller-buffer path conversion; CRT-local `FILE*` ownership stays in each client adapter. Frontend implementation fell from 256 to 86 LOC. R21 prevents policy duplication from returning. |
| F-02 | medium | auto-fixable | Canonical source-key normalize+compare existed in identity, operation admission and validation report code | Fixed in `f1de038` through `tp_srckey_validate_canonical()`. Each caller still maps status/message/severity independently. |
| F-03 | medium | auto-fixable | Large production files could grow without a visible inventory | Hard size enforcement was rejected by the owner. Keep a non-gating LOC inventory; size alone never rejects a change or requires a split. |
| F-04 | medium | auto-fixable, not attempted | Slash-normalized source-path text admission/equality is repeated in CRUD, canonical identity and operation preflight | Extract only bounded text admission/hash/equality. Do not merge effective canonical path identity or portability/case-fold reporting. |
| F-05 | high | manual-only | Atlas settings and sprite override constraints are expressed in operation admission, validation reporting and defensive Pack checks | Introduce a pure typed constraints evaluator later. Preserve three policies: first reject, accumulated findings, and final engine-assertion guard. Requires a cross-layer boundary-vector matrix. |
| F-06 | medium | manual-only | `tp_project`, `tp_validate`, `tp_op_validate`, history codec and filesystem TUs are responsibility/complexity hotspots | Split only where the responsibility map yields a clearer seam, one oracle-backed packet at a time. Parser/save and history replay are highest risk. |

## Validation consolidation boundary

Safe to share:

- pure canonical-key predicates (F-02);
- bounded source-path text admission/equality (F-04);
- pure pack constraint evaluation returning typed issues (F-05);
- exact capability and exporter-ID predicates already shared by core helpers.

Must remain separate:

- operation admission returns the first structured reject and field;
- `tp_validate` accumulates stable ordered findings and severities;
- `tp_pack` defensively blocks values that could reach assertion-based engine code;
- canonical project admission is a hard persistence gate, not a user-facing
  warning policy;
- history decode/replay validates untrusted bytes before swapping model state.

Combining the pure predicate is simplification. Combining these policies would
weaken contracts or destabilize structured output.

## LOC and complexity observations

Production C excludes vendor, generated, test, benchmark, pack-builder and GUI
selftest sources. Physical LOC is one of the main signals for locating mixed
responsibilities, but it is not an acceptance criterion. There are no hard
LOC/CC limits, baselines, ratchets, or no-growth rules.

A large file is reviewed together with its owned responsibilities, dependency
fan-out, and fault/contract matrices. A long function may remain when it is
cohesive and its regions are clear. Split functions only at a real semantic seam
or for actual reuse; do not create one-use microhelpers to satisfy a metric.

A diagnostic Clang-Tidy `readability-function-cognitive-complexity` run measured
the current main hotspots:

| Function | CC |
|---|---:|
| `tp_operation_validate` | 154 |
| `validate_operation_utf8` | 114 |
| `validate_atlas` | 92 |
| `tp_project_reject_unknown_schema_keys` | 85 |
| `tp_project_json_admit` | 74 |
| `tp_project_save_stage` | 71 |
| `tp_pack::validate_settings` | 70 |
| `validate_sources` | 62 |
| `encode_op` | 52 |

Candidate responsibility boundaries for the Phase 2 reassessment:

1. operation validation by operation family, only if the shared constraints
   work exposes a narrower stable seam;
2. validation indexes/materialization from atlas rules, only if ordered report
   orchestration remains simpler;
3. split filesystem platform backends from checked durability helpers;
4. split project path/model/writer/parser/bridge TUs without semantic edits;
5. split history writer, reader/schema and replay adapter under byte goldens.

## Verification

- baseline: `cmake --build --preset native-debug`; 84/84 CTest passed;
- F-01: Windows Unicode/long-path/raw-namespace tests preserved; full build and
  84/84 CTest passed;
- F-02: new canonical-key unit test failed before the API existed, then targeted
  source-key/operation/schema/validation tests and full 84/84 CTest passed;
- F-03: the original hard LOC gate passed over 87 production TUs, but its policy
  was rejected after review. P0-01 removed it from CTest and replaced it with a
  manually requested, non-gating inventory.

## Next simplification packets

Continue without feature work according to
`docs/plans/simplification-refactor-plan.md`. Phase 0 closes contract defects
before structural work; Phase 2 adds the shared primitives and boundary checks.
Physical splits are authorized only after the Phase 2 reassessment confirms a
clearer ownership seam and a bounded packet-plan protects the affected
contracts.

## Residual risk recorded during P0-07

History codec v1 `FRAME_MOVE` stores the parent atlas/animation IDs and the
`from`/`to` indexes, but not the moved frame's `{source,key}` identity. P0-07
can therefore harden collection removals and sprite replacement without a wire
change, while an in-range move against the wrong frame remains detectable only
by a future versioned history format. This is deferred to the codec work after
the P1-02 fixed-byte oracle; the simplification packet does not invent an
incompatible v1 field.

## P1-07 Save outcome table

The executable matrices in `test_save_io_contract.c` and
`test_session_save_io_contract.c` pin the complete ownership boundary. “No
checkpoint” below means the session does not advance its saved anchor or compact
its recovery journal; serialization of the private publication candidate is not
an acknowledged checkpoint.

| Seam | Destination after return | Published / fingerprint | Session baseline, path, dirty, Undo, events | Checkpoint / journal compaction | Public result | Retry rule |
|---|---|---|---|---|---|---|
| temp open | previous bytes | no / nil | unchanged | none | `FILE_IO_FAILED`, phase `temp_open`, CLI 8 | caller may issue a new Save |
| temp write | previous bytes | no / nil | unchanged | none | `FILE_IO_FAILED`, phase `temp_write`, CLI 8 | caller may issue a new Save |
| file sync | previous bytes | no / nil | unchanged | none | `FILE_IO_FAILED`, phase `file_sync`, CLI 8 | caller may issue a new Save |
| temp close | previous bytes | no / nil | unchanged | none | `FILE_IO_FAILED`, phase `temp_close`, CLI 8 | caller may issue a new Save |
| atomic replace | previous bytes | no / nil | unchanged | none | `FILE_IO_FAILED`, phase `atomic_replace`, CLI 8 | caller may issue a new Save |
| atomic create | destination remains absent | no / nil | old identity/fingerprint unchanged | none | `FILE_IO_FAILED`, phase `atomic_create`, CLI 8 | caller may issue a new Save |
| parent sync | new bytes authoritative | yes / published fingerprint | advances path, fingerprint and clean anchor; emits Saved | compact attempted exactly as normal Save | success plus `file_durability_uncertain` notice, CLI 0 | never retry as a failed publication |

All pre-publication errors also carry the attempted public path and the captured
errno-compatible native cause. The product performs no automatic atomic-replace
retry.

## P1-11 recovery oracle coverage

The recovery store already had strong executable ownership/fault coverage, so
P1-11 added only the missing deterministic equal-time ranking contract.

| Required boundary | Executable coverage |
|---|---|
| pinned journal / no-follow | `test_resolution_refuses_save_over_pinned_journal_before_write`, `test_scan_never_follows_journal_symlink`, `test_claim_rejects_lock_symlink_without_touching_target` |
| TOCTOU replacement safety | `test_live_owner_never_deletes_a_replacement_path`, `test_claimed_candidate_requires_bound_current_save_receipt` |
| exclusive claim | `test_live_slot_competition_and_permanent_lock`, `test_competing_orphan_claim_and_process_death`, `test_stale_lock_is_reused_not_recreated` |
| live-slot lifecycle | clean close, dirty preservation, orphan collision and degraded-slot tests in `test_recovery_store.c` |
| deterministic scan/ranking | `test_recovery_ranking_contract.c`: adoptability, timestamp, lexical tie-break, permutation-invariant cap |
| quarantine/discard | failed cleanup remains discoverable; discard deletes only the journal and retains the lock domain |
| disposable candidate/resolution ownership | receipt binding, cancel invalidation/process-lease release and Save-original exact-fingerprint tests |

## Completion addendum — 2026-07-19

### Verdict

The bounded simplification workstream is complete at `cf127c7`. Feature work
may resume after the normal three-platform CI merge gate. The implementation
keeps the owner decisions from this audit: LOC is diagnostic only, cohesive
functions may keep regions, and a physical split is accepted only when it
creates a clearer ownership boundary.

All three blocking review findings were closed before structural work:

- deterministic transaction first-reject: `5cff9c0`;
- shared policy-free constraint facts with separate operation, validation,
  canonical-admission and Pack policies: `82b7647` through `ad6f930`;
- hard LOC/complexity gate removed and inventory made non-gating: `fbfb19b`,
  with the policy retained in `AGENTS.md`.

The related contract gaps were also closed before their dependent moves:
canonical-v5 bytes (`e592889`), history v1 bytes (`072d547`), journal mixed
wire fixtures (`069fb6b`), ordered validation corpus (`9565bf8`), CLI payload
goldens (`9213417`), platform boundaries (`a3439c8`), typed Save outcomes
(`f85f13f`–`3a3e158`), client parity (`1202a7f`) and GUI action traces
(`924cd76`).

### Final responsibility decisions

| Baseline owner at `c672ff4` | Baseline LOC | Final family | Family LOC / largest TU | Decision |
|---|---:|---:|---:|---|
| `tp_fs_internal.c` | 818 | 3 TUs | 854 / 499 | split checked I/O from Win32 and POSIX backends behind one private contract |
| `tp_project.c` | 3181 | 6 TUs | 3262 / 1083 | split path, writer, staged Save, parser and Pack bridge; keep model CRUD together |
| `tp_validate.c` | 1276 | 6 TUs | 1370 / 334 | split report, indexes and rule families; keep one ordered orchestrator |
| `tp_op_validate.c` | 904 | 5 TUs | 1091 / 282 | split by operation family over shared typed facts |
| `tp_session_snapshot.c` | 994 | 2 TUs | 980 / 678 | split materialization from query surface |
| `tp_recovery.c` | 1906 | 6 TUs | 1851 / 392 | split store, claim, scan and platform backends; keep session adapter thin |
| `tp_txn_apply.c` | 1384 | 4 TUs | 1413 / 567 | split result, model lifecycle and journal integration; keep atomic commit owner |
| `tp_journal.c` | 1695 | 4 TUs | 2110 / 874 | split wire, I/O, reader and writer under fixed bytes; total LOC was not optimized |
| `tp_history_codec.c` | 835 | 2 TUs | 842 / 468 | split reader/hostile validation from writer/replay entry |
| `cli_mutate.c` | 1722 | 6 TUs | 1756 / 540 | split mutation families; retain one common CLI edit lifecycle |
| `gui_project.c` | 1859 | 5 TUs | 1820 / 1026 | split file, recovery and pending owners; keep typed mutation domain cohesive |
| `gui_actions.c` | 2368 | 6 TUs | 2317 / 606 | split deferred edit, preview, dialog, Pack and recovery domains |
| `main.c` | 1232 | 3 TUs | 1289 / 906 | extract parity and concrete resource bootstrap; keep frame orchestration |
| `gui_pack.c` | 960 | 3 TUs | 962 / 480 | native result slots, session-job adapter and preview each have one owner |
| `gui_canvas.c` | 950 | 2 TUs | 926 / 587 | extract GPU/image/page lifetime; keep view math, hit-testing, overlays and rendering together |
| `gui_view_settings.c` | 940 | 1 TU | 940 / 940 | keep: one settings view with clear regions; no one-use view microfiles |

Counts are physical production LOC observations, not acceptance thresholds.
Family totals may rise when a private contract, explicit fault path or clearer
ownership replaces implicit coupling. The important result is that the largest
TU no longer owns unrelated state machines.

Additional explicit keep decisions:

- `tp_session.c` remains the typed session orchestration boundary;
- `gui_project_mutations.c` remains one mutation domain despite its size;
- `gui_canvas.c` keeps transform math, hit-testing, overlays and rendering
  because they share one coordinate model;
- `gui_view_settings.c` keeps its regions;
- model-clone and semantic-diff deep copies remain deliberately separate due
  to allocator and fault-injection ownership.

### Duplication and abstraction review

The final search confirms one owner for the duplicated knowledge identified by
the original audit:

- stored source-path text: `tp_source_path_text.c`;
- pack/override representability facts: `tp_pack_constraints.c`;
- D4 geometry: `tp_transform.c`, consumed by Pack read and GUI canvas;
- canonical source keys: `tp_srckey.c`;
- Win32 UTF-8/long-path policy: `tp_fs_win32.c`, consumed by the CRT-local
  adapters in `apps/common/nt_utf8_fs.c`.

Intentional duplication remains only where policy or ownership is different:
CLI/GUI parsing and presentation, operation first-reject versus accumulated
validation versus Pack defensive checks, and allocator-specific deep copies.

No production `Service`, `Manager`, `Facade`, dispatcher, vtable or callback
layer was introduced. GUI decomposition uses direct concrete owners and four
family-private headers. R18 limits each header to its registered translation
units. `gui_pack_internal.h` exposes only native-result publication,
preview-result publication and preview-slot ownership query;
`gui_canvas_internal.h` shares only the concrete vertex layout required by VBO
creation and drawing. Boundary checks report no reverse client-to-core
dependency or private-header leak.

### Verification evidence

- native-debug build: success; CTest 99/99;
- native-release build: success; CTest 99/99;
- focused transaction, job/input-token, canonical GUI, action trace, client
  parity, transform and UV suites: success;
- `scripts/check_boundaries.sh`: `boundaries OK`;
- `git diff --check`: clean at every committed packet;
- `external/neotolis-engine/`: unchanged and read-only;
- tracked worktree clean after implementation; user-owned `.serena/` remains
  untracked and untouched.

One `tp_gui_action_trace` invocation transiently failed in the Save→New case
during a focused run, then passed immediately in isolation, in the repeated
focused matrix, and in both full 99-test debug/release runs. No Pack code was
involved in that scenario; it is recorded as a test-flake observation rather
than hidden or treated as a product regression.

Linux/macOS/Windows CI and the configured sanitizer job cannot be executed from
this local Windows workspace without publishing the branch. They remain the
normal PR merge gate; no local result is presented as a substitute.
