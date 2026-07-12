# 0007 — F1-01: scope структурных ID (atlas/anim/target, но не source) + anim id/name split

**Дата:** 2026-07-12
**Статус:** accepted
**Принял:** lead (делегированные полномочия), F1-01 review
**Реализуется в:** F1-01 (`tp_id`, `tp_project_migrate`, `tp_project` schema v2;
master spec §5, §5.1, §5.2, §5.4, §5.5, §5.6)

## Решение

F1-01 навешивает persistent random 128-bit ID (`tp_id128 id`) на **atlas,
animation, target** — все три уже struct-сущности. **Source ID в этот пакет НЕ
входит.** Sprite ID тоже не входит.

1. **Source отложен в F1-02.** Сегодня source — это голый `char **sources`
   (`tp_project.h`), к строке нельзя пристегнуть стабильный per-source ID.
   Master spec §5 и plan F1-01 task-2 перечисляют source в списке ID-сущностей,
   но реальную трансформацию source вводит **plan F1-02 task 1**
   (`tp_project_source {id, kind, ...}`) + task 2 (миграция v1 strings в tagged
   path records). Пристёгивать ID к source имеет смысл только вместе с этой
   struct-ой, поэтому source_id — часть F1-02, а не F1-01. Enum-значение
   `TP_ID_KIND_SOURCE` уже присутствует (append-only), чтобы F1-02 его использовал.

2. **`tp_sprite_id` промоутится, но не вызывается.** Primitive (`hash(source_id +
   normalized key)`, master spec §5.2) поднят в `tp_id.c` как доступная функция,
   но остаётся UNUSED: ему нужен настоящий `source_id` из F1-02 и wiring
   sprite-resolution из F1-03. Предоставляем primitive — не зовём его.

3. **Animation id/name split (task 3).** `tp_project_anim.id` теперь
   `tp_id128` (структурный ID, переживает rename/reorder/save/reload). Старая v1
   строка `id` была одновременно именем И ссылкой — она мигрирует в новое поле
   `char *name` (логическое/отображаемое имя, human reference key). Frame
   references остаются **по имени** до F1-03 (там их переведут на sprite ID).
   Name-keyed API (`add_animation`/`remove_animation`, CLI `anim <name>`, GUI
   rename) сохранён рабочим — F1-03 мигрирует селекторы на ID.

4. **Promotion policy: legacy synthesis (read-only) + fill-nil promote (writable).**
   - Load v1 (или v2 с отсутствующим `id`): loader синтезирует **детерминированный**
     ID из (kind + стабильный legacy tuple: atlas index; `"idx|name"`;
     `"idx|exporter|path"`). Повторные read-only load дают идентичные ID
     (master spec §5.5). Read-only консьюмеры (inspect/validate/pack) видят
     стабильные ID.
   - Writable-сессия зовёт `tp_project_promote_ids(rng)` до первой mutation
     (CLI `commit()`, GUI init/new/open/save + каждый `gui_project_touch`
     snapshot). Promote заполняет **только nil** ID свежими random ID через
     инъектируемый RNG; **атомарно** (RNG-сбой не меняет ни одного ID — staged) и
     **идемпотентно** (после заполнения — no-op, ID никогда не переприсваивается).

## Контекст и отклонение от §5.5

§5.5 говорит: «next successful save persists normal random IDs». Буквальная
реализация из brief-а — «promote заполняет любой nil structural ID через
`tp_id128_generate`». Эти две формулировки расходятся для v1-проекта,
загруженного writable-сессией: loader уже синтезировал НЕ-nil детерминированные
ID, поэтому fill-nil-promote для них — no-op, и save сохраняет **синтетические**
(детерминированные), а не свежие random ID.

**Выбрана fill-nil-only семантика** (осознанное отклонение, зафиксировано здесь):

- Совпадает с конкретной инструкцией brief-а и делает promote простым, атомарным
  и по-настоящему идемпотентным (второй вызов — гарантированный no-op).
- Детерминированные синтетические ID валидны и уникальны — они выполняют каждое
  функциональное требование F1-01 (ID переживает rename/reorder/save/reload;
  duplicate/malformed отвергаются). Единственное свойство, которое теряется, —
  «ID именно random, а не производный», и оно важно лишь для избегания
  cross-project коллизий ID (копирование сущности между проектами), что не
  является заботой F1-01.
- Свежесозданные сущности (fresh `new`, `atlas add`, `anim create`) имеют nil ID
  и получают **random** ID при promote — то есть «random IDs at creation»
  (master spec §5) для нового контента соблюдается; синтетические ID возникают
  только у мигрированного legacy-контента.
- Serializer пишет ID как есть; nil ID (не-промоутнутая сущность) сериализуется в
  `<kind>_0…0` и **отвергается при reload** (nil where required) — это громкая,
  отлаживаемая ошибка «забыл promote», а не тихая порча. Поэтому все writable
  frontends промоутят перед save/snapshot.

Detached from any random-vs-synthetic обязательства, апгрейд до
«re-randomize synthetic on writable attach» — обратно совместимое уточнение
(fresh random вместо стабильного synthetic), которое можно внести без смены
schema или продуктовой модели, если позднее это потребуется.

## Валидация (§5.6, подмножество для F1-01)

Loader отвергает: malformed shape-ID (`TP_STATUS_ID_MALFORMED`), неверный
kind-префикс, nil там, где нужен реальный ID, и duplicate structural ID
(`TP_STATUS_DUPLICATE_ID`). Пункты про source-key/sprite-record — F1-02/F1-03.
Anim frame references (по имени) валидирует существующий `dangling_anim_frame`
(cli validate); id-based reference validation придёт с F1-03.
