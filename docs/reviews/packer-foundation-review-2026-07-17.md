# Обзор технического фундамента ntpacker

**Дата:** 2026-07-17
**Область:** текущее ядро пакера, CLI, native GUI как клиент ядра, сохранение,
история, recovery, export и build-интеграция. Визуальный UI/UX намеренно не
оценивается: для него существует отдельный epic полной перестройки.

## Вердикт

Текущий ntpacker уже не выглядит как одноразовая утилита вокруг packing
algorithm. После этого прохода у проекта есть цельная архитектурная основа:
каноническая модель, структурные ID, typed operations/session, общая семантика
Undo/Redo и recovery, строгие machine-контракты CLI, единые UTF-8/filesystem
границы и детерминированный export.

Это хорошая база для продукта на годы. Особенно правильным оказалось решение
отказаться от legacy до появления клиентов: exact-only schema v5 и удаление
переходных миграций заметно уменьшили число состояний, которые пришлось бы
поддерживать навсегда.

Но называть ядро полностью crash-proof и завершённым пока нельзя. Главный
обязательный остаток — `H0.3-H0.5`: engine builder всё ещё вызывается внутри
процесса клиента. Известный некорректный пользовательский input теперь
отсекается structured preflight, однако allocation failure, codec/output I/O
или неизвестный engine assertion всё ещё способны завершить GUI или CLI. Пока
builder не изолирован private worker process, это не мелкий долг, а последняя
архитектурная брешь в обещании «bad input/failure never crashes the host».

Итоговая оценка:

- **архитектурное направление — одобрить;**
- **foundation slice — принять: финальные gates и независимый review зелёные;**
- **production crash-safety — не объявлять завершённой до H0.3-H0.5;**
- **новые продуктовые слои строить поверх typed session, не обходя его.**

## Что сделано хорошо

### 1. Один авторитетный mutation contract

GUI и CLI больше не должны изобретать собственные правила mutation, naming,
revision, validation и Undo. `tp_session` владеет live model, а typed operation
и transaction contracts находятся ниже клиентов. Это главный долгосрочный
выигрыш: будущие MCP и Dev API смогут быть другими transport shapes, но не
другой реализацией продукта.

### 2. Каноническая schema v5 без переходного багажа

Loader принимает ровно текущую каноническую схему; старые и будущие версии
отвергаются предсказуемо. Structural IDs глобально уникальны среди atlas,
source, sprite override, animation и target, nil/duplicate IDs закрыты
валидацией. Временные synthetic/migration identities удалены, поэтому ID не
меняются скрыто при первом Save.

Для greenfield-продукта это лучше, чем раннее обещание вечной совместимости со
схемами, которыми никто не пользуется. Когда появятся реальные проекты,
миграции следует добавлять как отдельные version-to-version продукты с
fixtures, а не сохранять speculative compatibility code.

### 3. История и recovery стали семантическими

Undo/Redo хранит компактные versioned history records и использует полный
checkpoint только как детерминированный fallback для неподдержанного или
слишком большого diff. Recovery journal проверяет checksum, mixed replay,
duplicate transaction и fault paths. Save публикуется через sibling temporary
file, sync, atomic replace и parent-directory sync; неопределённость уже после
успешной публикации возвращается отдельным
`file_durability_uncertain` notice, а не ложным «Save failed».

Это заметно сильнее обычной desktop-реализации «сериализовать снимок перед
каждым действием» и хорошо подходит как человеку, так и agent-клиенту.

### 4. Machine interface является продуктом, а не побочным CLI

Каждая CLI-команда имеет versioned `--json`, фиксированный exit-code contract,
structured errors/notices и dry-run. Malformed JSON, duplicate/unknown fields,
invalid UTF-8, invalid numeric forms и rejected operations обрабатываются без
abort. Сохранённые file workflows тем самым уже пригодны для CI и AI agents;
live unsaved workflow логично остаётся будущей задачей MCP/Dev API.

