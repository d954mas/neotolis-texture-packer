# C0-03 — Journal, ownership, and cache policy contract note

Status: spike-accepted. Normative source: `docs/ntpacker-master-spec.md`
§7.1–7.2, §10.3–10.5, §16–19, §22.1–22.3, §52.3, §52.5, §59 items 19–25 and 50,
and the already-decided `docs/decisions/0004-pack-supersession.md`. This note
records the seam contracts pinned by the C0-03 spike: the recovery-journal
record framing, the commit acknowledgement ordering, the session-authority
vocabulary, the memory Pack-result cache interface, and the Pack-supersession /
preview-selection policy. It introduces **no production journal**, **no OS
claim**, **no journal IO**, and **no final default values** — it is a CONTRACT
spike. It builds on the accepted C0-01 identity contract (128-bit IDs, the
portable FNV-1a/128 `tp_c0_hasher`, `tp_c0_detail` structured errors, never
abort) and C0-02 (transaction id + idempotency set), reusing the `tp_c0` library.

Reference implementation and golden tests:

- `include/tp_c0/tp_c0_journal.h` + `src/tp_c0_journal_frame.c` (encode/decode one
  record + FNV-1a/128 payload checksum), `src/tp_c0_journal_recover.c` (scan a
  byte buffer, stop at the first torn/corrupt record, rebuild the idempotency
  set). Golden BINARY vectors in `tests/test_c0_journal.c`.
- `include/tp_c0/tp_c0_ack.h` + `src/tp_c0_ack.c` — the commit acknowledgement
  state machine. Fixtures in `tests/test_c0_ack.c`.
- `include/tp_c0/tp_c0_authority.h` + `src/tp_c0_authority.c` — authority
  vocabulary + permission table. `tests/test_c0_authority.c`.
- `include/tp_c0/tp_c0_cache.h` + `src/tp_c0_cache_mem.c` — the memory
  Pack-result cache interface + reference in-memory impl. `tests/test_c0_cache.c`.
- `include/tp_c0/tp_c0_pack_super.h` + `src/tp_c0_pack_super.c` — the
  Pack-supersession / preview-selection state machine (decision 0004).
  `tests/test_c0_pack_super.c`.
- `packer/tests/test_raw_ownership.c` (`tp_raw_ownership` ctest) — the raw-RGBA
  ownership regression (links `tp_build`, drives `nt_builder` directly).

Structured errors extend the C0-01/02 `tp_c0_detail` vocabulary (append-only),
all pinned in `test_c0_error`. New tokens: `journal_short`, `journal_bad_magic`,
`journal_bad_version`, `journal_bad_kind`, `journal_bad_checksum`. The
idempotency-duplicate fault reuses the C0-02 `txn_duplicate_id`; the cache reuses
`id_nil`/`empty`/`null_arg`/`oom`/`buffer_too_small`.

---

## 1. Minimum append-record / checkpoint framing (task 1) — `tp_c0_journal`

Every journal record is a fixed-width **little-endian** frame followed by an
opaque payload; the checksum reuses the portable C0-01 FNV-1a/128, so a golden
byte vector is byte-identical on Linux/macOS/Windows.

- **Record header (28 bytes):** `magic[4]='T','P','J','1'` · `version u16`
  (=1) · `kind u16` · `payload_len u32` · `checksum[16] = FNV-1a/128(payload)`.
  All integers little-endian, written byte-by-byte (no host-endianness /
  `long`-width leakage). The checksum covers the **payload only** (per the task);
  the per-record magic + strict bounds guard the header, and a length that stays
  in range but is wrong changes the hashed bytes → `journal_bad_checksum`.
- **Framing-known payload prefixes** (so recovery rebuilds the idempotency set
  without parsing the opaque body): `TXN` = `txn_id[16] | revision_after i64le |
  body[...]`; `CHECKPOINT` = `revision i64le | state_hash[16] | body[...]`. `body`
  is opaque to the framing layer (a future binary txn encoding or C0-02 JSON).
- **Append framing** is one record after another; **checkpoint framing** is the
  same frame with `kind=CHECKPOINT`. Golden vectors (`k_txn_golden`,
  `k_ckp_golden`) pin the exact bytes; a test recomputes the embedded checksum
  from the golden's own payload to prove it is a real FNV, not a transcription.
