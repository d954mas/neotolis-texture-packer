# План реализации master spec

Статус: handoff-ready backlog

Нормативный источник: `docs/ntpacker-master-spec.md`

Язык реализации: C17
Граница движка: `external/neotolis-engine/` только для чтения; требуемые изменения идут отдельным issue/PR в репозиторий движка.

## 1. Назначение и правила исполнения

Этот документ переводит master spec в проверяемые вертикальные пакеты работ. Он не переопределяет продуктовые решения master spec. При конфликте текст master spec имеет приоритет.

Следующий агент должен:

1. брать один packet целиком или явно выделенный подпакет;
2. сначала добавлять отрицательные/контрактные тесты, затем реализацию;
3. проводить все persistent mutations через `tp_operations`/`tp_session`, как только F2 станет доступен;
4. сохранять CLI и GUI тонкими клиентами одного core API;
5. не менять engine submodule напрямую;
6. прикладывать к завершённому packet перечисленные ниже completion evidence;
7. не считать ручную GUI-проверку заменой executable acceptance tests.

## 2. Текущее состояние и исходный разрыв

У проекта сильная baseline-основа:

- deterministic project v1 и snapshot primitive: `packer/include/tp_core/tp_project.h:10-25,40-41,249-271`;
- pack/raw RGBA bridge: `packer/src/tp_pack.c:161-163,224-271`;
- normalized export representation и capability-aware exporters: `packer/include/tp_core/tp_export.h:41-57,151-186,251-288`;
- Neotolis JSON и Defold builtin exporters: `packer/src/tp_export.c:165-202`;
- полный для модели v1 file-oriented CLI: `apps/cli/main.c:320-372`;
- GUI snapshot Undo/Redo и semantic-bytes dirty baseline: `apps/gui/gui_project.c:18-23,76-100,532-576`;
- async GUI pack worker: `apps/gui/gui_pack.h:90-148`, `apps/gui/gui_pack.c:157-203,285-373,522-624`;
- native tests и CI на трёх ОС: `packer/tests/CMakeLists.txt:1-150`, `.github/workflows/ci.yml:21-43,72-79`.

Главный разрыв: это shared mutable model, но ещё не canonical operation/session architecture. Структурных ID нет, sources — строки, CLI и GUI применяют mutations разными путями, GUI Undo хранит snapshots, import/package/MCP surface отсутствует:

- индексная/именная модель: `packer/include/tp_core/tp_project.h:46-137,147-239`;
- string sources: `packer/include/tp_core/tp_project.h:112-114`, `packer/src/tp_project.c:941-942,1290-1300`;
- CLI load/mutate/save: `apps/cli/cli_mutate.c:340-345,1411-1458`;
- GUI-specific mutation wrappers: `apps/gui/gui_project.c:192-355,361-527`;
- validation находится во frontend: `apps/cli/cli_validate.c:37-55,192-295`;
- registry описывает только native exporters: `packer/include/tp_core/tp_export.h:251-288`.

## 3. Зависимости и рекомендуемый порядок

```text
C0 contract spikes
  |
  v
F1 identities + tagged path sources
  |
  v
F2 operations + transactions + minimum journal
  |                    |
  |                    +------------------+
  v                                       v
B0 native import IR                  F3 live session/history/pack
  |
  v
B1 linked atlas + Extract
  |
  v
B2 format packages
  |
  v
B3 Lua + reference formats
  |
  v
A live Dev API/MCP
  |
  v
X breadth
```

F2 minimum обязателен до Extract Sprites: filesystem staging не заменяет атомарную model transaction. B0 можно разрабатывать параллельно с поздними частями F2, но нельзя публиковать B1 mutation path в обход operations.

Порядок стадий отслеживает `docs/ROADMAP.md`; при расхождении в допустимом
параллелизме действует более строгий порядок ROADMAP. В частности, стадия B2
начинается после принятия gate F3, а не параллельно ему.

## 4. Общие quality gates

Каждый packet обязан сохранять:

- `cmake --preset native-debug` и `cmake --build --preset native-debug` без warnings;
- `ctest --preset native-debug`;
- `ctest --preset native-release` перед завершением эпика;
- `scripts/check_boundaries.sh`;
- deterministic JSON/golden bytes без timestamp, pointer, locale или случайного порядка;
- structured error вместо abort для любого внешнего invalid input;
- `--json` schema fixture и exit-code test для каждого нового CLI command;
- Linux/Windows/macOS CI совместимость;
- отсутствие изменений в `external/neotolis-engine/`.

Fault injection должен быть детерминированным: allocator/file/journal/worker seams передаются через test-only hooks или dependency structs, не через случайные environment failures.

---

## 5. C0 — контрактные spikes до изменения схемы

### C0-01 — Identity, path и Unicode contract spike

**Goal / user outcome.** Зафиксировать переносимые правила identity до записи schema v2, чтобы проект, открытый на другой ОС, адресовал те же сущности и диагностировал коллизии одинаково.

**Spec refs.** §5–5.6, §15, §16, §52.1, §59 items 1–8.

**Current code delta.** `tp_project` хранит только `project_dir`, массивы и mutable names (`packer/include/tp_core/tp_project.h:129-137`); нормализация ограничена slash replacement (`packer/src/tp_project.c:62-74`); source dedupe сравнивает пути (`packer/src/tp_project.c:339-364`).

**Dependencies.** Нет.

**Ordered bounded tasks.**

1. Написать decision fixture для canonical saved-project path на Windows/POSIX, включая symlink policy, case policy и nonexistent final path.
2. Выбрать C17 реализацию random 128-bit IDs без engine-private API; определить test RNG injection.
3. Зафиксировать text encoding и Unicode NFC implementation/library boundary.
4. Определить canonical binary/string shape ID и parser rules: prefix, lowercase/uppercase acceptance, zero/invalid values.
5. Зафиксировать deterministic legacy-ID input tuple и collision fallback.
6. Определить source-key normalization fixtures: `/`, `.`, repeated separators, `..`, absolute input, NFC, preserved case, Windows reserved names, case-fold collision.
7. Создать короткий contract note рядом с тестовыми fixtures; не вводить production schema в этом packet.

**Public/schema impact.** Нет on-disk schema change. Решения становятся входом F1 public API.

**Exact tests / fault injection.** Table-driven pure tests на path/source-key/ID parse; fixed RNG bytes; invalid RNG/short-read seam; equivalent composed/decomposed Unicode; cross-platform golden vectors.

**Completion evidence.** Один утверждённый fixture corpus; native-debug tests зелёные на Windows и хотя бы одной case-sensitive CI ОС; нет platform-specific различий golden output.

**Non-goals / blockers.** Не реализовывать live ownership claim. Open blocker: конкретная NFC dependency; если требуется новая библиотека, отдельно подтвердить supply-chain/license решение.

### C0-02 — Operation, revision и semantic-state contract spike

**Goal / user outcome.** Зафиксировать wire-neutral semantic operation vocabulary и commit acknowledgement до переноса CLI/GUI.

**Spec refs.** §6–9.5, §20.2–20.3, §21, §59 items 9–19, 52.

**Current code delta.** Core mutators адресуются индексами/именами (`packer/include/tp_core/tp_project.h:147-239`); GUI считает local `s_model_ver` (`apps/gui/gui_project.c:15,162-188`); CLI сохраняет после каждого process invocation (`apps/cli/cli_mutate.c:340-345`).

**Dependencies.** C0-01 ID shape.

**Ordered bounded tasks.**

1. Составить append-only operation-kind catalog для существующих mutation families.
2. Определить typed C payloads, selector-resolution boundary и canonical ID-only form.
3. Определить transaction request/result JSON fixtures: transaction ID, expected revision, label, author, operations, structured errors.
4. Зафиксировать semantic state identity: какие persistent поля входят, какие runtime поля исключены.
5. Зафиксировать revision_conflict/invalid_revision и full-batch validation ordering.
6. Определить before/after diff requirements для create/remove/move/set.
7. Написать golden contract tests, которые пока могут использовать parser/serializer skeleton без mutation engine.

**Public/schema impact.** Новая versioned operation/transaction JSON schema; C structs остаются внутренне эволюционируемыми до F2 completion.

**Exact tests / fault injection.** Unknown operation/version/field, duplicate transaction ID, malformed ID, ambiguous selector fixture, expected revision below/above current, stable error ordering.

**Completion evidence.** Golden request/result fixtures проходят decode→encode byte comparison; все текущие CLI mutation verbs сопоставлены операциям без «raw field patch» escape hatch.

**Non-goals / blockers.** Не реализовывать journal format и network protocol. Exact public method names остаются implementation-settled согласно §59 item 52.

### C0-03 — Journal, ownership и cache policy spikes

**Goal / user outcome.** Закрыть только решения, блокирующие корректные seams, не проектируя весь Epic A заранее.

**Spec refs.** §7.1–7.2, §10.3–10.5, §16–19, §22.3, §52.3, §52.5, §60.

**Current code delta.** Recovery journal/claim отсутствуют; GUI держит один result на atlas и один preview result (`apps/gui/gui_pack.c:34-49`); engine raw add deep-copy фактически делает в `external/neotolis-engine/tools/builder/nt_builder_atlas.c:760-795`, но public header не обещает lifetime (`external/neotolis-engine/tools/builder/nt_builder.h:416-425`).

**Dependencies.** C0-01 canonical project path, C0-02 commit contract.

**Ordered bounded tasks.**

1. Определить minimum append record/checkpoint framing, checksum, truncation recovery и idempotency retention seam.
2. Определить acknowledgement boundary: apply → append → publish; rollback при append failure.
3. Определить authority-state vocabulary без реализации OS claim.
4. Определить configurable byte budgets и cache interface без выбора окончательных default values.
5. Зафиксировать Pack supersession/preview-selection policy как state table
   fixtures: одна running Pack job на session плюс один latest requested
   intent; superseded job не может сам стать authoritative preview; explicit
   selection и Undo cache hit выбирают результат по hash, не по timing.
6. Добавить adapter-level ownership test plan для raw RGBA; если lifetime нельзя закрепить без public engine contract, оформить отдельный upstream issue, не patch submodule.

**Public/schema impact.** Journal внутренний и versioned; cache config не попадает в project schema.

**Exact tests / fault injection.** Short write, append fail, torn final record, bad checksum, duplicate transaction after restart; RGBA caller buffer mutate/free immediately после `nt_builder_atlas_add_raw` и последующая проверка pack result.

**Completion evidence.** Утверждённые binary vectors/state table и исполнимый raw-ownership regression test либо ссылка на upstream issue.

**Non-goals / blockers.** Не фиксировать checkpoint cadence, production cache budgets и singular authority cutover — это открытые решения §60.

### C0-04 — Deterministic raster color/orientation spike

**Goal / user outcome.** Зафиксировать кроссплатформенные правила декодирования
raster sources в canonical RGBA8 (§60 item 7) до того, как semantic image hash
станет контрактом (B1-01 task 8, F3-03 task 1).

**Spec refs.** §10.2, §11.1, §59 item 28, §60 item 7.

**Current code delta.** Production decode для path sources сейчас делегирован
engine builder; собственный packer-side код только пишет PNG
(`packer/src/tp_export_png.c:9`). Packer-side canonical decode появится в
B0-03/B1-01, и его policy должна быть решена заранее.

**Dependencies.** Нет. Результат обязателен до B0-03, B1-01 и F3-03.

**Ordered bounded tasks.**

1. Инвентаризовать текущие decode paths (engine builder, GUI thumbnails) и
   зафиксировать различия как baseline note.
2. Выбрать policy: EXIF/orientation application, ICC/gamma handling
   (initial policy: byte-preserving, содержимое трактуется как sRGB, ICC
   transforms не применяются), grayscale/palette expansion, 16-bit→8-bit
   rounding, JPEG без alpha → A=255.
