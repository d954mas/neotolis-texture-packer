# C0-01 — Identity, path, and Unicode contract note

Status: spike-accepted. Normative source: `docs/ntpacker-master-spec.md`
§5–5.6, §15–16, §52.1, §59 items 1–8. This note records the portable rules
pinned by the C0-01 spike. It introduces **no on-disk schema**; these decisions
become the input to F1's public API (F1-00/01/02/03) and to B0/B1 sprite IDs.

Reference implementation and golden tests:

- `packer/spike/c0/include/tp_c0/` + `packer/spike/c0/src/` — `tp_c0` library.
- `packer/spike/c0/tests/` — table-driven Unity tests, wired into ctest as
  `test_c0_error`, `test_c0_id`, `test_c0_shape_id`, `test_c0_path`,
  `test_c0_srckey`, `test_c0_legacy`.
- `packer/deps/utf8proc/` — vendored Unicode library (see its `VENDOR.md`).

Structured errors: every fallible entry point returns a `tp_c0_detail` machine
token (never aborts) plus optional human prose. Tokens are pinned in
`test_c0_error`; F1 promotes the surviving ones into the production status
vocabulary.

---

## 1. Canonical saved-project path (task 1) — `tp_c0_path`

Project identity (§5.1) is the canonical normalized path of the
`.ntpacker_project` file. The spike pins the **lexical** half of that contract;
`tp_c0_project_path_canonical()` touches no filesystem.

- **Host is an explicit parameter** (`TP_C0_HOST_POSIX` / `TP_C0_HOST_WINDOWS`),
  not `#ifdef`. Both rule sets are exercised on every OS, so golden vectors are
  identical across Linux/Windows/macOS (§4.3 determinism; no platform-specific
  golden output).
- **Separators.** WINDOWS treats `\` and `/` as separators; POSIX treats `\` as
  a literal filename byte. Output always uses `/`.
- **Accepted absolute forms.** POSIX `"/..."`; Windows `"X:/..."`, bare drive
  `"X:"` (→ `"X:/"`), and UNC `"//server/share/..."`.
- **Windows device / verbatim namespace.** After the `\`→`/` rewrite,
  `\\?\X:\...` (`//?/X:/...`) and `\\?\UNC\server\share\...` (`//?/UNC/...`, the
  `UNC` component matched ASCII case-insensitively) are **transparent lexical
  aliases**: the `\\?\` prefix is stripped and the remainder canonicalized as the
  drive or UNC form, so `\\?\C:\work\p` ≡ `C:\work\p`. Every **other** `\\?\...`
  form and **all** `\\.\...` device paths are rejected (`path_device`) — a device
  path is never a project file, and one file must have one identity (decision
  `docs/decisions/0006-windows-device-paths.md`). POSIX host is unchanged.
- **Rejected.** Non-absolute / relative (`path_not_absolute`), Windows
  drive-relative `"X:foo"` (`path_drive_relative`), malformed UNC without a share
  (`path_bad_unc`), Windows device/verbatim paths outside the two alias forms
  (`path_device`), over-length (`buffer_too_small`), empty (`empty`).
- **Normalization.** `.` dropped; repeated separators collapsed — **including
  doubled separators inside the UNC head** (`//server//share` → `//server/share`,
  `///server/share` → `//server/share`), with the UNC leading `//` preserved; a
  genuinely missing share (`//server`, `//server/`, `//server//`) is still
  `path_bad_unc`. `..` resolved **lexically** and **clamped at the root** (`/..`
  → `/`); trailing `/` stripped.
- **Case.** The canonical *string* preserves case except the Windows **drive
  letter is upper-cased**. Identity *equality* (`tp_c0_project_path_equal`):
  POSIX is byte-exact (case-sensitive); WINDOWS folds ASCII case (its filesystem
  is case-insensitive). Non-ASCII case-insensitivity on Windows volumes is a
  known simplification for the spike.
