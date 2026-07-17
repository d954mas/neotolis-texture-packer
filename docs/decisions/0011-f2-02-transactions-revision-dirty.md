# 0011 — F2-02: атомарные транзакции, revision и semantic dirty

> **Foundation addendum (2026-07-16):** accepted, implemented and reviewed as part of
> M0–M5. Intermediate ownership and cutover descriptions below are historical.
> Retained transaction IDs return structured `duplicate_id` without reapplying;
> durable result replay remains an optional advertised transport upgrade.

**Дата:** 2026-07-13
**Статус:** accepted; foundation implementation reviewed 2026-07-16
**Принял:** deep-reasoner (F2-02, делегированные полномочия); foundation lead review complete
**Реализуется в:** F2-02 (`tp_transaction` в core: `tp_project_clone.c`, `tp_semantic.c`,
`tp_txn_idset.c`, `tp_txn_apply.c`, `tp_txn_parse.c`, `tp_txn_lower.c`, `tp_txn_encode.c`;
public header `tp_core/tp_transaction.h`; `tp_project_clone` в `tp_project.h`).
Master spec §7–8, §9.1, §59 items 12–19. Plan F2-02 (строки 395–420).

## Область: что F2-02 РЕАЛЬНО поставляет (groundwork) vs что переносится в F2-03/04/05

Урок F1-03 / F2-01 (decisions 0009/0010): границу фиксируем ЧЕСТНО, без overclaim.

**F2-02 РЕАЛЬНО поставляет (core-tested groundwork):**
- **Атомарная multi-op транзакция** над живой `tp_project`: validate-all → apply →
  ровно один revision-bump → один change-record. Механизм — транзакционный **CLONE**
  (см. ниже). Любой отказ (валидация op N ИЛИ allocator) оставляет живую модель
  **байт-неизменной**.
- **Канонический revision** — runtime-счётчик (НЕ сериализуется в project-file, §414),
  стартует с 0, +1 на каждую committed-транзакцию. Save revision НЕ меняет (§420).
- **`expected_revision`** optimistic concurrency: equal → commit; ниже → `revision_conflict`
  (rebuild & retry); выше → `invalid_revision`. CRDT/auto-merge ЗАПРЕЩЕНЫ (§420).
- **Semantic dirty** = `identity(current) != identity(saved baseline)`, НЕ производное от
  revision. `mark_saved` ре-базирует baseline БЕЗ смены revision.
- **Idempotency-интерфейс** с pluggable retention store (in-memory default; F2-04 journal
  может backing’ить тот же интерфейс).
- **Versioned transaction request/result JSON contract** (decode → shape collect-all →
  lower → канонический byte-stable encode). cJSON — PRIVATE dep только в `.c`.

**НЕ входит в F2-02 (по плану):**
1. **Фронтенд-cutover — F2-05.** `apps/cli/cli_mutate.c` и `apps/gui/gui_project.c` НЕ
   маршрутизируются через транзакции; их мутаторы НЕ тронуты. Ни один shipping-путь пока
   не идёт через транзакционный engine. Доказательство engine — atomicity/property-suite +
   **CLI batch JSON golden** + fault injection (`test_transaction.c`, 29 кейсов), НЕ живой
   фронтенд.
2. **Semantic diff / inverse / Undo-Redo / snapshot oracle — F2-03.** Committed-RESULT эхает
   addressing + status + новый revision, БЕЗ вычисленного `diff`-объекта. Per-op before/after
   inverse НЕ вычисляется. (В тесте dirty инверсия строится РУКАМИ — это не F2-03-движок.)
3. **Recovery journal — F2-04.** Никакого on-disk transaction-record / crash recovery.
4. **Session/network protocol — F3 / Epic A.**
5. Pack/export и per-verb сериализация НЕ изменены (байт-идентичность: `cli_parity`,
   `cli_mutate_stable`, defold/json goldens — зелёные).

## Ключевые решения

### 1. Atomicity-механизм — транзакционный CLONE (не reversible-staging)
`tp_model_apply`: `tp_project_clone(model)` → apply каждой op к КЛОНУ через F2-01
`tp_operation_apply` op-by-op → при ПОЛНОМ успехе записать id, **swap** клона на место
(free старой модели) + revision += 1. При ЛЮБОМ отказе клон уничтожается, живая модель
**байт-неизменна** (§416).

**Почему clone, а не reversible-staging:**
- **Провабельно атомарен**: commit-точка — allocation-free pointer swap (+ free + counter).
  Единственная аллокация на commit-границе — `idstore.record`, вызываемая ДО swap; её OOM
  дискардит клон с байт-неизменной моделью, поэтому swap после успешного record не может
  провалиться.
