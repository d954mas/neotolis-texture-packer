# 0008 — F1-02: tagged source schema v3 + `kind` при миграции v2 bare-string

**Дата:** 2026-07-13
**Статус:** accepted (нужно подтверждение владельца по двум флагам ниже)
**Принял:** deep-reasoner (F1-02, делегированные полномочия), lead review pending
**Реализуется в:** F1-02 (`tp_project` schema v2→v3, `tp_project_source`,
`tp_project_migrate`, `tp_srckey`; master spec §5.2–5.3, §11, §59 items 4–8)

## Решение

Голый `char **sources` превращается в массив tagged-сущностей
`tp_project_source { tp_id128 id; tp_source_kind kind; char *path }`. Schema
бампается v2→v3, добавляется loader-миграция `case 2`, `sources` на диске
становится массивом объектов.

### 1. Вокабуляр `kind` — `folder` / `file` (append-only)

```c
typedef enum tp_source_kind {
    TP_SOURCE_KIND_FOLDER = 0, /* рекурсивно сканируемая папка (default, zero-value) */
    TP_SOURCE_KIND_FILE   = 1  /* один image-файл */
    /* TP_SOURCE_KIND_ATLAS зарезервирован под Epic B1 (foreign atlas), append-only */
} tp_source_kind;
```

Master spec §11 рисует source как `kind: path | atlas`, где `path` делится на
«image file» и «recursively scanned folder». Plan F1-02 task 1 требует явные
kinds `file` и `folder`. Мы моделируем именно эту под-классификацию §11 «path»
как два enum-значения (`folder`/`file`); `atlas` приходит в B1 (append-only —
enum не сдвигается). То есть on-disk вокабуляр становится `{folder, file, atlas}`
и остаётся совместимым с §11.

### 2. Миграция v2 bare-string → `kind = folder` (детерминированно, без диска)

**Проблема.** Загрузка НЕ должна трогать диск: отсутствующий на диске источник —
это состояние модели, а не ошибка загрузки (§25/§32, load-rules в `tp_project.h`).
Значит `kind` нельзя надёжно определить `stat`-ом при миграции v2-строки.

**Выбор — вариант (c) из brief-а:**

- Мигрированная v2-строка детерминированно получает `kind = folder`
  (zero-value; в v3-выводе ключ `kind` для folder **не пишется** — sparse).
  Одинаковый v2-файл → одинаковые v3-байты при каждой загрузке.
- **Новые** источники, добавленные через API, записывают истинный kind там, где
  вызывающий его знает: GUI «Add Files» → `file`, GUI «Add Folder» / CLI
  `source add` / любой `tp_project_atlas_add_source` без kind → `folder` (default).
- **Scan** продолжает классифицировать file-vs-folder в рантайме по `stat`
  (`tp_scan_is_dir`), поэтому: (a) pack/export байты не меняются; (b)
  mis-labeled мигрированный file-источник всё равно пакуется корректно. Хранимый
  `kind` становится авторитетным для F1-03 (`sprite_id` отсутствующего на диске
  источника, где `stat` невозможен).

Отвергнуто:
- (a) синтаксический хинт (расширение и т.п.) — нет надёжного признака
  file-vs-folder в строке пути; «`x.png` → file» ломается на папке `textures.v2`.
- (b) единый kind + полностью рантайм-классификация без хранения — теряется
  информация для F1-03 по missing-источникам.

### 3. Синтез source ID при миграции (fill-nil, как atlas/anim/target)

- **v1-файл** (`!v2`): `tp_project_assign_legacy_ids` синтезирует
  детерминированные ID для atlas/anim/target **и source** (расширено этим
  пакетом). Source tuple = `"<atlasIdx>|<path>"`, kind `TP_ID_KIND_SOURCE`
  (совпадает с golden-конвенцией `test_migrate` `"0|assets"`).
- **v2-файл** (`v2 && !v3`): atlas/anim/target уже несут non-nil ID; у sources
  ID нет. Новый `tp_project_assign_legacy_source_ids` синтезирует **только**
  source ID; nil у atlas/anim/target остаётся аномалией и по-прежнему
  отвергается `validate_ids` (ADR 0007 п.4 сохранён — регрессии нет).
- **v3-файл**: синтеза нет; nil/отсутствующий source ID — аномалия
  (`TP_STATUS_ID_MALFORMED`), как для atlas в v2.
- Writable-сессия: `tp_project_promote_ids` расширен — теперь заполняет и nil
  source ID свежим random (та же fill-nil-only механика, что для atlas/anim/target;
  §5.5-политику promote НЕ меняю — см. флаг 1).

Канонический порядок обхода (count/assign/promote/validate) единый:
`atlas → sources → animations → targets`. Для проектов без источников порядок
идентичен F1-01 (source-записи ничего не добавляют) — существующие golden не
меняются.

### 4. Валидация источников (§5.6, production `tp_srckey`)

`tp_srckey_normalize/casefold/collides/portability` подняты в production
(`packer/src/tp_srckey.c`, tp_status-модель). `validate` (cli) добавляет
**warning**-findings: `duplicate_source` (точный дубль пути), `source_collision`
(case-fold коллизия путей, §5.3 — не молчаливое слияние), `source_portability`
(reserved name / invalid char / trailing dot-space). Все — warning, поэтому
existing `--strict` exit-коды не меняются.

## Флаги владельцу

1. **§5.5 promote policy (synthetic-vs-random).** Source ID следует той же
   fill-nil-only политике, что atlas/anim/target (ADR 0007 «Контекст и отклонение
   от §5.5»). Это pending owner decision — я его НЕ меняю, только распространяю на
   source.
2. **`kind`-эвристика для migrated/missing источников = folder.** Если владелец
   предпочитает третий kind `path`/`unknown` вместо folder-default для
   мигрированных строк — это обратно совместимое уточнение (append-only enum,
   меняется токен сериализации), без смены schema-версии.
