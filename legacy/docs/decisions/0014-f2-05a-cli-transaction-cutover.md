# 0014 — F2-05a: CLI transaction cutover (ordinary CLI routed through the op/txn engine)

> **Foundation addendum (2026-07-16):** accepted, implemented and reviewed. Ordinary
> CLI mutations now execute through the common `tp_session` boundary; the
> journal-less one-shot ownership described below is retained as cutover history.

**Date:** 2026-07-13
**Status:** accepted; foundation implementation reviewed 2026-07-16
**Decided by:** F2-05a implementation agent (delegated); foundation lead review complete
**Implemented in:** `apps/cli/cli_mutate.c` (rewrite), one core change in
`packer/src/tp_op_validate.c`, coverage in `packer/tests/test_op_apply.c` +
`scripts/check_boundaries.sh` (rule R6). Master spec §4.1–4.2, §7.1, §14.2, §59
items 9–19. Plan §7 F2-05 tasks 1,2 + CLI side of 4,5.
**Amended by the F2-05a FIX pass (2026-07-13):** `apps/cli/cli_mutate.c` (F1/F3/F4/F6/F7),
`packer/src/tp_op_apply.c` (F2 storage-key), `scripts/check_boundaries.sh` (R6c),
`apps/cli/cli_mutate_family.cmake` + `packer/tests/test_op_apply.c` (F1/F2 coverage). See
the "F2-05a FIX pass" section below.

## Scope: lead split F2-05 into two reviewable halves

The plan's F2-05 packet ("frontend cutover") is split by the lead into:
- **F2-05a (THIS):** the ordinary **CLI** mutation verbs route through the F2-01/F2-02
  typed operation + transaction engine. The CLI stays FILE-oriented (one-shot, **no
  live journal**).
- **F2-05b (next):** the **GUI** cutover + the live journal/session, and retirement of
  the production snapshot-mutation path.

Non-goals here (deferred to F2-05b / later): no journal in the CLI, no GUI change, no
removal of the snapshot-mutation path, no arena integration.

## The cutover shape

### One verb = one transaction, on a journal-LESS model

Each mutating verb now: (1) loads the project; (2) resolves selectors + parses
arguments in the CLI; (3) builds one or more typed `tp_operation`s; (4) commits them
as **one atomic transaction** through a journal-less `tp_model`. `cli_mutate.c` no
longer writes any `tp_project` field and no longer calls the inline `tp_project_*`
mutators — those became `tp_operation`s.

```
p = tp_project_load(path)
m = tp_model_wrap(p)                     // journal==NULL, history==NULL: the in-memory
req = { schema, id_hex=random32hex,      //   clone-swap path (== the F2-02 baseline)
        expected_revision=0, ops[], n }
st = tp_model_apply(m, req, &res, &err)  // clone -> validate+apply each op -> swap+bump
if st == OK:  finish_saved(tp_model_project(m))   // promote ids (§5.5) + save
else:         emit_reject(res) ; map tp_status -> CLI exit code
tp_model_destroy(m)                      // frees the (saved) project
```

- **journal-less** = `tp_model_wrap` leaves `journal`/`history`/`coordinator` NULL, so the
  commit gate is the in-memory idstore record — exactly the F2-02/03 code path. No
  on-disk transaction record; revision is runtime-only and never persisted (§414).
- **transaction id** is a fresh random 32-lowercase-hex per invocation (idempotency is
  moot on a freshly-wrapped model at revision 0); `expected_revision = 0`.
- **created entities get their id at op-build time** (`tp_id128_generate`), stamped by
  apply (`id_synthetic=false`), so `promote_ids` at save is a no-op for them — instead
  of the old "create nil-id entity, promote assigns id at save". Both paths produce a
  random id that is not golden-pinned; re-save stays byte-stable (idempotent). The net
  effect on saved bytes is nil (proven by `cli_mutate_stable` + the family goldens).
- **`do_new` stays direct** (project-create lifecycle, not a mutation rule):
  `tp_project_create` + `tp_project_atlas_seed_default_target` + `finish_saved`. It
  builds no ops (brief-sanctioned).

### Verb → operation(s)

