# 0010 — F2-01: типизированный operation engine (catalog / apply / validate / encode / builders)

**Дата:** 2026-07-13
**Статус:** accepted (нужно подтверждение владельца по новым status-токенам и sprite-addressing)
**Принял:** deep-reasoner (F2-01, делегированные полномочия), lead review pending
**Реализуется в:** F2-01 (`tp_operation` в core: `tp_op_catalog.c`, `tp_op_validate.c`,
`tp_op_apply.c`, `tp_op_encode.c`, `tp_op_build.c`; id-addressed accessors в `tp_project`).
Промоут принятого C0-02 spike. Master spec §4.1–4.2, §6–6.2, §7, §54 Phase-0 item 4. Plan F2-01.

## Область: что F2-01 РЕАЛЬНО поставляет (groundwork) vs что переносится в F2-05/F2-02

Урок F1-03 (decision 0009 переоценил «фронтенд подключён»): фиксируем границу ЧЕСТНО.

**F2-01 РЕАЛЬНО поставляет (core-tested groundwork):**
- `tp_operation` — typed tagged union: append-only каталог из 20 op-kind → CLI verb +
  effect class (CREATE/REMOVE/MOVE/SET) + target ID kind, ЗАКРЫТЫЙ per-op field vocab,
  БЕЗ raw field-patch escape hatch (§6.2).
- **ID-only apply** одной операции: адресация по стабильному id (не по индексу/имени),
  stage-then-commit (alloc-fault до commit оставляет модель БАЙТ-неизменной).
- Валидация range/name/exporter/reference В CORE (перенос дублей из `cli_mutate.c` +
  gui wrappers), со структурным status id + offending field + context на каждом reject.
- selector→operation builders (CLI/MCP convenience) через production `tp_selector`.
- Канонический байт-стабильный op/result encoder (golden-pinned).

**НЕ входит в F2-01 (по плану):**
1. **Фронтенд-cutover — F2-05.** `apps/cli/cli_mutate.c` и `apps/gui/gui_project.c`
   НЕ маршрутизируются через engine; их мутаторы НЕ тронуты. Ни один shipping-путь пока
   не идёт через `tp_operation_apply`. Engine проверен CORE-фикстурами (forward+error на
   каждый kind) + **parity-тестом** (op apply == существующий мутатор, байт-в-байт
   сериализованный проект — `test_op_apply.c::test_parity_*`). Это превращает «мёртвый
   задел» в «доказанно-эквивалентный задел» и снимает главную претензию review.
2. **Транзакции / revision / semantic dirty — F2-02.** F2-01 применяет ОДНУ операцию
   (validate → stage → commit). Apply структурирован так, чтобы F2-02 обернул его в
   batch (revision counter, change record, expected_revision, idempotency).
3. **Session commands** (save/save_as/discard/undo/redo/new/pack/export) — НЕ модельные
   операции. `new` разложился бы на `atlas.create` + `target.create`, но остаётся lifecycle.
4. Pack/export apply НЕ изменён (байт-идентичность: `cli_parity`, `cli_mutate_stable`,
   defold/json goldens — зелёные).

## Ключевые решения

### 1. tp_operation — реальный tagged union, промоут C0-02 каталога
`kind` — тег; `atlas_id` — родительский атлас для ВСЕХ sub-entity ops (явное scoping,
см. §5); union-arm несёт типизированный payload. Строки malloc-owned, освобождает
`tp_operation_free` (switch по kind). Каталог append-only: новые kind — перед
`TP_OP_KIND_COUNT`, никогда не переставлять (journaled record не должен сдвигаться).
`tp_op_catalog_selfcheck` пинит row-order == enum.

### 2. Sprite addressing — канонический {source_id, src_key}; запись остаётся pending name-bridge
Sprite-операции адресуются КАНОНИЧЕСКОЙ идентичностью схемы v4 `{source_id, src_key}`
(из неё выводится `sprite_id`). Wire **НЕ несёт opaque sprite_id** — он не-инвертируем
(hash), а apply нужны компоненты; `sprite_id` выводится через `tp_sprite_id()` при
необходимости.

Apply НЕ штампует `{source_ref, src_key}` на запись override: он выводит **name-bridge =
`tp_sprite_export_key(src_key)`** (единственный владелец strip-ext) и ключит запись по
нему — РОВНО как `cli_mutate.c` сегодня, оставляя запись в форме PENDING. Это сохраняет
байт-идентичность с CLI (parity-тест `test_parity_sprite_override`). Штамповка
канонической идентичности на запись — это ленивый v3→v4 re-key
(`tp_project_resolve_atlas_sprites`, decision 0009 §2), НЕ дублируется здесь.
Итог: **операция id/key-адресована канонически, storage-представление не изменено**.
Frame references несут name-bridge (pending), идентично модели и CLI.

