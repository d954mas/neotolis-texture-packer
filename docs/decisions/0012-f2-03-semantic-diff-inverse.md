# 0012 — F2-03: semantic diff / точный inverse (Undo) + redo + snapshot oracle

**Дата:** 2026-07-13
**Статус:** accepted (нужно подтверждение владельца по inverse-механизму, revision-bump на Undo/Redo и границе F2-03/F3-02)
**Принял:** deep-reasoner (F2-03, делегированные полномочия), lead review pending
**Реализуется в:** F2-03 (`tp_diff` в core: `tp_diff_entity.c`, `tp_diff_capture.c`,
`tp_diff_apply.c`, `tp_history.c`; public header `tp_core/tp_diff.h`; поле `history` в
`tp_model` + capture-хук в `tp_txn_apply.c`). Master spec §9–9.5, §59 items 15–17.
Plan F2-03 (строки 422–448). Строится на F2-02 (`tp_model` / `tp_operation_apply` /
`tp_project_clone` / `tp_semantic_identity` / commit-путь).

**Review-фиксы (2026-07-13, adversarial 15-агентный review):** [1]/[2] defensive capture
(resolve+bounds-check ДО deref — enabled == history-less на hostile-input, без crash), [3]
reserve-before-record ordering (id-recorded ⟹ commit-can’t-fail; reserve-OOM ретраибелен),
[4] один `tp_txn__commit_reject` cleanup-хелпер, [5] форк deep-copy оставлен + completeness-
oracle, [6] REMOVE делегирован публичным функциям, [7]/[8] dedupe sprite-bridge + вычищены
мёртвые diff-поля/lookup. См. §3/§6 и таблицу покрытия ниже.

## Область: что F2-03 РЕАЛЬНО поставляет (groundwork) vs что переносится

Урок F1-03 / F2-01 / F2-02 (decisions 0009/0010/0011): границу фиксируем ЧЕСТНО, без overclaim.

**F2-03 РЕАЛЬНО поставляет (core-tested groundwork):**
- **Компактный per-op semantic diff** committed-транзакции: per-op before/after ДАННЫЕ +
  ordering position (НЕ full project snapshot как штатный механизм). Захватывается по ходу
  F2-02 commit’а.
- **Точный inverse (Undo)** одной транзакции + **redo replay** — восстановление
  предтранзакционного состояния БАЙТ-В-БАЙТ.
- **Минимальный in-memory undo/redo history-primitive** на `tp_model`: стек + курсор +
  **redo-branch discard** (новая транзакция после Undo сбрасывает redo-ветку).
- **Transaction label/author** переносятся в history-record.
- **ORACLE-suite** (`test_diff.c`, 46 кейсов), доказывающий A→forward→B→inverse→A′ ==
  A побайтно И == legacy full-snapshot restore (`tp_project_save_buffer`/`load_buffer`),
  плюс defensive-capture, reserve-before-record и all-fields completeness (review-фиксы).

**НЕ входит в F2-03 (по плану) — честная граница:**
1. **GUI/session Undo cutover — F3-02.** НЕ подключены Ctrl+Z, save-checkpoint visibility,
   ownership. History здесь — ENGINE-примитив; session/GUI-семантика ложится сверху позже.
2. **Frontend routing — F2-05.** Shipping CLI/GUI не идут через engine.
3. **Recovery journal / crash survival — F2-04.** History — in-memory session-state; НЕ
   сериализуется, НЕ переживает reopen/crash (plan non-goal §447).
4. **Legacy GUI snapshot stack (`apps/gui/gui_history.c`) НЕ удалён и НЕ перепроводен** —
   оставлен как есть до cutover’а. Oracle ВОСПРОИЗВОДИТ его full-snapshot restore
   (`save_buffer`→`load_buffer`) как comparison-oracle в тесте (см. §5). Это и есть
   «спрятать за test/debug adapter»: механизм полного снапшота живёт как oracle-примитив в
   core/тесте, а GUI-стек не трогаем.
