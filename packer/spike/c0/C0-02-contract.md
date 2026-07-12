# C0-02 — Operation, revision, and semantic-state contract note

Status: spike-accepted. Normative source: `docs/ntpacker-master-spec.md`
§6–9.5, §20.2–20.3, §21, §59 items 9–19 and 52 (with §5–5.6 for identity
context). This note records the wire-neutral semantic operation vocabulary and
commit acknowledgement pinned by the C0-02 spike. It introduces **no on-disk
project schema** and **no mutation engine**; it defines the versioned
operation/transaction JSON contract and the typed C payloads that F2 promotes
into the production op/session layer. It builds on the accepted C0-01 identity
contract (128-bit `<prefix><32 hex>` shape IDs; `tp_c0_detail` structured errors,
never abort) and reuses the `tp_c0` library.

Reference implementation and golden tests:

- `include/tp_c0/tp_c0_op.h` + `src/tp_c0_op_catalog.c`, `src/tp_c0_selector.c` —
  the operation catalog, closed field vocabulary, and selector resolver.
- `include/tp_c0/tp_c0_semantic.h` + `src/tp_c0_semantic.c` — the semantic-state
  field partition.
- `include/tp_c0/tp_c0_txn.h` + `src/tp_c0_txn_util.c` (shared decode
  primitives), `tp_c0_txn_parse.c` (request decode), `tp_c0_txn_validate.c`
  (revision precondition, idempotency set, full-batch validation),
  `tp_c0_txn_encode.c` (canonical byte-stable request + result encoder), and
  `tp_c0_txn_result.c` (result decode) — the transaction request/result contract.
  cJSON (engine-vendored, MIT) is a PRIVATE dep; it never appears on a `tp_c0`
  public header.
- `tests/test_c0_op.c`, `tests/test_c0_txn.c` — table-driven Unity suites wired
  into ctest as `test_c0_op`, `test_c0_txn`. The tests are the executable form of
  this note: every pinned rule below has a matching test.

Structured errors extend the C0-01 `tp_c0_detail` vocabulary (append-only), all
pinned in `test_c0_error`. New tokens: `op_unknown`, `bad_json`,
`txn_bad_version`, `txn_bad_id`, `txn_duplicate_id`, `txn_missing_field`,
`txn_bad_type`, `unknown_field`, `selector_ambiguous`, `selector_unresolved`,
`revision_conflict`, `invalid_revision`.

---

## 1. Append-only operation catalog + verb mapping (task 1) — `tp_c0_op`

All persistent model mutation is a **typed semantic operation targeting stable
IDs** (§6). The catalog is append-only: kinds are added before
`TP_C0_OP_KIND_COUNT`, never reordered or removed (a journaled/persisted op
record must not shift). Each row carries a wire name, a **before/after diff
class** (§6 below), the primary-target ID kind, and the current CLI verb.

Every current `ntpacker` mutation verb maps to a catalog operation — there is
**no raw field-patch escape hatch** (§6.2). The "set" verbs map to typed
`*.settings.set` / `*.set` ops with a **closed field vocabulary**
(`tp_c0_op_fields`), not arbitrary JSON paths.

| CLI verb | canonical operation(s) | class |
| --- | --- | --- |
| `atlas add` | `atlas.create` | create |
| `atlas remove` | `atlas.remove` | remove |
| `atlas rename` | `atlas.rename` | set |
| `set` | `atlas.settings.set` | set |
| `add` | `source.add` | create |
| `remove` | `source.remove` | remove |
| `sprite set <placement/packing>` | `sprite.override.set` | set |
| `sprite set rename=…` | `sprite.name.set` | set |
| `sprite set <field>=inherit` | `sprite.override.clear` | set |
| `sprite unset` | `sprite.override.clear` | set |
| `anim create` | `animation.create` (payload carries initial frames) | create |
| `anim remove` | `animation.remove` | remove |
| `anim set` | `animation.settings.set` | set |
| `anim add-frame` | `animation.frame.add` | create |
| `anim remove-frame` | `animation.frame.remove` | remove |
| `anim move-frame` | `animation.frame.move` | move |
| `target add` | `target.create` | create |
| `target remove` | `target.remove` | remove |
| `target set` | `target.set` | set |

Compound / lifecycle notes:

- **`new` is a session/lifecycle command, not a model operation** (§6.1 lists
  save/save_as/discard/undo/redo as session commands; project creation joins
  them). Its default seed decomposes into model ops `atlas.create` +
  `target.create`, so no bytes are written by a "raw" path.
- **`sprite set` is compound**: it lowers to up to two ops in one transaction —
  a `sprite.override.set` (placement/packing fields) and, when `rename=` is
  present, a `sprite.name.set` (a logical/export name is a distinct reference
  target, §5.2). An `=inherit`/empty value lowers to `sprite.override.clear` /
  `sprite.name.set(null)`.
- **Reserved ops** (spec-listed, `cli_verb == NULL`, no current verb):
  `source.replace` (linked-source repath, Epic B) and `animation.frames.set`
  (bulk whole-list set for MCP; the CLI uses the granular frame ops).

