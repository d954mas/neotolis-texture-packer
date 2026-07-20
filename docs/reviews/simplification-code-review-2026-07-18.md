# Simplification code review — 2026-07-18

Verdict at review time: do not merge. Resolved by the bounded simplification
series ending at `cf127c7`; completion evidence is recorded in
`simplification-audit-2026-07-18.md`.

До продолжения simplification-разбиения нужно устранить три P1: зависимость
первого transaction-reject от включённой истории, неполную pack-domain
валидацию и противоречащий решению владельца hard LOC/complexity gate.

## Scope

- review-диапазон: `origin/main..impl/master-spec`;
- подробно проверены последние simplification-коммиты `c508eea`, `f1de038`,
  `a90181a` и окружающие контракты в `packer/`, `apps/common`, CLI и GUI;
- `external/neotolis-engine/` не изменялся и рассматривался как read-only;
- feature completeness Import/Export IR, registry, MCP и Dev API не является
  дефектом этого review: это последующая roadmap-работа;
- проблемы, существовавшие уже в `origin/main`, отделены от регрессий текущей
  ветки.

## Findings

### P1 — История меняет первый structured reject транзакции

**Locations:** `packer/src/tp_txn_apply.c:618`,
`packer/src/tp_diff_capture.c:83`, `packer/src/tp_op_validate.c:578`.

**Trigger.** При включённой истории операция одновременно содержит invalid
UTF-8 и отсутствующий parent/entity. `tp_diff_capture_before()` выполняет lookup
до `tp_operation_apply()` и возвращает `not_found`. Без истории первым выполняется
`tp_operation_validate()`, поэтому возвращается `invalid_utf8` с другим field и
message.

Сессия включает историю штатно, поэтому результат операции зависит не от
payload/model, а от runtime attachment. Это нарушает deterministic first-reject
и parity общего operation/session слоя.

**Required fix.** На текущем transactional clone сначала выполнить каноническую
валидацию операции, затем capture, затем заранее провалидированный apply. Public
`tp_operation_apply()` должен сохранить validate+apply контракт; private
validated apply допустим только как узкая логическая граница. Нужен history
on/off golden на status, field, message и byte-unchanged model.

**Confidence:** CONFIRMED, 96%.

### P1 — `validate --strict` пропускает значения, которые отвергает Pack

**Locations:** `packer/src/tp_validate.c:1153`,
`packer/src/tp_op_validate.c:238`, `packer/src/tp_pack.c:77`,
`packer/src/tp_project.c:3367`.

**Trigger.** Канонический v5 project с `max_size=32` и `padding=64` не получает
`setting_out_of_range`: validation проверяет только `>= 0`. Operation admission
и Pack проверяют `value <= max_size`. Аналогичный drift есть для
`margin`/`extrude`, sprite override против atlas `max_size` и доменов animation
settings. Проверка raw `extrude > 0` для non-RECT фактически скрыта, потому что
`tp_project_atlas_to_settings()` сначала clamp-ит extrude.

В результате машинный оператор может получить чистый `validate --strict`, а
следующий Pack завершится structured failure.

**Required fix.** Ввести чистый typed evaluator над raw model/effective
candidate values. Operation выбирает первый issue и свой field/message;
validation накапливает все issues в стабильном порядке; Pack сохраняет
последний defensive barrier перед engine assertions. Нельзя объединять эти три
policy в один control flow.

**Confidence:** CONFIRMED, 99%.

### P1 — Hard LOC/complexity gate реализует отвергнутую policy

**Locations:** `AGENTS.md:93`, `CMakeLists.txt:37`,
`scripts/check_loc_budget.cmake:7`,
`docs/reviews/simplification-audit-2026-07-18.md:64`.

**Trigger.** Новый связный TU на 801 строку падает в обычном CTest независимо
от ясности ownership, а существующая длинная функция обязана дробиться из-за
числа, даже если regions отражают одну связную ответственность.

Это противоречит зафиксированному owner decision:

- LOC файла — важный hotspot signal, но не цель;
- никаких hard limits, baseline, ratchet или no-growth запретов;
- декомпозиция выполняется, когда возможна семантическая граница и код реально
  становится лучше;
- функцию делим только по логической границе или для реального reuse.

**Required fix.** Удалить `architecture_loc_budget` из CTest и нормативный hard
budget из `AGENTS.md`/аудита. При желании оставить отдельный non-gating LOC
inventory, который только сортирует hotspots и никогда не определяет pass/fail.

**Confidence:** CONFIRMED, 100%.

### P2 — Ошибка stored-source resolution маскируется fallback-ом к process CWD

**Locations:** `packer/src/tp_source_plan.c:66`,
`packer/src/tp_project.c:3329`.

**Trigger.** Если `source_base_dir + relative path` превышает output bound,
`tp_project_resolve_source_path()` возвращает `out_of_bounds`, но identity layer
повторяет путь через lexical absolute resolution от process CWD. Effective
source identity начинает зависеть от места запуска процесса.

**Required fix.** CWD fallback разрешён только для явного состояния «у project
нет base». `out_of_bounds` и остальные реальные resolution faults должны
возвращаться без замены.

**Confidence:** CONFIRMED, 86%.

### P2 — Save I/O failure классифицируется как malformed project

**Locations:** `packer/src/tp_project.c:2042`,
`packer/src/tp_project.c:2090`, `packer/include/tp_core/tp_error.h:28`.

**Trigger.** Unwritable directory, file sync failure или atomic replace failure
возвращает `TP_STATUS_BAD_PROJECT`; финальный replace также теряет конкретную OS
причину. Машина не может отличить корректный project с filesystem fault от
невалидного JSON.

