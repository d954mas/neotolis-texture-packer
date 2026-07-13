# 0013 — F2-04: минимальный recovery journal для side-effect workflows

**Дата:** 2026-07-13
**Статус:** accepted (нужно подтверждение владельца по: формат записи/версионирование, новый
commit-ordering + новый status-токен `journal_failed`, per-txn full-snapshot payload vs redo-log,
scope coordinator’а, compaction cadence)
**Принял:** deep-reasoner (F2-04, делегированные полномочия), lead review pending
**Реализуется в:** F2-04 (`tp_journal` в core: `tp_journal.c`, `tp_journal_io.c`; public header
`tp_core/tp_journal.h`; internal `packer/src/tp_journal_internal.h`; поля `journal`/`coordinator` в
`tp_model` + acknowledgement-gate в `tp_txn_apply.c`; coordinator-интерфейс + model↔journal glue в
`tp_core/tp_transaction.h`; новый статус `TP_STATUS_JOURNAL_FAILED`). Master spec §7.1–7.2, §22.3,
§44.2–44.4, §59 items 19/46–49/52. Plan F2-04 (строки 449–476). Строится на F2-02/F2-03
(decisions 0011/0012: `tp_model` / clone-swap commit / `tp_project_save_buffer`/`load_buffer`) и
на принятом C0-03 journal-spike (`packer/spike/c0/…journal…`, reference, НЕ линкуется).

## Область: что F2-04 РЕАЛЬНО поставляет (groundwork) vs что отложено

Урок F1-03/F2-01/F2-02/F2-03: границу фиксируем ЧЕСТНО, без overclaim.

**F2-04 РЕАЛЬНО поставляет (core-tested groundwork):**
- **Durable append-only journal** (sidecar) над инъектируемым I/O-seam’ом: versioned+keyed header,
  length-prefixed + CRC-32-checksummed записи, byte-детерминированный endian-стабильный формат.
- **Acknowledgement-gate в commit’е** (§7.1): транзакция НЕ объявлена committed, пока её запись не
  записана durable; append-failure → **точный rollback** (клон дискардится, живая модель
  байт-неизменна, committed-событие/результат НЕ публикуется), тот же txn id ретраибелен.
- **Recovery**: из checkpoint + journal-replay восстанавливаются current committed project state +
  retained transaction-ID set (§7.2). Acknowledged txn восстановим и НЕ дублируется; unacknowledged
  (torn/failed) txn невидим.
- **Corruption-политика** (§60/§22.3): structured recovery-status + safe fallback — replay
  ОСТАНАВЛИВАЕТСЯ на первой недекодируемой записи, НИКОГДА не угадывает повреждённое содержимое,
  восстанавливает до последней хорошей записи.
- **Side-effect coordinator interface** (prepare/publish/abort) + default no-op — seam, за который
  зацепится B1 (Extract), чтобы связать published файлы с транзакцией.
- **UB-clean reader** на произвольных/повреждённых/коротких/torn байтах + memory- и file-backend’ы.

**НЕ входит в F2-04 (по плану) — честная граница:**
1. **Фронтенд-cutover — F2-05.** Shipping CLI/GUI НЕ маршрутизируются через журнал. F2-05: GUI =
   journal-backed live, CLI = file one-shot БЕЗ live-journal. Здесь только engine-primitive +
   fault-suite, НЕ живой фронтенд.
2. **Полная B1-семантика Extract** (staging PNG, publish/replace, §44.2–44.4) — B1. F2-04 даёт
   только coordinator-интерфейс + no-op, plug-in-точки задокументированы.
3. **Power-loss durability / per-op `fsync`** — НЕ обещаны (v1 = process-failure recovery, §7.1,
   §22.3). `sync`-hook seam’а — best-effort (может быть NULL).
4. **Compaction cadence / retention window** — открыты (§60 item 1); формат ПОДДЕРЖИВАЕТ compaction
   (checkpoint-запись несёт retained-id set), но политика частоты — configurable/open (см. §Открытое).
5. Pack/export и project-сериализация НЕ изменены: журнал — SIDECAR, `tp_project_save_buffer`
   байт-идентичен, все F2-01/02/03 + pack/export goldens зелёные (78/78 ctest на обоих preset’ах).

## §1. Формат записи + версионирование (task 1)

Журнал = fixed header + последовательность length-prefixed + checksummed записей. ВСЕ multi-byte
целые пишутся **byte-at-a-time big-endian** (без `memcpy` многобайтных int, без host-endian
punning). Константы — `#define` (не const-массив-bound: macOS `-Wgnu-folding-constant` = VLA-reject).

