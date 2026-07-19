# Дорожная карта реализации master spec

**Статус:** производный execution tracker
**Источник истины:** [`ntpacker-master-spec.md`](ntpacker-master-spec.md)
**Детальная декомпозиция:** [`plans/master-spec-implementation-plan.md`](plans/master-spec-implementation-plan.md)

Этот документ отвечает только на вопросы «в каком порядке реализовывать» и
«каким проверяемым результатом заканчивается этап». Он не вводит продуктовых или
архитектурных решений. При любом расхождении действует master spec; открытые
контракты закрываются по §60 до публикации соответствующего API.

## Легенда статусов

| Статус | Значение |
|---|---|
| `BASELINE` | Возможность перечислена в текущем baseline master spec; это не оценка качества реализации |
| `PLANNED` | Этап ещё не принят |
| `IN PROGRESS` | Есть активный пакет реализации, но gate этапа не пройден |
| `BLOCKED` | Нельзя продолжить без явно указанного внешнего решения или prerequisite |
| `DONE` | Все задачи и gate этапа подтверждены кодом, fixtures и executable tests |

Статус этапа меняется на `DONE` только после проверки текущего репозитория. Само
наличие старого плана, частичной реализации или похожего API завершением не
считается.

## Текущий baseline

Master spec §2 фиксирует уже имеющуюся продуктовую основу: packing core, GUI и
CLI, project files, multi-atlas, file/folder sources и refresh, settings и
overrides, animations, Neotolis JSON и Defold export, capability-aware repack,
deterministic output, machine-readable CLI, project editing, dry-run,
inspect/validate, runtime exporter registration и raw RGBA inputs.

Этот baseline необходимо сохранять на каждом этапе. Новая работа начинается не
с повторной реализации этих возможностей, а с укрепления целевых identity,
operation, source, session и format-package contracts. Для pre-release project
schema обратная совместимость не является требованием: действует canonical v5.

## Outcomes эпиков

- **Epic A — Live automation and AI.** Пользователь открывает проект в GUI,
  подключает одного AI-контроллера, наблюдает его атомарное многошаговое
  изменение в той же live session и одним Undo возвращает точное предыдущее
  состояние и preview (master spec §57).
- **Epic B — Format ecosystem and atlas interoperability.** Пользователь
  импортирует TexturePacker/Pixi или Neotolis atlas, использует его как
  read-only source, безопасно извлекает full-size PNG, перепаковывает их и
  экспортирует в другой поддерживаемый формат (master spec §57).

Shared foundation — не третий пользовательский эпик. Это обязательная общая
основа для A и B (master spec §3).

## Architecture foundation M0-M5 checkpoint

**Статус:** `DONE` (2026-07-16). Exact-SHA terminal CI remains a handoff gate;
its current result is recorded on the PR and in the task handoff.

M0-M5 from `plans/architecture-foundation-plan.md` completed the shared
ownership foundation: one session-owned mutable model, immutable snapshots,
thin GUI/CLI adapters, bounded recovery, exact job/capability contracts and
closed deletion gates. Durable evidence is executable: the foundation plan §10
names the debug/release tests, benchmark harnesses, client parity corpus and
`scripts/check_boundaries.sh` deletion checks.

This checkpoint does **not** mark the complete F1/F2/F3 product phases DONE;
their remaining product gates still require separate phase audits. In
particular, visible shared History and full `pack_input_hash`/result LRU remain
F3 work.

**Resolved F3 finding — P-UNDO (`DONE`, 2026-07-17):** Undo/Redo now append
versioned compact `HISTORY` transitions. Full checkpoints are a deterministic
fallback only for unsupported or oversized diffs. Mixed TXN/HISTORY replay,
append/sync-failure rollback, hostile-input handling and admission budgets are
test-pinned.

**Owner recovery-policy update (2026-07-19):** the master spec no longer makes
journal durability part of the model commit contract. Recovery is a bounded
best-effort version-4 diff log with a healthy process/power-loss RPO of at most
5 seconds. Journal failure leaves the edit committed, enters a sticky degraded
state, and stops later dependent recording until a fresh checkpoint. This
supersedes every older gate below that says append/sync failure rolls back a
transaction or that visibility waits for durable acknowledgement. The current
rollback-on-journal-failure implementation is therefore refactor work, not the
target contract.

## Граф зависимостей