- **Recovery** (`tp_c0_journal_recover`) scans from the start, recovering every
  clean record until EOF, the first bad record, or the retention cap, then stops
  (spike policy: stop at first bad record rather than resync past a gap).
  `stop_reason` is the **single source of truth** (F9) and the outcome predicates
  derive from it — no hand-synced bool fields that can drift:
  - clean EOF → `stop_reason == TP_C0_OK`;
  - **torn/short final record** (`journal_short`) is the EXPECTED crash case →
    `tp_c0_journal_recovery_truncated()`; the clean prefix is recovered, no error;
  - **bad magic/version/kind/checksum** is unexpected corruption →
    `tp_c0_journal_recovery_corrupt()`; prefix still recovered;
  - **retention set full** (`journal_retention_full`) is a SPIKE CAP, NOT
    corruption and NOT a torn tail → `tp_c0_journal_recovery_capped()` (F2). The
    recovered id set is then **partial**: the caller must treat `capped()` as
    "cannot safely dedup" (a retried txn beyond the cap would not be seen as a
    duplicate). The fixed `TP_C0_JOURNAL_MAX_TXNS` cap is a spike artifact for
    determinism; **production uses dynamic storage so the set is always complete**.
  Recovery itself always returns `TP_C0_OK` (a torn tail / cap is not a caller
  error); the caller inspects `stop_reason` / the predicates.
- **Idempotency retention seam** (§7.2): recovery rebuilds the committed
  transaction-id set (`txns[]` + `tp_c0_journal_recovery_has_txn`). A retried
  (already-committed) transaction id is a known id → duplicate, not re-applied;
  callers seed the C0-02 `tp_c0_txn_idset` from it so the retry reports
  `txn_duplicate_id` after restart.
- **Encode faults:** a buffer too small returns `buffer_too_small` with
  `written=0` (this is the "append fail" that drives the ack rollback path); a nil
  txn id returns `id_nil`.
- **Max frame size guard (F1):** the on-disk `payload_len` is a `u32`, so before any
  cap check or copy both encoders reject a body whose framed size cannot be
  represented: `payload_len = prefix + body_len` must fit in `UINT32_MAX` and
  `HEADER_SIZE + payload_len` must not wrap `size_t`. A violation returns the new
  append-only token `journal_too_large` with `written=0` — never a truncated
  stamped length (which would make recovery hash fewer bytes than the checksum
  covers → a lost committed txn) and never a `memcpy` past `cap`. The stamped
  length and the checksum-hashed length are therefore always identical.

## 2. Acknowledgement boundary (task 2) — `tp_c0_ack`

A pure total state machine pinning §7.1's ordering `validate → apply → append →
publish` and rollback-on-append-failure:

```
RECEIVED -validate_ok-> VALIDATED -apply_ok-> APPLIED -append_ok-> JOURNALED -publish-> PUBLISHED
   | validate_fail          | apply_fail          | append_fail
   v                        v                     v
