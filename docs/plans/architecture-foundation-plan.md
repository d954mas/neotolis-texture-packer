# Architecture foundation plan

Статус: **IMPLEMENTED (M0–M5)** — local correctness, architecture,
performance, debug/release and boundary gates are recorded in
`docs/reviews/architecture-foundation-m1-m5.md`. Exact-SHA terminal CI is a
handoff gate whose current result is recorded on the PR and in the task handoff.

| Package | Status | Durable evidence |
|---|---|---|
| M0 | DONE | `docs/reviews/architecture-foundation-m0-baseline.md` |
| M1–M5 | DONE | `docs/reviews/architecture-foundation-m1-m5.md` |
| Migration/deletion ledger | CLOSED | `docs/reviews/architecture-foundation-migration-ledger.md` |

Нормативный источник: `docs/ntpacker-master-spec.md`

Связанные execution-документы:

- `docs/ROADMAP.md` — F2, F3 и A;
- `docs/plans/master-spec-implementation-plan.md` — общий backlog;
- `docs/decisions/0010-*` … `0015-*` — принятые F2/R контракты.

## 1. Цель

Подготовить понятный фундамент для многолетнего развития проекта без копирования
business rules между GUI, CLI, MCP и Dev API.

После foundation:

- одна `tp_session` единолично владеет mutable `tp_model`;
- frontend не владеет persistence, history, recovery или authority policy;
- public read boundary — один owned immutable session snapshot;
- operations, session commands, derived jobs и external side effects остаются
  разными контрактами;
- recovery доступен без GUI dependency;
- resource limits проверяются до materialization;
- каждый strangler slice удаляет superseded authoritative path;
- performance решения принимаются по корректным измерениям, а не по догадкам.

План реализует уже заявленные F3/A foundations. Он не добавляет новый product
scope поверх master spec.

## 2. Принципы и non-goals

### KISS

- Один serialized writer; никаких CRDT и distributed multi-writer.
- Синхронный session owner; actor thread/mailbox не вводится заранее.
- Обычные opaque C handles и value DTO; без service locator/DI-container.
- Один public snapshot lifetime contract; borrowed leases и COW — только по
  доказанному профилю.
- Прямые typed calls; без generic in-process event bus.
- Dependency injection только на side-effect boundaries: filesystem, claim/lock,
  clock, RNG и worker scheduling.

### DRY

В одном месте живут:

- validation/mutation semantics — operation/model layer;
- revision, idempotency, history и journal acknowledgement — model layer;
- live ordering, identity, saved-file fingerprint и events — session layer;
- orphan scan/claim/candidate/cleanup — recovery store;
- presentation и native dialogs — frontend adapters.

### Strangler, не big-bang

Каждый migration slice:

1. добавляет узкую финальную границу;
2. переводит одну operation/command family;
3. доказывает parity и fault behavior;
4. удаляет соответствующий старый branch/wrapper в том же slice.

Read-only shadow comparison допустим. Dual-write, две recovery state machine и
два mutable source of truth запрещены.

### Не делать в foundation

- COW/persistent entity graph;
- arena-backed mutable transaction candidate;
- generic query façade поверх модели;
- механическое дробление файлов без смены ownership;
- session authority handoff до появления второго реального host;
- одинаковый surface у CLI и live clients вопреки capability contract.

## 3. Зафиксированная классификация API

| Класс | Примеры | Commit/revision contract |
|---|---|---|
| Model transaction | typed operations | atomic; journal ACK до state/revision/event |
| Session command | Save, Save As, Discard, Undo, Redo | собственная command semantics; Undo/Redo создают model transition |
| Derived job | Pack, Inspect, Validate | immutable input token; progress/result не journal-gated model commit |
| External side effect | Export, Extract | prepare/publish/abort; не маскируется model transaction |
| Runtime event | source Refresh/status, diagnostics | не меняет project revision/dirty сам по себе |

`Pack` и `Export` не являются `tp_session_command`. Session может владеть их
runtime handles/order, но API и atomicity остаются разными.

## 4. Ownership table

