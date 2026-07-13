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

## Что должен подтвердить владелец
1. **Sprite-addressing** (§2): операция канонична `{source_id, src_key}`, но storage-запись
   остаётся pending name-bridge ради байт-идентичности с CLI. Штамповка v4-идентичности —
   F1-03 lazy re-key, не дублируется. Ок?
2. **Два новых status-токена** `unknown_op`, `out_of_range` (§4) — append-only.
3. **Явный `atlas_id`** на всех sub-entity ops (§3) вместо project-wide id-скана.
4. **Границу F2-01/F2-05** (engine core-tested, НЕ wired во фронтенд; parity доказывает
   эквивалентность) — что она задокументирована честно и это ожидаемо.