3. Подготовить golden RGBA8 decode vectors: PNG/WebP/JPEG, orientation-tagged,
   ICC-tagged, palette, grayscale, 16-bit fixtures.
4. Malformed/unsupported profile не меняет пиксельный результат policy и даёт
   structured notice.
5. Короткий contract note рядом с fixtures.

**Public/schema impact.** Нет schema change; policy становится входом
B0-03/B1-01 и semantic image hash.

**Exact tests / fault injection.** Byte-identical canonical RGBA8 golden
vectors на Linux/Windows/macOS CI; повреждённый ICC chunk; unknown EXIF
orientation value; truncated file → structured error.

**Completion evidence.** Golden decode vectors зелёные на трёх CI ОС;
policy note принят и связан из B0-03/B1-01.

**Non-goals / blockers.** Полноценный color management (ICC transforms,
wide gamut) не входит в v1.

---

## 6. F1 — stable identities и tagged path sources

### F1-00 — Production project identity boundary

**Goal / user outcome.** Saved project identity, unsaved session identity and
Save As transitions are usable by journal/recovery code before live IPC exists.

**Spec refs.** §5.1, §15–16, §59 items 1–3.

**Current code delta.** Project persistence receives paths from each frontend,
but there is no shared canonical project-identity API or temporary unsaved
session ID (`packer/include/tp_core/tp_project.h:129-137`).

**Dependencies.** C0-01 approved identity/path fixtures.

**Ordered bounded tasks.**

1. Implement a shared canonical saved-project path identity API for existing
   and not-yet-created Save As destinations.
2. Implement injectable random temporary runtime IDs for unsaved sessions.
3. Define and implement Save As identity transition as an atomic destination
   claim/key change for local session/journal consumers.
4. Keep identity out of `.ntpacker_project`; expose it through runtime/session
   DTOs only.
5. Add path move/copy, case, UNC/symlink policy and first-save fixtures from C0.

**Public/schema impact.** New core runtime identity API; no project schema field.

**Exact tests / fault injection.** Equivalent-path vectors on each OS, missing
destination parent, destination collision, RNG failure, first Save failure,
Save As rollback preserving the old identity.

**Completion evidence.** F2 journal can key both saved and unsaved sessions
without frontend-specific path logic; identity tests pass on all CI platforms.

**Non-goals / blockers.** No cross-process claim or dead-owner proof; those
remain Epic A.

### F1-01 — ID primitives и schema v2 migration

**Goal / user outcome.** Rename/reorder/save/reload больше не ломают ссылки на atlas/source/animation/target.

**Spec refs.** §5–5.1, §5.5–5.6, §54 Phase 0 items 1–2.

**Current code delta.** Schema v1 (`packer/include/tp_core/tp_project.h:40-41`); atlas/source/target не имеют ID, animation `id` одновременно имя и ссылка (`:71-91,96-137`).

**Dependencies.** F1-00, C0-01.

**Ordered bounded tasks.**

1. Добавить `tp_id128` parse/format/equality/hash и injected generator.
2. Добавить persistent ID fields atlas/source/animation/target.
3. Отделить animation ID от display/logical name; мигрировать frame references позднее в F1-03.
4. Поднять schema version и реализовать утверждённый C0 legacy-ID promotion:
   read-only load может использовать deterministic synthetic IDs, но writable
   session получает final IDs до первой mutation и сохраняет их без повторной смены.
5. Promotion всех structural IDs и references выполнять атомарно; save failure
   не создаёт частично remapped state.
6. Сериализовать IDs детерминированно; запретить duplicate/malformed IDs на load/validate.
7. Обновить inspect JSON и project round-trip fixtures.

**Public/schema impact.** `.ntpacker_project` schema v2; новые ID fields. Old files читаются, новые старым binary не поддерживаются.

**Exact tests / fault injection.** v1 repeated read-only load stable synthetic
IDs; writable attach promotes once; first save persists those exact IDs; save
failure не remap-ит IDs; RNG failure structured error; duplicate/malformed IDs;
rename/reorder/remove/Undo-oracle/save/reload identity invariants.

**Completion evidence.** Migration golden v1→v2; byte-identical second save; inspect показывает IDs; все v1 fixtures продолжают загружаться.

**Non-goals / blockers.** Persistent `project_id` запрещён. Project identity — canonical path, не поле файла.

### F1-02 — Tagged source schema и normalization

**Goal / user outcome.** Source становится адресуемой сущностью и готов к linked atlas без параллельной модели.

**Spec refs.** §5.2–5.3, §11, §42, §54 Phase 0 item 6, §59 items 5–8.

**Current code delta.** `char **sources` (`packer/include/tp_core/tp_project.h:112-114`) и string-array serializer (`packer/src/tp_project.c:941-942,1290-1300`).

**Dependencies.** F1-01, C0-01 normalization fixtures.

**Ordered bounded tasks.**

1. Ввести `tp_project_source {id, kind, ...}` с kinds `file` и `folder`.
2. Мигрировать v1 strings в tagged path records, сохранив порядок и semantics.
3. Централизовать project-relative path canonicalization и source-root escape checks.
4. Перевести scan/input assembly на tagged records.
5. Добавить exact и case-fold collision validation.
6. Перевести CLI inspect/add/remove и GUI source list на source ID внутри core; human selectors оставить adapter-ам.
7. Удалить production-path dependence от source array index.

**Public/schema impact.** `sources` становится массивом objects; kind vocabulary append-only.

**Exact tests / fault injection.** File/folder round-trip; duplicate paths; `..`, absolute escape, repeated slash, NFC, preserved case, reserved-name portability; missing source остаётся runtime/model finding, не load failure.

**Completion evidence.** Старые fixtures мигрируют; pack/export bytes baseline не меняются; source rename/reorder сохраняет source ID.

**Non-goals / blockers.** `atlas` kind добавляется в B1. Watchers и thumbnails не входят.

### F1-03 — Deterministic sprite IDs и canonical selectors

**Goal / user outcome.** Любой frontend однозначно выбирает одну и ту же sprite/entity без угадывания.

**Spec refs.** §5.2–5.4, §5.6, §59 items 5–7.

**Current code delta.** Sprite overrides keyed mutable `name` (`packer/include/tp_core/tp_project.h:46-64,181-204`); animations хранят frame names (`:69-80,215-224`); CLI atlas filter ищет name (`apps/cli/cli_pack.c:415-436`).

**Dependencies.** F1-02.

**Ordered bounded tasks.**

1. Реализовать stable hash с versioned algorithm tag для `source_id + normalized source-local key`.
2. Построить resolved sprite index `{sprite_id, source_id, key, logical/export name}`.
3. Перевести sparse sprite records и animation frame references на sprite IDs, сохранив display names.
4. Реализовать selector resolver для ID/name/path/compound selector.
5. Возвращать candidate list при ambiguity; никогда не выбирать первый match.
6. Добавить orphaned sparse records для исчезнувших source keys.
7. Расширить validate/inspect structured output.

**Public/schema impact.** Persisted sprite records/animation frames содержат canonical sprite IDs/source keys; derived sprites без metadata не обязаны сериализоваться.

**Exact tests / fault injection.** Rename logical name не меняет ID; external file rename = old missing + new ID; same filename in two sources ambiguous by name, resolvable compound selector; orphan reactivates при возврате key; collision injection в test hash seam.

**Completion evidence.** Все references переживают atlas/source reorder и save/reload; ambiguity JSON содержит стабильный candidate list.

**Non-goals / blockers.** Neotolis не переименовывает physical files.

---

## 7. F2 — typed transactions и minimum durable commit

### F2-01 — Typed operation engine

**Goal / user outcome.** Core имеет один semantic mutation vocabulary и
validation/error semantics, готовые для всех клиентов.

**Spec refs.** §4.1–4.2, §6–6.2, §7, §54 Phase 0 item 4.

**Current code delta.** Low-level mutators (`packer/include/tp_core/tp_project.h:147-239`), CLI-specific rules (`apps/cli/cli_mutate.c:1411-1458`) и GUI wrappers (`apps/gui/gui_project.c:192-527`).

**Dependencies.** F1-03, C0-02.

**Ordered bounded tasks.**

1. Добавить `tp_operation` tagged union и payload validation.
2. Реализовать ID-only apply для всех baseline mutations.
3. Создать shared selector-to-operation builders для CLI/MCP convenience layer.
4. Перенести range/name/exporter/reference validation в core.
5. Возвращать structured status ID + context, не только prose.

**Public/schema impact.** Versioned operation schema; append-only operation/status IDs.

**Exact tests / fault injection.** По одному forward/error test на каждый
operation kind; invalid ID/type/range/reference; allocator fail до apply не
меняет model.

**Completion evidence.** Operation catalog покрывает все baseline mutations
без raw-patch escape hatch; deterministic core fixtures зелёные.

**Non-goals / blockers.** Save/Undo/pack/export не являются model operations.

### F2-02 — Atomic transactions, revision и semantic dirty

**Goal / user outcome.** Multi-step agent/human edit либо применяется целиком одной Undo entry, либо не меняет проект.

**Spec refs.** §7–8, §9.1, §59 items 12–19.

**Current code delta.** GUI `s_model_ver` local (`apps/gui/gui_project.c:15,162-188`); dirty уже сравнивает serialized current/saved bytes (`:88-100`), но canonical revision отсутствует.

**Dependencies.** F2-01.

**Ordered bounded tasks.**

1. Добавить transaction validate-all phase на immutable/current model view.
2. Применять ordered operations к transactional clone или reversible staging state.
3. На успех increment revision ровно один раз и emit one change record.
4. Реализовать expected_revision semantics.
5. Вычислять semantic state identity отдельно от revision; runtime fields исключить.
6. Сделать transaction ID idempotency interface с pluggable retention store.

**Public/schema impact.** Transaction request/result schema и revision в inspect/session snapshot; revision не обязана сохраняться в project file между обычными sessions.

**Exact tests / fault injection.** Failure operation N из batch не оставляет операций 1..N-1; expected below/above; retry same transaction; edit→save→edit→inverse becomes clean при большей revision; allocator failure в каждой staging точке.

**Completion evidence.** Atomicity/property suite; CLI batch JSON golden; no partial serializer diff after any injected failure.

**Non-goals / blockers.** CRDT/auto-merge запрещены; Save revision не меняет.

### F2-03 — Semantic diff/inverse и snapshot oracle

**Goal / user outcome.** Точный Undo одной transaction без хранения full snapshot как штатного механизма.

**Spec refs.** §9–9.5, §59 items 15–17.

**Current code delta.** Snapshot stack: `apps/gui/gui_history.c:23-24,111-160`; buffer reload Undo: `apps/gui/gui_project.c:536-576`; `tp_project_save_buffer` уже oracle primitive (`packer/include/tp_core/tp_project.h:263-271`).

**Dependencies.** F2-02.

**Ordered bounded tasks.**

1. Для каждого operation kind записывать достаточные before/after data и ordering position.
2. Реализовать exact inverse и redo replay.
3. Добавить transaction label/author metadata.
4. Создать oracle harness A→forward→B→inverse→A и сравнить snapshot restoration.
5. Поддержать redo-branch discard после новой transaction.
6. Пока не удалять legacy snapshot implementation; спрятать за test/debug adapter.

**Public/schema impact.** Internal semantic diff records; visible history DTO позднее в F3.

**Exact tests / fault injection.** Create/remove/set/move/reference cascade; batch of 100 animations = one inverse; inverse apply allocation failure rollback; byte-identical A; stale/unknown entity in corrupted diff.