| Owner | Владеет | Не владеет |
|---|---|---|
| `tp_model` | mutable project, revision, semantic saved identity/dirty anchor, history, idempotency, attached journal ACK gate | path, exact file fingerprint, dialogs, jobs, controller |
| `tp_session` | `tp_model`, session identity/path, exact saved-file fingerprint, admission/order, runtime generations, event sequence, job handles | rendering, dialogs, protocol JSON, recovery root |
| `tp_session_snapshot` | immutable owned read state + `{revision, model_generation, source_generation, event_seq}` | mutation capability |
| `tp_recovery_store` | injected recovery root/backend, bounded scan, orphan-slot handles | live model, project Save policy, controller handoff |
| `tp_recovery_live` | live-slot path, OS liveness handle, metadata attachment и clean-close lifecycle одной session | model semantics, Save policy, orphan resolution |
| `tp_recovery_claim` | exclusive right to inspect/recover/discard one orphan slot | canonical project authority |
| `tp_project_lease` | OS-backed reservation одной canonical project identity на время publish/mutation authority | orphan journal lifecycle, controller handoff policy |
| frontend adapter | intent capture, native dialogs, presentation/result mapping | business rules и mutable model aliases |

Semantic dirty baseline существует только в `tp_model`. Exact on-disk
fingerprint существует только в `tp_session` persistence state.

### `tp_session` — orchestration boundary, не god object

`tp_session` координирует lifetime, admission, ordering и вызовы owning modules.
Он не имеет права:

- реализовывать model validation или mutation semantics;
- парсить/сериализовать recovery journal records;
- реализовывать filesystem, lock или project-lease backend;
- форматировать GUI strings, dialogs, JSON или transport errors;
- реализовывать Pack/Inspect/Validate/Export algorithms;
- хранить произвольное frontend/view state;
- становиться generic service locator, manager или dependency bag.

Если новая ответственность требует собственных invariants, fault matrix либо
может тестироваться независимо от session lifecycle, она остаётся в owning
module и вызывается из `tp_session` через узкий typed contract. Разделение файла
само по себе не считается разделением ответственности.

Минимальный `tp_project_lease` входит в foundation, потому что Recovery Save уже
может конкурировать с live writer. Он не смешивается с orphan
`tp_recovery_claim`. Foundation lease уже exclusive, non-transferable и
удерживается весь lifetime saved session. Controller epoch, stale-owner takeover,
host handoff и transfer protocol переносятся в Epic A поверх этого lease.

## 5. Целевая форма

```text
GUI views/dialogs           CLI             MCP / Dev API
       |                     |                    |
       v                     v                    v
 GUI adapter           CLI adapter        protocol adapter
       \_____________________|___________________/
                             |
                transaction / command / job APIs
                             |
                             v
                         tp_session
        identity | admission | snapshots | events | jobs
                       /               \
                      v                 v
                opaque tp_model   tp_recovery_store
              ops/history/journal  orphan claim/candidate
                      \                 /
                       v               v
                   persistence + filesystem backend
```

## 6. Обязательные foundation packages

Исполнимый dependency graph:

```text
M0 -> M1 -> M2
 \     \
  -> M3 -> M4
M2 + M4 -> M5
```

M1 и M3 начинаются после M0. M4 зависит от session shell M1 и от bounded
parser/ownership contracts M3. M2 и M3 могут идти параллельно; M5 начинается
только после завершения M2 и M4.

### M0 — Contract repair и measurement integrity

**Цель.** Устранить противоречия источников истины и получить честную baseline.

Задачи:

1. Синхронизировать master spec §7.2 с принятым owner decision по
   `duplicate_id` либо явно выбрать durable result replay. Implementation plan
   не может один переопределять master spec.
2. Зафиксировать ownership table и API classification коротким ADR/master-spec
   уточнением; этот plan остаётся derived execution document.
3. Исправить benchmark harness:
   - monotonic clock;
   - warm-up;
   - individual samples и p50/p95;
   - setup вне timed region;
   - проверка status каждого operation/append;
   - отчёт fixture shape/bytes/iterations.
4. Разделить benchmark normal transaction, journal-backed Undo/Redo, recovery,
   Save и GUI rows; существующий full-snapshot bench не называть normal commit.
