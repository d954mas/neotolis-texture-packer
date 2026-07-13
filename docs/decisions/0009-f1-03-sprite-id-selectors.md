# 0009 — F1-03: deterministic sprite_id, {source,key}-keyed records, lazy v3→v4 re-key, canonical selectors

**Дата:** 2026-07-13
**Статус:** accepted (нужно подтверждение владельца по флагам в §7)
**Принял:** deep-reasoner (F1-03, делегированные полномочия), lead review pending
**Реализуется в:** F1-03 (`tp_sprite_index`, `tp_selector`, `tp_project` schema v3→v4,
`tp_project_migrate`; wiring в `tp_input`/`tp_export_run`/CLI inspect/validate). Master
spec §5.2–5.4, §5.6, §59 items 5–7. Plan F1-03.

## Область: что F1-03 поставляет (groundwork) vs что переносится в F2

Review-gate уточнил границу (ruling lead-а, план F2-01):

**F1-03 РЕАЛЬНО поставляет (groundwork):** вывод `sprite_id`; runtime resolved sprite
index (`tp_sprite_index`); канонический selector resolver (`tp_selector`); персистентную
форму записи `{source, key}` в схеме v4 (сериализатор + loader, обе формы); ленивый
re-key механизм `tp_project_resolve_atlas_sprites`; index/§5.6 record-проверки в validate.
Это ЗАДЕЛ под id-based слой.

**НЕ входит в F1-03 (переносится в op-layer F2, план F2-01):**

1. **id-based ПРИМЕНЕНИЕ override/frame.** Pack/export по-прежнему матчат override и
   frame по мутабельному `name`-bridge (export-key), НЕ по `{source, key}` (см. §4).
   F1-03 это НЕ переписывает — переписка рискует байт-идентичностью и есть работа F2.
2. **production-триггер re-key.** `tp_project_resolve_atlas_sprites` СЕЙЧАС НЕ имеет
   ни одного production-вызывающего — его дергают только тесты, `inspect` и `validate`
   (ни один из них не сохраняет). Поэтому реальные GUI/CLI проекты держат записи в
   форме **PENDING `{name}`** (валидное v4 состояние, версия уже 4) и pack/export
   применяют их по name-bridge РОВНО как до F1-03. Перенос точки вызова (session
   resolution / `migrate` verb) и переход на id-based apply — F2.

До F2 name-based apply несёт два известных ограничения (см. §4a), которые id-based
apply снимает; оба — пре-существующие свойства name-keying, не регрессии F1-03.

## Контекст

`sprite_id = stable_hash(source_id + normalized source-local key)` (§5.2) уже был в
production `tp_id` (промоут F1-01), но не подключён. Sprite overrides и animation
frames были keyed мутабельным atlas-relative `name`. Требовалось: (а) вывести
детерминированный `sprite_id` на каждый resolved sprite; (б) перевести persisted
sprite records и frame references на канонический ключ; (в) добавить selector
resolver id/name/path/compound → ровно один id. **Жёсткое ограничение:** load НЕ
сканирует диск (отсутствующий source — состояние модели, не ошибка загрузки, §25/§32),
а `name` (atlas-relative, без расширения) нельзя отобразить в source-local key без
скана. **И** pack/export байты обязаны остаться идентичными.

## Решение

### 1. Форма ключа sprite-record: `{source_ref, source_local_key}`, sprite_id ВСЕГДА derived

Persisted v4 sprite record = `{source: "<source shape-id>", key: "<normalized
source-local key, расширение СОХРАНЕНО>", ...overrides}`. In-memory `tp_project_sprite`
получил `tp_id128 source_ref` + `char *src_key`. **`sprite_id` НЕ сериализуется
никогда** — он всегда выводится в момент resolution из ТЕКУЩИХ `source_id` + `key`.

Почему source_ref + key, а не opaque sprite_id VALUE: record остаётся robust к
незакрытому решению §5.5 (если source_id позже пере-рандомизируют, record всё равно
резолвится через source_ref + key и sprite_id пере-выводится). Мы НЕ трогаем §5.5
promote policy.