**Completion evidence.** Oracle suite покрывает каждый operation kind и совпадает со старым snapshot result.

**Non-goals / blockers.** Undo stack не обязан переживать normal reopen/crash.

### F2-04 — Minimum recovery journal для side-effect workflows

**Goal / user outcome.** Commit не объявляется успешным, если его recovery record не записан; Extract может безопасно связать published files с project transaction.

**Spec refs.** §7.1–7.2, §22.3, §44.2–44.4, §59 items 19, 46–49, 52.

**Current code delta.** Journal отсутствует; save пишет файл напрямую (`packer/src/tp_project.c:1023-1084`).

**Dependencies.** F1-00, F2-03, C0-03.

**Ordered bounded tasks.**

1. Реализовать versioned append record и checkpoint reader/writer.
2. Commit order: validate/stage model → apply → append → publish event/result.
3. При append failure exact rollback и отсутствие acknowledgement.
4. Восстанавливать current committed project state и retained transaction IDs.
5. Добавить explicit side-effect prepare/publish/abort coordinator interface для B1.
6. Документировать corruption result и safe fallback; не угадывать повреждённые records.

**Public/schema impact.** Local journal, keyed canonical project path для
saved project и temporary runtime session ID для unsaved; не входит
`.ntpacker_project`.

**Exact tests / fault injection.** Short write at every byte boundary, torn tail, checksum mismatch, append fail after model apply, checkpoint+journal replay, duplicate retry after restart, stale journal for moved project.

**Completion evidence.** Crash/fault suite доказывает: любой acknowledged transaction восстановим и не дублируется; неacknowledged transaction невидим.

**Non-goals / blockers.** Не обещать per-operation `fsync` или power-loss durability; exact compaction/cadence остаются configurable/open.

### F2-05 — CLI/GUI operation adapters и production cutover

**Goal / user outcome.** Ordinary CLI и GUI дают одинаковые project
results/diagnostics через core transactions, но сохраняют разные
runtime boundaries: CLI файловый, GUI journal-backed live.

**Spec refs.** §4.1–4.2, §7.1, §14.2, §59 items 9–19.

**Current code delta.** CLI и GUI применяют mutation rules разными
wrappers (`apps/cli/cli_mutate.c:1411-1458`,
`apps/gui/gui_project.c:192-527`).

**Dependencies.** F2-01–F2-04.

**Ordered bounded tasks.**

1. Перевести CLI mutation families на transaction API, сохранив
   one-shot load→commit→save semantics без live journal ownership.
2. Добавить transaction-aware CLI batch input; одиночный verb
   оборачивать одной transaction.
3. Перевести GUI mutations/Undo/Redo на operation+transaction+journal
   commit visibility.
4. Перенести frontend validation rules в core и оставить только
   selector/argument parsing и rendering.
5. Добавить boundary tests против прямых persistent field writes
   и duplicated validation в `apps/`.
6. Удалить production snapshot-mutation path только после parity;
   snapshot oracle остаётся в tests.

**Public/schema impact.** Текущие CLI schemas сохраняются либо
мигрируют versioned; GUI получает shared history/change DTO.

**Exact tests / fault injection.** CLI/GUI operation fixture parity, journal
append failure in GUI, CLI save failure, selector ambiguity, boundary seeded
violation, existing CLI e2e and GUI selftest.

**Completion evidence.** Boundary script ловит parallel mutation/validation
rules; одинаковый transaction fixture даёт byte-identical project и
structured diagnostics в CLI/GUI.

**Non-goals / blockers.** Ordinary CLI не становится live session и не
получает MCP ownership semantics.

---

## 8. B0 — native atlas import foundation

### B0-01 — Versioned Import IR и detection contract

**Goal / user outcome.** Atlas можно безопасно распознать/инспектировать без добавления в проект.

**Spec refs.** §29–31, §38–41, §49 Detection/Import, §50 B0.

**Current code delta.** `tp_pack_read_*` читает внутренний `.ntpack` (`packer/src/tp_pack_read.c:558-688`); importer API отсутствует. D4 helpers уже есть (`:25-45,350`).

**Dependencies.** F1-03; может идти параллельно F2-03/04.

**Ordered bounded tasks.**

1. Определить immutable, versioned Import IR: pages, regions, aliases, polygon, transform, trim/sourceSize, pivot, slice9, animations, notices.
2. Определить importer descriptor, exact markers, signatures, bounded probe result.
3. Реализовать candidate ranking без silent selection при ambiguity.
4. Добавить safe companion-file resolver.
5. Добавить `atlas detect` и `atlas inspect` с versioned JSON.
6. Вынести reusable D4 geometry functions из private pack reader без engine-type leakage.

**Public/schema impact.** Новый import IR/API и CLI schemas; project schema пока не меняется.

**Exact tests / fault injection.** Empty/huge/malformed JSON, duplicate keys/regions, ambiguous signatures, explicit override, path traversal companion, read truncation, cancellation/probe budget.

**Completion evidence.** Fixture candidates стабильны; ambiguous input требует выбора; inspect не пишет файлы/проект.

**Non-goals / blockers.** Не импортировать TexturePacker project files; probe только suggestion.

### B0-02 — Native Neotolis importer

**Goal / user outcome.** Собственный full-fidelity export можно прочитать обратно как canonical Import IR.

**Spec refs.** §41, §47.1, §48, §49 Import.

**Current code delta.** Writer существует (`packer/src/tp_export_json_neotolis.c:384+`); golden export fixtures существуют (`packer/tests/CMakeLists.txt:94-100`).

**Dependencies.** B0-01.

**Ordered bounded tasks.**

1. Добавить explicit Neotolis format marker в новый export schema с backward detection fixture для текущего формата.
2. Реализовать builtin importer через Import IR.
3. Разрешать multipage PNG companions только через safe resolver.
4. Восстанавливать aliases, animation, pivot, slice9, polygons, exact D4.
5. Выдавать structured notices для отсутствующего sourceSize/legacy metadata.
6. Добавить import/export round-trip oracle.

**Public/schema impact.** Version marker в Neotolis JSON; backward read policy должна быть fixture-pinned.

**Exact tests / fault injection.** Все 8 D4, concave polygon, trim, multipage, alias, animation, fractional pivot, missing/corrupt page, dimension overflow, duplicate visible names.

**Completion evidence.** Export→Import IR semantic equality на всех `tp_fixtures`; deterministic diagnostics.

**Non-goals / blockers.** Не сохранять foreign polygon как packing silhouette для нового pack.

### B0-03 — Region materializer и extract preflight

**Goal / user outcome.** Любой imported region корректно восстанавливается в full-size RGBA8 PNG plan без записи файлов.

**Spec refs.** §42–44.6, §49 Extract Sprites.

**Current code delta.** Raw RGBA pack path есть (`packer/src/tp_pack.c:161-163,271`); shared PNG export helper пишет atlas pages, но region extraction отсутствует (`packer/include/tp_core/tp_export.h:231-248`).

**Dependencies.** B0-02, C0-03 ownership contract.

**Ordered bounded tasks.**

1. Реализовать rectangle/polygon inverse-D4 sampling в deterministic RGBA8.
2. Восстанавливать full `sourceSize`, trim offset и transparent canvas.
3. Сформировать immutable extract plan с region→path mapping и metadata transfer.
4. Нормализовать output paths, aliases, case collisions и conflict policy.
5. Добавить `atlas extract --dry-run --json`; запись остаётся B1.
6. Встроить size/output-count limits.

**Public/schema impact.** Extract plan/report schema; никаких project mutations.

**Exact tests / fault injection.** Pixel oracle для 8 D4 и polygon mask, pixel-art no-resample, negative trim offsets, no sourceSize notice, alias duplicate bytes, traversal/absolute/Windows reserved path, conflict list completeness, huge allocation rejection.

**Completion evidence.** Dry-run перечисляет полный deterministic artifact map; ни один fault test не создаёт output.

**Non-goals / blockers.** Никакого automatic suffix rename; primary UI не спрашивает Original/Trimmed.

---

## 9. B1 — linked atlas sources и transactional Extract Sprites

### B1-01 — Atlas source kind и runtime source state

**Goal / user outcome.** Existing atlas используется как read-only live source, не материализуя derived regions в project JSON.

**Spec refs.** §11–11.5, §40.3, §42, §49 Linked sources, §50 B1.

**Current code delta.** Manual scan/stat cache (`apps/gui/gui_scan.c:25-48,72-116`) и F5 fingerprint (`apps/gui/gui_actions.c:657-758`); watcher/generation/status отсутствуют.

**Dependencies.** F1-03, B0-02, F2-02, C0-04 color/orientation policy.

**Ordered bounded tasks.**

1. Добавить tagged source kind `atlas` с path, selected format ID и import options.
2. Derived regions строить on demand с deterministic sprite IDs.
3. Запретить region-level add/remove/rename mutation для linked source.
4. Ввести runtime source status/generation/error store вне project model.
5. Реализовать verify-on-open/Refresh/Pack/Export/Extract.
6. Добавить platform watcher adapter как runtime event seam без direct
   project mutation; routing через authoritative session queue выполняет F3-01.
7. Передавать materialized RGBA обычному `tp_pack_input`.
8. Вычислять semantic image hash из width, height и canonical RGBA8
   pixels; file bytes/mtime используются только как read/decode hints.
9. Добавить typed `source.format.set`/`Change Format`: новый
   importer и options коммитятся только после успешной current-read
   validation; failure сохраняет old format/options.

**Public/schema impact.** Schema v2 source variant `atlas`; runtime state не сериализуется.

**Exact tests / fault injection.** Changed/corrupt/missing metadata/page; selected
format не меняется после refresh; Change Format success/failure with
old-format preservation; watcher burst/coalescing; revision/dirty/history не
меняются; identical RGBA with changed mtime gives same semantic hash;
changed pixels change it; stale full pixels не используются.

**Completion evidence.** Linked Neotolis fixture pack-ится byte-equivalent extracted folder fixture; source runtime errors структурированы.

**Non-goals / blockers.** Нет continuous auto-pack; нет persisted derived regions.

### B1-02 — Extract staging, publish и project transaction

**Goal / user outcome.** «Extract Sprites» гарантирует zero changes до
publication boundary. После успешной publication model replacement либо
коммитится одной Undoable transaction, либо возвращается explicit
recovery state с уже опубликованными файлами и без скрытого model commit.

**Spec refs.** §43.4, §44–44.6, §49 Extract Sprites, §59 items 46–49.

**Current code delta.** CLI dry-run infrastructure есть для pack (`apps/cli/CMakeLists.txt:120-129`), но side-effect coordinator отсутствует.

**Dependencies.** B0-03, B1-01, F2-02, F2-04, F2-05. Это жёсткий gate.

**Ordered bounded tasks.**

1. Re-read current atlas и пересчитать complete preflight.
2. Записать все PNG во staging directory на том же publish boundary, где возможно.
3. Проверить conflicts повторно перед publish; default fail, overwrite только explicit.
4. Publish files; затем commit transaction `source.replace` + transferred metadata.
5. При model/journal failure сообщить recovery state; не удалять published user files автоматически.
6. Undo восстанавливает только project source/metadata, не удаляет files.
7. Добавить CLI command и GUI «Extract Sprites» с единственным primary folder prompt.
8. Подключить CLI/GUI `Change Format` к B1-01 typed operation;
   показывать validation diagnostics до commit.

**Public/schema impact.** External side-effect command/report; `source.replace` operation.

**Exact tests / fault injection.** Fail на каждом staged file, publish rename, conflict appearing between preflight/publish, overwrite false/true, journal append fail after publish, alias files, read-only output, disk full, Undo after user edits extracted PNG.

