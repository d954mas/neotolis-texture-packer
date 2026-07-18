# Simplification and legacy decomposition plan

**Status:** reviewed umbrella roadmap; execution authorized only through the
Phase 2 reassessment gate

**Date:** 2026-07-18

**Normative product source:** `docs/ntpacker-master-spec.md`

## 1. Objective

Временно остановить feature work и упростить текущий canonical foundation до
того, как на него будут добавлены Import/Export IR, format registry, MCP и Dev
API. Результат должен иметь более ясное ownership, меньше duplicated knowledge
и направленные зависимости при сохранении текущих product contracts.

Это не кампания по уменьшению количества строк. LOC файла используется как один
из главных сигналов, где искать смешанные ответственности, но не как acceptance
criterion.

Этот документ задаёт dependency/ownership roadmap, а не разрешает выполнить все
перечисленные splits одним большим циклом. Перед каждым physical split Phase
3–5 создаётся отдельный bounded packet-plan с exact files/symbols, private seam,
automated checks, rollback boundary и completion evidence. После Phase 2 вся
карта повторно сверяется с фактическим кодом; без этого gate Phase 3–5 не
авторизованы.

## 2. Locked owner decisions

Эти решения не пересматриваются внутри данного плана:

1. Новые возможности временно не добавляются; исправления найденных contract
   defects и test oracles входят в simplification scope.
2. Нет hard LOC/CC limits, baseline, ratchet и no-growth gate.
3. Для каждого крупного файла учитываются physical LOC, owned responsibilities,
   dependency fan-out и fault/contract matrices. Ни одна метрика сама по себе не
   требует split.
4. Длинная функция может остаться, если она связная, ясно размечена regions и
   выполняет одну ответственность.
5. Функция делится только по реальной семантической границе или когда меньшая
   функция действительно переиспользуется. One-use microhelpers ради CC/LOC не
   создаются.
6. Физическое перемещение кода и изменение поведения не смешиваются в одном
   commit.
7. Operation first-reject, accumulated validation, canonical persistence gate,
   Pack defensive barrier и hostile history replay — разные policies. Общими
   могут быть только чистые typed predicates/evaluators.
8. `external/neotolis-engine/` остаётся read-only.
9. Checked-in schema/wire fixtures неизменяемы в move-only commit. Любое
   обновление fixture выполняется отдельным явно одобренным contract-change
   commit.

## 3. Completion criteria

Audit завершён, когда одновременно выполнено следующее:

- закрыты P1 из `docs/reviews/simplification-code-review-2026-07-18.md`;
- у каждого крупного target-файла есть одно из решений: split выполнен,
  оставить цельным с обоснованием или отложить из-за конкретного contract gap;
- повторяющееся domain knowledge имеет одного owner; намеренная duplication
  явно задокументирована;
- public schemas, wire bytes, statuses, finding order, Save semantics, Undo/Redo
  и tool parity защищены независимыми oracles;
- dependencies идут от clients к typed core contracts, не обратно;
- каждый commit зелёный и имеет одну семантическую цель;
- итоговый LOC inventory приложен как наблюдение, а не pass/fail gate;
- feature work можно возобновить без необходимости снова разбирать foundation.

## 4. Change budget and commit protocol

Вместо числового source-size budget применяется конкретный change budget:

- один commit — одна responsibility boundary, один defect или один contract
  oracle;
- один move-only commit не меняет status, field, message, schema, bytes, order,
  allocator ownership или side-effect order;
- один behavior commit содержит regression test и минимальное production change;
- platform split перемещает один backend за commit;
- high-risk codecs не перемещаются, пока fixed bytes не читаются и не пишутся
  независимо;
- перед physical split создаётся bounded packet-plan; umbrella roadmap не
  заменяет его;
- GUI state не распределяется по новым globals: сначала вводится один явный
  state owner, затем модули получают узкие ссылки/DTO;
- локально перед каждым commit: targeted tests, полный native-debug CTest,
  `git diff --check`; boundary gate — в Bash-capable среде/CI;