- **Переиспользует F2-01 apply БЕЗ изменений** (stage-then-commit каждой op).
- **НЕ тащит вперёд работу F2-03**: reversible-staging потребовал бы per-op inverse (ровно
  то, что делает F2-03). Clone — единственный механизм, дающий атомарность без inverse.
- Цена — full deep-copy на транзакцию. Для desktop-tool с человеко/агентными batch’ами это
  приемлемо; при доказанной необходимости F2-03/позже можно оптимизировать (structural
  sharing / undo-diff), не меняя контракт `tp_model_apply`.

### 2. OOM-safe deep-clone `tp_project_clone` (public, `tp_project.h`)
Ранее клона не было. Реализован в `tp_project_clone.c`. Инвариант OOM-безопасности: каждый
owned-указатель растущего клона ВСЕГДА либо валидный malloc, либо NULL; счётчики коллекций
покрывают только слоты со скопированными скалярами и установленными (или NULL) указателями.
Поэтому единственный `tp_project_destroy` на частичном клоне освобождает ровно построенное —
без утечки, без double-free. Приём: `*dst = *src` копирует скаляры, СРАЗУ NULL’ит owned-поля
(снимает алиасы), затем deep-copy по одному элементу. Массив аллоцируется точным размером
(calloc), count растёт по мере заполнения. PIN: `test_clone_byte_identity`
(clone → serialize обоих → идентично) + `test_clone_alloc_fault_sweep` (провал на КАЖДОЙ
глубине аллокации → NULL, no leak). Test-only fault seam `tp_project__test_set_clone_alloc_fail`.

### 3. Revision — runtime, НЕ persisted; ровно один bump; один change-record
Revision живёт в `tp_model` (wrapper: project + revision + saved baseline identity + idstore).
Никогда не пишется в `.ntpacker_project` (§414). Committed-транзакция инкрементит его ровно
один раз; change-record = сама committed-транзакция (id, новый revision, label, author, ops,
status), эхнутая в `tp_txn_result`. НЕ per-op inverse diffs.

### 4. Dirty = semantic identity (отдельно от revision), с исключением frames-order
`tp_semantic_identity(project)` — детерминированный endian-stable 128-bit хеш (FNV-1a/128 seam
из `tp_id`, byte-at-a-time, big-endian, БЕЗ libm, БЕЗ `__int128`) над УЧАСТВУЮЩИМ persistent-
разделом. Числа хешируются через каноничный decimal / `%.9g` текст (те же формы, что пишет
сериализатор) → identity байт-идентичен на всех OS.

- **Участвуют**: atlas name + 10 knobs + id; source id + normalized key (path); sprite name-
  bridge + source_ref + src_key + origin + slice9 + rename + 5×ov_*; animation id + name + fps
  + playback + flips + **frames**; target exporter_id + out_path + enabled + id; все structural id.
- **Исключены runtime** (никогда не dirty): revision-счётчик, dirty-флаг, Undo/Redo, saved
  baseline, session/authority, pack results + hashes, source watchers/mtime/pixels, thumbnails,
  GUI view state + `s_model_ver`, **project path** (identity-КЛЮЧ, §5.1, не content),
  `schema_version` (serialization envelope).
- **Order rule**: ID-keyed коллекции (atlases/sources/sprites/animations/targets) —
  **order-normalized** через КОММУТАТИВНУЮ 128-bit сумму per-element хешей (allocation-free,
  no sort, no XOR-cancellation — каждый элемент несёт свой уникальный id). Единственное
  исключение — animation `frames`: порядок СЕМАНТИЧЕН (playback), кадры фолдятся в array-order.
- **Source `kind`:** semantic identity включает `{source_id, key, kind}`. Обоснование: kind
  — persistent сериализуемый content, чья смена меняет packing и (для missing source) sprite-id
  derivation; over-detect (лишний dirty) — БЕЗОПАСНОЕ направление ошибки (никогда не заявить
  clean при изменённом content). ← **точка подтверждения владельцем.**

PIN: `test_dirty_is_semantic_identity` (edit→save→edit→inverse→clean при БОЛЬШЕМ revision),
`test_identity_excludes_runtime` (path/schema_version не влияют), `test_identity_order_normalized`
(reorder targets ≡), `test_identity_frames_order_semantic` (reorder frames ≠).

### 5. Idempotency — pluggable retention store; reuse токена `duplicate_id`
`tp_txn_idstore` — интерфейс (`contains`/`record`/`destroy` + ctx). In-memory default
(`tp_txn_idset.c`, растущий hex-set). Записываются ТОЛЬКО committed id → idempotency блокирует
ровно повторы применённых транзакций. Повтор seen id → **`duplicate_id`** (существующий
`TP_STATUS_DUPLICATE_ID`, ровно как назвал brief «spike token txn_duplicate_id»; message
отличает от structural-id-коллизии). F2-04 journal может backing’ить тот же интерфейс без
касания apply-ядра. `record` транзакционен: OOM → не записал → caller дискардит клон → модель
байт-неизменна. PIN: `test_idempotent_retry`.