**Completion evidence.** Fault matrix документирует exact files/model state для каждой точки; default conflict оставляет zero changes; успешный run даёт полный mapping и одну Undo entry.

**Non-goals / blockers.** Не удалять файлы Undo; не suffix-rename; не prompt per file.

### B1-03 — GUI source diagnostics и thumbnail LRU

**Goal / user outcome.** Пользователь видит current/error state и preview без удержания всех full-size images в памяти.

**Spec refs.** §11.3–11.5, §42.2–42.3, §59 items 26–27,31–32.

**Current code delta.** Canvas имеет single decode cache key (`apps/gui/gui_canvas.h:45`); source list и manual refresh уже существуют, отдельного thumbnail budget LRU нет.

**Dependencies.** B1-01. Session queue integration не входит в этот packet.

**Ordered bounded tasks.**

1. Добавить lazy aspect-preserving thumbnail generation, max 256 physical px.
2. Разделить CPU decoded thumbnail и GPU texture byte budgets.
3. Показывать loading/current/error/generation и target importer selection.
4. Evict безопасно без влияния на pack source semantics.
5. Выдавать immutable runtime status DTO, который F3 позже
   доставит через session/history surfaces.

**Public/schema impact.** Только runtime/UI DTO и local config; project schema не меняется.

**Exact tests / fault injection.** Budget eviction order, GPU upload failure, corrupt image, rapid replace/delete, DPI max-side, no full-image retention assertion через accounting seam.

**Completion evidence.** Deterministic cache unit tests и GUI selftest state fixtures; memory counters не превышают budgets.

**Non-goals / blockers.** Full decoded source-image general LRU запрещён v1.

---

## 10. F3 — live session, semantic history и pack-result behavior

### F3-01 — Serialized `tp_session` queue

**Goal / user outcome.** GUI edits, Undo/Redo, refresh и будущий MCP наблюдают один authoritative ordered commit stream.

**Spec refs.** §4.6, §6.1, §7.1, §9.1–9.3, §21, §59 items 18–19.

**Current code delta.** GUI pending flags работают на main thread (`apps/gui/gui_actions.c:33-47`), pack worker публикуется polling-ом (`apps/gui/gui_pack.c:522-624`), но general session abstraction отсутствует.

**Dependencies.** F2-05.

**Ordered bounded tasks.**

1. Ввести `tp_session` owner thread/queue и immutable snapshot DTO.
2. Разделить model operations, session commands, derived jobs и side effects.
3. Route GUI mutations/Undo/Redo/save/refresh через queue.
4. Worker completion публиковать command/event через queue.
5. Route watcher/runtime-source events через queue без revision/dirty mutation.
6. Добавить subscribe sequence/revision и full resync snapshot.
7. Запретить worker/frontends direct authoritative model mutation boundary tests.

**Public/schema impact.** In-process session API и event DTO.

**Exact tests / fault injection.** Concurrent producer ordering, callback reentrancy, queue OOM, worker completion after Undo/save/close, shutdown drain, event subscriber disconnect.

**Completion evidence.** Thread sanitizer/synchronization tests где доступны; deterministic event transcript; GUI baseline e2e зелёный.

**Non-goals / blockers.** Network transport и ownership handoff входят A.

### F3-02 — Shared semantic history и checkpoints

**Goal / user outcome.** Один Ctrl+Z отменяет целую human/agent transaction; save виден checkpoint-ом, но не Undo step.

**Spec refs.** §8–9.5, §54 Phase 2 items 2–4.

**Current code delta.** Snapshot stack и coalescing (`apps/gui/gui_history.c:111-160`), GUI buffer reload (`apps/gui/gui_project.c:536-576`).

**Dependencies.** F3-01, F2-03.

**Ordered bounded tasks.**

1. Хранить semantic transaction diffs и history position в session.
2. Реализовать Undo/Redo как новые transactions/revisions.
3. Добавить non-Undoable Save checkpoint и runtime refresh entries.
4. Сохранять history при GUI/MCP ownership transfer; reset normal reopen.
5. Перевести History panel на shared DTO.
6. Оставить snapshot oracle только в tests/debug и удалить production stack после полного покрытия.

**Public/schema impact.** History DTO; on-disk project без audit log.

**Exact tests / fault injection.** New edit after Undo drops redo; save failed no checkpoint; ownership attach preserves depth; normal reopen resets; corrupted inverse blocks atomically; 100-op transaction one entry.

**Completion evidence.** Snapshot oracle suite + GUI shortcuts + session transcript tests; revision строго возрастает.

**Non-goals / blockers.** Crash recovery full Undo stack не восстанавливает.

### F3-03 — Pack hash, freshness и result cache

**Goal / user outcome.** После Undo предыдущий preview появляется сразу из cache; завершившийся старый pack остаётся видимым и честно помечается stale.

**Spec refs.** §10–10.5, §54 Phase 2 items 5–7, §59 items 20–32.

**Current code delta.** Один result slot на atlas (`apps/gui/gui_pack.c:34-49`), snapshot compare `model_changed` (`:272-285`), refresh epoch latch (`apps/gui/gui_actions.c:54-60,758,807,905`).

**Dependencies.** F3-01, C0-03 cache interface, B1-01.

**Ordered bounded tasks.**

1. Определить canonical `pack_input_hash` из semantic settings, normalized source RGBA dimensions/pixels и target adaptation inputs.
2. Snapshot jobs получают hash и immutable inputs.
3. Хранить latest completed preview независимо от current hash.
4. Реализовать dedicated byte-budget LRU для inactive compressed results.
5. Выбирать authoritative preview по explicit selection/completion sequence, не timing race.
6. На Undo/Redo искать cache hit; на miss оставлять stale, не auto-pack.
7. Снять GUI-local boolean/epoch semantics после parity tests.

**Public/schema impact.** Runtime hash/result IDs и pack event/report; hash не обязан сохраняться в project.

**Exact tests / fault injection.** Two concurrent-sequence completions, edit during pack, source refresh during pack, Undo cache hit/miss, eviction exact bytes, corrupt compressed cache entry, cancellation, identical pixels with changed mtime unchanged hash, changed RGBA changes hash.

**Completion evidence.** Deterministic state-machine tests; UI показывает current/stale корректно; no auto-pack assertions.

**Non-goals / blockers.** Cache memory-only; timestamps/raw bytes/mtime не являются semantic hash.

---

## 11. B2 — unified format package system

### B2-01a — Descriptor, versions и capability contract

**Goal / user outcome.** Builtin и external formats могут описываться
одним versioned descriptor и exact capability vocabulary.

**Spec refs.** §29–37, §49 Package system/Export, §50 B2.

**Current code delta.** `tp_exporter` содержит id/label/extension/caps/write (`packer/include/tp_core/tp_export.h:251-288`); два hardcoded builtins (`packer/src/tp_export.c:165-202`); project target хранит exporter ID/path/enabled (`packer/include/tp_core/tp_project.h:85-91`).

**Dependencies.** F1-03, F2-05, B0-02, B1-02, F3-03 (stage gate по ROADMAP: B2 после F3).

**Ordered bounded tasks.**

1. Определить manifest descriptor с отдельными manifest/package/API/data versions и profiles.
2. Расширить capability vocabulary и canonical conventions fixtures.

**Public/schema impact.** Versioned descriptor, capability и Export IR contracts;
project target пока не мигрирует.

**Exact tests / fault injection.** Unsupported API/manifest, unknown data
version/profile/option, invalid D4/geometry/metadata modes, canonical descriptor
encoding and fixture-derived capability vectors.

**Completion evidence.** Descriptor/capability fixtures утверждены без
output-byte и project-schema changes.

**Non-goals / blockers.** Не мигрировать builtins/targets/reports в этом
packet.

### B2-01b — Builtin, target schema и report migration

**Goal / user outcome.** Current Neotolis/Defold formats и project targets
переходят на descriptor contract без output drift; report называет exact
toolchain.

**Spec refs.** §30–37, §49 Package system/Export, §50 B2.

**Current code delta.** Two hardcoded builtins
(`packer/src/tp_export.c:165-202`); project target хранит exporter ID/path/enabled
(`packer/include/tp_core/tp_project.h:85-91`).

**Dependencies.** B2-01a.

**Ordered bounded tasks.**

1. Встроить manifests Neotolis/Defold и адаптировать existing
   writer/importer через общий handler contract.
2. Мигрировать target schema к `format_id`, concrete data version,
   optional profile/options.
3. Записывать package origin/version и algorithm profile в export
   report, не project.
4. Сохранить target-specific preflight/loss notices.

**Public/schema impact.** Target schema migration и versioned export report.

**Exact tests / fault injection.** Migration unknown/unsupported
format/data/profile, capability adaptation, deterministic report sans timing,
builtin descriptor-level parity с прежними golden files.

**Completion evidence.** Старые projects мигрируют; Neotolis/Defold output
bytes unchanged для эквивалентного profile; manifests видны `format inspect`.

**Non-goals / blockers.** Project не pin-ит package/API/manifest version.

### B2-02 — Discovery, `.ntformat` и duplicate policy

**Goal / user outcome.** Project-local/global format packages обнаруживаются предсказуемо и несовместимый/duplicate package никогда не запускается случайно.

**Spec refs.** §31–34, §40, §48–49.

**Current code delta.** Process-local `tp_exporter_register` без origin/discovery (`packer/src/tp_export.c:227-239`).

**Dependencies.** B2-01b.

**Ordered bounded tasks.**

1. Реализовать development directory loader и deterministic `.ntformat` archive reader.
2. Добавить fixed discovery locations и explicit package path CLI.
3. Валидировать manifest до handler load; reject duplicate IDs с перечислением origins.
4. Добавить project-local origin и deterministic ordering.
5. Реализовать declarative signatures/detection поверх unified descriptors.
6. Добавить `format list/inspect/validate/test`.

**Public/schema impact.** `.ntformat` manifest/archive contract и CLI JSON schemas.

**Exact tests / fault injection.** Zip traversal/bomb/duplicate entries, bad UTF-8, duplicate format IDs, incompatible version, missing handler, discovery order permutation, unreadable package, project-local/global collision.

**Completion evidence.** Same registry listing на трёх ОС; malicious archives не пишут вне staging и не исполняются.

**Non-goals / blockers.** Marketplace/native DLL plugins не входят.

### B2-03 — Target preview и package conformance harness

**Goal / user outcome.** Пользователь/агент видит adaptation/loss до export, а package author получает воспроизводимый test harness.

**Spec refs.** §37.1, §48–49.

**Current code delta.** Existing prediction и preview hooks: `packer/src/tp_export.c:279-335`, `apps/gui/gui_pack.h:63-78`; текущие tests разнесены по exporter-ам (`packer/tests/CMakeLists.txt:94-140`).

**Dependencies.** B2-02.

**Ordered bounded tasks.**

1. Обобщить preflight/report на format/data/profile/options.
2. Показывать target-specific preview/diagnostics в GUI до export.
3. Реализовать package fixture layout `tests/export|import|detect`.
4. Прогонять builtin packages через тот же descriptor-level harness.
5. Проверять declared artifact list и deterministic output.

**Public/schema impact.** Unified conformance report schema.

**Exact tests / fault injection.** Missing/extra artifact, nondeterministic handler detector, all transforms, multipage, alias, animation, malformed fixture, cancellation.

**Completion evidence.** Builtins и sample external package проходят один `format test`; GUI diagnostics совпадают с CLI JSON.

**Non-goals / blockers.** GUI package editor не нужен.

---

## 12. B3 — sandboxed handlers и reference formats

### B3-01 — Lua/template contract spike и sandbox

**Goal / user outcome.** External package запускается автоматически, но не имеет OS/network/process/native-module access и не может оставить partial output.