```
header (28 байт):  MAGIC[8]="NTPKJRNL" | format_version u32 BE (=1) | key[16]
record:            payload_len u32 BE | payload[payload_len] | crc32 u32 BE
                   crc32 = CRC-32/IEEE (0xEDB88320, byte-at-a-time, table-less) над
                          (payload_len-bytes ++ payload) — endian-стабилен, ловит flip/torn
payload:           rec_type u8: 1=TXN, 2=CHECKPOINT
  TXN:             type(1) | tx_id[32] (hex ascii) | revision i64 BE (8) | snapshot[rest]
  CHECKPOINT:      type(1) | revision i64 BE (8) | id_count u32 BE (4) | id[32]*id_count | snapshot[rest]
```

**Ключевые решения формата:**
- **`format_version` в header** → self-describing, forward-safe: неизвестная версия → `BAD_HEADER`
  (никогда не угадываем чужой формат).
- **`key[16]` в header** — journal keyed (см. §7). Stale journal moved-проекта детектируется здесь.
- **Single length-prefix на запись** (не redundant вложенные длины): snapshot = `payload_len − fixed`,
  bounds-checked. Меньше рассинхрон-векторов.
- **CRC покрывает и length-field, и payload** → искажённая длина тоже ловится (если ≥8 байт есть).
- **Per-record CRC** → torn/short tail детектируется, не misread’ится (crux fault-suite).

**Почему payload = ПОЛНЫЙ post-commit project snapshot (а НЕ redo-log из ops):**
Recovery восстанавливает состояние ПРЯМО из последнего хорошего snapshot’а через уже-golden’ый
`tp_project_load_buffer` — **НЕ пере-прогоняя apply**. Это исключает второй mutation-путь на recovery
(нет determinism-риска re-apply, нет «apply упал на replay»), и переиспользует байт-стабильную
пару save/load, на которой уже стоит F2-03-oracle. Цена (честно): запись на транзакцию тяжелее
redo-log’а (полный проект против дельты). Митигируется compaction’ом (checkpoint заменяет весь
журнал одной свежей записью); cadence — открытый пункт. Для «minimum recovery journal» v1
(process-failure) простота+доказуемость важнее компактности.

## §2. THE crux — новый commit-ordering и как всё держится вместе (tasks 2–3)

`tp_txn__commit_validated` (см. `tp_txn_apply.c`): clone → (history capture по op’ам) → **gate** →
allocation-free swap → push → publish. Порядок (journal == NULL → **РОВНО** путь F2-02/F2-03, все
существующие goldens байт-неизменны):

1. `clone = tp_project_clone(m->project)` (fallible OOM).
2. `if history`: `record_new` + per-op `capture_before → tp_operation_apply(clone) → capture_after →
   push_op` (ops применены к КЛОНУ; всё pre-swap, fallible).
3. `if history`: `tp_history_reserve` (fallible OOM).
4. **coordinator.prepare** (если задан) — staged side-effects ДО gate (prepare-fault → reject, ничего
   не staged, abort не нужен).
5. **THE GATE (последний fallible шаг):**
   - `if journal`: `snap = tp_project_save_buffer(clone)` → `tp_journal_append_txn(journal, id,
     rev+1, snap)` — **durable append = точка acknowledgement (§7.1)**.
   - `else if idstore->record`: in-memory record (РОВНО F2-02).
6. **gate-fail → точный rollback:** coordinator.abort → `tp_txn__commit_reject` (клон дискардится,
   diff освобождается, живая модель/revision **байт-неизменны**, committed-результат НЕ публикуется) →
   return статус. Для journal — `TP_STATUS_JOURNAL_FAILED`; txn РЕТРАИБЕЛЕН.
7. **swap** (`destroy(project); project=clone; revision+=1`) — allocation-free, infallible.
8. `if history`: `push_reserved` (alloc-free).
9. **coordinator.publish** — side-effects live, т.к. txn durably acknowledged.

### Инвариант «gate passed ⟹ commit не может упасть» + отсутствие poison

`tp_journal_append_txn` внутри — **reserve→write→register**:
1. `id_index_reserve` — pre-grow in-memory retained-id индекса на один слот (fallible OOM), **ДО**
   durable-write; НИЧЕГО не пишет и НЕ регистрирует id при OOM.
2. durable `write_record` — если write короткий/упал → store **truncate’ится назад к prior length**
   (torn tail НЕ остаётся); если и truncate упал → журнал **poisoned** (дальнейшие append’ы
   отклоняются, чтобы хороший record никогда не оказался спрятан за mid-stream torn-записью).
3. `id_index_put_reserved` — infallible вставка в зарезервированный слот, ТОЛЬКО на успехе write.