5. Pack/export и per-verb сериализация НЕ изменены; F2-02 atomicity/dirty НЕ изменены
   (существующие byte-identity goldens зелёные; `test_transaction.c` 29 кейсов без изменений).

## Ключевое архитектурное решение: inverse = STATE-CAPTURE (B), не inverse-as-operations (A)

Диф записывает СЫРЫЕ before/after ДАННЫЕ затронутой сущности + position (формы C0-02 §6:
CREATE=after entity+position; REMOVE=before entity+position; MOVE=from/to index;
SET=before/after field values), и отдельный diff-apply восстанавливает данные напрямую.
Отвергнут вариант (A) inverse-as-operations (каждая op → обратная op(s)).

**Почему (B), а не (A):**
- **Byte-identity под order-чувствительным сериализатором.** `tp_project_save_buffer` пишет
  коллекции в ARRAY-порядке. Append-only op-каталог НЕ умеет позиционного создания
  (`atlas.create`/`source.add` всегда append в конец) — (A) не восстановил бы удаление из
  СЕРЕДИНЫ массива байт-в-байт. (B) вставляет захваченную сущность на точный index →
  порядок восстановлен точно.
- **Coarse REMOVE.** `atlas.remove` роняет весь подграф (sources/sprites/animations/targets).
  (A) инвертировал бы это в хрупкий multi-op batch реконструкции с точным совпадением
  re-created id/position; (B) восстанавливает захваченный подграф verbatim.
- **Sparse sprite-overrides.** `sprite.override.set/.clear/.name.set` по каталогу — класс SET,
  но эффект на массив `sprites[]` — create/remove/modify sparse-записи. «Обратный SET» (A) не
  выражает переход присутствия; record-snapshot (B) выражает.
- **Совпадение с контрактом C0-02 §6** (data-shaped) и **переиспользование F2-02
  clone/swap** для stage-then-commit inverse (см. §3).

**Цена / компромисс (честно):** (B) — ВТОРОЙ mutation-путь (diff-apply), который должен
оставаться консистентным с моделью и сериализатором. Именно это стережёт ORACLE: побайтная
эквивалентность diff-restore и full-snapshot-restore для КАЖДОГО op-kind + cascade — прямое
доказательство, что второй путь не разошёлся с первым.

### Per-op diff-формы (все 20 kinds каталога → 4 класса эффекта)

- **COLL (CREATE/REMOVE)** — `atlas/source/animation/target/frame` create/remove: захват
  созданной/удалённой сущности (deep-copy) + `position` (index). Undo created→remove@pos /
  removed→insert@pos; redo зеркально. `atlas.remove` захватывает ВЕСЬ подграф.
- **FRAME_MOVE** — `animation.frame.move`: `from_index`+`to_index` (фактические, clamped как
  их сдвинул apply). Undo move(to→from); redo move(from→to).
- **SET (field snapshot)** — `atlas.rename` (name), `atlas.settings.set` (10 knobs),
  `source.replace` (path), `target.set` (exporter/out/enabled), `animation.settings.set`
  (fps/playback/flips), `animation.frames.set` (весь список frames): before/after значения,
  in-place.
- **SPRITE_RECORD** — `sprite.override.set/.clear/.name.set`: before/after sparse-запись
  {present?, index, копия полей}. Reconcile: present-переход → insert/remove, оба present →
  replace-in-place. Запись мелкая → захват целиком компактен.

Все reserved-ops (`source.replace`, `animation.frames.set`) покрыты и протестированы.

## §3. Как capture цепляется к F2-02 commit БЕЗ слома atomicity

`tp_txn__commit_validated` (F2-02): clone модели → apply каждой op к КЛОНУ → swap.
F2-03 РАСШИРЯЕТ это, только когда `m->history != NULL` (иначе — **ровно** путь F2-02, все
29 F2-02-кейсов без изменений):