5. Allocation/syscall counters добавлять рядом с измеряемым hotspot, а не строить
   общий profiler framework заранее.

Gate:

- normative docs соответствуют executable behavior;
- benchmarks не считают failed operations успешными samples;
- NORMAL/LARGE/HUGE baseline воспроизводима в release build;
- абсолютные thresholds пока advisory, hard gates — correctness/count/complexity.

M0 hard-gates корректность измерения: status accounting, целостность fixtures,
воспроизводимость и честный отчёт count/complexity. Честно измеренное нарушение
целевого контракта следующего package завершает M0 и становится routed finding:
GUI allocation/filesystem и row-complexity findings блокируют M2, а recovery
limits/copies findings блокируют M3. Они не требуют реализации M2/M3 внутри M0.

Completion evidence: `docs/reviews/architecture-foundation-m0-baseline.md`.

### M1 — Session shell + единственный owned snapshot

**Цель.** Ввести финального live owner без transport-specific abstraction.

Задачи:

1. Добавить opaque `tp_session`, который единолично владеет существующим
   `tp_model`.
2. Перенести в session identity/path, exact saved-file fingerprint, admission
   sequence, runtime generations и event sequence.
3. Оставить revision и semantic dirty baseline внутри model; session их только
   читает/публикует.
4. Добавить один opaque owned `tp_session_snapshot` с immutable ID-based queries.
   Не публиковать `tp_project *`, даже `const`.
5. Добавить transaction admission с stable IDs, expected revision и caller
   transaction ID.
6. Добавить typed session commands только для Save/Save As/Discard,
   Undo/Redo.
7. Добавить ordered committed-model events и snapshot/resync primitive.
8. Перевести один GUI mutation+read slice как contract probe.

Gate:

- одна ownership chain `session → model`, без frontend mutable aliases;
- snapshot lifetime/generation покрыты sanitizer tests;
- append failure не публикует state/revision/event;
- session API не содержит GUI strings/dialogs или protocol JSON;
- session implementation не содержит model validation, recovery codec,
  filesystem/lock backend или Pack/Export algorithms;
- actor thread, borrowed view и controller epoch не появились без потребителя.

### M2 — GUI vertical cutover и steady-frame cache

**Цель.** Перевести GUI по operation families, удаляя старые пути монотонно.

Для каждой family вести migration ledger:

1. adapter резолвит index/name в stable ID в момент intent capture;
2. intent содержит captured ID + expected revision;
3. family проходит через session admission;
4. parity/fault tests зелёные;
5. соответствующие `GEDIT_*` branch и `gui_project_*` wrapper удалены.

Дополнительные задачи:

1. Перевести все GUI reads на cached `tp_session_snapshot`.
2. Кэшировать row model по
   `{atlas_id, snapshot model_generation, source_scan_generation}`.
3. `source_scan_generation` изменяется в одном scan invalidation chokepoint.
4. При rebuild строить cache-owned scratch override index; не добавлять persistent
   index в project без отдельного профиля.
5. Unchanged frame делает только key comparison.

Gate:

- 0 filesystem calls и 0 project-row heap allocations после warm-up unchanged frame;
- rebuild O(rows + overrides), не O(rows × overrides);
- нет queued collection indices;
- завершённая family не имеет старого authoritative path;
- `gui_project_get()` mutable escape удалён после последнего slice.

### M3 — Journal/resource hardening

**Цель.** Сделать adversarial input и долгие sessions bounded до MCP/Dev API.

Задачи:

1. После M0 idempotency decision ввести bounded lookup:
   - binary 128-bit key;
   - open-address hash;
   - отдельный FIFO/ring retention order;
   - eviction только после успешного durable append;
   - recovery применяет ту же deterministic policy.
2. Определить byte+count limits для:
   - journal records;
   - replay window;
   - transaction bytes/op count;
   - retained IDs/results;
   - history steps/bytes;
   - single record parse/materialization.
3. Проверять limits до grow/copy/cJSON materialization.
4. Recovery владеет одним raw journal buffer; record payloads — borrowed
   length-delimited slices, без malloc-copy каждой operation.