### 6. Порядок валидации + дивергенция shape-collect-all vs semantic-apply-on-clone
Порядок (`tp_model_apply_json`): (1) **structural** декод envelope — fail-fast и в одиночку
(bad JSON, bad/absent schema, `bad_version`, missing/typed field, bad 32-hex id, число вне
range-checked ±2^53, unknown envelope/transaction key); (2) **idempotency** (seen id →
`duplicate_id`); (3) **revision precondition** — mismatch короткозамыкает и реджектит В
ОДИНОЧКУ (op_index −1) ДО per-op работы; (4) **per-op SHAPE** (`unknown_op`, unknown field,
malformed `*_id`) — collect-all в стабильном порядке (op_index asc, потом field order);
(5) lower в typed `tp_operation`; (6) atomic commit на клоне.

**Shape collect-all vs semantic apply-on-clone:** сбор всех semantic-фолтов против
начальной статической entity-таблицы ломает реальные intra-batch зависимости
(op0 создаёт atlas, op1 в него добавляет). Поэтому:
- **Shape-фолты** (model-НЕЗАВИСИМые: unknown op/field, malformed id) — collect-all ДО apply.
- **Semantic-фолты** (model-ЗАВИСИМые: dangling id, range, name-collision) — валидируются
  против ПРОГРЕССИВНО-применяемого КЛОНА (через `tp_operation_apply`, который validate→apply),
  что и делает create-then-use batch валидным, и репортят ПЕРВЫЙ провинившийся op (валидность
  op N зависит от успеха 1..N-1, полный collect-all не well-defined при intra-batch зависимости).

PIN: `test_json_structural_fail_fast`, `test_json_revision_short_circuit_alone`,
`test_json_shape_collect_all` (3-op фикстура: `[op0 unknown_field, op1 id_malformed, op2 unknown_op]`).

### 7. JSON-контракт: dynamic storage, UB-free числа, unknown-field REJECT
- Versioned envelope, `schema=1` — единственная принимаемая версия. **Dynamic storage**: НЕТ
  фиксированных мелких op/error-капов — большие batch’и («100
  анимаций в одной транзакции») поддержаны.
- **Byte-stable canonical encode**: ключи по возрастанию, discriminator (`schema`/`op`) первым,
  2-space/LF/trailing-newline, integral через **PRId64** (без decimal point), fractional `%.9g`
  — идентично `tp_project` writer’у. label/author sparse-omitted. Op-объект эмитится
  ПЕРЕИСПОЛЬЗУЯ F2-01 `tp_operation_encode` (единственный владелец per-kind canonical shape) со
  сдвигом отступа — batch-op байт-идентичен standalone-op.
- **Number handling UB-free** (`tp_txn_json.h`): каждое attacker-число идёт через `j_i64`
  (integral И в пределах ±2^53); вне диапазона → `out_of_range`, дробное/inf/NaN →
  `invalid_argument`; НИКОГДА out-of-range double→int cast (UBSan abort). INT — `int64_t`, так
  `5000000000` round-trip’ится на всех OS. PIN: `test_number_handling`, `test_json_request_encode_golden`.
- **Unknown-field policy = REJECT** на уровне envelope/transaction (structural) и operation
  (shape collect-all) — строже file-loader’а (dropped mutation-field мог бы заставить клиента
  поверить в несуществующий эффект).

### 8. Новые status-токены (append-only, sentinel LAST, пиннится в `test_status_id`)
Добавлено ДВА: `revision_conflict` (`expected < current`: stale, rebuild&retry),
`invalid_revision` (`expected > current`: клиентский баг). Idempotency НЕ добавляет токен —
переиспользует `duplicate_id` (как назвал brief). Токен-инфляции избегали.

### 9. Review-fix (adversarial review wf_c95eae55): JSON-контракт выровнен с typed-path
Ревью нашло, что JSON entry point (`tp_model_apply_json`) РАЗОШёлся с typed
(`tp_model_apply`). Исправлено; НОВЫХ токенов не добавлено (переиспользованы
`out_of_range`/`id_malformed`/`invalid_argument`/`bad_version`). Изменения контракта:

- **ЕДИНЫЙ preflight** (`tp_txn__preflight` в `tp_txn_apply.c`, вызывается ОБОИМИ путями):
  (a) валидация transaction-id = 32-lowercase-hex; (b) NULL-safe reset; (c) idempotency;
  (d) revision precondition. Убирает дрейф между путями. `tp_txn__is_hex32_lower` — один
  общий чек (и в structural-decode envelope, и в preflight).