**Spec refs.** §32, §45–46.4, §49 Lua, §52.4, §59 items 33–36.

**Current code delta.** External execution runtime отсутствует; writer contract вызывает native function pointer напрямую (`packer/include/tp_core/tp_export.h:258-275`).

**Dependencies.** B2-03. Concrete template syntax остаётся open decision до spike approval.

**Ordered bounded tasks.**

1. Реализовать embedding принятого решения docs/decisions/0001 (PUC Lua 5.5,
   latest patch, официальные багфиксы, запрет binary chunks; откат на 5.4.x
   только при доказанном потоке багов в GC/hooks) и выбрать mechanically
   constrained template syntax.
2. Определить immutable IR bindings и staged artifact writer API.
3. Отключить OS/io/package/debug/native modules и dynamic loading.
4. Ввести memory, instruction, output-count/bytes limits и hook cancellation.
5. Добавить JSON parse/emit и bounded binary reader/writer host services.
6. Protected-call errors преобразовывать в structured package diagnostics.
7. Публиковать outputs только после handler success и declared-list validation.

**Public/schema impact.** Lua/template handler API version входит format API; template syntax versioned.

**Exact tests / fault injection.** Infinite loop, recursion/memory bomb, output bomb, forbidden module/global, traversal, malformed binary, cancel at each hook, exception after N artifacts, nondeterminism fixture.

**Completion evidence.** Sandbox escape suite зелёный на трёх ОС; ни один failure не публикует partial output и не мутирует project.

**Non-goals / blockers.** No trust prompts; no native modules; security review обязателен перед automatic execution release.

### B3-02 — TexturePacker JSON Hash / Pixi reference package

**Goal / user outcome.** Пользователь импортирует/экспортирует распространённый JSON Hash atlas с trim/rotation/metadata/animations.

**Spec refs.** §47.2, §48–49, §50 B4.

**Current code delta.** Нет external reference package; generic Import/Export IR появятся B0/B2.

**Dependencies.** B3-01.

**Ordered bounded tasks.**

1. Реализовать format family с generic/Pixi/Phaser profiles.
2. Добавить declarative detection и bounded probe для ambiguous JSON.
3. Реализовать import/export trim, 90° rotation, anchor/pivot, 9-slice, animations.
4. Добавить explicit loss/adaptation notices для unsupported combinations.
5. Поставить package fixtures и round-trip tests.

**Public/schema impact.** External package manifest/profiles; core schema не расширять ad hoc.

**Exact tests / fault injection.** Реальные минимальные fixtures разных data versions/profiles, ambiguous JSON, missing page, rotated trim, animation aliases, unknown extension fields.

**Completion evidence.** Package проходит общий `format test`; imported sprites можно link/extract/repack/export в Neotolis.

**Non-goals / blockers.** Не импортировать TexturePacker project settings.

---

## 13. A — live automation и AI

### A-01 — Session host и canonical-path claim

**Goal / user outcome.** Для одного saved project существует ровно одна authoritative live session; ordinary CLI не создаёт скрытую вторую копию.

**Spec refs.** §13–19, §26 Session/Startup, §59 items 1–3,9–11,50.

**Current code delta.** GUI хранит process-global project state (`apps/gui/gui_project.c:12-23`); CLI всегда file-oriented (`apps/cli/main.c:320-372`).

**Dependencies.** F3-03, F2-05, B3-02, C0-03 ownership spike.

**Ordered bounded tasks.**

1. Подключить production identity API F1-00 к session registry/claim.
2. Реализовать platform claim/registry abstraction с owner proof metadata.
3. Блокировать mutating ordinary CLI `project_live`; explicit offline force требует явного flag/report.
4. Поддержать GUI open/close claim lifecycle без headless ownership handoff.
5. Добавить external project modification detection перед save.

**Public/schema impact.** Local claim/recovery records, не project file; new CLI error/status IDs.

**Exact tests / fault injection.** Two processes race claim, stale/dead claim,
moved project path, Save As identity transition, GUI crash, PID reuse defense,
offline force.

**Completion evidence.** Process-level tests доказывают single authority; ordinary CLI никогда молча не перезаписывает live project.

**Non-goals / blockers.** Exact dead-owner proof и singular cutover требуют утверждённой C0 state machine.

### A-02 — Local Dev API и mirror synchronization

**Goal / user outcome.** External controller получает snapshot, commits transactions и наблюдает ordered events без прямого доступа к model memory.

**Spec refs.** §20–22, §26 Transactions/Recovery.

**Current code delta.** Transport/API отсутствуют; session event seam появится F3-01.

**Dependencies.** A-01, F3-01, F2-04.

**Ordered bounded tasks.**

1. Выбрать local-only transport из spec constraints и versioned framing.
2. Реализовать handshake/capabilities/session identity.
3. До открытия mutation endpoint применить minimum authorization gate: global
   mode, project decision, Ask/Allow/Deny, one controller, explicit replacement,
   revoke и reconnect rules keyed canonical path.
4. Реализовать snapshot, canonical transaction endpoint и event subscription.
5. Добавить sequence-gap detection и resync.
6. Не публиковать commit до journal acknowledgement.

**Public/schema impact.** Versioned Dev API protocol; operation schema переиспользуется без второго mutation vocabulary.

**Exact tests / fault injection.** Ask/deny/allow before mutation, revoke
mid-request, explicit controller replacement, partial frame, oversized request,
disconnect before/after commit, duplicate transaction, slow reader/backpressure,
sequence gap, stale revision, reconnect, malformed UTF-8/JSON.

**Completion evidence.** Black-box client transcript tests; recovered acknowledged commit виден после host restart ровно один раз.

**Non-goals / blockers.** Remote/network exposure запрещён; transport не security boundary против local same-user attacker.

### A-03 — `ntpacker mcp` mode

**Goal / user outcome.** Один AI agent может inspect/validate/edit/pack/export/extract текущую live session компактным MCP surface.

**Spec refs.** §14–15, §24–27 A3.

**Current code delta.** CLI binary уже имеет one-shot dispatch и versioned JSON (`apps/cli/main.c:48-75,154-207`), но MCP mode отсутствует.

**Dependencies.** A-02, F3-03, B1-02, B2-03, B3-02.

**Ordered bounded tasks.**

1. Добавить отдельный `mcp` mode в тот же binary, не смешивая one-shot CLI semantics.
2. Реализовать unbound startup и explicit project selection/new project rules.
3. Экспортировать compact tools поверх session snapshot/transaction/jobs/side effects.
4. Structured resources/events должны не дублировать operation schemas.
5. Добавить one MCP process per project и reconnect behavior.
6. Зафиксировать capability/version manifest.

**Public/schema impact.** MCP tool/resource schemas versioned через canonical contracts.

**Exact tests / fault injection.** Unbound ambiguous selection, explicit missing path, two MCP processes, controller replacement, cancellation, malformed tool args, batch of 100 operations one Undo, export/extract partial failures.

**Completion evidence.** End-to-end: GUI open → MCP multi-edit → GUI sees one history item → one Undo restores exact state/preview.

**Non-goals / blockers.** Несколько AI writers/CRDT не входят.

### A-04 — Authorization, recovery UX и handoff hardening

**Goal / user outcome.** Human явно контролирует integration mode, может revoke controller и восстановить committed state после crash.

**Spec refs.** §18, §19, §22–23.4, §26 Authorization/Recovery.

**Current code delta.** Authorization/recovery UI отсутствуют.

**Dependencies.** A-03.

**Ordered bounded tasks.**

1. Добавить полную authorization/revoke/replacement UX поверх A-02 gate и
   visible controller identity/state.
2. Реализовать GUI/headless ownership handoff после Dev API/MCP resync:
   committed state/history сохраняются, worker/thread state не переносится,
   running Pack отменяется; новый Pack требует explicit request.
3. Реализовать recovery detection, preview и confirm flow.
4. Compaction/checkpoint policy сделать configurable и наблюдаемой.
5. Добавить crash-loop/corruption safe fallback без silent data loss.

**Public/schema impact.** Local permissions/recovery metadata, не embedded project settings.

**Exact tests / fault injection.** Revoke mid-request, reconnect after revoke, corrupted checkpoint+journal, crash after append/before event, GUI crash/headless continuation, permission record for moved/copied project.

**Completion evidence.** Recovery/authorization process matrix зелёная; UI и MCP сообщают одно authority state.

**Non-goals / blockers.** Не обещать protection от malicious same-user process; не добавлять cloud auth.

---

## 14. X — breadth после доказательства двух эпиков

### X-01 — Additional formats и import candidates

**Goal / user outcome.** Расширять interoperability без изменений pack orchestration.

**Spec refs.** §47, §50 B4, §54 Phase 5.

**Dependencies.** A-04, B3-02, доказанный package conformance harness.

**Tasks.** Первым breadth packet реализуется Defold import:
`.tpatlas` with animations, `.tpinfo` layout-only, safe companion resolution и
existing demo import→extract/repack fixtures. Затем кандидаты оцениваются
по fixture availability и user value: libGDX, manual grid-sheet importer,
дополнительные compatibility profiles. Каждый новый format —
отдельный package с detect/import/export fixtures.

**Tests/evidence.** Общий `format test`, Defold
missing/inconsistent/corrupt companion cases, animation mismatch/path traversal,
round-trip где meaningful, malformed/companion/capability fixtures.

**Non-goals.** Не ставить цель «десятки форматов»; не менять core ради одного format-specific field без capability/IR review.

### X-02 — Optional watch/auto-pack mode

**Goal / user outcome.** Если подтверждён спрос, дать explicit automation mode без изменения default manual Pack semantics.

**Spec refs.** §54 Phase 5, §59 items 21–23,31–32.

**Dependencies.** F3-03, B1 watchers, A session host.

**Tasks.** Отдельный opt-in session command/config; debounce/coalesce; cancel/restart policy; structured job events.

**Tests/evidence.** Default mode никогда auto-pack; burst changes дают bounded jobs; stale/errors остаются видимыми.

**Non-goals.** Не менять settled default: watcher обновляет state, Pack остаётся explicit.

### X-03 — GUI refinements и performance profiling

**Goal / user outcome.** Улучшать UX после стабилизации contracts, не создавая GUI-only capability.

**Spec refs.** §53.5, §54 Phase 5, §56.

**Dependencies.** F3/B2/A surfaces.

**Tasks.** Profile pack-result compression/budgets, thumbnail budgets, large-history behavior, target preview, History/Controller panels, accessibility/keyboard flows.

**Tests/evidence.** Measured traces и budget tests; GUI selftests; CLI/MCP parity remains green.

**Non-goals.** Полная замена GUI не требуется.

---

## 15. Milestone acceptance gates

### Foundation accepted

- schema v1 migrates to v2 без потери baseline behavior;
- IDs стабильны при rename/reorder/save/reload;
- все persistent mutations проходят typed transactions;
- atomic rollback/revision/dirty/inverse/journal fault suites зелёные;
- CLI и GUI operation parity доказан fixtures, не только prose.

### Epic B accepted

- fixture matrix покрывает полный Neotolis oracle (D4, polygons, multipage,
  aliases, animations) и реальный capability subset TexturePacker/Pixi
  profiles; неподдерживаемые комбинации проверяются как explicit loss/adaptation;
- linked atlas read-only source pack-ится через normal raw RGBA path;
- Extract выполняет complete preflight/staging, conflict-safe publish и одну project transaction;
- format descriptors едины для builtin/external, packages sandboxed и testable;
- conversion workflow проходит CLI `--json` и GUI без format-specific core fork.

### Epic A accepted

- GUI и MCP подключены к одной authoritative session;
- multi-edit виден как один commit/Undo;
- acknowledged transaction переживает crash и не дублируется;
- canonical-path claim исключает hidden second writer;
- revoke/reconnect/ownership transfer имеют executable process tests.