Итог: id регистрируется в fast-index’е **ⁱтолько после успешного durable append’а**. Значит:
- **append-fail** (OOM в reserve ИЛИ I/O в write) → ничего durable, id НЕ зарегистрирован →
  тот же txn id ретраится и коммитится (как F2-03 fix [3] reserve-before-record, но теперь якорь —
  durable append, а не in-memory set; **нет unrecord-хука и он не нужен**).
- **append-ok** → id зарегистрирован (infallibly) И durable — согласованы навсегда.

Это переносит F2-02/03-инвариант «id recorded ⟹ commit can’t fail» на «**append acknowledged ⟹
commit can’t fail**»: всё после успешного gate (swap/push/publish/out-echo) — allocation-free либо
«не un-commit’ит» (out-echo как в F2-02).

### Почему idstore->record ЗАМЕНЁН журналом (а не вызывается ДОПОЛНИТЕЛЬНО)

Если бы в journal-пути мы ЕЩЁ и вызывали `idstore->record` (fallible OOM) ПОСЛЕ durable append’а —
это второй fallible шаг после точки acknowledgement → его OOM либо ломает инвариант (durable запись
есть, а idstore пуст → within-session retry double-apply), либо (если откатить) resurrect’ит
unacknowledged txn после краха. Поэтому: **при attached journal идемпотентность отвечает journal’овый
retained-id индекс** (`tp_journal_contains`), `m->idstore` не пишется (остаётся owned, но unused).
Preflight ветвится: `dup = journal ? tp_journal_contains(...) : idstore->contains(...)`. journal-less
ветка — байт-идентична F2-02.

## §3. Recovery / replay + corruption-политика (tasks 4, 6)

`tp_journal_recover` (pure, blob-level) → `tp_model_recover` (glue: load_buffer → wrap → attach):
1. `io.read_all` → один buffer (bounded реальным размером store’а).
2. Validate header: `len<28` / magic≠ / version≠ → `BAD_HEADER`; key≠expected → `STALE_KEY`
   (moved-проект, НЕ применяется).
3. Декодирование записей front-to-back, **size_t-математика, bounds-check ДО КАЖДОГО чтения**
   (`len−off >= 4` перед length; `len−after_len >= plen` перед payload; `len−crc_off >= 4` перед crc;
   `idc <= avail/32` — overflow-safe перед id-list). CRC-verify. Malformed payload / unknown
   rec_type → corruption-boundary.
4. **STOP на первой недекодируемой записи** (torn → `TRUNCATED`, checksum/malformed → `CORRUPT`),
   восстановление до последней хорошей: state = последний хороший snapshot, retained-set =
   аккумулированные id (CHECKPOINT сбрасывает set к своему id-list’у → корректно для compaction).
5. `tp_model_recover` при torn/corrupt хвосте **truncate’ит io к `stop_offset`** — очищает хвост,
   чтобы продолженные append’ы оставались recoverable (никогда не угадываем за последней хорошей).

Structured `tp_journal_recovery{status, bytes_total, stop_offset, records_recovered, revision,
snapshot}` — differentiates benign torn-tail (ожидаем при process-failure) от реальной corruption.
**Никакого guessing**: повреждённый record → граница, не «починка».

## §4. Coordinator interface для B1 (task 5)

`tp_side_effect_coordinator {ctx, prepare, publish, abort}` + `tp_side_effect_coordinator_noop()`.
Commit драйвит его вокруг gate: prepare (после apply-к-клону, ДО gate) → gate → на acknowledged
commit publish() (side-effects live) / на любом rollback после успешного prepare abort() (discard).
Симметрично для journal и journal-less (fires вокруг gate в обоих; NULL coordinator/hooks → no-op →
существующие goldens байт-неизменны). Это ИНТЕРФЕЙС + no-op; полный Extract-binding (staged PNG →
publish/replace, §44.2–44.4; файлы переживают Undo, §44.4/§49) — B1.

## §5. Journal location / keying (spec)

Keyed 16-байтным `key` в header: для saved-проекта — hash canonical project path; для unsaved —
temporary runtime session ID (spec §59 items 1/3, §23.2). Журнал — **sidecar**: НЕ часть
`.ntpacker_project`, НЕ меняет project-сериализацию. Stale journal moved-проекта детектируется key-
mismatch’ем на recovery (`STALE_KEY`, не misapplied). `tp_model_recover(io, key, …)` — caller
передаёт ожидаемый key.

## §6. Детерминизм / CI

- **Byte-identity**: журнал sidecar → `tp_project_save_buffer` и все pack/export + F2-01/02/03
  goldens байт-идентичны (78/78 ctest оба preset’а; `test_journal_is_sidecar_byte_identical`
  пинит журнал-vs-без-журнала эквивалентность project-байт).
