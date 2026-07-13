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

4. **Promotion policy: legacy synthesis (read-only) + re-randomize-synthetic promote
   (writable).** (§5.5-отклонение ниже РАЗРЕШЕНО в пользу spec-literal random, 2026-07-13.)
   - Load **v1** (id-less файл): loader синтезирует **детерминированный** ID из
     (kind + стабильный legacy tuple: atlas index; `"idx|name"`;
     `"idx|exporter|path"`). Повторные read-only load дают идентичные ID
     (master spec §5.5). Read-only консьюмеры (inspect/validate/pack) видят
     стабильные ID.
   - Load **v2**: синтеза НЕТ. Корректно сохранённый v2 всегда несёт non-nil ID
     (promote это гарантирует), поэтому nil или **отсутствующий** структурный `id`
     в v2 — настоящая аномалия «забыл promote / порча файла», а не legacy-случай:
     loader НЕ чинит её молча, а отвергает `TP_STATUS_ID_MALFORMED` (nil-value
     ловится ещё при парсе shape-ID, отсутствующий ключ — на
     `tp_project_validate_ids`). Синтез гейтится флагом `v2` в `tp_project_load`.
   - Writable-сессия зовёт `tp_project_promote_ids(rng)` до первой mutation
     (CLI `commit()`, GUI init/new/open/save + каждый `gui_project_touch`
     snapshot). Promote выдаёт свежий random ID (инъектируемый RNG) каждой сущности,
     чей ID **nil** (свежесозданная) ИЛИ был **синтезирован loader-ом** (legacy-gap,
     транзиентный флаг `id_synthetic`), затем сбрасывает флаг. Реальный загруженный ID
     (v3/v4, или atlas/anim/target из v2-файла) имеет `id_synthetic == false` и **не
     трогается**. **Атомарно** (RNG-сбой не меняет ни одного ID/флага — staged) и
     **идемпотентно** (после promote нет ни nil, ни synthetic — второй вызов no-op).

## §5.5: RESOLVED — spec-literal random (решение владельца, 2026-07-13)

§5.5 говорит: «the next successful save persists normal random IDs». Первоначально
(F1-01) была выбрана **fill-nil-only** семантика: loader синтезирует НЕ-nil
детерминированные ID, а fill-nil-promote для них — no-op, поэтому первый writable
save мигрированного проекта сохранял **синтетические** ID, а не свежие random.
Владелец выбрал **буквальную §5.5**: первый writable attach мигрированного legacy-
проекта должен персистить свежие RANDOM ID. Отклонение закрыто.

Реализация (`bool id_synthetic`, транзиентный, НЕ сериализуется):

- **Read-only load детерминирован**: повторные read-only load legacy-файла дают
  идентичные синтетические ID (inspect/validate/pack не меняются). Promote на
  read-only путях не зовётся.
- **Writable attach ре-рандомизирует synthetic**: `assign_legacy_scoped` (loader)
  ставит `id_synthetic = true` каждой заполненной сущности; `tp_project_promote_ids`
  выдаёт свежий random ID каждой сущности с `id nil OR id_synthetic` и сбрасывает
  флаг. Реальный загруженный ID имеет `id_synthetic == false` и не трогается.
- **Per-entity granularity (v2-корректность)**: v2-файл несёт реальные
  atlas/anim/target ID, но синтезированные SOURCE ID. Promote ре-рандомизирует
  ТОЛЬКО source ID; реальные ID сохраняются. Project-level флаг «был мигрирован»
  недостаточен — нужна per-entity гранулярность (закреплено тестом
  `test_migrated_v2_partial_promote_sources_only`).
- **Атомарность + идемпотентность**: все random ID стейджатся до записи (RNG-сбой →
  модель байт-в-байт не меняется); второй promote — no-op (нет ни nil, ни synthetic).
- Свежесозданные сущности (`new`, `atlas add`, `anim create`) имеют nil ID и так же
  получают random ID при promote — «random IDs at creation» (master spec §5) держится.

## Валидация (§5.6, подмножество для F1-01)

Loader отвергает: malformed shape-ID (`TP_STATUS_ID_MALFORMED`), неверный
kind-префикс, nil там, где нужен реальный ID, и duplicate structural ID
(`TP_STATUS_DUPLICATE_ID`). Пункты про source-key/sprite-record — F1-02/F1-03.
Anim frame references (по имени) валидирует существующий `dangling_anim_frame`
(cli validate); id-based reference validation придёт с F1-03.