### Release acceptance

- `native-release` build/ctest зелёный на Linux/Windows/macOS;
- package sandbox security suite зелёный;
- no engine submodule modifications;
- JSON/protocol/schema fixtures versioned и опубликованы в docs/formats либо рядом с owning API;
- release report перечисляет exact tool/package/data/algorithm versions.

## 16. Handoff checklist для каждого следующего агента

В итоговом сообщении по packet обязательны:

1. выполненные task IDs;
2. список изменённых public/schema contracts;
3. migration/backward-compatibility результат;
4. exact команды тестов и их результаты;
5. fault-injection cases, реально запущенные, а не только запланированные;
6. boundary/engine impact;
7. оставшиеся blockers/non-goals;
8. ссылки `path:line` на production implementation и acceptance tests;
9. явное подтверждение, что CLI/GUI/MCP parity не ухудшилась, либо список ещё не подключённых frontend adapters.

Пакет нельзя отмечать завершённым, если implementation существует, но required schema fixture, migration test, structured error path или fault gate отсутствует.

## 17. P — Performance / «быстрый проект» (workstream владельца)

**Стоячая цель (владелец):** быстрый проект. Аллокации в горячих путях — особенно **per-frame в GUI update-цикле** — цель к устранению. Аллокация-сознательность — постоянный приоритет, не разовый пакет.

**Замеры (сделаны, лид):** `tp_project_clone` — обычный проект (10 атласов, 2000 оверрайд-спрайтов, ~218 КБ) **0.068 мс**; огромный (100 атласов, 100k оверрайд-спрайтов, ~10.6 МБ) **5.86 мс**. Прокси: ~250k мелких malloc+free = **~10.5 мс** чистых накладных — malloc/free доминирует в стоимости клона; memcpy остаток ~1–2 мс. Вывод: клон-на-транзакцию сейчас ОК (даже на огромном < одного кадра); оптимизация — по замеренной нужде.

**Лестница оптимизации копирования (обоснование зафиксировано):**
1. **Клон (сейчас)** — простой, безопасный (живую модель не трогаем до alloc-free свопа), замер ок.
2. **Арена под клон** — `tp_arena` (уже есть: растущая chained + `reset` переиспользование) убирает malloc/free churn; оценка 3–5× на огромном проекте, архитектуру clone-swap и безопасность не трогает. НЮАНС СКОУПА: клон транзакции становится МУТАБЕЛЬНОЙ живой моделью после свопа → чистая арена-обратка требует либо арена-aware памяти модели (add=arena, remove=leak-до-следующего-clone-swap, компактится на клоне), либо арена только для throwaway read-only снапшотов. Не 10-строчный твик.
3. **Inverse-откат (без клона)** — применять на месте, откатывать записанным inverse (F2-03 готов); O(изменений), не O(проекта). РИСК (владелец верно заметил): откат бежит под тем же OOM, что уронил прямой проход → может оставить живую модель битой. Только с митигацией (пред-резерв памяти отката). Клон этого лишён.
4. **COW / persistent structures (Immer-стиль)** — near-free снапшоты + undo структурным шарением; ДАЁТ СВЕРХ клона: lock-free консистентные снапшоты для конкурентных читателей (фон-паковка/MCP/зеркало) + дешёвое определение изменений (инкрементальная перепаковка). Но полный переезд модели на иммутабельные refcount-деревья в C — дорого/рискованно. Наши нужды (снапшот читателю) уже дёшево покрыты клоном; запись сериализована (один писатель, optimistic revision, без CRDT), так что тяжёлая часть COW не нужна. ПАРКОВАНО: пересмотреть на F3, только если конкурентные снапшоты станут частыми на большом проекте.

**Задачи (планируются; порядок/тайминг — по решению владельца):**
- **P-01 — Arena-backed transaction clone.** Замерить реальный арена-клон vs malloc-клон на огромном `tp_project` (не прокси); интегрировать в `tp_txn__commit_validated`, сохранив byte-identity клона (F2-02 голдены) и no-leak. Решить скоуп-нюанс (арена-aware модель vs компакт-на-клоне). Замер сделать до/вместе с интеграцией.
- **P-02 — Update-loop allocation audit → per-frame zero-alloc.** Read-only исследование (отчёт `scratchpad/mem-alloc-investigation.md`, лид) → устранить per-frame аллокации в `apps/gui` (reset-per-frame скретч-арена; проверить, что Clay/nt_ui не реаллоцирует буферы каждый кадр). Цель: ~0 malloc/кадр в горячем цикле.
- **P-03 — (если нужно) inverse-откат** вместо клона на больших проектах, с митигацией OOM-риска.
- **P-04 — (парковано) COW/persistent** — только если F3-конкурентность потребует частых снапшотов на большом проекте.

**Инфра готова:** `tp_arena` (растущая + reset), F2-03 inverse/replay — оба строительных блока уже есть, так что оптимизация — локальная работа, не переписывание.

## 18. Resilience — краш-восстановление и диагностика (workstream владельца)

Три задачи. **H** — hardening по внешнему ревью Codex (делаем ПЕРВЫМ, до merge в main); затем **R** и **D** — независимый workstream владельца после D2-замеров F2-04 (обсуждение в чате). W (welcome-экран) — не сейчас. Порядок работ: **H → R (+ P1-1/P1-5) → D → merge F2 в main** (после визы владельца на ADR 0010–0015).

**R — журнал восстановления v2 (diff-based, per-project).** F2-04 v1 пишет полный снапшот проекта на КАЖДЫЙ коммит (D2: HUGE ~245 мс/коммит, серилизация-bound; журнал пухнет всю сессию, не сбрасывается на Save). Переезд:
1. записи журнала = **сериализованная ОПЕРАЦИЯ** (`tp_txn_request_encode` — уже готовый byte-stable кодер; каталог операций сделан журналопригодным) вместо полного снапшота. **РЕШЕНИЕ ВЛАДЕЛЬЦА (2026-07-14): формат B (операция), НЕ семантический diff (вариант A).** Обоснование: A требовал большой новый сериализатор для 9 форм диффа + голдены; B переиспользует протестированный кодер, replay в ~2× дороже но всё равно <2мс на 10k. Риск B (дрейф семантики применения операции между версиями) приемлем для короткоживущего crash-recovery сайдкара (чистится на Save, версионируется). Числа спайка (бенч `tp_bench_diff_journal`, коммит 74c98e8): запись ~0.02мс/169Б (vs v1 63мс/17МБ); replay 10k правок <2мс даже на HUGE → восстановление упирается в 1 загрузку чекпойнта (~десятки мс), не в replay. **Каденс (ФИНАЛЬНОЕ решение владельца, R3):** чекпойнт **на attach** (initial baseline) + **компакция на Save** (сжать окно replay в 0); **периодического внутрисессионного чекпойнта НЕТ, per-N НЕТ** — спайк доказал, что восстановление упирается в одну загрузку чекпойнта, поэтому окна Save-to-Save достаточно;
2. **компакция на Save** сбрасывает журнал к одному свежему чекпойнту == только что сохранённого состояния (окно Save-to-Save; спек §22.3 явно разрешает «save может компактить/чистить журнал»); сохраняет retained-id set + revision (идемпотентность §7.2 переживает компакцию);
3. восстановление = последний чекпойнт + **in-place replay** diff'ов (undo-история на восстановлении не нужна);
4. **модель — per-PROJECT, НЕ per-window** (как Word Document Recovery / VS Code hot-exit — восстанавливают документ, не окно): журнал на каждый редактируемый проект в recovery-папке, ключ = стабильный recovery-id (переживает Save-As), запись хранит путь+имя+время; блокировка на журнал (liveness). На старте — **сканируем папку → список проектов с несохранёнными изменениями от крашнутых сессий (осиротевшие журналы, процесс-владелец мёртв)**;
5. UX (владелец): по проекту **[Отклонить]** / **[Сохранить (заменить файл, старый → `.bak`)]** / **[Сохранить как…]** — решаем ПРЯМО НА СТАРТЕ, НЕ открываем dirty-редактор. Сохранить = пересобрать восстановленное состояние + записать в исходный файл (старый переименовать в `.bak`); Сохранить как = в новый файл, оригинал не трогаем (для «не уверен»); untitled → только Отклонить / Сохранить как. Отклонить = удалить журнал. Покрывает мультиокно: одно крашнулось/живут/все крашнулись — каждый проект в списке независимо;
6. **замеры:** запись diff + **время восстановления vs число diff** (owner-вопрос «2ч работы») + стоимость чекпойнта → выбор каденса.
7. **несовместимый/старший формат журнала — показать, НЕ терять молча (решение владельца 2026-07-14 по R1-review [0]).** Формат-бамп (v1→v2 в R1, далее) НЕ несёт back-compat: старший журнал читается как несовместимый, байты на диске сохраняются (не удаляются), но auto-replay нет. Для пре-релиза это принято (реальных v1-журналов нет; R2 всё равно меняет payload-семантику снапшот→операция). НО скан R5 обязан **пометить журнал несовместимой версии как «найдены recovery-данные старого формата»** и провести через ту же модалку (минимум: сообщить пользователю, не игнорить тихо) — иначе апгрейд бинаря = тихая потеря несохранённого. (Отдельный recovery-status для version-mismatch vs bad-magic заводить в R5, где появится потребитель.)
Меняет payload F2-04 → **core-пакет**. Связано с A-04 (recovery UX). Escape-hatch: возможно разбить core (v2 payload/компакция/replay) + GUI (recovery-папка, скан, модалка-список).

**R5a — DONE (core, no GUI).** Format-groundwork для R5b (GUI recovery-папка/скан) и R6 (модалка-список), аддитивно к журналу, изолировано round-trip-тестами:
- **Metadata-запись (rec_type 3)** `{timestamp, path, name}` — UTF-8, path может быть пустым (untitled → path ""), BE-сериализация byte-at-a-time. Кэшируется на journal-объекте (`tp_journal_set_metadata`, timestamp — caller-supplied unix-seconds, core детерминирован, не зовёт `time()`) и **re-emit на компакции** (`tp_journal_compact`) — переживает Save/undo-сброс; fail-closed poison, если re-emit не записался. Восстановление кладёт последнюю META (last-wins) в `tp_journal_recovery.metadata`; META НЕ txn/ckpt — не влияет на `records_recovered`/replay/retained-ids.
- **Peek API** (`tp_journal_peek`) — читает header + metadata + status БЕЗ реконструкции модели (скан зовёт per-file): status, format_version, header-key, has_checkpoint, record_count, meta. Делит header-validate + frame-walk (`frame_parse_at`, `decode_payload`, `parse_meta`, `classify_stop`) с `tp_journal_recover` — не форкает второй парсер.
- **BAD_HEADER split (plan item 7)** → `TP_JOURNAL_RECOVERY_BAD_MAGIC` (не наш файл) vs `TP_JOURNAL_RECOVERY_VERSION_MISMATCH` (наш файл, чужая версия формата). Поведение сохранено (ничего не восстановлено, байты на диске целы, return OK); GUI не тронут (не потребляет enum напрямую — recover возвращает `*out==NULL` → fresh init). R5b/R6 спец-кейснут VERSION_MISMATCH в модалке.
Файлы: `packer/src/tp_journal.c`, `packer/include/tp_core/tp_journal.h`, `packer/src/tp_journal_internal.h`, `packer/tests/test_journal.c` (7 новых тестов + `test_bad_header`→`test_bad_magic_vs_version_mismatch`). **Осталось: R5b** (GUI recovery-папка, скан папки → список крашнутых проектов через peek, блокировка/liveness) **и R6** (модалка-список Отклонить/Сохранить/Сохранить как).