1. После clone — `tp_diff_record_new(label, author, op_count)` (ops-массив предвыделен на
   op_count → push_op не растёт).
2. В цикле по ops: `capture_before(clone, op)` → `tp_operation_apply(clone, op)` (F2-01,
   без изменений) → `capture_after(clone, op)` → `push_op` (move, alloc-free).
3. `tp_history_reserve(history)` — гарантирует слот ДО record/swap.
4. `idstore.record(id)` — ПОСЛЕДНИЙ fallible-шаг.
5. **swap** (allocation-free) + revision += 1.
6. `tp_history_push_reserved(history, rec)` — alloc-free, сбрасывает redo-ветку.

**Defensive capture (review [1]/[2], исправлено).** `capture_before` выполняется ДО того,
как `tp_operation_apply` валидирует op. Поэтому capture ОБЯЗАН быть так же защищён, как
`tp_diff_apply.c`: резолвит родительский atlas + адресуемую суб-сущность и bounds-check’ит
КАЖДЫЙ индекс ДО любого разыменования. Dangling id → `NOT_FOUND`, out-of-range index (напр.
`animation.frame.remove`) → `OUT_OF_BOUNDS` — структурированный статус, НИКОГДА не NULL-deref
/ OOB-read. Commit-цикл трактует ref/bounds-фейл capture как reject этой op (op_index i, тот
же статус, что возвращает history-LESS путь на этот же ввод); OOM в capture — alloc-фейл
(op_index -1). Итог: включение history НЕ меняет контракт «bad input → structured error,
model byte-unchanged, never crash». PIN: `test_capture_dangling_*`,
`test_capture_oob_frame_remove_index` (enabled == disabled результат, байт-неизменно).

**Порядок fallible-шагов (review [3], исправлено): reserve ДО record.** ВСЯ fallible-работа
(clone / record_new / capture / `history_reserve`) идёт ДО `idstore.record`; после record —
только alloc-free swap + revision++ + push. Инвариант «id recorded ⟹ commit не может упасть»:
раньше `reserve` стоял ПОСЛЕ `record`, и OOM в reserve оставлял id в idstore навсегда (нет
unrecord-хука) при дискарде транзакции → каждый retry ловил `DUPLICATE_ID` (тихо потерянная,
неретраибельная правка). Теперь reserve-OOM падает ДО record → id НЕ записан → тот же
transaction id ретраится и коммитится. PIN: `test_reserve_oom_leaves_id_retryable`
(fault-seam `tp_history__test_fail_next_reserve`).

**Инвариант atomicity сохранён:** весь diff-record строится ДО swap. ЛЮБОЙ отказ (clone /
record_new / capture ref-fault-или-OOM / reserve / idstore.record) — ДО swap: клон
дискардится, diff освобождается, живая модель **байт-неизменна**, revision не меняется,
history-entry не появляется. Cleanup-and-fail всех веток вынесен в ОДИН хелпер
`tp_txn__commit_reject` (review [4]: было ~7 почти одинаковых блоков). Post-swap шаги (push,
out-echo) allocation-free либо «не un-commit’ят» (как F2-02 out-echo). PIN:
`test_capture_alloc_failure_fails_commit_atomically` (sweep каждой diff-аллокации commit’а →
reject + byte-unchanged + нет history-entry).

## §4. Inverse/Redo apply — stage-then-commit через F2-02 clone/swap

`tp_model_undo`/`tp_model_redo`: clone живой модели → `tp_diff_record_apply(clone, rec,
reverse)` (Undo — ops в ОБРАТНОМ порядке; Redo — прямом) → на ПОЛНЫЙ успех swap + revision
+= 1 + сдвиг курсора. Отказ mid-inverse (аллокация ИЛИ corrupted diff) → клон дискардится,
живая модель/revision/курсор **байт-неизменны** (ROLLBACK). Переиспользует ту же clone-swap
атомарность, что и forward commit. Diff-аллокации при вставке идут через diff fault-seam →
sweep’ятся. PIN: `test_inverse_alloc_failure_rolls_back` (sweep КАЖДОЙ staging-глубины
inverse’а atlas.remove → OOM + byte-unchanged + курсор цел).