### 3. Явный parent `atlas_id` на каждом sub-entity op (divergence от C0-02)
C0-02 минимальный vocab полагался на project-wide уникальность id (`source.remove` =
`{source_id}`). F2-01 несёт `atlas_id` на source/sprite/anim/target ops и оперирует
ВНУТРИ этого атласа — explicit-over-implicit, без project-wide id-скана, совпадает с тем,
как builder резолвит (в контексте атласа). Каталог vocab обновлён соответственно.

### 4. Новые status-токены (append-only, sentinel LAST, пиннится в `test_status_id`)
Добавлено ДВА: `unknown_op` (kind не в каталоге), `out_of_range` (значение payload вне
допустимого диапазона — клиент правит значение). Переиспользовано: `id_malformed`
(битый/нулевой id), `not_found` (dangling id-reference ИЛИ unresolved selector —
различает message), `invalid_argument` (пусто/NULL/неверная форма), `out_of_bounds`
(frame index вне диапазона), `duplicate_id` (create с уже занятым id). Токен-инфляции
избегали: near-duplicate `bad_reference` схлопнут в `not_found`.

### 5. Stage-then-commit apply + alloc-fault seam
Простые ops делегируют OOM-safe мутаторам `tp_project` (провал grow/dup возвращает OOM,
не инкрементируя count → модель не тронута). Единственный истинно КОМПАУНД —
`animation.create` с начальными кадрами (и reserved `animation.frames.set`): все кадры
собираются в staging-буфер ЧЕРЕЗ fault-seam ДО касания модели, splice только при полном
успехе. Test-only `tp_op__test_set_alloc_fail(N)` роняет N-ю staging-аллокацию;
`test_alloc_fail_before_commit` пинит, что провал оставляет проект байт-неизменным.

### 6. CREATE-ops несут id новой сущности (детерминизм), apply штампует + гасит synthetic
`atlas.create`/`source.add`/`animation.create`/`target.create` несут id создаваемой
сущности; apply создаёт через существующий мутатор, затем присваивает `id` из операции
и ставит `id_synthetic=false` (promote не пере-рандомизирует). Deterministic и обратимо.

### 7. id-addressed accessors добавлены в `tp_project` (задел task 2)
Добавлены (только новые функции, поведение существующих не тронуто):
`tp_project_find_atlas_by_id`, `tp_project_atlas_find_animation_by_id` /
`_remove_animation_by_id`, `tp_project_atlas_find_target_by_id` / `_remove_target_by_id`
(source уже имел find/remove_by_id). Это «id-based apply на уровне операции», о котором
говорит план.

### 8. Encoder байт-стабилен (конвенции `tp_sb`), frame.remove по индексу
Encoder использует те же правила, что `tp_project` writer (2-space/LF/%.9g/ключи по
возрастанию, `op` первым, trailing NL) — goldens байт-идентичны на всех OS. Sparse: SET
эмитит только поля из presence-mask. `animation.frame.remove` адресуется ИНДЕКСОМ (порядок
кадров семантичен), не именем — divergence от C0-02 `{anim_id, frame, index}`.

## Ревизия по adversarial-review (wf_b51c2d8f-b99): 9 подтверждённых дефектов

Review подтвердил, что промоут местами разошёлся с ORACLE (`apps/cli/cli_mutate.c` + его
`tp_project_*` мутаторы). Принцип фикса: op-engine — **точный промоут CLI-мутатора, ни
строже, ни слабее**. Каждый фикс доказан parity/behavior-тестом (test_operation.c /
test_op_apply.c), НЕ на веру.

**Корректность/UB:**
- **[1] Отрицательный `frame_count`.** `validate_frames` теперь отклоняет `n < 0`
  (`out_of_range`, field `frame_count`) для `animation.create` и `.frames.set` — раньше
  цикл `for(i=0;i<n;i++)` пропускал, apply писал `frame_cap=-1`, а последующий
  `frame.add` считал `&frames[-1]` (heap-underflow). Тест: reject + модель байт-неизменна.