- **[8] Typed path теперь валидирует формат id** ДО idempotency: пустой/garbage/не-32-hex/
  UPPERCASE id → `id_malformed` (раньше typed-путь принимал любой id, второй garbage-id
  ложно коллизил `duplicate_id`). Меняет поведение: typed `tp_model_apply` с плохим id
  теперь reject, не commit.
- **[schema] Строгий schema** через `j_i64` (не усечённый `->valueint`): `{"schema":1.9}` →
  `invalid_argument` (не-integer), out-of-range → `out_of_range`, `2` → `bad_version`.
  Раньше 1.9 усекалось в 1 и молча принималось.
- **[field] Rejected-result эхает `field`** (sparse — опускается при ""), в каноничном
  ascending-порядке `code`<`field`<`message`<`op_index`, как F2-01 `tp_op_result_encode`.
  Байт-goldens расширены; committed-golden не изменился.
- **[OOM] Shape-фолт НИКОГДА ложно не коммитит под allocation-pressure**: reject-решение
  больше НЕ зависит от `error_count>0`. `tp_txn__result_add_error` возвращает bool; shape
  collect-all держит sticky `any_fault`/`store_oom` — дропнутая (OOM) error-запись всё равно
  форсит reject (`oom`), модель байт-неизменна. Test-only seam
  `tp_txn__test_set_add_error_fail`.
- **[6] Present-but-non-string addressing id** (`"atlas_id":123`) теперь фиксируется в
  collect-all (`id_malformed`), так что весь batch репортит все плохие id за один проход
  (раньше ловилось fail-fast в lowering — один плохой id за round-trip).
- **[3] Malformed-JSON reject сохраняет revision** (`out->revision = m->revision`) — клиент
  больше не думает, что модель сбросилась в 0.
- **[1] NULL-`out` не крашит** ни на одном пути JSON (twin typed поддерживал `out==NULL`):
  wrapper собирает в локальный result, если caller передал NULL.
- **[0] sprite.override.set int16-поля** (`ov_shape/ov_allow_rotate/ov_max_vertices/
  ov_margin/ov_extrude`) range-check ДО narrowing cast (`opt_i16`, `[INT16_MIN..INT16_MAX]`):
  `ov_margin:65535` больше не wrap’ится в `-1` (== `OV_INHERIT`) с молчаливым дропом. Sweep
  `tp_txn_lower.c` подтвердил: единственный оставшийся integer-narrowing cast — slice9,
  уже range-checked `[0..65535]`.
- **[tokens]** mask↔token для sprite-clear остаются двумя ручными списками (в `tp_op_encode.c`
  и `tp_txn_lower.c`), но закреплены round-trip pin-тестом `test_sprite_clear_field_roundtrip`
  (encode(mask)→decode→mask == identity для всех 7 полей). Общая таблица — follow-up.
- **DEFERRED (не correctness, honest note):** [9] `fill_result_op` addressing-map всё ещё
  дублирует op-encoder PUSH_ID switch (байт-идентичен, риск — copy-paste дрейф); [10]
  `emit_op_embedded` encode-then-reparse не заменён на indented-encode (перф/чистота, не баг).
  Обе — follow-up, чтобы не рисковать byte-стабильностью goldens.

## Что core-тестировано СЕЙЧАС vs отложено

| Возможность | Статус F2-02 |
| --- | --- |
| Атомарный batch, revision, expected_revision, dirty, idempotency, JSON contract | ✅ core-tested (`test_transaction.c`, 29 кейсов) |
| deep-clone byte-identity + OOM sweep на каждой глубине | ✅ core-tested |
| batch == one-by-one F2-01 apply (typed + через JSON round-trip) | ✅ core-tested |
| per-op inverse diff / Undo-Redo / snapshot oracle | ⛔ F2-03 |
| on-disk journal / crash recovery | ⛔ F2-04 |
| CLI/GUI, идущие через транзакционный engine | ⛔ F2-05 (frontend cutover) |

## Что должен подтвердить владелец
1. **Atomicity-механизм = clone** (§1): провабельно атомарен, reuse F2-01, не тащит F2-03
   inverse; цена — deep-copy на транзакцию. Ок как v1?
2. **Source `kind` в semantic identity** (§4): identity включает
   `{source_id, key, kind}`. Приемлемо (over-detect как безопасное направление)?
3. **Два новых status-токена** `revision_conflict`/`invalid_revision` (§8); idempotency
   переиспользует `duplicate_id`.
4. **Semantic-фолты валидируются first-op-wins на клоне, не collect-all** (§6) — необходимая
   дивергенция ради intra-batch зависимостей (create-then-use). Shape-фолты — collect-all.
5. **Границу F2-02/F2-03/F2-04/F2-05** (§Область): engine core-tested, НЕ wired во фронтенд.