**Effect classes.** Every op is exactly one of CREATE / REMOVE / MOVE / SET, and
every class has a real member (MOVE = `animation.frame.move`). The class fixes
the diff requirement (§6).

## 2. Typed payloads + selector-resolution boundary (task 2)

**Canonical operations store IDs only** (§5.4). A frontend may accept a
human-friendly selector (name / index / path), but it **resolves to exactly one
ID before transaction validation**; an ambiguous selector is an error with a
candidate list, never a guess.

- `tp_c0_selector` (kind = ID / NAME / INDEX) + `tp_c0_selector_resolve` model the
  request-edge resolution against a live entity table. Source-path selectors are
  normalized via **C0-01 `tp_c0_srckey`** *before* this layer (so name compares
  here are plain byte-equality — no path logic is duplicated). Ambiguous →
  `selector_ambiguous` (+ candidate IDs in prose); no/dangling match →
  `selector_unresolved`.
- **Typed payload = op kind → closed key vocabulary with typed values**
  (`tp_c0_op_fields`). Addressing keys are the `*_id` fields; an op that still
  carries a `selector` key is **not canonical** and the encoder refuses it
  (`selector_unresolved`).
- **Fixed-arity tuples are scalar fields** in the canonical schema (`origin_x`,
  `origin_y`, `slice9_l/r/t/b`); only genuinely variable-length lists (`frames`,
  the clear `fields` list) are JSON arrays. This keeps one array value kind.
- **Unknown-field policy = REJECT** (`unknown_field`), enforced at the envelope,
  transaction, and operation object levels. This is **deliberately stricter than
  the on-disk project-file loader**, which *ignores* unknown keys for
  forward-minor compatibility: a mutation *request* that silently dropped a field
  could make a client believe an effect occurred that did not, and §59 item 38
  keeps one current API version with no compatibility layer. (Decision, §59 item
  52.)

## 3. Transaction request/result JSON (task 3) — `tp_c0_txn`

Versioned envelope; `schema` is the only accepted version (`TP_C0_TXN_SCHEMA` =
1). A request (§7):

```
{ "schema": 1, "transaction": {
    "author": <string>, "expected_revision": <int>, "id": <32-hex>,
    "label": <string>, "operations": [ { "op": <wire>, <*_id + typed fields> } ] } }
```

- **transaction id** is a 32-lowercase-hex 128-bit token (no shape prefix, since
  it is not a structural entity); malformed → `txn_bad_id`. Deterministic and
  reuses the C0-01 hex conventions. (Decision, §59 item 52.)
- **Canonical encoding is byte-stable**: object keys ascend by ASCII, the
  discriminator (`schema` at the envelope, `op` in an operation) is emitted
  first, 2-space indent, LF, a trailing newline; integral numbers emit with no
  decimal point and fractional numbers via `%.9g` — identical to the
  `tp_project.c` writer. `label`/`author` are omitted when empty (sparse). Array
  **element order is preserved** (it is data). `test_c0_txn` pins decode→encode
  byte equality and canonicalization of a shuffled/compact input.
- **Number classification is UB-free and OS-independent.** A JSON number decodes
  as the INT kind only when it is integral **and** within the exactly-representable
  range ±2^53 (`9007199254740992`); anything outside that, or fractional/inf/NaN,
  decodes as NUM (`%.9g`). INT storage is `int64_t` and integral output is emitted
  with `PRId64`, so a value like `5000000000` is `"5000000000"` on every OS rather
  than overflowing 32-bit `long` on Windows into `"5e+09"`. Every decode of an
  attacker-supplied number (op-field values, `expected_revision`, `revision`,
  diff `before_index`/`after_index`/`position`, `op_index`) routes through a
  shared range-checked converter that returns `txn_bad_type` ("number out of
  range") instead of an out-of-range `double`→integer cast (a UBSan abort in
  Debug CI). `test_c0_txn` pins the `5000000000` round-trip and the `1e300`
  fallback.
- **Result** is committed or rejected; keys ascend (`operations`/`errors`,
  `revision`, `status`, `transaction_id`):

```
committed: { "schema":1, "result": { "operations": [ {"op":…, <*_id>, "diff":{…}} ],
    "revision":<new>, "status":"committed", "transaction_id":<hex> } }
rejected:  { "schema":1, "result": { "errors": [ {"code":<token>,"message":…,"op_index":<n>} ],
    "revision":<unchanged>, "status":"rejected", "transaction_id":<hex> } }
```

## 4. Semantic state identity (task 4) — `tp_c0_semantic`

`dirty = current semantic state identity != saved baseline identity`, and dirty
is **not** derived from revision numbers (§8). `tp_c0_semantic_field` /
`tp_c0_runtime_excluded` pin the partition (`test_c0_op`):

- **Participates** (persistent content + structural IDs): atlas name + the 10
  packing knobs; source id + normalized key; sprite id + overrides + logical
  `rename` name; animation id + fps/playback/flips + **frames**; target
  exporter_id/out_path/enabled; every structural ID.