`name` (export-key: расширение снято, папки сохранены) СОХРАНЁН как in-memory **bridge**:
это ключ, по которому name-based pack/export путь (`tp_input`, `tp_export_run`,
`cli_inspect`, `cli_validate`) продолжает матчить override на sprite. Для migrated
record `name` выводится на load = `strip_ext(key)` — **без скана**.

### 2. Ленивая v3→v4 миграция (без диска на load)

- v3 name-keyed override грузится как **PENDING** record (`name` задан, `source_ref`
  nil, `src_key` NULL). sprite_id/source вывести нельзя (нет скана).
- v4 loader принимает ОБЕ формы: `{source, key, ...}` (migrated) ИЛИ `{name, ...}`
  (pending). Поэтому `tp_migrate` case 3 ничего не переписывает в дереве (как case 1/2);
  сам loader version-agnostic по sprite records.
- При ПЕРВОЙ resolution (`tp_project_resolve_atlas_sprites(p, atlas, idx)`, где `idx` —
  resolved sprite index, построенный сканом) каждый pending record матчится по export-key
  bridge против индекса: **ровно одно** совпадение → заполняем `source_ref` + `src_key`
  (record становится migrated); **ноль** → остаётся pending (soft-orphan, продолжает
  применяться по имени); **>1** (cross-source коллизия) → остаётся pending, НИКОГДА не
  угадывается (validate помечает ambiguous). Первый SAVE после resolution пишет v4 форму.
- Тот же проход re-keys animation frames (frame — это тоже sprite reference).

### 3. Orphan semantics

Orphan = MIGRATED record, чей `(source_ref, src_key)` отсутствует в текущем индексе.
Хранится verbatim (sparse), неактивен (name-based apply естественно ни на что не
попадает), и **реактивируется автоматически** когда ключ возвращается: scanned sprite
снова получает тот же export-key → override применяется; sprite_id пере-выводится
идентично. resolution НЕ трогает уже-migrated records, поэтому stored orphan сохраняет
идентичность. Известный край: удаление+повторное добавление ИСТОЧНИКА даёт новый random
source_id, так что реактивация по source_ref не сработает (name-based apply в рамках
того же path — сработает); полноценная source-identity-стабильная реактивация — предмет
незакрытого §5.5.

### 4. Байт-идентичность pack/export

Ключевое наблюдение: внутри ЛЮБОГО packable атласа `export-key ↔ sprite` — биекция
(два sprite с одним export-key дают export-name коллизию в `tp_normalize` и не пакуются
вместе). Поэтому матчинг override по export-key (`name` bridge) и по `(source, key)`
дают ИДЕНТИЧНЫЙ результат для всех packable атласов; расхождение возможно только в уже
непакующихся (cross-source name collision). `tp_input`/`tp_export_run` НЕ изменены (всё
ещё матчат по export-key). Frames проецируются в export-key namespace в `build_norm_opts`.
Проверено: `cli_parity` (defold+json pack goldens байт-в-байт), `cli_mutate_stable`
(net-zero round-trip байт-в-байт), `tp_export_defold`/`tp_export_json` goldens — зелёные.

### 4a. Известные ограничения name-based apply (снимаются id-based apply в F2)

Пока apply идёт по name-bridge, есть два края, где он расходится с идеальным
`{source, key}` apply. ОБА — пре-существующие свойства name-keying (v3 вело себя так же,
это НЕ регрессии F1-03), оба живут только в НЕПАКУЮЩИХСЯ атласах / особых раскладках, где
§4-биекция export-key↔sprite не держится, и оба снимает id-based apply в F2:

1. **Два источника с одинаковым filename** (finding 0). Override/frame, привязанный к
   спрайту одного source, по общему name-bridge попадает И на одноимённый спрайт другого
   source — name-bridge их не различает. id-based apply различает по `{source, key}`.
   В packable атласе такой набор всё равно даёт export-name коллизию (validate:
   `duplicate_export_key` / `export_name_collision`), так что до F2 это не молчаливая
   потеря — validate ругается, и pack на таком атласе не проходит.