- после platform/serialization/recovery packet обязательна трёхплатформенная CI
  matrix; sanitizer job сохраняется;
- `.serena/` не трогается.

LOC inventory должен выводить для production TU: physical LOC, responsibility
labels, direct internal dependencies и решение `split / keep / conditional`.
Он не должен завершаться ошибкой из-за числа строк.

Каждый bounded packet-plan Phase 3–5 обязан зафиксировать:

- base commit и единственного owner-а изменений;
- исходные и целевые files/symbols;
- формируемый private/public seam и запрещённые dependency directions;
- список неизменяемых contracts и exact automated oracles;
- пошаговые green commits, каждый с одной целью;
- CMake changes для shipped/test targets и platform selection;
- rollback boundary и условие `keep instead of split`, если seam ухудшает код;
- targeted/full/CI verification evidence.

## 5. Responsibility map and decisions

### 5.1 Core

| Current TU | LOC | Current responsibilities | Decision |
|---|---:|---|---|
| `tp_project.c` | 3387 | path algorithms; model CRUD/defaults; deterministic writer; staged Save/publication; JSON admission/parser/load; pack bridge | Mandatory semantic split. Preserve one project model vocabulary and byte-identical v5 writer. |
| `tp_recovery.c` | 2004 | store paths; Win32/POSIX backend; live slot; claim/candidate; resolve/discard; ranking; scan; session adapter | Mandatory staged split by independent state machines; OS backend first. |
| `tp_journal.c` | 1788 | wire primitives; durable writer; recovery reader/walk; peek | Conditional on fixed wire fixtures; writer/reader are real independent invariants. |
| `tp_txn_apply.c` | 1456 | model lifetime; result assembly; atomic commit; preflight; journal recovery bridge | Split only after first-reject bug is fixed; commit engine remains one atomic owner. |
| `tp_validate.c` | 1341 | report ownership; indexes; source/sprite/target/settings rules; ordered orchestration | Split report/index/rules, while one orchestrator keeps exact order. |
| `tp_session_snapshot.c` | 1064 | materialization; DTO/scalar queries; selectors; serialization | Conditional split into materialization/query/selector modules. Not ahead of higher-risk mixed owners. |
| `tp_session.c` | 933 | session orchestration, recovery health, events, jobs, transaction/save commands, queries | Keep for now. Regions match the normative orchestration boundary; split only if a concrete independent invariant appears. |
| `tp_op_validate.c` | 927 | common admission plus atlas/source/sprite/animation/target family rules | Reassess after constraints extraction. Family-file split is allowed because operation families are semantic domains; long dispatcher/functions alone are not a reason. |
| `tp_fs_internal.c` | 897 | UTF-8 path policy; Win32/POSIX backend; checked I/O/sync/publication | Mandatory platform/backend split behind one private facade. |
| `tp_history_codec.c` | 892 | wire writer; wire reader/shape validation; public encode; atomic replay adapter | Last core split; fixed v1 bytes and hostile replay first. |
| `tp_txn_parse.c` | 830 | lexical JSON admission; envelope parse; lower/apply entry | Conditional admission/parser split after higher priorities. |
| `tp_project_identity.c` | 702 | sprite pack-domain representation; structural IDs; canonical graph gate | Move constraints to their owner; then split IDs from canonical graph only if private API stays narrow. |

Explicit `keep` decisions:

- `tp_json_internal.c`: raw JSON admission and duplicate-key rejection;
- `tp_image.c`: bounded raster ingress, RGBA decode, matched release;
- `tp_utf8.c`: strict UTF-8 scalar/C-string validation;
- `tp_srckey.c`: source-local key normalization/canonicality/portability;
- `tp_session.c`: current orchestration boundary;
- `tp_project_clone.c` and `tp_diff_entity.c`: independent allocator/fault seams
  justify their separate deep-copy implementations.

### 5.2 Clients