Первый полный CTest-прогон попал именно в generic finalization failure; отдельный
повтор `tp_project` прошёл. Алгоритм staged publication не опровергнут, но текущая
диагностика не позволяет установить причину transient failure.

**Required fix.** До физического split Save/FS добавить append-only file-I/O
status либо равноценную typed category, phase/path/native-error context и
отдельное CLI mapping. Это отдельное намеренное contract change с golden tests,
а не часть move-only commit.

**Confidence:** CONFIRMED для misclassification; причина transient test failure
не подтверждена.

### P2 — F-02 изменил caller-visible normalization prose

**Location:** `packer/src/tp_op_validate.c:431`.

**Trigger.** Для `src_key="./hero.png"` прежний reject сообщал
`frame key must already be normalized as 'hero.png'`; после `f1de038` caller
возвращает вложенное `frame key is not ...: source key must ...` сообщение.
Status и field сохранились, но simplification-коммит заявлен как
contract-preserving, а изменение не зафиксировано тестом или решением.

**Required fix.** Либо вернуть caller-owned mapping через typed reason +
canonical spelling, либо явно решить, что operation prose не является stable
contract. Не размножать normalize+compare обратно по callers.

**Confidence:** CONFIRMED как observable change; severity намеренно P2, потому
что status/field не изменились.

### P2 — Contract oracles недостаточны для безопасных move-only splits

**Locations:** `apps/cli/cli_json_check.c:55`,
`packer/tests/test_diff.c:165`, `packer/tests/test_project.c:506`,
`packer/tests/test_validate.c:55`, `apps/cli/test_utf8_argv.c:126`.

Подтверждённые gaps:

- pack, mutation и version CLI payloads обычно проверяют JSON shape, но не exact
  schema/value golden; это слабее активного `docs/formats/cli-report.md`;
- history codec v1 проверяется собственным encoder↔decoder round-trip, но не
  независимыми fixed bytes;
- canonical-v5 writer сравнивается с повторным save того же writer, но не с
  внешним byte fixture;
- полный order/severity/context/message зафиксирован только для части validation
  families;
- Windows long-path тест далеко не достигает последней допустимой и первой
  запрещённой длины, включая UNC form.

Каждый gap блокирует только связанный high-risk split, а не весь проект.

**Required fix.** Добавить независимые fixed fixtures до перемещения writer,
codec, validation families, CLI output families и Windows backend.

**Confidence:** CONFIRMED, 95%.

### P2 — History removal доверяет позиции больше, чем identity

**Location:** `packer/src/tp_diff_apply.c:80`.

**Trigger.** CRC-valid HISTORY transition может содержать корректный element,
но `position` другого корректного entity. Collection removal удаляет по позиции
и не сверяет structural ID; для frame и `TP_DIFF_SHAPE_SPRITE_RECORD` не
сверяется `{source,key}` перед remove/replace. Итоговый graph может остаться
каноническим, поэтому terminal validation не гарантирует, что изменён именно
закодированный element.

**Required fix.** До history split добавить literal hostile fixtures в обеих
directions для collection/frame/sprite-record paths. Reachable формы сравнивают
identity перед remove/replace; unreachable формы остаются постоянными decoder
rejection tests.

**Confidence:** PLAUSIBLE, 67%.

### P2 — R21 защищает только один frontend-файл

**Location:** `scripts/check_boundaries.sh:647`.

**Trigger.** Повтор `MultiByteToWideChar`/`GetFullPathNameW` в другом production
CLI/GUI TU проходит, потому что rule сканирует только
`apps/common/nt_utf8_fs.c`.

**Required fix.** Сканировать production sources во всех `apps/common`,
`apps/cli`, `apps/gui`, сохранив узкие исключения для легитимного process ingress.

**Confidence:** CONFIRMED, 96%.

## Pre-existing issues kept separate

- `apps/gui/gui_canvas.c:24` дословно зеркалит D4 decode из
  `packer/src/tp_pack_read.c:26`. Это существовало в `origin/main`; текущие
  реализации совпадают, но drift может сломать overlay/hit-testing. Нужен один
  pure geometry owner без engine-type leakage.
- CLI JSON checker уже в `origin/main` проверял большинство schema только как
  «number». Текущая ветка не создала gap, но запланированный `cli_mutate` split
  нельзя делать, не усилив oracle.

## Good decisions to retain

- `c508eea` убрал реальное дублирование UTF-8→UTF-16/long-path policy, сохранив
  caller-owned buffers и CRT-local `FILE*` ownership.
- `f1de038` правильно выбрал чистый canonical source-key predicate как shared
  knowledge; исправлять нужно только observable mapping, не сам owner.
- Save использует sibling temp, file sync, fingerprint recheck, atomic publish и
  parent sync; pre-publication failure не публикует staged model.
- History replay работает через clone/swap и terminal canonical validation.
- `tp_project_clone` и `tp_diff_entity` намеренно имеют разные alloc/fault seams;
  их deep-copy duplication не следует сводить к generic allocator framework.
- CLI и GUI остаются тонкими над одним typed operation/session layer; их parsing
  и presentation не обязаны иметь одинаковую форму.

## Verification performed

- `cmake --build --preset native-debug`: no work to do, success;
- первый `ctest --preset native-debug --output-on-failure`: 84/85, transient
  failure `tp_project` на final save;
- isolated `ctest --preset native-debug -R '^tp_project$'`: pass;
- `scripts/check_boundaries.sh` локально не запустился: доступен только Windows
  WSL shim, но Linux distribution не установлен. Gate остаётся обязательным в
  Linux CI;
- worktree содержит только пользовательский untracked `.serena/`; он не
  изменялся.