2. **NFC-bridge vs сырое NFD-имя файла** (finding 2). Для migrated записи name-bridge =
   `strip_ext(NFC(key))`; если файл на диске назван в NFD (типично для macOS), сырой
   export-key при apply может не совпасть с NFC-bridge и override не применится. Все
   фикстуры ASCII, границы держатся; помечено для CI-наблюдения. id-based apply сравнивает
   нормализованный source-local key, а не сырое имя, и от этого расхождения не зависит.

### 5. Selector / compound-selector синтаксис

`tp_selector_resolve` (в core; frontend передаёт строку) резолвит в РОВНО один id:
- canonical id: `atlas_`/`source_`/`anim_`/`target_`/`sprite_` + 32 hex;
- scoped compound `<scope>:<rest>`: `atlas:`/`source:`/`anim:`/`target:`/`sprite:` (по
  kind), либо `source_<hex>:<key>` (sprite внутри конкретного source — cross-source
  disambiguation);
- bare token: поиск по всем kind (atlas name, source path, anim name, target out_path,
  sprite export/source key).

Структурные kinds резолвятся project-wide без диска; sprites — только в поданном
resolved index (контекст одного атласа). `sprite_<32hex>` — новая текстовая форма
sprite_id (в `tp_sprite_index`, т.к. в `tp_id_kind` нет SPRITE; derived, не structural).

### 6. Ambiguity candidate shape

>1 совпадение → `TP_STATUS_AMBIGUOUS_SELECTOR` + СТАБИЛЬНЫЙ candidate list (порядок обхода
проекта, НИКОГДА не сортировка по совпадению, НИКОГДА не auto-pick первого). Каждый
candidate = `{kind, id (tp_id128), atlas_index, idtext (canonical id текст), label
(human)}`. Ноль → `TP_STATUS_NOT_FOUND`. Добавлены два append-only status кода:
`not_found`, `ambiguous_selector`.

### 7. Animation frames — та же {source,key} форма (флаг владельцу)

Frames переведены на `tp_project_frame {name bridge, source_ref, src_key}` идентично
sprite records (миграция ленивая, тот же resolution проход, байт-идентичность через
name bridge в `build_norm_opts`). On-disk: migrated frame = объект `{key, source}`,
pending frame = голая строка (v3-совместимо, смешанный массив; loader принимает обе).

Альтернатива, которую отклонили: оставить frames строками (export-key). Функционально
эквивалентно для всех packable атласов (см. §4) и уже переживает reorder/reload, но
противоречит §5.4 "references target IDs" и plan task 3. Поскольку GUI собирается
локально (можно верифицировать компиляцию), выбрали полную форму.

## Что должен подтвердить владелец

1. **Форма записи** `{source_ref, source_local_key}` c always-derived sprite_id (а не
   persisted sprite_id VALUE) — foundational; robust к §5.5 re-randomization.
2. **Кто триггерит persist**: миграция pending→v4 применяется + персистится только когда
   какой-то WRITABLE поток делает resolution-скан И save. **Такого production-вызывающего
   ПОКА НЕТ** (исправление review-gate: раньше здесь ошибочно значилось «это делает GUI
   live/save»). Ни GUI, ни ordinary mutating CLI не вызывают `tp_project_resolve_atlas_sprites`
   — его дергают только тесты, `inspect` и `validate`, и ни один из них не сохраняет.
   Поэтому v4 файл, отредактированный через `ntpacker set/sprite/...` (или в GUI),
   сохраняется с pending `{name}` записями (валидное v4 состояние, версия уже 4), а
   pack/export применяют их по name-bridge. Перенос точки вызова в op-layer (session
   resolution / `migrate` verb) + переход на id-based apply — задел F2 (план F2-01).
   Механизм корректен и не сканирует на load; активация — F2.
3. **NFC-край**: name bridge migrated записи = `strip_ext(NFC(key))`; если исходное имя
   файла было не-NFC (macOS NFD), bridge может не совпасть с raw export-key при apply.
   Все фикстуры ASCII, границы держатся; помечено для CI-наблюдения.
4. **Frames в полной {source,key} форме** (§7) vs строковая упрощённая — подтвердить объём.