| Current TU | LOC | Current responsibilities | Decision |
|---|---:|---|---|
| `apps/gui/gui_actions.c` | 2477 | deferred edits; selection; animation preview; dialogs; confirm; pack/export; rename; recovery; deferred side effects | Mandatory split by action domain after state/side-effect order is pinned. |
| `apps/gui/gui_project.c` | 1970 | frontend state; coalescing; pending buffer; session lifecycle; recovery; mutation wrappers; undo/redo; file ops | Mandatory split, preserving exactly one frontend state owner and session as mutation authority. |
| `apps/cli/cli_mutate.c` | 1805 | common edit lifecycle plus source/sprite/animation/target/atlas families | Strong low-risk family split after exact CLI goldens. |
| `apps/gui/main.c` | 1340 | bootstrap/resources; input/shortcuts/frame; headless parity dev seam; main | Extract parity and bootstrap; keep frame orchestration. |
| `apps/gui/gui_pack.c` | 1022 | job adapter; result state; export-target preview/cache | Split adapter from preview only if shared job state remains single-owner. |
| `apps/gui/gui_canvas.c` | 1004 | duplicated D4; GPU lifetime; source/page resources; view math; render helpers; input | First remove D4 duplication. Resource/render split is conditional on a clean state boundary. |
| `apps/gui/gui_view_settings.c` | 982 | one settings-panel view with coherent regions | Keep. Regions are clearer than one-use view microfiles. |
| `apps/common/nt_utf8_fs.c` | 101 | CRT-local `fopen/remove/rename` adapters | Keep thin. No path policy or heap/`FILE*` ownership moves into core. |
| `apps/common/nt_utf8_argv.c` | 293 | Windows process/environment UTF-16 ingress and argv ownership | Keep. CLI parsing stays outside core. |

## 6. Duplication map

| Knowledge | Current sites | Decision |
|---|---|---|
| Win32 UTF-8/long-path policy | core + formerly `apps/common` | F-01 is correct; broaden R21 to all production frontends. |
| Canonical source-key normalize+compare | identity, operation, validation | F-02 owner is correct; settle caller-visible reason/message mapping. |
| Bounded slash-normalized stored source-path text | project CRUD, identity/source plan, operation preflight | F-04: one pure text module for admit/normalize/hash/equality only. Do not absorb realpath/case-fold/reporting. |
| Pack/override constraints | operation, validation, Pack, canonical representability | F-05: pure representability predicates plus raw/effective constraint facts. Canonical admission, operation, validation and Pack retain four separate policies. |
| D4 geometry | pack reader and GUI canvas | One engine-free integer/float transform owner tested over all eight masks. |
| Entity deep copy | model clone and semantic diff | Keep deliberate duplication because allocator ownership and fault injection differ. |
| CLI/GUI intent parsing | each client | Keep different interface shapes; parity is asserted at typed operation/final-byte level. |

## 7. Contract-oracle matrix

| Contract | Current strength | Required oracle before related split |
|---|---|---|
| Operation reject order | Broad status/field vectors, but history-on/off gap | Multi-invalid exact result with and without history; assert full non-publication state, retained-ID retry and intra-batch dependencies. |
| Validation findings | Exact two-finding case; many code-only checks | Platform-neutral exact ordered corpus plus platform path cases and schema-2 CLI goldens. |
| Pack constraints | Separate hand-picked vectors | One fact table driven through canonical representability, operation projection, validation accumulation and Pack barrier without sharing policy order/prose. |
| Canonical v5 writer/parser | Repeated writer self-comparison and broad semantic parser tests | Immutable fixed bytes from deterministic IDs plus independent positive and negative admission fixtures. |
| History codec v1 | Semantic round-trip | Literal big-endian encode bytes and independent decode fixtures for every diff shape/direction, built without production codec helpers. |
| Journal wire | Strong hostile behavioral suite | Literal big-endian header plus META/CHECKPOINT/TXN/HISTORY fixtures, sync/CRC coverage and independent mixed-prefix cases. |
| Save/publication | Strong fault/state suite | Preserve; add typed replace/sync fault context and transient diagnostics. |
| CLI JSON | Some exact errors/validation; most shape-only | Exact version/pack/mutation/error/notice goldens; mask only documented timings. |
| Filesystem paths | Good Unicode/platform coverage | Exact Win32 drive/UNC last-valid/first-invalid boundaries and platform conformance table. |
| Undo/Redo session semantics | Strong examples, not one complete all-family oracle | Every semantic family: A→B→Undo→byte-A→Redo→byte-B, revisions/dirty/branch discard and journal-failure non-publication. |
| Client parity | GUI/direct-session plus separate CLI E2E | Complete shipped mutation-surface manifest through real lowering adapters with deterministic/shared IDs. |
| GUI action state | Existing selftests cover selected flows | Exact action trace for deferral/coalescing/confirm/recovery/selection/Undo/Save and message/side-effect order. |