| verb | operation(s) (one transaction) |
| --- | --- |
| `set` | 1× `atlas.settings.set` (mask = the named knobs) |
| `sprite set` | `sprite.override.set` (if any override field) **then** `sprite.name.set` (if `rename`) |
| `sprite unset` | 1× `sprite.override.clear` (mask = ALL → drop record) |
| `anim create` | 1× `animation.create` (name, default fps/playback, frames) |
| `anim remove/set/add-frame/remove-frame/move-frame` | 1× the matching animation op |
| `target add/remove/set` | 1× `target.create` / `.remove` / `.set` |
| `add` (source) | N× `source.add` (one per non-dup path) |
| `remove` (source) | 1× `source.remove` |
| `atlas add/remove/rename` | 1× `atlas.create` / `.remove` / `.rename` |
| `new` | **direct** (create + seed_default_target; no op) |

### What moved to core vs what stayed in the CLI

**Moved to core (deleted from the CLI), authoritative on every commit:**
- **atlas knob value RANGES** (`max_size` [1..4096], `padding/margin/extrude` ≥0,
  `alpha_threshold` [0..255], `max_vertices` [1..16], `shape` [0..2],
  `pixels_per_unit` >0 finite) → `validate_atlas_settings`.
- **animation `fps`** (>0 finite) → `validate_anim_knobs`.
- **frame.move `to_index` clamp** (already in core apply); **`frame_count ≥ 0`** (core-only).

**Stayed in the CLI (selector/argument PARSING + rendering — NOT re-implemented core
rules):**
- numeric/enum PARSE (`to_int` with a fits-int marshalling guard, `to_bool`, `to_float`,
  csv parsers, `parse_playback` for the [0..6] enum domain);
- **sprite override field ranges** (`shape/allow_rotate/max_vertices/margin/extrude/
  slice9/origin`) — these marshal into a **sparse int16 slot whose sentinel is
  `INHERIT`(-1)**; parsing a value into that slot legitimately needs the field domain (a
  stray `-1` must not silently mean "inherit", an over-`32767` value must not wrap). Core
  re-validates the same ranges on commit (`validate_sprite_set`) — defense-in-depth, not
  a divergence;
- **name-uniqueness** for `atlas add/rename` and `anim create` — a CLI **selection**
  policy (keep name-keyed selection unambiguous), kept to preserve the exact `usage`
  diagnostic + exit 2. Core independently rejects a duplicate *atlas* name (harmless
  redundant defense; never fires because the CLI catches it first) but does **not** reject
  a duplicate *animation* name — so keeping the CLI check is REQUIRED there;
- **exporter-id** registry lookup (a vocabulary check that maps to usage/2; core
  classifies an unknown exporter as `not_found`/3, so keeping the CLI check preserves the
  exit-2 contract);
- **selector resolution** (atlas/anim/target/frame/source by name or index) and
  **source-path dedupe**;
- result **rendering** (`cli_emit_mutation`, `anim list` JSON, human lines).

## Byte-identity: how it holds

The whole point. Verified: `cli_parity`, `cli_mutate_stable`, `cli_pack_*`,
`tp_export_defold`, `tp_export_json` **byte-identical** (all green, both presets). The
CLI e2e goldens pin **exit codes + saved-project substrings + JSON success payloads**;
they do **not** pin error-message text/id.

- **Saved-project bytes:** core's apply writes the identical fields/values the inline
  mutators did (verified field-by-field against `tp_op_apply.c`); the canonical writer is
  unchanged; net-zero add/remove re-saves byte-identical (`cli_mutate_stable`).
- **Exit codes (the machine contract):** preserved. Selector-miss stays in the CLI →
  exit 3. Value/name/vocabulary errors → exit 2 (CLI parse errors directly; a committed
  transaction's reject via `map_reject_exit`: `not_found`/`out_of_bounds` → 3, OOM/RNG →
  1, everything else — `out_of_range`/`invalid_argument`/… — → 2).
- **JSON success payloads:** unchanged (`{"schema":1,"ok":true,"verb":..,"count":..}`).

## CLI-vs-core divergence table (the F2-01-noted reconciliations)