REJECTED                 REJECTED              ROLLED_BACK
```

- **Load-bearing invariant (pinned across the whole table):** the ONLY legal edge
  into `PUBLISHED` is `JOURNALED + PUBLISH`. Publishing before the journal append
  is impossible by construction; an `APPLIED` transaction whose append fails goes
  to `ROLLED_BACK`, never `PUBLISHED`. A new revision is retained/visible **only**
  when published (a rollback reverts the apply-time revision bump).
- The machine only ever sees a controlled pipeline event sequence (not
  caller/disk input), so an illegal `(phase,event)` is a programming misuse
  reported via a `legal` out-flag (phase unchanged) rather than a structured disk
  token — and never an abort. Terminal phases accept no further events.
- No apply engine, no journal IO: contract shapes + a transition function only.
  "GUI commit timing" (when exactly the GUI surfaces the published event) stays
  OPEN per §52.5.

## 3. Authority-state vocabulary (task 3) — `tp_c0_authority`

Vocabulary + a pure permission predicate table; **no** OS lock/claim and **no**
cutover protocol (those are OPEN per §60 item 2 / §52.5).

- States: `owner` (authoritative host — GUI open, or MCP when GUI closed),
  `observer` (Dev-API client / recovery mirror — reads and forwards, never
  authoritative), `transfer_pending` (ownership cutover in flight; old host
  draining to a safe boundary), `released` (no authority).
- Capabilities: `apply` / `publish` / `pack`. **Predicate table:** `owner`
  permits all three; `observer`, `transfer_pending`, and `released` permit
  none. That "exactly one state accepts writes/publish/pack" is the executable
  form of singular authority (§16, §22.2) — the three non-owner states are
  distinct vocabulary points (they differ only in the open cutover machine).

## 4. Configurable byte budgets + cache interface (task 4) — `tp_c0_cache`

A memory Pack-result cache **interface** keyed by `pack_input_hash` with a
reference in-memory implementation. The byte budget is a **constructor
parameter** — no production default is chosen (§60 item 3 / §52.3 leave concrete
budgets, compressed representation, and GPU thresholds open).

- `get`/`put`/`evict` by 128-bit hash; a `get` is by hash, independent of
  insertion/completion order (the property task 5 relies on). Put copies the blob
  in and frees it internally (no cross-CRT handoff); a returned pointer is
  cache-owned.
- **Budget LRU over UNPINNED entries:** after a put, unpinned LRU entries are
  evicted while unpinned bytes exceed the budget and more than one unpinned entry
  remains (a single over-budget item is retained — soft cap). The **active result
  is `pin`-able**: pinned entries are never evicted and do not count against the
  budget (§10.4 "the active result is pinned … inactive results use a separate
  byte-budget LRU"). Faults: `id_nil`/`empty`/`null_arg`/`oom`, and
  `buffer_too_small` when the fixed entry table is full and all entries are
  pinned (never an abort).

## 5. Pack supersession / preview-selection (task 5) — `tp_c0_pack_super`

Encodes `docs/decisions/0004-pack-supersession.md` exactly, as a pure state
machine. The struct is transparent so fixtures can construct exact scenarios.

- **One running job + one latest intent:** `request` while idle → `started`
  (running job); `request` while a job runs → `queued` (replaces the single
  pending intent; never a parallel job). A newer request replaces the pending
  one, so a replaced intent never runs and never enters the cache.
- **Supersession by request seq, not by timing:** on `complete`, the finished job
  enters the cache and becomes the authoritative preview **only if it is still the
  freshest intent** (`running_seq == latest_seq`) **and the preview is not
  user-pinned** (see the explicit-selection refinement below); otherwise
  `superseded` (cached only, preview untouched). A late-finishing / earlier job
  therefore **never overwrites a newer authoritative preview** (§10.3) — pinned
  directly with a constructed "straggler" whose request seq is older than the latest.
- **Explicit selection is sticky (F4 — refinement of decision 0004, for owner
  review):** an explicit `select` marks the preview `preview_is_explicit`; a job
  `complete` becomes the preview only when the preview is NOT explicitly selected,
  so a completing in-flight job cannot silently discard a user's explicit choice
  (§10.3 makes explicit selection a first-class action). A NEW `request` clears the
  flag (the user asked for a fresh pack, so its result is wanted). The completing
  job still enters the cache regardless.
- **Every successful result enters the cache** (`in_cache` by hash), superseded
  or not.
- **Selection is by hash:** `select` (explicit selection and Undo cache-hit) picks
  the preview by `pack_input_hash`; a hit → `selected`, a miss → `miss` (preview
  unchanged / out of date; no Pack auto-started, §10.4). Freshness (§10.1) is
  `preview_hash == current_pack_input_hash`.
- **Ownership transfer drops the session pack intent (F3 — refinement of decision
  0004, for owner review):** `transfer` cancels the running Pack (§59 item 24) AND
  drops the never-run pending intent; only the preview + cache membership survive.
  Leaving the pending set would let a later `request`+`complete` resurrect the
  stale pre-transfer pending as a running job (an unrequested Pack). Decision 0004
  said transfer cancels "only the running Pack"; a never-run pending intent is part
  of the same session intent and must not outlive the transfer.

## 6. Raw-RGBA ownership (task 6) — `packer/tests/test_raw_ownership.c`

The engine `nt_builder_atlas_add_raw` **deep-copies** the caller's RGBA at add
time (`external/neotolis-engine/tools/builder/nt_builder_atlas.c:771-775`: malloc
+ memcpy, then `sprite->rgba = copy`; the sibling `atlas_add` copies the same
way). Because the copy is confirmed in the source, exercising it is safe (no UB).

The regression fills an RGBA buffer, calls `nt_builder_atlas_add_raw`, **mutates
and frees that caller buffer immediately** (before `end_atlas`/`finish_pack`,
where the blitting happens), packs, reads the pack back with the packer's own
reader, and asserts the packed page still holds the ORIGINAL pixels. It drives
`nt_builder` directly (mirroring `tp_pack.c run_builder`) because `tp_pack()`
cannot interleave the mutate/free. Result: **copy is safe** — the test passes.

Because the **public header does not promise this lifetime**
(`nt_builder.h:424`), `upstream-issue-raw-lifetime.md` is drafted (NOT filed; the
submodule is NOT patched) asking the engine to document the rgba-buffer lifetime
in the public header. The packer only needs a documented contract, not a
behaviour change.

## 7. This note (task 7)

Lives beside the fixtures/tests. Any change to a pinned rule above must land with
the matching golden-test update in `packer/spike/c0/tests/` (and
`packer/tests/test_raw_ownership.c`) — the tests are the executable form of this
contract.

---

## Settled decisions (recorded per §59 item 52, for lead review)

1. **Journal record framing.** 28-byte fixed header (`magic 'TPJ1'` · `version
   u16=1` · `kind u16` · `payload_len u32` · `checksum[16]`), little-endian,
   FNV-1a/128 checksum over the payload only. TXN/CHECKPOINT payload prefixes as
   in §1. This is the seam byte format the real journal writer/reader will agree
   on; it is not the whole journal.
2. **Recovery policy:** stop at the first torn/corrupt record (no resync past a
   gap); `stop_reason` is the single source of truth and `truncated()` /
   `corrupt()` / `capped()` are derived predicates (F9, no hand-synced bools). A
   torn final record is tolerated (`truncated()`), other corruption is flagged
   (`corrupt()`), and the fixed spike retention cap is a distinct honest outcome
   (`capped()` / `journal_retention_full`) that is NEITHER corruption nor a torn
   tail and signals a partial (cannot-dedup) id set (F2). Recovery never returns
   an error for a torn tail or the cap.
3. **Idempotency seam:** the committed txn-id set is rebuilt from the journal and
   a duplicate reuses the C0-02 `txn_duplicate_id`.
4. **Ack ordering:** phases received/validated/applied/journaled/published +
   rejected/rolled_back; PUBLISH legal only from JOURNALED; revision retained only
   when published; illegal transitions reported via a `legal` flag (no new token).
5. **Authority vocabulary:** owner/observer/transfer_pending/released; only owner
   permits apply/publish/pack.
6. **Cache interface:** keyed by `pack_input_hash`; byte budget is a parameter;
   unpinned byte-budget LRU with a pinnable active result; fixed spike entry cap
   (`TP_C0_CACHE_MAX_ENTRIES`).
7. **Pack supersession:** authoritative preview chosen by request seq on complete
   (and only when the preview is not user-pinned — F4), by hash on explicit/Undo
   selection; an explicit selection is sticky until the next request (F4); transfer
   drops BOTH the running Pack and the never-run pending intent (F3). F3 and F4 are
   refinements of decision 0004 flagged for owner review (no new decisions entry).
8. **New `tp_c0_detail` tokens** (append-only): `journal_short`,
   `journal_bad_magic`, `journal_bad_version`, `journal_bad_kind`,
   `journal_bad_checksum`, `journal_too_large` (F1: framed size exceeds the u32
   on-disk length / size_t byte math), `journal_retention_full` (F2: recovery
   retention set full — a spike cap, NOT corruption).

## Open per §60 (deliberately NOT fixed here)

- **§60 item 1 / §52.5 — Journal internals beyond the seam framing:** checkpoint
  cadence, compaction, the dedup **retention window** size (the spike uses a
  fixed `TP_C0_JOURNAL_MAX_TXNS` cap for determinism and reports overflow as the
  honest `journal_retention_full` outcome; production uses dynamic storage so the
  recovered id set is always complete), corruption/resync policy beyond
  torn-tail + bad-checksum detection, and optional `fsync` modes.
- **§60 item 2 / §52.5 — Ownership state machine:** the process-claim mechanism,
  proof a host is dead, and the singular **authority-cutover** protocol (only the
  vocabulary + permission table are fixed; no transition machine).
- **§60 item 3 / §52.3 — Cache budgets:** concrete CPU/GPU byte budgets, the
  compressed Pack representation, and eviction/GPU-residency thresholds (the
  budget stays a parameter; no default is baked).
- **GUI commit timing** (§52.5): when the GUI surfaces the published event.
- **Raw-RGBA lifetime as a PUBLIC contract:** pending the drafted upstream issue;
  the packer relies on the observed copy, pinned by the regression test.