**R5b-1 — DONE (GUI wiring того, что R5a отгрузил как core).** R5a добавил `tp_journal_set_metadata`, но GUI его не звал → журнал не самоописывался. Теперь каждый recovery-журнал, который создаёт GUI, пишет `{project path, display name, timestamp}`, обновляясь на КАЖДОМ изменении идентичности проекта. Механика:
- **Model-glue** `tp_model_set_recovery_metadata(m, timestamp, path, name, err)` (`tp_txn_apply.c`, декларация в `tp_transaction.h`) — форвардит в приаттаченный журнал; нет журнала → no-op OK (как `tp_model_compact_journal`); NULL path/name → ""; NULL model → INVALID_ARGUMENT; ошибка записи возвращается вызывающему для soft-канала (non-fatal — метаданные информационны, не валят правку/Save).
- **Единый chokepoint** — вызов сидит в `set_path()` (`gui_project.c`), после `recompute_name()`. `set_path` — единственная точка смены идентичности (New/Open/Save-As/adopt-recovered), и журнал уже приаттачен `wrap_model` до того, как её вызовут её колеры, поэтому один вызов покрывает все флоу. Timestamp — `(int64_t)time(NULL)` (GUI поставляет время, core детерминирован; добавлен `#include <time.h>` в libc-блок — macOS include-order). Guard `s_model && recovery_active()` + no-op-on-no-journal glue → безопасно при отключённом recovery / journal-less. Ошибка → `note_recovery_degraded`.
- **Закрывает review-finding [1]** (свежий журнал, который аттачит `try_adopt_recovered`, раньше НЕ нёс метаданных) и чинит stale-path на Save-As: `set_path(path)` обновляет кэш до `tp_model_compact_journal`, поэтому компакция re-emit'ит НОВЫЙ путь, а не старый.
Файлы: `packer/src/tp_txn_apply.c`, `packer/include/tp_core/tp_transaction.h`, `apps/gui/gui_project.c`, `apps/gui/gui_selftest.c` (J16: untitled + Save-As peek round-trip), `packer/tests/test_journal.c` (`test_model_set_recovery_metadata_glue`). **Папка/скан/keying (одна фиксированная ячейка → many-per-project) отложены в R5b-2.**

**R5b-1 fix-round (адверсариальный review, 5 findings).** (1/2/4) `tp_journal_set_metadata` стал **cache-authoritative + best-effort durable** (зеркалит swallow-логику `tp_journal_compact`): кэш `j->meta_*` — источник истины для того, что персистит следующая компакция, поэтому COMMIT КЭША ПЕРВЫМ, затем best-effort durable append, который SWALLOW'ит сбой на ЗДОРОВОМ журнале (write_record уже откатил рваный хвост → store recoverable, кэш держит истину, следующая компакция re-emit'ит). Ошибка — ТОЛЬКО при OOM или genuinely poisoned журнале (откат не удался), поэтому `note_recovery_degraded` в `set_path` теперь срабатывает лишь на РЕАЛЬНОЙ потере, а не на транзиентном meta-hiccup. (3) `try_adopt_recovered` теперь **переносит идентичность краш-проекта** на свежий (adopted) журнал: захватывает `info.metadata` (strdup path/name ДО `recovery_free`, free на всех return-путях), и ПОСЛЕ `set_path("")` override'ит метаданные журнала оригинальным path/name (timestamp `time(NULL)` — «когда сессию последний раз трогали»), тогда как ЖИВОЕ окно остаётся untitled — split «live identity vs recovery-scan identity» для скана R5b-2 и R6 «Save (backup original)». (5) ACCEPTED (без кода): durable META на Save-As + re-emit компакцией — один лишний мелкий write+fsync на Save, намеренно (Save не hot). Тесты: `test_set_metadata_write_failure_no_effect` → `test_set_metadata_write_failure_still_caches` (FLIP: OK+не-poisoned+метаданные переживают компакцию); selftest J17 (adopt-carry: живое окно untitled, но META свежего журнала несёт оригинальный path/name). Файлы: `packer/src/tp_journal.c`, `packer/include/tp_core/tp_journal.h`, `packer/tests/test_journal.c`, `apps/gui/gui_project.c`, `apps/gui/gui_selftest.c`.