```text
BASELINE
   |\
   | +------> H0  fallible builder containment --------+
   |                                                  |
   v
  F1  persistent identities + tagged project schema
   |\
   | +-----------> B0  native import foundation
   v                  |
  F2  typed ops -------+
   |                   |
   +------> B1  linked atlas sources + Extract Sprites
               |
               v
              F3  session/history/Pack <--------------+
              |
              v
              B2  package registry
               |
               v
              B3  sandboxed Lua + templates
               |
               v
               A  live automation and AI
               |
     B1 -------+----------------> Breadth
```

Допустимый параллелизм: `B0` может идти параллельно `F2` после `F1`. `B1` требует и import materializer, и
typed transaction foundation: Extract Sprites не должен сначала закрепить
временную mutation-модель. Полный Epic A следует после package runtime (`B3`),
как требует master spec §54. `Breadth` следует после Epic A и доказанных
import/source (`B1`) и package (`B3`) путей.

## Этапы и задачи

### Promoted contract groundwork

Временный pre-production spike-слой выведен из build и больше не является
этапом или prerequisite. Принятые результаты находятся в production core/tests
и decisions 0002, 0004–0006, 0010–0018. Уникальные byte-exact raster-oracles сохранены как
non-normative research в `docs/research/raster-normalization-goldens.md`.
Незакрытые решения master spec §60 отслеживаются непосредственно тем active
packet, который публикует соответствующий API.

### H0 — Fallible builder containment

**Статус:** `IN PROGRESS` — strict UTF-8 image ingress, canonical RGBA8 decode и
known-assert preflight реализованы; containment output/allocation/unknown assert
failures ещё требует private worker по decision 0018.
**Prerequisite:** текущий `BASELINE`.
**Основание:** master spec §10.5–§10.6 и acceptance criterion 54.

| ID | Результат |
|---|---|
| `H0.1` | Все path sources читаются shared UTF-8 FS/decoder boundary и передаются builder как raw RGBA8 |
| `H0.2` | Core preflight возвращает structured error для transparent-at-threshold, invalid slice9, impossible footprint и spacing/settings assertions |
| `H0.3` | Versioned bounded private worker protocol; builder abort/non-zero exit/malformed response становятся `builder_crashed`/`builder_failed` |
| `H0.4` | Только ASCII relative worker staging; parent владеет UTF-8 read/publication, cancellation, timeout и cleanup |
| `H0.5` | Byte-identical normal-result oracle и crash/Unicode/full-disk/cancellation regression suite |

**Gate:** ни один user/resource/output-path failure движка не завершает client
host и не заменяет last successful preview; Unicode/long parent paths проходят
через private staging; worker crash/cancel не оставляет опубликованный partial
artifact. Квалифицирующий upstream fallible sink API может заменить процесс без
изменения public contracts.

### F1 — Persistent identities и tagged project schema

**Статус:** `DONE` (2026-07-17) — exact-only schema-v5 load/write,
atomic private-adoption ID assignment, canonical `{source,key}` records and
noncanonical save/checkpoint rejection are executable-test pinned.
**Prerequisite:** текущий `BASELINE` и master spec.
**Основание:** master spec §5, §11, §54 Phase 0.

| ID | Результат |
|---|---|
| `F1.1` | Canonical project-path identity; временный runtime ID для unsaved session; без persistent `project_id` |
| `F1.2` | Random persistent IDs для atlas/source/animation/target и deterministic derived `sprite_id` |
| `F1.3` | Нормализация source-local keys, collision/traversal validation и однозначные selectors |
| `F1.4` | Canonical tagged source records и sparse sprite/frame records только в форме `{source,key}` |
| `F1.5` | Exact-only schema v5 loader; atomic assignment missing IDs на private session-adoption candidate; без legacy promotion/migrations |

**Gate:** v1–v4/future versions дают structured version error и не переписываются;
save/checkpoint не публикуют noncanonical graph; rename/reorder/save/reload не
меняют structural IDs; одинаковый source key воспроизводит `sprite_id`;
malformed/duplicate IDs и portability collisions возвращаются как structured findings.

### F2 — Typed operations, transactions и revisions

**Статус:** `IN PROGRESS` — typed operation/session foundation реализован;
полный phase gate ещё требует отдельного аудита.
**Prerequisite:** `F1`.
**Основание:** master spec §6–§8, §27 A0.