- **Symlinks / junctions are NOT resolved.** Canonicalization is lexical, so a
  not-yet-created **Save As** destination canonicalizes fine (its final
  component need not exist). Symlink/junction equivalence is a filesystem
  property resolved by the OS realpath step **once the file exists** (at/after
  first save). Until then two lexically different but symlink-equivalent
  destinations are **distinct identities**. F1-00 layers the realpath step on
  top of this lexical contract.
- **Unsaved sessions.** Before first save there is no path identity: the session
  uses a temporary runtime ID (a random `tp_c0_id128`, task 2), per §5.1/§16.
  `Save As` is an atomic identity transition to the new canonical path (F1-00).

## 2. Random 128-bit ID + RNG seam (task 2) — `tp_c0_id`

- `tp_c0_id128` is 16 raw bytes; `bytes[0]` is the most-significant octet in the
  hex text (big-endian text ↔ binary agreement).
- **RNG seam.** `tp_c0_rng { fill(ctx,out,len) -> int }`. A return `!= len`
  (short read) or `< 0` (failure) is surfaced as a structured error
  (`rng_short` / `rng_fail`) — never an abort. On failure `*out` is left nil.
  Fault-injection seams are exercised in `test_c0_id` (fixed bytes, short read,
  hard failure, null args).
- **Default generator.** `tp_c0_rng_os()` uses Windows `rand_s` / POSIX
  `/dev/urandom` — **no engine-private API**, no extra link deps.
- **Nil ID.** All-zero is the reserved "none" value; generation never yields it
  from a healthy RNG, and consumers reject nil where a real ID is required.

## 3. Text encoding + Unicode NFC boundary (task 3)

- Vendored **utf8proc 2.11.1** (MIT, Unicode 17.0.0), pinned with source URL and
  SHA-256 in `packer/deps/utf8proc/VENDOR.md`. Engine submodule untouched
  (decision `docs/decisions/0002-utf8proc-nfc.md`).
- **NFC** = `utf8proc_map(UTF8PROC_NULLTERM | UTF8PROC_STABLE | UTF8PROC_COMPOSE)`.
  **Case-fold** adds `UTF8PROC_CASEFOLD` (full folding: `ß` → `ss`, verified in
  `test_c0_srckey`).
- Invalid UTF-8 yields a structured `invalid_utf8` error, never a crash.
- utf8proc allocations are consumed and freed **inside `tp_c0_srckey.c`**; all
  public outputs go into caller buffers (no cross-CRT malloc/free handoff).

## 4. Canonical shape ID + parser (task 4) — `tp_c0_id`

- **Textual shape:** `<prefix><32 hex>`, prefixes `atlas_`, `source_`, `anim_`,
  `target_`. **Binary shape:** the 16 bytes; the kind is contextual (the storing
  field decides it), so the prefix is a presentation/validation affordance.
- **Format** emits **lowercase** hex (canonical). **Parse** accepts hex in either
  case and re-emits lowercase; the **prefix is case-sensitive lowercase**
  (`ATLAS_...` is rejected).
- **Nil** (`..._000...0`) parses structurally; callers reject it via
  `tp_c0_id128_is_nil` (`id_nil`).
- **Invalid inputs** (each with a distinct token): unknown/missing/wrong-case
  prefix (`id_bad_prefix`), body not 32 hex (`id_bad_length`), non-hex digit
  (`id_bad_hex`), trailing bytes (`id_trailing`), empty (`empty`), null
  (`null_arg`), small format buffer (`buffer_too_small`), INVALID kind on format
  (`id_bad_prefix`).

## 5. Deterministic sprite / legacy IDs + collision fallback (task 5) — `tp_c0_id`, `tp_c0_legacy`

- **Stable hash.** Versioned, platform-independent **FNV-1a/128** (golden
  vectors cross-checked against an independent reference in `test_c0_id` /
  `test_c0_legacy`). Streaming form composes IDs without concatenation buffers.