- **[2] `source.add` терял `source_id` на path-dedupe.** Мутатор `add_source_kind`
  дедупит совпадающий '/'-нормализованный путь (count не растёт), из-за чего id операции
  НЕ штамповался, но apply возвращал committed — id-контракт нарушался (позже
  id-адресованная операция → NOT_FOUND). **Решение:** `validate` теперь отклоняет
  `source.add`, чей нормализованный путь уже есть в атласе (`invalid_argument`, field
  `key`), через НОВЫЙ публичный предикат `tp_project_atlas_has_source_path` (обёртка над
  тем же static `atlas_has_source_path`, что дедупит мутатор → семантика dedupe идентична).
  Обоснование: id-контракт «создать НОВЫЙ source с ЭТИМ id по этому пути» не может
  honor путь, уже принадлежащий другому source; переприсвоение id существующему source
  сломало бы уже сохранённую ссылку. apply дополнительно defense-in-depth: dedupe-no-op
  (недостижим после validate) → `invalid_argument`, НИКОГДА не «committed» с потерянным id.
  **Parity-нюанс (честно):** CLI `add` молча дедупит (`dup++; continue`), engine
  КОНФЛИКТ ОТКЛОНЯЕТ. Это осознанная дивергенция ради целостности id; F2-05 CLI-адаптер
  будет pre-check/skip до построения операции, сохраняя CLI-UX.

**Op-vs-CLI parity:**
- **[3] `atlas.create` дублирующее ИМЯ.** validate отклоняет create с уже занятым именем
  (`invalid_argument`, field `name`) — как CLI `atlas add` (exact `strcmp`, case-sensitive,
  зеркалит `resolve_atlas`).
- **[4] `atlas.rename` коллизия имени.** validate отклоняет rename на имя ДРУГОГО атласа;
  rename-в-себя (то же имя) разрешён — как CLI `atlas rename` (`other>=0 && other!=ai`).
- **[6] `frame.move` большой `to_index`.** validate БОЛЬШЕ не ограничивает `to_index`
  (CLI `anim move-frame` клампит большой/отрицательный destination к последнему/первому
  слоту; мутатор клампит `dst`). `from_index` по-прежнему проверяется. apply клампит
  `to_index` в `[0, frame_count-1]` ДО вычитания `to - from` (произвольный клиентский int
  иначе переполнил бы `int` — UB); результат байт-идентичен CLI-идиоме.
- **[7] Верхняя граница `padding/margin/extrude`.** Была `[0..4096]`, CLI `set` принимает
  любой `>= 0` (авторитетный кламп — в tp_pack). Теперь только `>= 0` (`min_i`); `max_size`
  сохраняет `[1..4096]`, остальные knob-границы (alpha_threshold/max_vertices/shape/
  pixels_per_unit) уже совпадали с CLI и не тронуты.

**Builder:**
- **[5] `target.set`/`anim.remove` резолвили sub-entity ГЛОБАЛЬНО.** Резолв цели шёл
  project-wide, а `out->atlas_id` брался из отдельно-резолвнутого атласа → уникальная цель
  в атласе B молча спаривалась с атласом A (validate/apply потом → NOT_FOUND). Добавлен
  `resolve_in_atlas`: цель резолвится, затем `res.atlas_index` сверяется с резолвнутым
  атласом; cross-atlas матч → NOT_FOUND. Применено к ОБОИМ multi-resolve builder'ам
  (`target.set` — дефект из review; `anim.remove` — идентичная форма того же бага, чинится
  тем же helper'ом, чтобы не оставить известный дефект в соседе).

**DRY:**
- **[8]** `find_source/find_anim/find_target` в validate теперь тонкие const-адаптеры над
  публичными `tp_project_atlas_find_*_by_id` (дублирующие циклы удалены; поведение то же).
- **[9]** `dup_str` в build.c удалён — используется `tp_strdup` из `tp_strutil.h`.

**Status-токены:** новых НЕ добавлено (избегаем инфляции, §4). Коллизии имени/пути и
underflow переиспользуют `invalid_argument` / `out_of_range`. `test_status_id` не тронут.

## Что должен подтвердить владелец
1. **Sprite-addressing** (§2): операция канонична `{source_id, src_key}`, но storage-запись
   остаётся pending name-bridge ради байт-идентичности с CLI. Штамповка v4-идентичности —
   F1-03 lazy re-key, не дублируется. Ок?
2. **Два новых status-токена** `unknown_op`, `out_of_range` (§4) — append-only. Review-фиксы
   новых токенов НЕ добавили (коллизии → `invalid_argument`).
3. **Явный `atlas_id`** на всех sub-entity ops (§3) вместо project-wide id-скана.
4. **Границу F2-01/F2-05** (engine core-tested, НЕ wired во фронтенд; parity доказывает
   эквивалентность) — что она задокументирована честно и это ожидаемо.
5. **`source.add` осознанно строже CLI** (§[2] выше): при дублирующем пути engine
   ОТКЛОНЯЕТ (id-контракт), тогда как CLI молча дедупит. F2-05-адаптер восстановит
   CLI-UX через pre-check/skip. Подтвердить, что дивергенция приемлема на уровне engine.