**R5b-2 — DONE (GUI recovery-папка + per-session журналы + startup scan + adopt-newest).** Recovery переехал с ОДНОЙ фиксированной ячейки под exe-dir на **папку `<app-data>/recovery/` с одним журналом на editing-сессию** + **startup-скан**, который находит журналы, осиротевшие крашнутой сессией, и **авто-адоптит новейший с несохранённой работой** (сегодняшний single-slot UX, обобщённый). **Без модалки** (интерактивный chooser — R6); неадоптированные журналы **ОСТАЮТСЯ на диске** (R6 их покажет). Recovery остаётся fail-closed + non-fatal.
- **KEYING (решение владельца 2026-07-15):** имя ЖИВОГО журнала = **per-session random id** (`tp_id128_generate` → `<session-id-hex>.ntpjournal`), НЕ хеш пути. Почему: path-keyed имена **КОЛЛИЗЯТ** — переоткрытие крашнутого проекта X хотело бы `hash(X).ntpjournal`, но его же сирота уже занимает это имя, и fail-closed «слот должен быть пуст» в attach тогда отказал бы (journal-less регрессия). Уникальные per-session имена этого избегают и не требуют re-keying на Save-As. **Идентичность проекта берётся из МЕТАДАННЫХ журнала** (`{path,name,time}`, пишутся R5b-1 в chokepoint `set_path` + переносятся на adopt), читается сканом через `tp_journal_peek`. Имя файла — просто уникальный хэндл. **In-header key остаётся фиксированным `recovery_key()` тегом** («ntpk_recovery_01») — не трогаем; все журналы делят его (валидация формата/приложения).
- **Скан-алгоритм** (`gui_project_scan_pick`, `gui_project.c`) по каждому `<folder>/<name>.ntpjournal` != basename живого слота: (1) **liveness-probe** `<cand>.lock` ОТДЕЛЬНЫМ transient-хэндлом (никогда не трогает статический `s_recovery_lock` живой сессии; read-only — не создаёт .lock, `OPEN_EXISTING`/no-`O_CREAT`): захватываемо ⇒ мёртвый владелец = сирота; держится ⇒ живой инстанс ⇒ SKIP+LEAVE. (2) **peek+классификация**: адоптабельно ТОЛЬКО `status==OK && has_checkpoint && record_count>1` (есть post-checkpoint правки = несохранённая работа); всё прочее (EMPTY / checkpoint-only / TRUNCATED / CORRUPT / VERSION_MISMATCH / BAD_MAGIC) — НЕ кандидат ⇒ ОСТАВИТЬ нетронутым. (3) новейший по `meta.timestamp`. Benign TOCTOU (acquire-then-release — hint): два инстанса на одну сироту в худшем случае восстановят одно и то же состояние — потери нет.
- **DELETION RULE (единственные удаления во всём пакете):** (i) успешно-адоптированный source-журнал + его `.lock` (его работа теперь в свежем живом журнале → иначе re-adopt на следующем запуске); (ii) собственный журнал живой сессии + `.lock` на чистом shutdown (существующее поведение). Больше НИЧЕГО не удаляется; при неудаче adopt source НЕ удаляется. `try_adopt_recovered(void)` → `try_adopt_recovered(const char *source)`: рекаверит из `source` (не из живого слота), `wrap_model` вешает свежий журнал на ЖИВОЙ слот, затем удаляет source (если `source != s_recovery_path` — живой слот никогда не удаляется здесь). `gui_project_init_adopt(source)` (обёртка `gui_project_init()` = adopt-from-live-slot, in-place/selftest-путь).
- **Core-примитив** `tp_scan_list_dir(dir, suffix, tp_str_list*)` (`tp_scan.c`/`.h`) — публичный non-recursive листер имён regular-файлов по суффиксу, переиспользует тот же платформенный `FindFirstFileA`/`opendir+stat` код, что `tp_scan_dir` (macOS include-order: libc первым). **main.c** (`#ifndef NTPACKER_GUI_SELFTEST`): `app_data_root` → `%s/recovery` → `ensure_dir` → `make_session_slot` → `enable_recovery` → `scan_pick` → `init_adopt`; каждый snprintf с проверкой truncation (fail-closed), любой сбой → recovery отключён на этот запуск (не краш). Production-скан ОСТАЁТСЯ внутри `#ifndef NTPACKER_GUI_SELFTEST` (selftest-exe гоняет THIS exe non-headless; J18-J21 гоняют скан на ИЗОЛИРОВАННЫХ temp-папках через test-seam'ы, так production-скан не адоптит тестовый журнал).
- Тесты: core unit `test_list_dir_suffix`/`_all_regular_files`/`_missing_returns_false` (`test_scan.c`); selftest **J18** (скан выбирает новейшую сироту B, адоптит её состояние, удаляет адоптированную, ОСТАВЛЯЕТ A), **J19** (live-locked сирота SKIP+LEAVE), **J20** (no-work + BAD_MAGIC — не адоптит НИ ОДНУ и не удаляет НИ ОДНУ), **J21** (адоптированный source удалён — сложено в J18). Детерминизм timestamp'ов — seam `gui_project__test_set_recovery_now`. Файлы: `packer/src/tp_scan.c`, `packer/include/tp_core/tp_scan.h`, `packer/tests/test_scan.c`, `apps/gui/gui_project.c`, `apps/gui/gui_project.h`, `apps/gui/gui_selftest.c`, `apps/gui/main.c`. **Следующее — R6 (модалка-список несохранённых сессий + Discard).**

**R6a — DONE (recovery-resolution decision/action layer; headless-testable, БЕЗ UI).** Тестируемый слой решений/действий, который вызовет стартовая модалка R6, отдельно от неё — НЕТ `nt_ui`, НЕТ `main.c`-wiring, НЕТ смены поведения старта (существующий авто-адопт R5b-2 остаётся до R6b). Всё в `apps/gui/gui_project.c` + `.h` (у `recovery_key()` file-static-линкидж → все функции R6a живут там). Части:
- **`gui_recovery_collect(folder, live_slot, *out)`** — богатый коллектор (в отличие от `gui_recovery_candidates`, который несёт только пути): по каждому осиротевшему `.ntpjournal` (исключая live-слот по basename + live-locked через `recovery_orphan_unlocked`) `tp_journal_peek` → запись `gui_recovery_entry {journal_path, orig_path, name, timestamp, status, adoptable}`. `adoptable=true` для OK/TRUNCATED/CORRUPT+checkpoint+record_count>1; **VERSION_MISMATCH попадает в список с `adoptable=false`** (старый формат — показать для Discard, R1-review [0] «не терять молча»); BAD_MAGIC/EMPTY/no-work/STALE_KEY — SKIP. name = `meta.name` → basename(`meta.path`) → «untitled». Newest-first, cap `GUI_RECOVERY_MAX_CANDIDATES`. Read-only, УДАЛЯЕТ НИЧЕГО.
- **`recover_to_clone(journal_path)`** (static) — вынесен префикс `try_adopt_recovered` (recover→clone), но СТОП на `tp_project_clone(tp_model_project(rm))` + `tp_model_destroy(rm)`: НЕ `wrap_model`, НЕ `set_path`, НЕ трогает живую модель/`s_recovery_path`. Read-only+no-create open; `tp_model_recover` забирает io на всех путях (нет утечки), возможный poison throwaway-журнала иррелевантен (клоним состояние + уничтожаем rm). Не требует активного recovery.
- **`save_backup_original(p, orig, err)`** (static) + `.bak`-helper (ранее не было): если `orig` существует — `remove(<orig>.bak)` (Windows-rename-если-dest-есть) → `rename(orig, <orig>.bak)`; **если backup-rename ПРОВАЛИЛСЯ — возврат fault БЕЗ перезаписи `orig`** (никогда не затираем файл пользователя без бэкапа). Нет `orig` — просто save. `tp_mkdirs_parent` перед записью. Зеркалит `gui_log_file.c rotate_files`.
- **`gui_recovery_resolve(journal_path, orig_path, action, target_path, err_out, cap)`** — одна точка входа модалки. **НЕДЕСТРУКТИВНО ПРИ СБОЕ:** журнал (+`.lock`) удаляется ТОЛЬКО после УСПЕШНОГО save (или явного Discard). DISCARD → `delete_adopted_source` (переиспользован из R5b-2). SAVE_ORIGINAL/SAVE_AS → `recover_to_clone` → **promote ids** (`tp_project_promote_ids`, как `gui_project_save_as`) → `save_backup_original`/`tp_project_save` → удалить журнал ТОЛЬКО на OK. Клон всегда уничтожается; живая модель не трогается. SAVE_ORIGINAL требует `orig_path != ""` иначе INVALID_ARGUMENT.
- **ID-promotion finding:** восстановленный клон УЖЕ несёт promoted structural ids (чекпойнт-база промоутится в `attach_recovery_journal` ДО чекпойнта), поэтому promote в resolve — защитный идемпотентный no-op. Но он ОБЯЗАТЕЛЕН: `tp_project_save` сериализует nil-id БЕЗ ошибки, а reload такой файл отвергает (ID_MALFORMED) — promote гарантирует, что сохранённый recovery-файл перечитывается; RNG-fault fail-closed (save нет, журнал остаётся).
- Тесты (selftest, изолированные temp-папки, зеркалят J18-J25): **J26** collect (saved-adoptable + untitled-adoptable + VERSION_MISMATCH-non-adoptable в списке newest-first с верными name/orig_path/status/adoptable; live-locked + BAD_MAGIC исключены); **J27** SAVE_AS (target грузится в восстановленное, журнал+`.lock` удалены); **J28** SAVE_ORIGINAL — (a) `<orig>.bak` со старым содержимым + `orig` = восстановленное + журнал удалён, (b) нет оригинала → нет `.bak`, save идёт, (c) **data-safety:** backup-rename форсированно провален (непустая директория на месте `<orig>.bak`) → resolve non-OK, оригинал НЕ перезаписан, журнал НЕ удалён; **J29** Discard (журнал+`.lock` исчезли, sentinel цел); **J30** save-fail (SAVE_AS в неоткрываемый путь → non-OK И журнал ОСТАЁТСЯ). J1-J25 зелёные. Файлы: `apps/gui/gui_project.c`, `apps/gui/gui_project.h`, `apps/gui/gui_selftest.c`, `docs/plans/master-spec-implementation-plan.md`. **Следующее — R6b (nt_ui модалка на старте + wiring вместо авто-адопта).**

**D — диагностика крашей (для владельца, чтобы чинить баги).** Сейчас: краш-хендлера НЕТ, логи только в консоль (в файл не пишутся) → после краша пользователю нечего прислать. Добавить:
1. логи в **ротируемый файл** (app-data/рядом с exe);
2. **краш-хендлер** — Win `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` (`.dmp`); POSIX signal-handler + backtrace/core; на краше пишем дамп + флашим лог;
3. при следующем запуске «в прошлый раз был краш — отправить отчёт?» (папка `.dmp` + `.log`).
Цель: пользователь шлёт владельцу логи+дамп для починки. Полностью независимо от R (разные цели: R возвращает работу пользователю, D даёт разработчику диагностику).

**H — hardening по внешнему ревью (Codex, диапазон `2d6777d..8f3b409`, 2026-07-14).** Владелец прогнал Codex по всему F2-рефакторингу. 14 находок (8 P1 + 6 P2) проверены 4 независимыми верификаторами + 2 аудита покрытия (поверхность мутаций «сверху» + каталог операций «снизу»). Итог: 1 жёсткий блокер, 9 реальных фиксов в батч, 2 сложены в R, 1 решение по спеке, 3 ложных/безвредных. Полная таблица + доказательства `file:line` — `scratchpad/codex-review-triage.md`. **F2 в main НЕ мержим, пока H+R не закрыты.** Codex нашёл настоящую потерю данных (P1-8), которую внутренний гейт пропустил.

Батч H (все фиксы на ветке до merge; порядок — блокер первым):
1. **P1-8 (БЛОКЕР, data-loss)** — старт с файлом-аргументом после краша: `main.c` вызывает `gui_project_open(proj_arg)` и уничтожает восстановленную модель БЕЗ dirty-промпта + статус «Ready»/«Opened X» затирает recovery-уведомление до первого кадра. Двойной клик по `.ntpacker_project` после краша = тихая полная потеря несохранённого. Фикс: если recovery adopted (dirty) — не открывать arg молча (сохранить восстановленное, отложить/провести через существующий dirty-gate), не затирать recovery-notice; вынести решение в тестируемую `gui_project_*` функцию + J-регрессия. GUI.
2. **P1-2** — `TP_OP_ANIMATION_RENAME` op: rename анимации через `commit_txn_now` → undoable + crash-safe + журналируемый + доступен в CLI; поправить устаревший коммент «journal-less» (модель теперь journal-backed). **Решение владельца 2026-07-14: делаем op.** core + GUI.
3. **P1-4** — `validate_frames`: reject `frames==NULL && frame_count>0` (сейчас NULL-deref; с wire недостижимо, но контракт «UB-clean на произвольном payload» нарушен). core, ~1 строка.
4. **P2-10** — `source.add` validate: reject `kind ∉ {FOLDER,FILE}` (сейчас молча коэрсит любой не-`file` в folder → live-vs-recovered/live-vs-disk дивергенция; снимает латентный риск для B1 `ATLAS=2`). core, ~2 строки.
5. **P1-6** — `attach_recovery_journal` fail-closed: после `remove()` слота проверить, что он пуст (иначе открыть с truncate / уйти в journal-less + `note_recovery_degraded`); закрывает тихий append-за-устаревший-заголовок при провале `remove()`. GUI.
6. **P2-14** — Add Atlas: имя по скану уникальности (как у анимаций), не `atlas<count+1>` — иначе клинит после удаления среднего атласа. GUI.
7. **P2-13** — Add Files (мультивыбор): собрать N источников в ОДНУ транзакцию (`gui_project_add_sources`, как `anim_add_frames`), не N (undo-гранулярность + атомарность частичного отказа). GUI.
8. **P2-12** — CLI JSON-ошибка: включить `field` и `op_index` из `tp_txn_error` (сейчас теряются, машинный клиент видит только id+prose). CLI.
9. **G3 (НОВОЕ, из аудита поверхности)** — поле пути таргета (out-path) коммитит `TARGET_SET` НА КАЖДУЮ КЛАВИШУ (`TARGET_SET` структурный, не коалесится → валится прямо в `commit_txn_now`): N undo-шагов + N полных снапшотов; на HUGE = D2-обрыв (17МБ/245мс) на каждую букву; прямо противоречит инварианту коалесинга ADR 0015. Фикс: сделать поле пути/экспортёра коалесящимся (ключ `CK_TARGET`) либо коммит только по Enter/blur (не по `changed`). GUI.

Складываются в **R** (обе про журнал, R его и так переделывает + бампает формат v1→v2):
- **P1-1 — DONE (R4, checkpoint-on-undo).** undo/redo не аппендят в журнал (`tp_history.c`), а `tp_model_recover` реплеит post-checkpoint op-payloads → краш на позиции undo воскрешает отменённую правку. **Решение: recovery восстанавливает ТОЛЬКО состояние документа, undo-стек не персистится.** Обратную операцию НЕ журналируем: undo применяет `tp_diff_record`, а не `tp_txn_request`, и у инверсии remove (re-add с точным id+полным состоянием) нет прямой op в каталоге, т.е. reverse-op-кодирование невозможно единообразно. Вместо этого после успешного undo/redo чекпоинтим получившееся (post-undo) состояние в журнал через уже отгруженный `tp_model_compact_journal` (R3) — recovery грузит отменённое состояние напрямую. Хук в GUI (`gui_project_undo`/`_redo` → `checkpoint_after_history`, NON-FATAL через `note_recovery_degraded`, как Save-хук); 2 core-теста в `test_journal.c` (undo→compact→recover == «one», redo→compact→recover == «two»). Severity была wrong-result (файл проекта не трогается; восстановленное всегда dirty за Save-промптом), окно узкое (краш ровно после undo без последующего коммита).
- **P1-5** — раздутое поле длины записи классифицируется как «оборванный хвост» ДО CRC → физически удаляет валидные записи после испорченной (нарушает C2 + ложное утверждение ADR 0013). До GUI не доходит (recovery берёт последний хороший снапшот + стирает слот), но обязательно до не-GUI потребителя журнала. Фикс — sync-word / устойчивость кадрирования при бампе формата v1→v2.

Спека/доки (без кода):
- **P2-9 — РЕШЕНИЕ ВЛАДЕЛЬЦА 2026-07-14: вариант A.** Узаконить `duplicate_id` в мастер-спеке §7.2 как корректный контракт на повтор транзакции. Безопасная суть (не применять дважды; retained-id переживают рестарт) уже реализована и покрыта `test_transaction.c`; снимается только буква §7.2 «вернуть прежний результат». B (durable result-replay) — аддитивный апгрейд, если появится ретраящий сетевой MCP-клиент. Обоснование низкого риска: ретрай физически невозможен у существующих клиентов — GUI in-process/синхронно/свежие монотонные id; CLI one-shot (перезапуск = новый id = новая транзакция, не повтор). Ретрай реален только у будущего долгоживущего MCP-агента по транспорту с потерей ответа; id создаваемых сущностей генерит клиент, единственный «теряемый» датум — точная ревизия.
- **P1-3 / P1-7 / P2-11 — reviewed-and-accepted, кода не трогаем.** P1-3 (sprite override по export-имени игнорит source — задокументированный bridge ADR 0009/0010; вредный кейс двух source с одним export-key уже ловится `export_name_collision` в validate/pack, т.е. только в непакуемом атласе). P1-7 (правки при отказе журнала ОТКЛОНЯЮТСЯ на коммите, а attach-fail и 2-е окно СИГНАЛЯТ пользователю — санкционировано ADR 0015). P2-11 (recovery сбрасывает revision→0 / retained-id — намеренный fix[0]+[2], предотвращает `DUPLICATE_ID`-фриз, безвреден для одно-клиентного GUI).

Аудит покрытия (оба прохода, 2026-07-14) — **поверхность чистая**: покрыто 100% (все меню/контекст-меню/шорткаты/поля/экспорт-модалка; OS drag-drop отсутствует); каждая существующая мутация идёт через конвейер `op→txn→diff→history→journal`, КРОМЕ 3 дыр выше (P1-2 rename, P2-13 Add Files, G3 путь-по-клавише); каталог из 20 операций прошит равномерно, ни одна не пропускает слой (validate/lower/apply/diff-прямой+обратный/encode-decode/тесты; есть oracle-тест полноты diff). «Недостающие операции» из нижнего прохода — reorder/MOVE для атласов/источников/анимаций/таргетов (MOVE есть только у кадров) и смена `kind` источника folder↔file — это фичи, которых в GUI НЕТ вовсе (порядок коллекций меняется только add-в-конец/remove, обе операции), → scope только при добавлении drag-reorder/смены типа, НЕ баги. `source.replace` + `animation.frames.set` — зарезервированы намеренно (нет CLI-verb/билдера).