**UB-clean на hostile diff:** каждая entity-ссылка резолвится и КАЖДЫЙ index bounds-check’ится
ДО разыменования (size_t-математика). Stale/unknown id → `NOT_FOUND`; out-of-range position →
`OUT_OF_BOUNDS` — структурированная ошибка, НИКОГДА не crash/UB. PIN:
`test_corrupted_diff_unknown_atlas`, `test_corrupted_diff_bad_position` (модель байт-неизменна).

**Revision на Undo/Redo:** каждый успешный Undo/Redo — НОВОЕ committed-состояние, revision
+= 1 (монотонность §8; не «откат» revision). Не влияет на byte-identity: revision — runtime,
НЕ сериализуется (save_buffer его исключает). Dirty остаётся identity-derived: Undo к saved
baseline — CLEAN даже при БОЛЬШЕМ revision. PIN:
`test_dirty_clean_after_undo_to_saved_baseline`. ← **точка подтверждения владельцем.**

## §5. In-memory history model + redo-branch discard

`tp_history`: `records[]` (owned) + `count` + `pos` (курсор). `records[0..pos-1]` — undoable,
`records[pos..count-1]` — redo-ветка. `can_undo = pos>0`; `can_redo = pos<count`. Push:
освобождает redo-ветку → `count=pos` → кладёт record на курсор → `pos++`. Undo/Redo двигают
курсор. Deep-copy/ownership: record ВЛАДЕЕТ захваченными строками/сущностями, освобождает
ровно один раз (`tp_diff_op_free` идёт по всем возможным owned-указателям; NULL для
незадействованной формы — no-op, паттерн `tp_operation_free`). PIN:
`test_redo_branch_discard`, `test_multi_step_undo_redo`.

**Legacy snapshot как oracle-adapter:** oracle сохраняет full-snapshot A через
`tp_project_save_buffer`, после diff-Undo восстанавливает через `tp_project_load_buffer` и
сравнивает: diff-restore == full-snapshot-restore == A. Это «legacy механизм» как
comparison-oracle; GUI `gui_history.c` не тронут.

## §6. Deep-copy / ownership / детерминизм

- **Element deep-copy/free** (`tp_diff_entity.c`) для source/sprite/anim(+frames)/target/atlas-
  подграфа — ОТДЕЛЬНО от `tp_project_clone.c` (осознанно): свой diff fault-seam (`tp_diff__alloc`)
  не трогает clone-alloc-count goldens F2-02, и diff владеет своими данными со своей single-free
  дисциплиной. OOM-safe инвариант тот же, что у clone (owned-указатель всегда valid|NULL,
  count покрывает только заполненные слоты → single destroy без leak/double-free).
- **Форк deep-copy НЕ дедуплицирован (review [5]) — осознанно, с safety net’ом.** Дедуп через
  параметризацию clone-хелперов аллокатором рискует goldens byte-identity/alloc-count F2-02
  (сопряжение diff-fault-инъекции с clone-seam’ом — ровно то, что §6 разделял намеренно). Форк
  ОСТАВЛЕН, на ОБЕИХ копиях — громкий FORK-SYNC-комментарий с перекрёстной ссылкой. ЗАЩИТА:
  **completeness-oracle** `test_completeness_oracle_*` — для КАЖДОГО entity-kind выставляет
  КАЖДОЕ persistent-поле (вкл. RESOLVED sprite/frame: `source_ref`+`src_key`) в non-default,
  затем `atlas.remove` (и per-kind remove) → inverse → байт-идентично И == legacy-snapshot.
  Пропущенное форком поле падает здесь громко (проверено: временный пропуск `kind` в
  `fill_source` ⇒ FAIL).