## 8. Execution sequence

Phases are ordered by dependency, not by desired line reduction. Independent
oracle packets inside Phase 1 may run in parallel after Phase 0 is green.

### Phase 0 — Correct policy and confirmed behavior defects

#### P0-01 — Correct the written quality policy

**Commit A:** update `AGENTS.md` and the current audit: remove hard LOC/CC,
baseline, ratchet, no-growth and mandatory function-split wording; record the
locked decisions from §2.

**Commit B:** unregister `architecture_loc_budget`, remove the failing budget
script, and optionally replace it with a non-gating `report_loc_inventory`
command. Do not silently rename a gate while preserving failure semantics.

**Acceptance:** a necessary cohesive file is never rejected by size alone;
inventory still makes file hotspots visible.

#### P0-02 — Restore deterministic transaction first-reject

1. Add one history-on/off regression covering invalid UTF-8 plus missing parent.
   Assert exact result `op_index`, error count, status, field and message;
   project bytes; revision; semantic dirty identity; event sequence; Undo/Redo
   positions; retained transaction-ID absence; journal length/record count; and
   successful retry with the same transaction ID after correcting the payload.
2. Validate each operation against the current clone before diff capture.
3. Introduce a private prevalidated apply seam only if it avoids duplicate work
   without weakening stage-then-commit/OOM behavior. Keep it adjacent/private;
   public apply and recovery replay continue to validate+apply.
4. Cover an op that depends on an entity created earlier in the same batch.

**Acceptance:** history attachment never changes rejection; capture never sees
an invalid operation; apply failure remains atomic.

#### P0-03 — Stop accidental CWD source identity fallback

1. Add no-base, overflow, absolute, drive/UNC and POSIX relative vectors.
2. Fallback to CWD only for the explicit no-base status.
3. Propagate `out_of_bounds` and other resolution faults.

**Acceptance:** source identity is independent of process CWD whenever project
base exists.

#### P0-04 — Implement constraint facts and close pack-validation drift

This packet is a sequence of separate green commits, not one wide change:

1. **P0-04A, oracle/facts.** Build a shared raw/effective boundary-vector table:
   `-1`, `0`, `max_size-1`, `max_size`, `max_size+1`, storage maxima, invalid
   shape, RECT/non-RECT extrude, inherited and explicit sprite overrides. Define:
   - pure representability predicates for storage/wire shape; canonical
     load/adoption/save may consume only these and must still allow a
     representable-but-user-invalid project to load for validation;
   - pure raw/effective pack-constraint enumeration into caller-owned storage or
     iterator.
   The shared layer exposes facts only: no policy order, status, field,
   severity, prose or control flow.
2. **P0-04B, operation migration.** Map facts to the existing operation-owned
   first-reject order/status/field/message. Behavior must be byte/field identical.
3. **P0-04C, Pack migration.** Use the same facts while keeping Pack-owned first
   defensive status/message before engine assertions. Pin direct Pack behavior
   separately from operation behavior.
4. **P0-04D, validation fix.** Accumulate raw-model facts in validation-owned
   order/severity/prose before `tp_project_atlas_to_settings()` applies the
   intentional export clamp. Include atlas values and sprite overrides against
   effective `max_size`.

**Acceptance:** `validate --strict` reports every setting that a subsequent Pack
would reject; policy-specific structured output remains stable.

#### P0-05 — Validate animation domains