- **sprite_id** (§5.2) = `hash("sid1" tag + source_id bytes + 0x00 +
  normalized source-local key)`. A logical/export rename never feeds it, so it is
  rename-invariant; a source-key change (external file rename) changes it — the
  old sprite becomes missing, the new key a new sprite (§5.3, §59 item 7).
- **Legacy synthetic IDs** (§5.5) = `hash("lid1" tag + kind + 0x00 + tuple + 0x00
  + salt_le32)`, `tuple` a caller-built stable discriminator from the v1 file
  structure. Repeated read-only loads produce identical IDs.
- **Collision fallback.** `tp_c0_legacy_assign` processes entries in array order;
  on a clash with an already-assigned ID (or a nil hash) it bumps the salt and
  re-hashes until unique — deterministic and reproducible across loads. The base
  hash is an **injectable seam** so tests force collisions; the default never
  exhausts in practice, but a degenerate hash reports `collision_exhausted`
  rather than looping (bounded sweep).
- The next successful save persists normal **random** IDs for saved structural
  entities (F1-01); derived sprite IDs stay deterministic.

## 6. Source-key normalization (task 6) — `tp_c0_srckey`

Canonical persistent source-local key (§5.3, §59 item 8):

- UTF-8, Unicode **NFC**, `/` separator (`\` also accepted as a separator so a
  Windows-authored folder addresses the same sprite on Linux).
- `.` components and repeated separators dropped; **preserved letter case**.
- **`..` rejected outright** (`key_traversal`) — a source key must never escape
  its root (stricter than the project-path `..` resolution, which is a distinct
  policy).
- **Absolute rejected** (`key_absolute`): a leading `/` or `\` (including
  `"///"`), a Windows `X:` drive prefix, **or a drive prefix revealed only after
  `.`-component stripping** — the **first emitted component** beginning `X:` is
  rejected (so `./C:/x` → `key_absolute`, not the accepted `C:/x`). This
  guarantees **idempotence**: `normalize(normalize(x)) == normalize(x)` for every
  accepted `x`. A drive-looking spelling in a *later* component (`a/C:/b`) is not
  absolute — the portability scan flags its `:` as an invalid char instead.
- Empty result (e.g. `"."`, `"./"`) → `empty`. Invalid UTF-8 → `invalid_utf8`.
- **Case-fold is used ONLY for portability collision detection**
  (`tp_c0_srckey_collides`): `Button.png` and `button.png` are distinct keys but
  a reported cross-platform collision; `STRASSE.png` and `straße.png` collide via
  full folding. NFC-equivalent inputs normalize to the **same** key and are not a
  collision. `tp_c0_srckey_collides` folds via utf8proc directly (allocations
  freed in-TU per §3), so it is **not** limited by the key length — a near-limit
  key whose full case-fold expands up to ~3x still yields a correct verdict. The
  caller-buffer `tp_c0_srckey_casefold` must itself budget for that ~3x expansion.
- **Portability validation** (`tp_c0_srckey_portability`, a warning bitmask, not
  a normalization error): Windows reserved names (`CON`/`PRN`/`AUX`/`NUL`/
  `COM1-9`/`LPT1-9`, extension-insensitive), invalid characters
  (`<>:"|?*` or control chars), and trailing dot/space in a component.

## 7. This note (task 7)

Lives beside the fixtures/tests. Any change to a pinned rule above must land with
the matching golden-test update in `packer/spike/c0/tests/` — the tests are the
executable form of this contract.

---

## Open blockers / non-goals

- No live ownership claim, journal, or cross-process logic (Epic A / C0-03).
- Windows non-ASCII path case-insensitivity is simplified to ASCII folding.
- Filesystem-level realpath/symlink resolution is layered by F1-00 on top of the
  lexical contract; it is intentionally out of the pure spike.
- No new external dependency beyond utf8proc, whose MIT license and pinned
  provenance are recorded in `packer/deps/utf8proc/VENDOR.md`
  (decision `docs/decisions/0002-utf8proc-nfc.md`).