- **UB-clean на untrusted bytes**: каждое чтение из журнала bounds-checked ДО deref, size_t-мат,
  big-endian byte-at-a-time, никаких out-of-range double→int (double’ов нет), absurd length-prefix
  bounds-fail’ит без huge-alloc. `test_recover_arbitrary_bytes` (fuzz 0..320 + absurd len),
  `test_short_write_every_boundary` (recover на КАЖДОЙ усечённой длине).
- **Endian-stable**: byte-at-a-time codecs + CRC.
- **No libm**, **NUL-free brace-init MAGIC**, **`#define` bounds** (не const-массив).
- **No-leak на всех error-путях** (short write, torn read, append-fail, poison, OOM): файлы/буферы
  освобождаются на каждом пути; ownership io/journal/recovery.snapshot строго задокументирован
  (`j_consumed`-флаг против double-free в `tp_model_recover`). LSan в CI — гейт.
- **0 warnings** оба preset’а (`-Werror -Wconversion -Wshadow …`); 13 cgltf-warning’ов —
  pre-existing third-party (отфильтрованы), НАШ код 0-warning.

## §7. Новый status-токен: `TP_STATUS_JOURNAL_FAILED` (`"journal_failed"`)

Против токен-инфляции (ethos 0011/0012) — но durable append-failure это ГЕНУИННО новый, cl
ient-actionable класс отказа, прямо названный spec’ом (§7.1 «If journal append fails, the
transaction is rolled back»): это НЕ OOM (durability, а не память) и НЕ validation-reject; caller
ретраит тот же txn id. OOM-класс journal-фолтов (index/buffer alloc) переиспользует `oom`. Append-only
в конец enum’а (существующие токены не сдвигаются); `test_status_id` пин добавлен, `-Wswitch`
держит `tp_status_str`/`tp_status_id` в lockstep.

## §8. Что core-тестировано СЕЙЧАС (fault-suite `test_journal.c`, 15 кейсов)

| Возможность | Тест |
| --- | --- |
| Sidecar byte-identity (журнал не меняет project-байты) | `test_journal_is_sidecar_byte_identical` |
| Checkpoint + journal replay → state + retained-id set восстановлены | `test_checkpoint_and_replay` |
| Duplicate retry after restart → dedup (§7.2) | `test_duplicate_retry_after_restart` |
| Append-fail после apply → точный rollback, no ack, retryable, recover-once | `test_append_failure_rolls_back` |
| Append OOM (reserve) → ничего durable, retryable, register-once | `test_append_oom_is_retryable` |
| Short write на КАЖДОМ byte-boundary → prefix-recover, no UB | `test_short_write_every_boundary` |
| Torn tail → invisible, no dup on retry | `test_torn_tail_invisible` |
| Checksum mismatch (flip) → corruption boundary, safe fallback | `test_checksum_mismatch` |
| Stale journal (moved project) → key-detected, not applied | `test_stale_key_not_applied` |
| Arbitrary/garbage + absurd length → structured status, no huge alloc | `test_recover_arbitrary_bytes` |
| Bad header (magic/version) | `test_bad_header` |
| Poison на failed-truncate → дальнейшие append’ы отклонены | `test_poison_on_truncate_failure` |
| Coordinator prepare/publish/abort ordering (success/append-fail/prepare-fail) | `test_coordinator_ordering` |
| No-op coordinator | `test_coordinator_noop` |
| Реальный on-disk file journal round-trip | `test_file_journal_roundtrip` |

## §Открытое / что должен подтвердить владелец

1. **Формат записи + versioning** (§1): MAGIC/version/key header, length+CRC-32 framing, TXN/CHECKPOINT
   payload. Ок как v1-контракт?
2. **Per-txn FULL snapshot payload** (§1) vs redo-log из ops: выбрано ради recovery-без-re-apply
   (нет второго mutation-пути, переиспользование golden save/load); цена — тяжелее запись, митигируется
   compaction’ом. Приемлемо для v1?
3. **Commit-ordering + инвариант** (§2): gate = durable append, reserve→write→register, idstore
   заменён journal-индексом при attached journal. Ок?
4. **Новый токен `journal_failed`** (§7) — единственный новый статус. Ок (vs переиспользование)?
5. **Coordinator scope** (§4): интерфейс + no-op, fires вокруг gate (journal и journal-less); полный
   Extract-binding — B1. Достаточно для F2-04?
6. **Compaction cadence / retention window — ОСТАВЛЕНО ОТКРЫТЫМ** (§60 item 1): формат поддерживает
   (checkpoint несёт retained-id set), политика частоты не зашита. Подтвердить, что это ок отложить.