| ID | Результат |
|---|---|
| `F2.1` | Разделить model operations, session commands, derived jobs и external side effects |
| `F2.2` | Все persistent mutations выражаются typed operations по stable IDs |
| `F2.3` | Atomic transaction: prevalidation, expected revision, rollback, один commit event |
| `F2.4` | Monotonic revision, semantic saved baseline и независимый dirty state |
| `F2.5` | Idempotent external transaction IDs в рамках определённого retention contract |
| `F2.6` | Минимальные semantic inverse data и append-only journal до переключения GUI/Undo/Redo на live commit path; ordinary CLI остаётся file-oriented |

**Gate:** batch либо применяется полностью, либо не меняет состояние; один batch
создаёт одну semantic history unit; model commit не зависит от recovery I/O, а
journal failure оставляет commit видимым и переводит recovery в sticky degraded;
revision conflict/invalid revision и live-session retry idempotency test-pinned;
GUI и CLI не содержат параллельных mutation rules.

### B0 — Native import foundation

**Статус:** `IN PROGRESS` — session queue, semantic history and bounded local
journal are implemented; visible shared History and Pack hash/LRU remain.
**Prerequisite:** `F1`.
**Основание:** master spec §35, §38–§41, §43, §50 B0.

| ID | Результат |
|---|---|
| `B0.1` | Versioned Import IR и native Neotolis importer |
| `B0.2` | Deterministic rectangular/polygon/D4 materializer в canonical RGBA8 |
| `B0.3` | `atlas detect`, `atlas inspect`, `atlas extract` на одном core пути |
| `B0.4` | Exact/ambiguous/unknown detection fixtures без silent importer choice |

**Gate:** multi-page, trim, aliases, animations, все D4 transforms и polygon
regions проверены fixtures; ambiguous detection требует явного выбора; bad input
завершается structured error, а не crash.

### B1 — Linked atlas sources и Extract Sprites

**Статус:** `PLANNED`
**Prerequisites:** `F1`, `F2`, `B0`.
**Основание:** master spec §11, §42–§44, §50 B1.

| ID | Результат |
|---|---|
| `B1.1` | Read-only linked atlas source хранит format choice и importer region keys; `Change Format` коммитится только после успешной current-read validation |
| `B1.2` | Watch/Refresh/open/Pack/Export проверяют descriptor и companion files без project mutation |
| `B1.3` | Runtime source status/generation, lazy thumbnails и current-read error policy |
| `B1.4` | Extract Sprites: complete preflight, staging, conflict policy и publication |
| `B1.5` | Materialized files добавляются в текущий atlas одной model transaction; Undo не удаляет опубликованные файлы |

**Gate:** linked source остаётся read-only; watcher не меняет revision/dirty и не
запускает Pack; overwrite только explicit; ошибка до commit не оставляет partial
project mutation или опубликованный неполный набор.

### F3 — Semantic history и Pack session behavior

**Статус:** `PLANNED`
**Prerequisites:** `F2`, `B1`, `H0`.
**Основание:** master spec §7.1, §9–§10, §27 A1, §54 Phase 2.

| ID | Результат |
|---|---|
| `F3.1` | Одна serialized session queue для GUI, future MCP, Undo/Redo и Refresh |
| `F3.2` | Semantic forward/reverse diffs are production history; serialized checkpoint round-trips remain test oracles only |
| `F3.3` | Shared visible History с non-Undoable Save checkpoints |
| `F3.4` | Immutable asynchronous Pack jobs и ordered result selection |
| `F3.5` | `pack_input_hash`, stale-preview UX и memory-only byte-budget result LRU |
| `F3.6` | Extend bounded best-effort recovery, health/watermark reporting, and the 5-second healthy RPO to ownership transfer and mirrors without making recovery a commit gate |

**Gate:** forward + reverse даёт byte-identical исходное состояние; Undo/Redo
создают новые revisions; save не меняет revision; stale result остаётся видимым;
watch/edit/Undo не запускают Pack; journal failure не откатывает transaction и
не скрывает commit event, а явно деградирует только recovery.

### B2 — Format-package registry

**Статус:** `PLANNED`
**Prerequisites:** `F3`, `B0`.
**Основание:** master spec §29–§37, §40, §50 B2.

| ID | Результат |
|---|---|
| `B2.1` | Manifest разделяет manifest/package/API/data-format versions и profile |
| `B2.2` | Built-in manifests и directory discovery для global/project-local packages |
| `B2.3` | Deterministic duplicate-ID detection/error reporting со всеми origins и `.ntformat` distribution; no silent shadowing |
| `B2.4` | Versioned Export IR, capability vocabulary и target-specific diagnostics |
| `B2.5` | Declarative signatures и explicit Change Format flow |