### 5. Файловая и Unicode-граница стала общей

Windows argv переводится из UTF-16 в strict UTF-8 один раз. Core filesystem,
scan, identity, fingerprint, image decode, project load/save и GUI diagnostics
используют одну политику. Длинные source keys и paths больше не проходят через
маленькие frontend buffers.

Во время review дополнительно устранены несколько опасных silent-truncation
seams: Save As, Add Folder, export target browse/edit, pack work dir, screenshot
path, default target path, page identity и coalescing key. Важный принцип здесь
не размер конкретного массива, а атомарность: значение либо помещается и
сохраняется целиком, либо операция явно отвергается.

### 6. Packing-сильные стороны сохранены

Проект не свёлся к архитектурному framework. Он сохраняет практические
дифференциаторы: silhouette/NFP packing, concave geometry, полный D4 transform,
multi-page output, canonical raw RGBA ingress, Defold export и собственный
Neotolis runtime pack. Preflight теперь закрывает известные engine assertions,
page explosion ограничен, а exporter capability loss видим до записи.

### 7. Проверки ориентированы на инварианты

Тесты фиксируют не только happy path, но и byte parity, deterministic output,
recovery faults, short writes, post-publication durability, hostile JSON/UTF-8,
long paths, ID collision, stale revision и client parity. Sanitizer jobs
расширены в CI. Это правильный тип тестовой базы для инфраструктурного ядра.

## Что было плохо и исправлено в этом проходе

| Проблема | Риск | Исправление |
|---|---|---|
| VS Code запускал бинарь из sticky self-test configuration | приложение завершало headless path, окно оставалось белым | обычный `native-debug` всегда конфигурируется с self-test `OFF`; отдельный test preset — `ON`; F5 сначала reconfigure/build |
| Frontend buffers 256/600/1024 bytes | тихое перенаправление Save/Export или потеря части path/key | общий 4096-byte contract, checked all-or-nothing helpers, boundary tests |
| Sprite coalescing key хранил только 255 bytes | два разных long keys заменяли друг друга, теряя первое изменение | exact `TP_SRCKEY_MAX` key и regression с одинаковым 255-byte prefix |
| Browse target повторно отправлял exporter/enabled | path-only действие могло затереть соседнее поле | masked out-path operation + немедленный gesture flush |
| Page identity строилась через 288-byte buffer | коллизия/усечение public page name | exact arena allocation и long-name regression |
| Project JSON принимал часть некорректных чисел/ключей | неканоническое состояние и разное поведение клиентов | strict canonical-v5 parser, closed keys, finite/integer validation |
| Save publication не различала pre/post-publish failure | UI мог сообщить failure после уже совершившегося Save | typed durability notice и fault tests |
| Pack мог неограниченно создавать страницы | resource exhaustion | collision-safe lower bound, duplicate-aware guard, `TP_PACK_MAX_PAGES` |
| Exporter/preview ID проходил через локальные `char[64]`/`char[256]` | разные зарегистрированные форматы могли стать неразличимыми | единый strict-UTF-8 `TP_EXPORTER_ID_MAX` (255 bytes + NUL), validation на registry/model/operation/job границах и exact-copy tests |
| Relativizer использовал `strtok` и максимум 256 компонентов | параллельный Save был нерentrant, глубокий path терял хвост, разные UNC shares смешивались | streaming component parser, явные POSIX/drive/UNC roots и regressions на 300 компонентов и UNC server/share |
| Validation findings хранили только усечённый текстовый context | два длинных объекта с общим prefix были неразличимы для agent/CLI | exact report-owned contexts, structural IDs, bounded materialization и `validate --json` schema 2 |

## Что остаётся слабым

### P0: builder containment

Нельзя пытаться закрыть эту проблему ещё сотней preflight-проверок. Правильное
решение уже записано в decision 0018 и roadmap H0:

1. versioned bounded parent/worker protocol;
2. только ASCII-relative paths внутри staging;
3. UTF-8 input и финальная publication принадлежат parent;
4. timeout/cancel/crash/malformed response становятся structured result;
5. normal worker result byte-identical текущему in-process oracle;
6. ни crash, ни full disk не заменяют last successful preview и не оставляют
   публичный partial artifact.

Это первая следующая фаза, а не улучшение «когда-нибудь».

### Экосистема форматов пока узкая

Сильная format architecture описана, но сегодня реально поставляются главным
образом Neotolis JSON/runtime pack и Defold. Нет зрелого package discovery,
template/Lua sandbox, большого набора готовых runtime integrations и importers.
Это продуктовый gap относительно TexturePacker и Free Texture Packer, но не
причина ломать core: следующий формат должен подключаться через registry/IR, а
не новой веткой в pack orchestration.

### Import/linked-atlas vertical slice ещё впереди

Native atlas inspect/materialize, linked read-only sources и transactional
Extract Sprites остаются основным способом доказать, что Import IR и tagged
source model не только хорошо описаны, но и удобны в реальной реализации.

### Hash/cache/watch/live collaboration ещё не закончены

Canonical pack-input hash, result LRU, watchers, event sequence/resync, MCP и
Dev API нужны для полного vision. Их не следует реализовывать одним большим
framework epic. Каждый слой должен входить вертикально, с одним реальным
клиентским workflow и fault/contract tests.

### Остаточный upstream Unicode-риск GUI resources

Project/image/scan/save границы пакера уже strict UTF-8 и используют Win32 wide
API. Однако bundled `ntpacker_ui.ntpack` загружается через публичный native
resource loader движка, который внутри всё ещё использует narrow `fopen`.
Поэтому Unicode/extended-length install directory может сорвать загрузку UI.
Engine submodule здесь read-only: это должен закрыть отдельный issue/PR в
neotolis-engine, а не локальный патч его working tree.

## Что можно было бы сделать иначе

1. **Не строить generic platform раньше H0 и native import.** Typed session уже
   является достаточным швом. Сначала два тяжёлых end-to-end доказательства —
   crash-contained Pack и atlas import/extract — затем transport/event layers.
2. **Сделать caps частью типов и shared helpers.** Найденные truncation bugs
   появились там, где frontend локально придумал 256/600/1024. Для path,
   source-key, format ID и output enumeration должен существовать один contract
   и один checked constructor.
3. **Продолжать удалять заменённый код в том же slice.** Для проекта без
   клиентов совместное добавление нового boundary и удаление старого пути
   дешевле и безопаснее долгой dual-stack миграции.
4. **Не превращать engine-specific ограничения в public model.** Process
   staging должен скрыть narrow/asserting builder, чтобы будущий fallible engine
   sink можно было подставить без изменения GUI/CLI/session contracts.
5. **Держать performance budgets рядом с correctness gates.** Immutable
   snapshots, long-key buffers и recovery могут незаметно разрастись; benchmark
   должен оставаться обязательным evidence, а не разовой оптимизацией.

## Сравнение с конкурентами

Это сравнение не утверждает, что все продукты являются прямыми заменами.
Aseprite — прежде всего sprite editor, libGDX packer — часть runtime ecosystem,
а ntpacker целится в самостоятельный atlas compiler с human/agent parity.