Add a separate green packet for loaded animation FPS/playback values. Reuse a
pure domain predicate only if duplicated knowledge is demonstrated; otherwise
keep the validation rule local. Do not fold animation behavior into the pack
constraint module.

#### P0-06 — Restore F-02 caller-owned normalization message

Extend the canonical-key result with typed reason/canonical spelling and restore
the prior operation-owned message for normalization mismatch. Add an exact
regression for that rejection. This pins this observed legacy mapping without
declaring every diagnostic prose string globally stable. Do not duplicate
normalization.

#### P0-07 — Harden history removal identity

Add literal hostile fixtures with mismatched position/identity for
atlas/source/animation/target/frame and `TP_DIFF_SHAPE_SPRITE_RECORD`, in both
directions and for removal/replacement. Reachable records must compare structural
ID, or `{source,key}` for sprite/frame, and reject atomically. Unreachable forms
remain permanent decoder-rejection tests rather than prose-only evidence.
Record frame-move's lack of encoded identity as an explicit residual risk and do
not invent a wire change inside this simplification packet.

**Phase 0 gate:** no open P1; full suite green; no new feature surface.

### Phase 1 — Add independent contract oracles

Each packet is a separate green commit and unlocks only its dependent split.

#### P1-01 — Canonical project bytes

- deterministic rich v5 model with fixed structural IDs;
- checked-in exact bytes covering sparse defaults, all entity kinds and Unicode;
- writer→golden and golden→loader tested independently;
- immutable negative admission corpus for version classification,
  duplicate/unknown keys, numeric edges, invalid UTF-8, malformed/wrong-kind/
  duplicate IDs, canonical references and trailing bytes.

Unlocks project writer/parser moves.

#### P1-02 — History codec v1 bytes

- literal fixed **big-endian** bytes/hex for every diff shape, forward and
  reverse, constructed without production codec/endian helpers;
- explicit null/string sentinel, path-context, truncation and trailing-byte cases;
- encoder and decoder tested against fixtures, not only each other.

Unlocks history codec split.

#### P1-03 — Journal wire fixtures

- literal big-endian header plus META, CHECKPOINT, TXN and HISTORY records,
  sync word and checksum coverage;
- torn tail, bad length/checksum, committed prefix and mixed record sequences;
- fixture construction does not call `tp_jrn_put_*`, `tp_jrn_crc32` or another
  production wire helper; writer bytes and reader behavior are independent.

Unlocks journal writer/reader split.

#### P1-04 — Complete validation output

- a platform-neutral deterministic corpus, not one artificial all-family file;
- multiple atlases/entities and duplicate groups to pin iteration/cardinality;
- exact core order, severity, code, IDs, context, totals and message;
- exact schema-2 CLI JSON for the neutral corpus;
- separate platform-specific source/path cases;
- bounded/truncated report stays deterministic.

Unlocks validation family moves.

#### P1-05 — CLI schemas and payloads

- exact goldens for version manifest, inspect, validate, pack, mutation success,
  mutation error and durability notice;
- assert exact schema number per verb;
- mask `timings_ms`; independently replace only the known absolute test-root
  prefix with a token, while asserting absolute form, platform separator policy
  and every output suffix before substitution;
- preserve exit-code matrix.

Unlocks `cli_mutate` split and typed Save I/O contract change.

#### P1-06 — Filesystem platform boundaries

- exact Win32 drive and UNC last-valid/first-invalid output lengths;
- raw namespace/device rejection, invalid UTF-8, Unicode, create-only/replace,
  sync and parent-sync vectors;
- POSIX no-follow/symlink and invalid-name cases preserved.

Unlocks filesystem/recovery backend moves.

#### P1-07 — Classify Save I/O failures

After CLI goldens exist, add append-only `TP_STATUS_FILE_IO_FAILED` for
pre-publication temp open/write/file-sync/close and atomic replace/create faults,
with typed phase/path/native cause and a distinct reserved CLI exit mapping.
Parent-directory sync remains the existing post-publication
`TP_STATUS_FILE_DURABILITY_UNCERTAIN`: published bytes and fingerprint are
authoritative, CLI returns success with a notice, and callers must not retry as
if no write occurred.