| # | divergence (F2-01 note) | reconciliation | byte-identity |
| --- | --- | --- | --- |
| a | `atlas.create`/`.rename` reject a duplicate name; the CLI also rejects (its own policy) | **CLI keeps its name-uniqueness check** (exit 2, exact message). Core's rejection is redundant defense that never fires. **preserve-observable** | exit 2 + message unchanged |
| b | `frame.move` large `to_index`: CLI clamps to append; F2-01 apply now clamps identically | build the op with the raw `to_index`; **core apply clamps** `[0, frame_count-1]` before the subtraction (F2-01 fix [6]) | committed bytes identical (`cli_parity`-class; pinned by `tp_op_apply.c::test_parity_frame_move_to_end`) |
| c | `padding/margin/extrude` >4096: F2-01 relaxed core to ≥0 to match the CLI | value range **moved to core** (`min_i`, ≥0). CLI does parse + fits-int only | identical accept/reject set for all in-int values |
| d | `source.add` duplicate path: core REJECTS (id-contract); the CLI silently de-dups | **CLI keeps its `source_matches` dedupe** (+ intra-batch dedupe) and simply does **not build** a `source.add` op for a dup — so the transaction only ever carries non-dup adds and core's reject never fires. **preserve-observable** | exit 0, saved bytes identical |

**Sprite source-id gap (new architectural finding, one core change).** F2-01's sprite ops
require the source to exist (`find_source`), but the CLI adds overrides **by export-key on
a source-less atlas** (the `sprite` golden runs on a fresh project with zero sources).
This was unmodeled. **Core change:** `tp_operation_validate` now treats a **nil
`source_id` as a PENDING (name-keyed) override** for `sprite.override.set/.clear` and
`sprite.name.set` — skip `find_source` when the id is nil; a non-nil unknown source still
rejects `NOT_FOUND`. This is exactly the "PENDING record" state decision 0010 §2 already
documents (leaves `source_ref` nil). The CLI passes
`source_id = nil`, `src_key = <the raw key>`. Locked by
`test_op_apply.c::test_apply_sprite_pending_no_source`; the existing non-nil rejection
tests still pass. **(Storage-key form: the F2-05a FIX pass below keys a PENDING record by
the VERBATIM `src_key` for byte-identity with the pre-cutover inline CLI; a source-attached
record keys by the export bridge. See F2.)**

## Diagnostics: preserved where pinned, re-derived where not

Exit codes are the pinned machine contract and are **byte-identical**. Error **id/message
text is not pinned by any golden**; for the two checks that moved to core, the emitted
structured error now carries **core's** id/message instead of the CLI's generic
`usage` — a deliberate, documented **re-derivation** (more specific, and the single
authority all clients share):

| input | before (CLI inline) | after (core reject via adapter) | exit |
| --- | --- | --- | --- |
| `set … max_size=99999` | id `usage`, "max_size = '99999' must be an integer in [1..4096]" | id `out_of_range`, "max_size = 99999 must be in [1..4096]" | 2 (same) |
| `set … alpha_threshold=999` | id `usage` | id `out_of_range`, "alpha_threshold = 999 must be in [0..255]" | 2 (same) |
| `anim set … fps=-1` | id `usage` | id `out_of_range`, "fps must be positive finite" | 2 (same) |

Everything else (parse failures, unknown keys/fields, name collisions, exporter
vocabulary, selector-miss) keeps its **exact** pre-cutover id + message.

**Benign edge-input tightenings that are KEPT** (not golden-pinned; core behaviour is the
more-correct one, and one removes a determinism-invariant violation — enumerated in FULL
for honesty. The F2-05a FIX pass below re-audited these and KEEPS all three):
- **T1 — non-finite sprite `origin`** (`origin=inf,0` / `nan`): the CLI had **no** finite
  check on origin; core now rejects it (`out_of_range`, message "origin must be finite",
  exit 2). Was silently accepted + saved (an unroundtrippable `inf`) before. Left as core's
  `out_of_range` and deliberately NOT given a CLI-side finite guard, so it stays CONSISTENT
  with its siblings `pixels_per_unit` / `fps` (both parsed by the CLI, finite/range-rejected
  by core under the same `out_of_range` id). **KEEP.**
- **T2 — a padding/margin/extrude knob value > INT_MAX** (e.g. `padding=5000000000`): the
  old inline `to_long` path **silently wrapped** on the narrowing long→int cast and PERSISTED
  a value that was **non-deterministic across OS** (LLP64 Windows `long`==32-bit vs LP64
  Linux/mac `long`==64-bit). `to_int` (strtoll + fits-int) now rejects it as usage (exit 2).
  Reverting would reintroduce a determinism-invariant violation, so keeping the reject is
  **REQUIRED. KEEP.**
- **T3 — move-frame index > INT_MAX** (`anim move-frame … 9999999999 0`): the old path did a
  garbage `long→int` cast and fell into a nonsense out-of-bounds (exit 3) that varied by OS
  `int`/`long` width; `to_int` now rejects the absurd index as usage (exit 2) before the op
  is built. **KEEP.**