- **Positional collection primitives** (insert/remove/replace @index) на сырых dynamic-массивах:
  grow через seam, memmove, zero-hole (partial fill destroy-safe). Bounds → `OUT_OF_BOUNDS`
  отдельно от OOM. **REMOVE-направление (source/target/frame/atlas) делегировано публичным
  позиционным функциям** (`tp_project_atlas_remove_source/_remove_target`,
  `tp_project_anim_remove_frame`, `tp_project_remove_atlas`) — bounds+free+shift живут в ОДНОМ
  месте (review [6]: убран риск дрейфа free-дисциплины); diff после делегации только зануляет
  освобождённый tail-слот (инвариант «[count,cap) = NULL»). INSERT-направление (позиционная
  вставка) — единственно новая возможность diff’а, живёт локально.
- **Детерминизм/CI:** fixed-width ints; `#define` (не const) для любых bound; byte-at-a-time,
  без libm/`__int128`; каждый diff-apply путь bounds-checked → чистый под ASan+UBSan
  `-fno-sanitize-recover=all`. 0 warnings `-Werror` (incl. `-Wconversion`) на обоих preset’ах.

## §7. Status-токены: НОВЫХ НЕТ (переиспользованы)

Токен-инфляции избегали (ethos 0011). Corrupted diff → существующие `not_found`
(stale/unknown id) / `out_of_bounds` (bad position); «нечего undo/redo» → `not_found`
(caller гейтит на `tp_model_can_undo/redo`); OOM → `oom`; нет history → `invalid_argument`.
`test_status_id` не тронут.

## Что core-тестировано СЕЙЧАС vs отложено

| Возможность | Статус F2-03 |
| --- | --- |
| Per-op diff + точный inverse + redo для КАЖДОГО op-kind, byte-identical | ✅ core-tested (`test_diff.c`) |
| == legacy full-snapshot restore (oracle) | ✅ core-tested |
| Reference cascade (atlas.remove подграф; source.remove с зависимыми) | ✅ core-tested |
| 100 анимаций одной транзакцией = один inverse | ✅ core-tested |
| Inverse-apply alloc-failure rollback (sweep) + capture-alloc atomicity | ✅ core-tested |
| Redo + redo-branch discard + multi-step | ✅ core-tested |
| Corrupted/hostile diff → структурированная ошибка, byte-unchanged | ✅ core-tested |
| Defensive capture: dangling id / OOB index при history == history-less (no crash) | ✅ core-tested (review [1]/[2]) |
| Reserve-OOM ДО record → transaction id ретраибелен | ✅ core-tested (review [3]) |
| All-fields completeness oracle (per entity-kind, каждое поле) — форк-safety-net | ✅ core-tested (review [5a]) |
| GUI Undo/Redo (Ctrl+Z, checkpoint, ownership) | ⛔ F3-02 |
| History в journal / crash survival | ⛔ F2-04 |
| CLI/GUI через engine | ⛔ F2-05 (frontend cutover) |

## Что должен подтвердить владелец
1. **Inverse = state-capture (B), не inverse-as-operations (A)** (§Ключевое): byte-exact под
   order-чувствительным сериализатором, coarse-remove verbatim, sparse-sprite переходы; цена —
   второй mutation-путь, стрежённый oracle’ом. Ок?
2. **Undo/Redo bump’ают revision** (+1 каждый, монотонно; §4): dirty остаётся identity-derived.
   Приемлемо (vs восстановление старого revision)?
3. **Границу F2-03/F2-04(journal)/F2-05(cutover)/F3-02(session Undo)** (§Область): engine
   core-tested, НЕ wired во фронтенд; legacy `gui_history.c` не удалён/не перепроведен.
4. **Новых status-токенов нет** (§7) — переиспользованы `not_found`/`out_of_bounds`/`oom`.