Build an exact outcome table for every seam: destination bytes, published flag,
returned fingerprint, session saved baseline/path, checkpoint, journal
compaction, status/notice and retry rule. `tp_project_save.c` owns the
platform-neutral staged sequence; `tp_fs_*` owns typed OS primitives only;
`tp_session` alone owns the saved-file fingerprint. Do not add product retries
for atomic replacement in this packet.

#### P1-08 — Real client parity corpus

Create a coverage manifest for the complete shipped mutation surface across
atlas/source/sprite/animation/target operations: success, errors, no-op,
field-presence SET masks, omitted-versus-present fields, selectors/ambiguity,
notices and exit codes. Drive the real CLI and GUI lowering adapters, not a
direct-session shortcut. Use test-only deterministic RNG injection or harvest
CLI-created IDs and replay those exact IDs through the GUI adapter before
comparing structured results and canonical final bytes. Implement the manifest
family-by-family in separate green commits.

Unlocks broad client-file moves.

#### P1-09 — Undo/Redo and journal non-publication oracle

For every semantic operation family, assert
`A → apply → B → Undo → byte-identical A → Redo → byte-identical B`, monotonic
revisions, saved-baseline dirty identity, Redo-branch discard and exact history
depth. Inject journal failure and assert no publication of model, revision,
history, retained ID or event.

Unlocks transaction/history physical moves.

#### P1-10 — GUI state/action trace oracle

Before GUI decomposition, pin between-frame deferred queue timing, coalescing,
selection/preview, confirmation, recovery, Undo/Redo, Save and success-message/
side-effect ordering. Inventory fields as session-shared, action-private or
view-local; one private state object must not erase those ownership classes.

Unlocks P5-01/P5-02.

#### P1-11 — Recovery fault/state oracle

Before recovery movement, ensure executable coverage for pin/no-follow, TOCTOU,
exclusive claim, live-slot lifecycle, deterministic scan/ranking,
quarantine/discard and disposable candidate ownership. Add only missing cases;
do not duplicate already-strong recovery tests.

Unlocks P4-01.

### Phase 2 — Establish single owners for duplicated knowledge

#### P2-01 — F-04 source-path text primitive

Create one internal module owning only bounded text admission, slash
normalization, hash and equality. Migrate in separate commits:

1. project CRUD/dedupe;
2. source-plan/canonical graph text preflight;
3. operation candidate preflight.

Keep lexical absolute resolution, realpath identity, portability case-folding and
validation prose in their current owners. Its table-driven contract explicitly
covers strict UTF-8, byte bound, slash and dot-component behavior, traversal and
absolute-path rejection, drive/UNC/POSIX forms, Unicode, and hash/equality
invariants. It explicitly does **not** own source-local key identity,
normalization-to-NFC, realpath or portability case-folding.

#### P2-02 — Shared D4 geometry

Promote D4 as a legitimate engine-type-free public core geometry contract in
`packer/include/tp_core/tp_transform.h`; it is part of the canonical result
model, not a generic helper exposed only to reduce duplication. Own integer point
decode, float affine decode and output dimensions. Test all eight masks,
rectangular dimensions and fractional points. Migrate pack reader, then GUI
canvas, in two move-only commits. Never include builder-private engine geometry
headers in GUI.

#### P2-03 — Strengthen targeted duplication boundaries

Broaden R21 across production frontend TUs while retaining explicit process
ingress exceptions. Add a seeded selftest proving another GUI/CLI file is caught.
Do not add generic architecture rules that reject code based on size.

#### Phase 2 reassessment gate

Re-run responsibility/dependency/LOC inventory and the full suite. For every
proposed Phase 3–5 split, write a bounded packet-plan or change the decision to
`keep` with evidence. Phase 3–5 target shapes below are provisional until this
gate passes; their presence in the roadmap is not execution authorization.

### Phase 3 — Lower-risk physical decomposition

#### P3-01 — Filesystem backend split