(The originally-flagged third tightening — a sprite override key *with an extension* being
stored under the ext-stripped export bridge — was in fact a byte-identity REGRESSION vs the
pre-cutover verbatim storage and is REVERTED by the fix pass; see **F2** below.)

## Batch semantics

One verb = one transaction (task 2). Multi-op verbs commit atomically: `sprite set` with
`rename` = `[override.set, name.set]` in that order (fields-first-rename-last, matching
the old inline order); `add` with N paths = N `source.add` ops. If any op rejects, the
whole batch rolls back (the F2-02 clone-swap leaves the live model byte-unchanged) and
nothing is saved — the same "reject → no save" the inline path had. An all-dup `add`
commits an **empty** transaction (0 ops) and re-saves the unchanged project (exit 0),
matching the old behaviour. Intra-batch source dedupe is handled in the CLI (a queued
canonical-path set), so a repeated path in one `add` invocation de-dups rather than
tripping core's dup-path reject.

## Boundary test (task 5)

`scripts/check_boundaries.sh` rule **R6**, scoped to `apps/cli/cli_mutate.c`:
- **R6a** — bans calls to the inline `tp_project_*` mutators the ops replaced (the crisp
  proof of the cutover; `tp_project_create`/`seed_default_target` are lifecycle, allowed);
- **R6b** — bans direct writes into the loaded project's arrays (`p->atlases[…].field =`).
- A **self-test** feeds seeded-bad and known-good samples to each detector every run, so
  "a seeded boundary violation is caught" is asserted, not assumed. (A blanket
  field-name write ban is deliberately avoided: the op payload structs reuse the same
  field names, so it would false-positive on legitimate op-building.)

## Battery (both presets, real output)

- `cmake --build --preset native-debug --clean-first` → 0 non-cgltf warnings.
- `cmake --build --preset native-release --clean-first` → 0 non-cgltf warnings.
- `ctest --preset native-debug` → **78/78 passed**. `ctest --preset native-release` →
  **78/78 passed**.
- `bash scripts/check_boundaries.sh` → **boundaries OK**.
- Byte-identity goldens green: `cli_parity`, `cli_mutate_stable`, `cli_pack_*`,
  `tp_export_defold`, `tp_export_json`.

## F2-05a FIX pass (byte-identity + parity corrections, 2026-07-13)

A 20-agent adversarial review of the cutover found byte-identity / diagnostic-parity
regressions the goldens missed (they did not exercise these edges). This pass FIXES them and
ADDS the missing coverage. It does NOT revert the cutover shape; every change preserves or
RESTORES pre-cutover observable behaviour.

**F1 — `sprite set … <override>=inherit rename=X` no longer reorders the sprites array.**
`do_sprite_set` now emits `SPRITE_NAME_SET` (the rename) **BEFORE** `SPRITE_OVERRIDE_SET`.
The transaction applies ops in order to the clone (op N sees ops 1..N-1), so the rename makes
the record non-default first; when the override then clears its last field to `INHERIT`, the
post-set prune keeps the still-renamed record **in place** instead of dropping it and letting
the rename re-append it at the array END. Matches the pre-cutover single in-place edit
(add_sprite → set fields → set_rename → one prune). A pure clear (no rename) still prunes to
default → record dropped, as before. Proof: `cli_mutate_sprite` builds three retained records
`s0`/`s1`/`s2` and clears+renames the MIDDLE one, asserting `s1` stays between `s0` and `s2`
(the bug moved it after `s2`).

**F2 — CLI-added sprite overrides store/clear under the VERBATIM key (byte-identical), not
the ext-stripped export bridge.** Root cause: `tp_op_apply.c` applied the
`tp_sprite_export_key` bridge at STORAGE time for ALL sprite ops. Fix (apply layer,
`sprite_store_key`): a PENDING override (nil `source_id` — every CLI `sprite set`/`unset`,
which key by name before any source scan) stores under the raw `src_key` VERBATIM; a
SOURCE-ATTACHED override (non-nil `source_id`, whose `src_key` is a source-local path) still
bridges so the pack/export path resolves it. This restores the pre-cutover bytes
(`sprite set hero.png` → record `"hero.png"`, not `"hero"`) AND fixes `sprite unset hero.png`
silently missing a pre-existing verbatim `"hero.png"` record. Scope proof: the change is gated
on nil `source_id`, so the non-nil bridge tests (`test_apply_sprite_ops`,
`test_parity_sprite_override`, `test_json_ov_int16_range`) and the F1-03
sprite-index/selector tests are unaffected (they use non-nil sources, or ext-less keys where
verbatim == bridge). Locked by `test_apply_sprite_pending_verbatim_ext_key` +
`cli_mutate_sprite`'s `img.png` round-trip. **`tp_op_validate.c`'s nil-source PENDING logic
was NOT touched** — it never dictated the storage-key form.