**Gate:** builtins проходят тот же package contract, что и внешние handlers;
project target хранит только spec-defined format/data/profile/options; duplicate
IDs и unsupported API versions fail loudly; capabilities проверены golden tests.

### B3 — Sandboxed Lua и constrained templates

**Статус:** `PLANNED`
**Prerequisite:** `B2`.
**Основание:** master spec §32, §45–§48, §50 B3.

| ID | Результат |
|---|---|
| `B3.1` | Sandbox: immutable inputs, staged outputs, protected calls, deterministic host services |
| `B3.2` | Memory/instruction/output limits и cooperative cancellation |
| `B3.3` | Lua exporter/importer/probe bindings через canonical IRs |
| `B3.4` | Constrained export-only template runtime; complex transformation остаётся в Lua/core |
| `B3.5` | Package conformance runner и malformed/adversarial corpus |

**Gate:** handler не получает произвольный OS/filesystem/network access; timeout,
OOM, bad output и cancellation изолированы; staged files публикуются только после
полной проверки; builtin/Lua/template fixtures дают deterministic reports.

### A — Live automation and AI

**Статус:** `PLANNED`
**Prerequisites:** `F3`, `B3`.
**Основание:** master spec Part II, §27, §54 Phase 4.

| ID | Результат |
|---|---|
| `A.1` | In-process session abstraction и любое число GUI views одной session |
| `A.2` | Private local Dev API: discovery, snapshot/resync, transaction apply и ordered events |
| `A.3` | `ntpacker mcp`: unbound discovery, explicit one-project binding, compact tools/resources |
| `A.4` | Один external controller, authorization по canonical project path, revoke/reconnect |
| `A.5` | GUI/MCP ownership handoff, recovery mirror, journal/checkpoints и stale-host cutover |
| `A.6` | Ordinary CLI остаётся one-shot/file-oriented и не редактирует скрытую копию live project |

**Gate:** все acceptance criteria master spec §26; end-to-end Epic A outcome из
§57; mutation следует общему model commit/event contract, а recovery health и
durable watermark сообщаются отдельно; concurrent second controller и
ambiguous project selection fail explicitly.

### Breadth — Reference formats и расширение ecosystem

**Статус:** `PLANNED`
**Prerequisites:** `B1`, `B3`, `A`.
**Основание:** master spec §47–§50 B4, §54 Phase 5.

| ID | Результат |
|---|---|
| `BR.1` | Дополнительные TexturePacker/Pixi/Phaser data versions и profiles после base reference package `B3` |
| `BR.2` | Defold importer поверх существующего export contract |
| `BR.3` | Следующий формат выбирается по fixtures/user value; текущие кандидаты — libGDX и manual grid sheet |
| `BR.4` | Дополнительные data versions/profiles без обхода package contract |
| `BR.5` | GUI refinements только после подтверждённых workflow needs |

**Gate:** Epic B outcome и acceptance criteria master spec §49/§57; каждый новый
формат имеет package fixtures, round-trip/loss expectations и одинаково доступен
через core, CLI и GUI surfaces, где capability применима.

## Общие gates для каждого landing

1. Engine submodule остаётся read-only; доказанная engine-проблема уходит отдельным
   issue/PR в engine repository.
2. Native debug/release build и релевантные ctests проходят без новых warnings.
3. Invalid input возвращает structured error/notice и не приводит к abort/crash.
4. Deterministic fixtures и текущие format/CLI contracts не меняются без явного
   versioned migration.
5. Новая capability сначала реализована в core operation/source/import/export
   слое; frontend не дублирует validation, naming, capability или Undo rules.
6. Изменение public contract закрывает соответствующий open item §60 через
   prototype, fixture и golden acceptance test.

Полные task packets, acceptance tests и предлагаемая нарезка commits находятся в
[`plans/master-spec-implementation-plan.md`](plans/master-spec-implementation-plan.md).

## Вне scope этой дорожной карты

Roadmap не добавляет цели поверх master spec. В частности, текущими целями не
являются distributed/multi-writer collaboration, CRDT/automatic merge,
distributed Undo, arbitrary native plugins, trust prompts для sandboxed packages,
disk-persistent Pack cache, автоматическое переименование физических source
files, автоматический Pack от watcher events и GUI polish как самостоятельный
приоритет. Полный normative список — master spec §25 и §56.

Exact journal format, ownership cutover, cache budgets, template syntax, public
schema field names, extraction publication recovery и color-management profile
остаются открытыми контрактами §60. Они не считаются решёнными этим документом.