Target shape:

- `tp_fs_internal.h` — stable private facade and typed results;
- `tp_fs_win32.c` — Win32 path/backend primitives;
- `tp_fs_posix.c` — POSIX path/backend primitives;
- `tp_fs_io.c` — platform-neutral checked reads/writes and limits.

Order: select Win32 source in CMake, move it verbatim; select/move POSIX; move
neutral helpers. One backend per commit. `tp_fs_sync` and `tp_fs_sync_parent`
remain platform backend operations. Publication ordering never moves here:
`tp_project_save.c` owns the staged sequence, while FS exposes typed primitives.
P1-05, P1-06 and P1-07 are prerequisites. No Save semantic change in move
commits.

#### P3-02 — CLI mutation families

Keep one dispatcher/common lifecycle and a narrow `cli_mutate_internal.h`.
Extract, one family per commit:

1. source;
2. atlas settings/lifecycle;
3. sprite overrides/naming;
4. animations;
5. targets.

Parsing remains client-owned; typed operations remain core-owned. Every family
commit runs its exact JSON/exit/final-project oracle.

#### P3-03 — Project module

Move in dependency-friendly order:

1. pack/path bridge (`tp_project_pack_bridge.c`);
2. path/source-base algorithms (`tp_project_path.c`);
3. deterministic serializer (`tp_project_write.c`);
4. staged Save/publication (`tp_project_save.c`);
5. JSON admission/parser/load (`tp_project_parse.c`);
6. leave model lifetime/CRUD/defaults as `tp_project_model.c` or the final
   `tp_project.c` owner.

Before each move, define only the private functions required by the next layer
in responsibility-specific headers such as `tp_project_path_internal.h`,
`tp_project_write_internal.h` and `tp_project_save_internal.h`. Do not expand the
existing catch-all `tp_project_internal.h`. Writer and parser must remain
independent enough that fixed bytes catch coordinated drift.

#### P3-04 — Validation module

First extract only `tp_validate_report.c` for owned report
storage/materialization/truncation. Then extract source, sprite/animation,
target/settings rule domains one at a time, keeping each index beside the rule
that owns it. Create a shared index module only after actual cross-domain reuse
is demonstrated. `tp_validate.c` retains public entrypoints and the single
ordered orchestrator. Do not turn every rule into a file; exact finding order is
asserted after every move.

#### P3-05 — Operation validation reassessment

After F-04/F-05 extraction, measure remaining ownership. If family boundaries
are clean, move atlas, source/sprite, animation and target handlers one at a
time behind one public dispatcher. If the remaining file is one cohesive
admission policy, keep it despite LOC. Do not split long functions into
one-use helpers merely to lower CC.

#### P3-06 — Snapshot module, conditional

Only after P3-03/P3-04 settle internal DTOs, consider materialization,
query/serialization and selector resolution files. Preserve one owned immutable
snapshot and prohibit borrowed live-model aliases.

### Phase 4 — High-risk state, recovery and wire decomposition

#### P4-01 — Recovery

1. Under P1-06/P1-11, first form a green backend seam: embedded lock/pin value
   types and operations accepting only those values, not whole recovery state
   structs. This is a behavior-preserving design commit, not a move.
2. Move Win32 and POSIX storage/lock implementations one backend per commit.
3. Separate store/live-slot lifecycle.
4. Separate claim/candidate/resolution/discard state machine.
5. Separate scan/ranking.
6. Leave session attach/resolve as a thin adapter.

Every commit preserves pin/no-follow, TOCTOU defenses, exclusive claim,
quarantine/discard semantics and disposable candidate ownership.

#### P4-02 — Transaction application

After P0-02 and P1-09, separate:

- model lifetime/adoption;
- result/error/request assembly;
- the single atomic commit engine;
- journal recovery bridge.

The commit engine remains one unit because validation, clone, diff capture,
journal acknowledgement and live swap form one atomic protocol. Split helpers
around it, not the protocol into independently callable phases.

#### P4-03 — Journal