> **FLAGGED ambiguity (owner decision).** The verbatim-vs-bridged choice affects PACK-TIME
> override matching. The pack path looks up overrides by the export key computed from the
> scanned source file (`tp_input.c`: `find_sprite(a, export_key(scanned_name))`), so a pending
> override whose key carries an extension (`"hero.png"`) is stored verbatim but the scanned
> `hero.png` bridges to `"hero"` at pack time → it will NOT match. **This is EXACTLY the
> pre-cutover behaviour** (the old CLI also stored `"hero.png"` verbatim and also would not
> match at pack). Per the fix brief, byte-identity wins and pre-cutover bytes are reproduced;
> the "should an ext-carrying PENDING key ALSO be matchable at pack time" question is left OPEN
> for the owner (it would be a separate, behaviour-changing decision, not a byte-identity fix).
> For all ext-LESS keys (every golden + normal usage) verbatim == the export key, so pack
> matching is unaffected.

**F3 — move-frame out-of-range `from` keeps the pre-cutover diagnostic.** A CLI pre-check in
`do_anim` move-frame emits `frame_not_found` / "animation '…' has no frame at index N"
(exit 3) instead of letting core surface `out_of_bounds` / "from_index N out of [0..M)". `to`
stays clamped by apply, as before. (Exit code 3 was already parity; this restores the
structured `error.id` + message.)

**F4 — an OS-RNG fault while minting a created entity's id now surfaces as `rng_failed`, not
`oom`.** `do_add` / `target add` / `atlas add` / `anim create` split the `cli_gen_id` failure
(RNG fault → `rng_failed`) from the strdup/calloc failure (→ `oom`). Pre-cutover this fault
surfaced only at promote time as `rng_failed`; the cutover minted ids at op-build and had
folded it into `oom`.

**F5 — the boundary guard now catches ALIASED in-place writes (R6c).** `check_boundaries.sh`
adds a detector banning assignment through the loaded-project alias names cli_mutate holds
(`a`/`an`/`t`, e.g. `a->max_size = 512;`) which R6b's literal `p->atlases[…]` match missed.
Its self-test fires on a seeded aliased write and passes the legitimate op-payload /
alias-READ forms.

**F6 — the moot transaction id is a fixed 32-hex constant, no longer an OS-RNG draw.** The
one-shot CLI runs exactly one transaction per process on a fresh journal-less model +
in-memory idstore, so idempotency is moot and the id is never serialized. A constant removes
an RNG failure mode from every verb with zero observable-output change (`hex32` was removed as
its only caller).

**F7 — `do_add` no longer double-canonicalizes each path.** `source_matches` takes the
already-computed canonical path instead of recomputing it (`do_remove_source` computes it once
at the call site).

**F8 — atlas.create builder: LEFT hand-rolled (noted).** `tp_op_build_atlas_create` is an
identical payload construction, but adopting it for only atlas.create would make ONE of the 16
hand-rolled CLI ops special (the others mint ids + fill payloads inline and deliberately do NOT
use the selector-resolving `tp_op_build_*` family, to keep the CLI's exact by-name-resolution
diagnostics). The id-mint + F4 RNG handling stay in the CLI regardless, so the builder gains
nothing here; left uniform for maintainability — zero byte/diagnostic change either way.

## Owner confirmation points

1. The **nil-source pending sprite override** core change (the only tp_core semantic
   change) — completing decision 0010 §2's documented PENDING state so the CLI's
   source-less `sprite set` is expressible as an op. OK?
2. The **diagnostic re-derivation** for the two moved value-range checks (id
   `usage` → `out_of_range`, message from core; exit code preserved). OK given no golden
   pins error text?
3. **Name-uniqueness stays a CLI selection policy** (atlas + anim) rather than moving to
   core — required for anim (core doesn't reject dup names) and preserves exact
   diagnostics for atlas. OK?