- **Excluded runtime state** (never dirty, never an Undo entry): the revision
  counter itself, dirty flag, Undo/Redo history, saved-baseline snapshot,
  session/ownership/authority, pack results + `pack_input_hash`/`preview_hash`/
  freshness, source runtime status/watchers/mtime/decoded pixels, thumbnails, GUI
  view state, the GUI local `s_model_ver`, the project file **path** (that is the
  project *identity key*, §5.1, not content), and `schema_version` (serialization
  envelope).
- **Ordering rule (decision):** identity is computed over collections **keyed by
  stable ID and order-normalized** — atlases/sources/sprites/animations/targets
  are ID-addressed and have no reorder operation — **except an animation's
  `frames`, whose order is itself semantic** (playback order). The `ordered` flag
  marks that single exception. The on-disk save order stays a serialization
  detail; F2 computes identity from this partition.

## 5. Revision precondition + full-batch ordering (task 5)

`tp_c0_revision_check`: `expected == current` → commit; `expected < current` →
`revision_conflict`; `expected > current` → `invalid_revision` (§8). No CRDT/merge
in v1 — the caller rebuilds and retries.

Validation phases (`tp_c0_txn_validate`), pinning "validate the whole batch
before any application":

1. **structural decode** — envelope faults fail fast and alone (`bad_json`,
   `txn_bad_version`, `txn_missing_field`, `txn_bad_id`, `txn_bad_type`,
   envelope/transaction `unknown_field`).
2. **revision precondition** — checked against the whole batch; a mismatch
   **short-circuits** and rejects alone (op_index `-1`), before any per-op work.
3. **per-op semantic checks** — `op_unknown`, `unknown_field`, malformed `*_id`
   (C0-01 `id_bad_*`), unresolved/dangling id reference, unresolved selector —
   **collected in stable order**: by `op_index` ascending, then the op's field
   order. `test_c0_txn` pins a three-op request whose errors emit as
   `[op0 unknown_field, op1 id_bad_hex, op2 op_unknown]`.

Idempotency (§7.2): `tp_c0_txn_idset` is the retention set; re-adding a seen id →
`txn_duplicate_id`; a non-hex id → `txn_bad_id`.

## 6. Before/after diff requirements (task 6)

A committed op records enough for exact reverse apply (§9). The requirement is
fixed by effect class (`tp_c0_diff`), pinned by golden fixtures:

- **CREATE** — `after` = the full created entity's fields + `position` (ordering
  slot); reverse = remove.
- **REMOVE** — `before` = the full removed entity + `position`; reverse = re-create
  at position.
- **MOVE** — `before_index` + `after_index`; identity unchanged; reverse swaps them.
- **SET** — `before` + `after` field values; reverse = set back to `before`.

These are contract shapes with golden request/result fixtures, not an engine:
the spike does **not** compute diffs from a live model (that is F2). `test_c0_txn`
round-trips a committed result carrying a CREATE diff and a MOVE diff.

## 7. This note (task 7)

Lives beside the fixtures/tests. Any change to a pinned rule above must land with
the matching golden-test update in `packer/spike/c0/tests/` — the tests are the
executable form of this contract.

---

## Settled decisions (recorded per §59 item 52)

1. Operation wire names as tabled in §1 (e.g. `atlas.settings.set`,
   `animation.frame.move`); frame edits are granular ops plus a reserved bulk
   `animation.frames.set`.
2. `new`/project creation is a session command; its seed is `atlas.create` +
   `target.create`. `sprite set` is a compound verb lowering to override + name
   ops.
3. Transaction id = 32 lowercase hex (128-bit, unprefixed). `sprite_id` canonical
   text is bare 32-hex (C0-01 defines shape prefixes only for
   atlas/source/anim/target); frame references are sprite_ids.
4. Canonical op fields: scalar fields for fixed tuples, JSON arrays only for
   variable lists; numbers integral-without-decimal or `%.9g`.
5. Unknown-field policy = REJECT (stricter than the file loader's ignore).
6. Canonical JSON ordering: ascending keys, discriminator first, 2-space/LF/
   trailing-newline, sparse empty optionals.
7. Semantic-identity ordering: ID-keyed collections order-normalized; animation
   frames order-semantic; project path and `schema_version` excluded.
8. Validation ordering: structural fail-fast → revision short-circuit → per-op
   collect-all in (op_index, field) order.

## Open blockers / non-goals

- No journal record format and no network/session protocol (Epic A / C0-03).
- No mutation engine and no live diff computation — diffs are contract-shaped
  golden fixtures; `tp_c0_txn_validate` produces a committed **stub** (revision
  incremented, addressing echoed, no diffs).
- Reference-existence checking is opt-in (an entity table); full reference
  validation and selector resolution over a live project are F2 work.
- Spike storage caps (`TP_C0_MAX_OPS`, `TP_C0_MAX_FIELDS`, `TP_C0_STR_CAP`) are
  fixed-size for determinism; F2 uses dynamic storage for large batches
  (the spec's "100 animations in one transaction").
- Exact public C method names remain implementation-settled (§59 item 52); the
  wire vocabulary and JSON schema above are the pinned contract.