5. Добавить length-aware parser и единое recovery ownership/free правило.
6. Определить atomic history eviction. Операция больше всего history budget
   structured-reject'ится; silent «commit without Undo» запрещён.
7. При journal degradation не отключать idempotency молча: retryable external
   mutation подтверждается только при healthy journal ACK и bounded ID/result
   retention. Session либо сохраняет действующий in-memory retention, либо
   возвращает structured unavailable до mutation.

Gate:

- supported max input восстанавливается в принятом budget;
- сверх лимита — ранний structured reject до materialization;
- recovery time растёт линейно на 1k/10k/max records;
- raw journal storage copies ≤ 1× journal bytes;
- operation payload copies = 0;
- refs/index/parser имеют отдельные явные component budgets;
- ни один accepted retryable external transaction не остаётся без bounded
  idempotency retention;
- RSS остаётся advisory metric, не расплывчатым correctness gate.

### M4 — Shared orphan recovery extraction

**Цель.** Вынести доказанную Epic R state machine из GUI без переписывания
journal codec и без преждевременного host handoff.

Задачи:

1. Добавить opaque `tp_recovery_store`, `tp_recovery_live` и
   `tp_recovery_claim`.
2. Store создаёт `tp_recovery_live`, которым владеет `tp_session`. Live handle
   владеет live-slot path, OS liveness handle, metadata attachment/update,
   degraded mode и clean-close lifecycle; `tp_model` владеет только journal
   codec и ACK attachment.
3. Зафиксировать destroy order: detach/destroy model journal, затем close live
   handle, затем destroy session.
4. Перенести recovery root/backend, bounded scan/classification, live-slot
   create/attach/metadata/close, orphan claim, recover-to-owned-candidate и
   cleanup/discard в core.
5. Store получает app-data root снаружи; не читает GUI environment policy.
6. File backend выражает existing-only/create-new/no-follow/pinned-handle и typed
   причины `not_found/exists/permission/busy/link/io_error`.
7. Lock files постоянны; обычный release снимает OS lock, но не создаёт новый
   inode-domain через unlink/recreate.
8. Recovery candidate создаёт только detached
   `tp_session_create_detached_recovery(...)`: без controller, live-host
   registration и event publication. Перед записью Save Original приобретает
   `tp_project_lease` для original canonical identity, Save As — для canonical
   destination. Lease удерживается через conditional publication до verified
   receipt либо failure; конфликт возвращает structured
   `project_live`/`identity_collision` и не изменяет файл.
9. Claim удерживается через всю save-resolution transaction:

   ```text
   acquire orphan claim
   -> recover owned candidate
   -> create/adopt detached recovery session
   -> acquire canonical project lease
   -> Save Original/Save As with expected fingerprint
   -> receive verified save receipt
   -> finalize/delete pinned claimed journal
   -> release project lease
   -> release orphan claim
   ```

   Finalize принимает тот же claim/candidate token и receipt, привязанный к
   target/fingerprint; повторное открытие journal только по path запрещено. При
   любой ошибке journal остаётся, claim освобождается без cleanup.
10. Discard выполняется store под удерживаемым orphan claim.
11. Реализовать минимальный OS-backed `tp_project_lease`: canonical identity key,
    acquire-existing/create-new semantics, no-follow/pinned handle и typed
    conflict. Та же lock domain обязательна для всех writers:
    - saved `tp_session` держит lease от Open/успешного Save As до close или
      смены identity;
    - detached recovery session держит его только на время conditional save;
    - CLI перед file mutation проверяет/acquires тот же lease и при конфликте
      возвращает structured `project_live`;
    - Save As сначала acquires destination lease, публикует файл, затем атомарно
      меняет session identity/lease и освобождает старый lease; при ошибке
      destination lease освобождается, старый остаётся.
    Lease не содержит controller epoch или handoff state machine.
12. Перевести GUI modal на shared API и удалить GUI scan/claim/recover state machine.
13. Добавить process tests для live-session/recovery/CLI lease collision,
    Save As lease transition, active-slot competition,
    competing orphan claim,
    process death, clean close, stale lock, permission, symlink/reparse, cleanup
    failure и pinned-file replacement; тесты не требуют GUI.