Under P1-03 fixtures, target `wire`, `writer + admission`, `reader/walk`, and an
optional thin `peek` TU only if peek has a narrow consumer contract. Admission
stays with writer/state capacity policy, not with peek. `tp_journal_io.c` remains
the injected fault seam. No wire changes in move commits.

#### P4-04 — History codec

Last core packet. Under P1-02, P0-07 and P1-09, separate wire writer, wire
reader/shape validation and replay adapter. Preserve decode-before-apply, limits,
reverse ordering, terminal canonical validation and clone/swap atomicity.

### Phase 5 — GUI decomposition

GUI packets begin only after P1-08, P1-10 and the core boundaries they consume
are stable.

#### P5-01 — `gui_project`

1. Inventory session-shared, project-adapter-private and view-local fields, then
   encapsulate only the project-adapter-owned state in one explicit private
   object without changing public calls.
2. Extract coalescing/pending-buffer logic with explicit state input.
3. Extract typed mutation wrappers.
4. Extract recovery resolution.
5. Extract file operations and Undo/Redo adapters.

No module may write persistent project fields directly; session remains the only
mutation authority.

#### P5-02 — `gui_actions`

First encapsulate queue/preview/recovery/gesture/dialog ownership in a
`gui_actions_state` object while preserving the ownership classes from P1-10;
do not distribute them across new globals. Then extract with narrow substates or
DTOs in semantic order: deferred edit queue; selection/preview actions; dialogs
and confirm flow; pack/export actions; recovery modal; deferred side-effects
coordinator. Preserve between-frame execution and success-message ordering
exactly.

Every GUI move updates both the shipped `ntpacker-gui` source list and the
manually enumerated headless GUI test target in `apps/gui/CMakeLists.txt`.

#### P5-03 — Remaining GUI hotspots

- `main.c`: move headless parity seam, then bootstrap/resource setup; keep frame
  loop and shortcuts together if they remain one orchestration unit;
- `gui_pack.c`: separate session job adapter from export preview only after a
  single job/result state owner is explicit;
- `gui_canvas.c`: after D4 dedupe, split GPU resource lifetime from rendering
  only if both can use a narrow canvas state contract;
- `gui_view_settings.c`: keep unless a new independently reusable view domain
  appears.

### Phase 6 — Closure

1. Run full native-debug and native-release suites; three-OS CI and sanitizer.
2. Run boundary checks in Bash-capable CI.
3. Produce before/after non-gating LOC/responsibility inventory.
4. Re-run core↔clients duplication search and dependency-cycle inspection.
5. Update the simplification audit with completed/deferred/keep decisions and
   exact commit/test evidence.
6. Resume feature roadmap only after there are no open P1s and every deferred
   high-risk split names its missing oracle or concrete reason to keep.

## 9. Dependency summary

| Packet | Must precede |
|---|---|
| P0-02 first-reject fix | transaction split, GUI/CLI parity changes |
| P0-04 constraints + P0-05 animation domains | validation and operation family split |
| P1-01 project bytes | project writer/parser split |
| P1-02 history bytes + P0-07 identity + P1-09 semantics | history codec split |
| P1-03 journal bytes | journal split |
| P1-04 validation golden | validation split |
| P1-05 CLI goldens | P1-07 Save status, CLI split |
| P1-05 + P1-06 + P1-07 | filesystem and project Save physical split |
| P1-06 + P1-11 | recovery backend split |
| P1-08 client parity | broad CLI family moves |
| P1-10 GUI trace | `gui_project` / `gui_actions` moves |
| P2-01 source-path owner | project/operation decomposition involving path code |
| P2-02 D4 owner | canvas resource/render split |
| Phase 2 reassessment | every Phase 3–5 bounded packet-plan |

## 10. Explicit non-goals

- no Import/Export IR, registry, MCP, Dev API or new formats in this workstream;
- no engine submodule edits;
- no public API expansion merely to share a private helper;
- no generic service locator, allocator framework or mega-`utils` module;
- no unification of frontend parsing/presentation;
- no mass rename/format pass mixed with code movement;
- no claim that fewer LOC automatically means simpler code.