| Продукт | Где сильнее ntpacker | Где ntpacker сильнее / может отличаться |
|---|---|---|
| [TexturePacker](https://www.codeandweb.com/texturepacker/documentation) | зрелый GUI, широкий набор framework formats, Smart Update, texture compression, поддержка и production polish; есть [custom exporters](https://www.codeandweb.com/texturepacker/documentation/custom-exporter) и развитый [CLI](https://www.codeandweb.com/texturepacker/documentation/commandline) | silhouette/concave packing как центральная возможность; open typed session/operation contracts; stable structural IDs, recovery и AI-first headless parity вместо автоматизации только вокруг saved settings |
| [libGDX TexturePacker](https://libgdx.com/wiki/tools/texture-packer) | очень простая Gradle/runtime интеграция для libGDX, проверенный MaxRects workflow, низкий порог для Java game builds | runtime-neutral project model, richer geometry/metadata, GUI+CLI+future MCP над одним core, более строгие failure/durability contracts |
| [Free Texture Packer](https://github.com/odrick/free-tex-packer) | готовая open-source GUI/CLI утилита с большим числом templates/export formats | долгосрочная архитектура model/session/recovery, native core, canonical identities и first-class machine mutation; upstream README также предупреждает, что активная разработка ограничена critical fixes |
| [Aseprite](https://www.aseprite.org/docs/sprite-sheet/) | полноценное pixel-art authoring, layers, tags, slices и удобный [CLI export](https://www.aseprite.org/docs/cli/) из исходного artwork | multi-source general atlas project, произвольные внешние изображения, silhouette packing, conversion/import direction и отделение authoring от reproducible build artifact |

Рациональная позиция ntpacker — не «ещё один TexturePacker с другим GUI». Более
защищаемая ниша: **детерминированный atlas compiler и live-capable project core,
которым на равных управляют человек, build system и AI agent**, при этом
silhouette packing и open interoperability дают технический, а не только
интерфейсный дифференциатор.

## Рекомендуемая последовательность

1. Закрыть `H0.3-H0.5` и сделать Pack/Export failure-isolated.
2. Реализовать native Neotolis atlas detect/inspect/materialize как первый
   Import IR vertical slice.
3. Добавить linked atlas source и transactional Extract Sprites.
4. Ввести canonical input hashes, stale/current rules, bounded result LRU и
   watcher generation contract.
5. Доказать unified format package/Import+Export IR вторым форматом из реального
   спроса (libGDX — разумный кандидат), затем templates и sandboxed Lua.
6. Поверх завершённого session/event contract добавить Dev API и MCP.
7. Вести отдельный UI/UX epic, не перенося presentation decisions обратно в
   core.

## Performance evidence

Историческое before/after измерение показало, что отказ от snapshot cloning не
ухудшил p50 за пределами шума (примерно 121.65 ms → 123.30 ms на HUGE snapshot),
при этом project-clone allocations снизились с 200,802 до нуля, а live bytes —
с 20,258,645 до 8,827,416 (около −56%). Compact history record для того же
класса изменения занимает 80 bytes вместо 23,956,055-byte checkpoint
(примерно в 299,451 раз меньше).

Финальный Release recheck на текущем дереве прошёл все NORMAL/LARGE/HUGE
сценарии (`2` warm-up + `3` измеряемых запуска). Для HUGE fixture snapshot p50
составил `140.1996 ms`, project-clone allocations остались равны `0`, DTO
занял `8,827,416` live bytes; Undo/Redo добавили по `80` bytes, recovery сделал
ровно одну копию raw storage. Временные значения benchmark намеренно помечены
advisory и зависят от нагрузки машины; hard correctness/resource counters
прошли. Основной оставшийся cost — materialization большого DTO snapshot и
serialization/recovery, а не ownership модели.

## Acceptance gate этого review

Финальные результаты:

- полный native Debug build и CTest: `84/84`;
- полный native Release build и CTest: `84/84`;
- `scripts/check_boundaries.sh`: `boundaries OK`;
- `git diff --check`: clean;
- Release foundation benchmark: NORMAL/LARGE/HUGE, `tp_bench_foundation: OK`;
- независимый финальный read-only review: `APPROVE`, новых или неучтённых
  P0/P1 findings в foundation slice нет;
- `.serena/` сохранена вне изменений, engine submodule не имеет diff.

Production crash-safety при этом намеренно не объявляется завершённой: уже
учтённый P0 H0.3-H0.5 остаётся открытым до изоляции builder в private worker.