Gate:

- все Epic R fault contracts сохранены;
- core recovery tests не требуют GUI executable;
- journal не удаляется до подтверждённого save/discard;
- MCP/Dev API смогут использовать recovery без GUI dependency;
- `tp_recovery_claim` не притворяется project authority lease.

### M5 — Thin-boundary finish и capability parity

**Цель.** Доказать, что разделение реально, и удалить migration scaffolding.

Задачи:

1. Оставить в GUI intent capture, dialogs, rendering и typed result mapping.
2. CLI остаётся one-shot/file-oriented, но переиспользует operations,
   validation, naming и persistence semantics.
3. Добавить in-process headless adapter/harness до transport work.
4. Создать capability-based parity matrix:
   - общий transaction/error corpus для применимых клиентов;
   - session events/recovery/jobs только для live hosts;
   - CLI не обязан эмулировать live session/history/recovery.
5. Усилить `scripts/check_boundaries.sh`: запрет frontend model internals,
   recovery ownership и duplicate business rules; отдельно запретить session
   зависимости на GUI/protocol modules и прямое размещение recovery codec,
   filesystem/lock backend или Pack/Export implementation в session module.
6. Удалить старые GUI globals/wrappers и transitional adapters, для которых
   migration ledger закрыт.

Gate:

- один golden transaction/session corpus проходит через применимые adapters;
- frontend не содержит validation/naming/dirty/Undo/recovery policy;
- unsupported capability возвращает typed capability result;
- нет второго source of truth или abandoned compatibility layer.

## 7. Отдельные post-foundation packets

Они важны, но не смешиваются с ownership foundation.

### A-FOLLOW — Canonical project authority и host handoff

Запускается при появлении второго реального live host в Epic A.

- расширить foundation `tp_project_lease` transfer/handoff semantics, не меняя
  его существующий long-lived exclusive session lifetime и не смешивая с
  `tp_recovery_claim`;
- admission close/quiesce/acquire/release state machine;
- controller ID + epoch/fencing;
- target не принимает mutation до lease acquire;
- source не принимает mutation после release;
- PID/timestamp — диагностика, не доказательство смерти owner;
- отдельный ADR + two-process handoff tests до реализации.

### P-UNDO — Compact Undo/Redo acknowledgement

**Status: TRIGGERED by M0 measurements.** The foundation keeps the measured
journal-backed semantic-history path correct and single-owned; compact acknowledgement
work remains a separate post-foundation packet. See the ROADMAP checkpoint and the
M1–M5 completion review for the measured evidence.

M0 обязательно измеряет текущий journal-backed Undo/Redo checkpoint path. Normal
recovery TXN остаётся owner-locked serialized-operation format.

Если принятый LARGE/HUGE latency/record-size budget нарушен:

1. Добавить отдельный versioned one-way `HISTORY_TRANSITION` только для
   Undo/Redo.
2. Undo кодирует resulting `before` в reverse order; Redo — resulting `after` в
   forward order.
3. Recovery применяет transition к current state, но не восстанавливает Undo
   history/cursor.
4. Synthetic public operations для restore-at-position не добавлять.
5. Normal `TXN` writer не менять.
6. Новый writer использует один новый format version; старый sidecar имеет явную
   read/migrate/version-mismatch policy.
7. Existing checkpoint остаётся deterministic fallback для unsupported/oversized
   transition на migration stage; timing-based выбор запрещён.

Gate при срабатывании trigger:

- все supported history shapes имеют parity Undo/Redo transition;
- обычный accepted Undo/Redo больше не пишет полный checkpoint;
- fallback разрешён только для явно oversized, legacy или migration случаев и
  не может маскировать target path;
- append failure сохраняет model state, revision и history cursor;
- fault/migration tests покрывают mixed sequence
  `CKPT -> TXN -> HISTORY_TRANSITION -> TXN` и старый format.

Этот packet не блокирует правильную session boundary, но не может быть молча
отложен, если M0 докажет нарушение поддерживаемого UX/memory budget.

### P-PROFILE — Дешёвые оптимизации по evidence

Кандидаты:

- immutable pack/export snapshot через malloc clone;
- composite stale token `{model_generation, source_runtime_generation}`;
- удалить ранний Save content hash, оставить late two-pass stable fingerprint;
- reuse save receipt `{bytes, len, fingerprint, normalized paths}`;
- после `mark_saved` установить известный clean state без повторного full hash;
- incremental semantic identity только после profile;
- arena clone только для immutable read snapshots;
- COW/persistent model только если всё более простое не укладывается в budget.

У каждой оптимизации обязательны before/after profile и удаляемая причина.

## 8. Performance и memory gates

### Foundation hard contracts по owning package

Это hard acceptance contracts соответствующих owning slices, а не
ретроактивные exit criteria M0. M0 устанавливает baseline и не позволяет скрыть
регрессию. M2 владеет unchanged-frame и row-rebuild gates; M3 владеет
pre-materialization limits, recovery copies и bounded resource gates.

- unchanged GUI project work: 0 heap allocations, 0 filesystem calls;
- row rebuild: доказанная O(rows + overrides);
- Save destination reads: ≤ 2× destination bytes + small fixed overhead;
- byte+count limits проверяются до materialization;
- benchmarks проверяют status и не усредняют failed samples;
- allocation high-water измеряется по компонентам;
- NORMAL сохраняет correctness, allocation и asymptotic guarantees; timing
  regression policy включается только после M0 calibration.

### Reference targets, калибруются M0

До калибровки это review targets, не flaky multi-OS CI hard-fail:

| Сценарий | Initial target |
|---|---|
| 10k-row rebuild | p95 < 8 ms или < 0.5 frame budget |
| Single-field HUGE transaction | p95 < 16.7 ms; < 8 ms stretch |
| Local-edit HUGE Undo/Redo | p95 < 50 ms; < 16.7 ms stretch |
| Max supported journal recovery | p95 < 1 s |
| Max recovery op payload copies | 0 |

После стабильной reference-machine baseline принятые thresholds становятся
versioned performance contract. Multi-OS CI следит за trends; hard timing gate
использует контролируемый runner либо ratio к baseline.

## 9. Proposed minimal file boundaries

Начать с минимального числа ownership-модулей:

```text
packer/include/tp_core/tp_session.h
packer/src/tp_session.c

packer/include/tp_core/tp_recovery.h
packer/src/tp_recovery.c

apps/gui/gui_session_adapter.c
apps/gui/gui_recovery_modal.c
```

Разделять `tp_session_command.c`, `tp_session_snapshot.c` или platform claim
backend только после появления второй независимой причины изменения/тестирования.
Не создавать `utils.c`, `manager.c`, generic `context` bag или event bus.

## 10. Verification matrix

Каждый mandatory package проходит:

- native debug/release build + ctest;
- GUI selftest;
- Linux/Windows/macOS CI;
- `scripts/check_boundaries.sh`;
- sanitizer build там, где поддерживается toolchain;
- fault tests owning module;
- deterministic golden DTO/event/journal fixtures;
- independent correctness + architecture review;
- deletion gate: superseded path действительно удалён.

Дополнительно:

- session snapshot — lifetime/generation/sanitizer tests;
- GUI cutover — per-family migration ledger и parity corpus;
- journal hardening — adversarial count/bytes/OOM corpus;
- orphan recovery — real process/lock/filesystem tests;
- journal version change — mixed/corrupt/torn migration corpus;
- performance change — before/after samples и allocation/syscall evidence.

## 11. Foundation definition of done

Foundation завершён, когда:

- существует одна ownership chain `session → model`;
- приложения не получают mutable project/model pointers;
- `tp_session` остаётся orchestration boundary и не содержит запрещённых owning
  responsibilities из §4;
- GUI migration ledger закрыт и старые branches/wrappers удалены;
- recovery scan/claim/candidate/cleanup не зависят от GUI;
- CLI и live hosts делят применимые business contracts без ложной surface parity;
- resource budgets executable и проверяются до materialization;
- hot-path regressions видны в честной benchmark evidence;
- master spec, ADRs, roadmap, implementation plan и код не противоречат друг
  другу;
- post-foundation authority/performance packets имеют явные triggers и не
  маскируются словом «потом».
