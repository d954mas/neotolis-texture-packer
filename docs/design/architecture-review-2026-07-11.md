# Architecture Review — 2026-07-11 (full technical audit)

Owner's brief: *"We are building a texture packer we will maintain for years, at the
cutting edge. Lay a clear, flexible, easily extensible base. The whole packer is a set
of features, tools, visuals. Explicit decomposition, no hidden or non-obvious
couplings. Built on the engine's principles. Free, open, best texture packer in the
world."*

Method: six independent deep audits (core library, exporter layer, GUI architecture,
layer boundaries/tool parity, test+CI infrastructure, competitive/technology gap),
followed by lead adversarial verification of every load-bearing claim directly in the
code (see Appendix A). Yardstick: the engine principles (explicit over implicit, keep
it simple, set of modules, builder validates, fail early) plus this repo's own hard
invariants in AGENTS.md.

---

## 1. Verdict

**Unit-level quality is high; the structural base is not yet decade-grade, and the
debt is concentrated in one place.** The individual files are disciplined: uniform
error model, arena-owned results, bounds-checked parsers with real version handshakes,
a genuinely deterministic serializer, a sound async worker, clean include hygiene
(no GUI TU touches `packer/src` internals, no `nt_*` types leak from public
`tp_*.h`). The exporters honor the headline invariant: adding a format does not touch
pack orchestration. That is all real and worth keeping.

But measured against "explicit decomposition, no hidden couplings, easily extensible,
two equal clients", three structural debts dominate, and all three **compound
monthly**:

1. **The op layer is eroding before the CLI exists.** The only implementation of
   "how you actually pack a project" — folder expansion, sprite-name/override-key
   derivation, per-sprite override→engine encoding, the effective-shape rule,
   animation frame identity, degradation preview text, default-target seeding — lives
   in `apps/gui/`, not `tp_core`. Every one of these must be duplicated by the CLI
   (Phase 4), and duplication of already-inconsistent rules is how "byte-identical
   parity" silently becomes false. Two shipping defects already fell out of this
   (§3.1, §3.2).
2. **The settings/override schema has no single source of truth.** One knob is
   hand-declared and hand-mapped across ~9 sites; per-sprite overrides across ~6.
   Adding one packing knob today is a 9–14 file edit where a forgotten site is a
   *silent* wrong default.
3. **The safety nets are missing exactly where a years-long tool gets hurt**: the two
   parsers that ingest user files have no fuzzing and never run under a sanitizer in
   CI; the threaded async layer and all GUI logic have zero CI coverage; determinism
   is only proven per-machine, never cross-OS; and the whole verify/release recipe
   lives in the lead's memory instead of an in-repo `scripts/check.sh`.

None of this requires a rewrite. Every fix below is a consolidation of code that
already works — but the window to do it cheaply is **now, before the CLI is written**,
because today it is a *move* and after Phase 4 it becomes a *reconcile-two-diverged-copies*.

---

## 2. What is verified GOOD (keep, and keep gated)

- **Layer hygiene:** `packer/` contains zero UI/CLI code; GUI includes only public
  `tp_core/*` headers; public headers leak no `nt_*` types; path relativization and
  the caps clamp (`tp_export_effective_settings`) correctly live in core and are
  shared by GUI preview and export.
- **Format discipline:** `.ntpacker_project` writes `version` first, refuses newer
  files with a clear message, ignores unknown keys; `.ntpack` read-back checks magic +
  all three engine format versions loudly (`tp_pack_read.c:140/445/580`).
- **Determinism-by-construction:** canonical writer, sorted emission, `%.9g`, two-run
  byte-identical tests on project save, pack, and both exporters.
- **Async pack layer:** explicit job kinds, correct acquire/release handoff, cancel =
  discard-on-land, stale kept honest by memcmp gating. No race found by two
  independent reviews.
- **Plugin seam:** `tp_exporter` descriptor + registry + `tp_export_run` resolving
  purely by id — exporter #3 does not touch orchestration (verified walk-through,
  §3.3).
- **Test volume on core happy paths:** ~5,200 test lines vs ~4,600 source lines,
  fixture-driven goldens, D4-decode and UV-property tests, override validation.

---

## 3. Findings

Severity: **P0** = architectural, compounds with every feature; **P1** = significant,
will bite within a few features or is user-visible; **P2** = real but low blast
radius. Items marked **[verified]** were re-checked in code by the lead.

### 3.1 P0 — Op-layer erosion: the project→pack bridge is GUI-only

- `tp_core` explicitly refuses input gathering (`tp_project.h:262-264`,
  `tp_export_run.h:12-15` — "sprites[] are the call site's responsibility"). The
  bridge lives in `apps/gui/gui_pack.c`: `assemble()` (folder expansion + raw-name
  policy, :186-223) and `desc_add()` (override lookup by stripped key, the
  `0/1/2 → 1/2/3` shape re-encoding, slice9-forces-RECT effective-shape, extrude
  gating; :111-170) **[verified]**. Directory recursion + the image-extension
  whitelist (what counts as a sprite source) is `gui_scan.c`.
- **Sprite-name identity exists in four copies that already disagree** **[verified]**:
  `gui_rows.c:58` (`strip_ext`), `gui_rows.c:48` (`path_stem`),
  `gui_pack.c:83` (`strip_ext_keep_folder`), `tp_normalize.c:34` (`final_name`).
  For a dotfile in a subfolder (`tank/.png`, accepted by `has_image_ext`):
  row/override key = `"tank/"` but pack-time lookup key = `"tank/.png"` — an override
  set in the UI silently fails to apply. Latent edge case, but it is exactly the
  failure four copies invite.
- The effective-shape/extrude rule is encoded independently in `gui_pack.c:145-166`,
  `gui_view_settings.c:558`, and `tp_pack.c:135-151` — three hand-synced copies.
- Degradation preview text (`gui_pack_preview_diff`, `gui_pack.c:1036-1098`) is a
  second, GUI-side enumeration of what exporter writers *also* decide to drop — two
  sources of truth for the flagship "what will this format cost you" feature.
- Natural frame ordering (`nat_cmp`) and common-prefix anim ids live in `gui_rows.c`;
  default-target seeding hardcodes `"json-neotolis"` in `gui_project.c:115-122,341`.

**Why it compounds:** every GUI feature that touches packing semantics widens the gap
the CLI must re-implement; every copy is a divergence surface. AGENTS.md's "features
land in core first" is currently violated by ~7 capabilities (inventory in §5).

**Fix:** one core packet (§6, packet A): `tp_source_scan` (injected FS walker) +
`tp_project_atlas_build_descs()` + canonical `tp_sprite_export_key()` +
`tp_anim_frames_sorted()` + `tp_export_predict_loss()` + registry-driven default
target. GUI collapses to calls; CLI reuses.

### 3.2 P0 — Animation frame identity is an unvalidated cross-layer string contract (shipping defect)

Frames are stored in override-key space (GUI selection → `multi_sel_add(sprite_name)`),
but `tp_normalize` uses them **verbatim as final export names**
(`tp_normalize.c:89-134`, `(void)prep` — no validation against the sprite set)
**[verified]**. A rename override makes `final_name` = rename verbatim
(`tp_normalize.c:34-38`) while the animation still holds the old key
(`commit_sprite_rename` touches only the rename field) **[verified]**. Result: the
exported atlas contains an animation whose frames reference a sprite name that no
longer exists — silently, today, in the GUI-only product.

**Fix:** `tp_normalize` must resolve frames through the same override map and **fail
loudly** (`TP_STATUS_INVALID_ARGUMENT`) on a dangling frame; GUI rename should remap
frames (or surface the breakage). Add the negative test that pins the contract.

### 3.3 P0 — Settings/override schema declared ~9×, no single source of truth

One atlas knob (e.g. `max_size`) is independently written at: `tp_pack.h:94-103`,
`tp_pack_settings.c:21-30`, `tp_pack.c:72-96` (validate), `tp_pack.c:229-238`
(builder map), `tp_project.h:101-110`, `tp_project.c:216-225` (defaults),
`tp_project.c:1508-1517` (to_settings), `tp_project.c:879-904` (emit),
`tp_project.c:1242-1251` (load). Per-sprite override fields repeat across 6 more
sites. Only enum *values* are `_Static_assert`-pinned, not the field set.

**Fix:** define the knob/override set once as an X-macro / field-descriptor table;
generate struct members, defaults, validation ranges, sparse-serialize, load, and the
builder map from it. This also shrinks the `tp_project.c` god-file (§3.8).

### 3.4 P0 — Degradation notices: scattered producers, no consumer

- Drop decisions are re-implemented inside each writer (pivot/slice9/polygon/alias/
  multipage checks hand-mirrored in `tp_export_json_neotolis.c:157-249` and
  `tp_export_defold.c:350-387`); a third exporter can silently forget one — the exact
  failure the "never error, always notice" design exists to prevent.
- The async export path — the one the GUI button drives — counts and then **frees the
  notices unread** (`gui_pack.c:377,387`) **[verified]**; the UI shows only
  "(metadata notices raised)". The user can never learn *which sprite lost what*.

**Fix:** one shared pre-writer pass over `tp_export_prepared` that computes every
capability drop and emits notices once (writers only emit output); persist the full
notice list on the job result; render it (backlog C2 notices panel is the natural
sink; CLI echoes to stderr). This must land **before** the template/30-format work.

### 3.5 P0 — Safety nets missing where a file-opening tool lives or dies

- **Parsers unfuzzed, unsanitized in CI:** `tp_pack_read.c` (691 L hand-rolled binary
  parser) has 3 top-level negative tests; deep corruption branches never execute
  under ASan anywhere (CI builds `native-release` only; ASan/UBSan are Debug-only,
  non-Windows). `tp_project` loads numbers verbatim (NaN/Inf/negative accepted,
  `tp_project.c:1138`). Both parsers already take `(buf,len)` — they are ready-made
  fuzz targets with none attached.
- **No frozen goldens:** every golden `.ntpack` is regenerated from the live engine at
  test time (`.gitignore` even excludes `*.ntpack`); a lockstep reader+writer
  reinterpretation passes green while years-old user files misparse.
- **GUI logic + threaded async layer have zero CI coverage:** the only automated
  exercise needs GL and is default-OFF; `gui_history.c` (undo), `gui_rows.c`
  (nat_cmp/build_rows), and the pack/cancel/stale state machine are headless-testable
  today but untested. TSan is a manual, GL-requiring runbook.
- **Determinism is a product promise verified only per-machine:** no CI job compares
  artifact hashes across the 3-OS matrix; `%.9g` is libc-dependent and no
  `setlocale(LC_NUMERIC,"C")` guard exists anywhere in tp_core **[verified by grep]**
  — a non-C locale in a future CLI emits `63,5` into JSON/protobuf.
- **Bus-factor one:** no `scripts/check.sh` (the engine next door has one plus eight
  gate scripts), no `.clang-format`/`.clang-tidy`, the draw-hash baseline lives in a
  machine-local scratchpad, release zips ship without checksums.

**Fix:** packet E (§6): in-repo `check.sh` + CI lint; libFuzzer harnesses + committed
malformed corpus + a Linux Debug(ASan/UBSan) CI job; frozen golden bytes; extract
`gui_logic` static lib (history/rows/pack/project, no GL) with a headless ctest +
TSan-able target; cross-OS hash-diff CI job; `SHA-256SUMS` in release.yml.

### 3.6 P1 — Windows non-ASCII paths are broken across the whole stack

All file I/O uses ANSI `fopen` (`tp_project.c:1043,1329`, all exporters, stb PNG
writes); no UTF-8 manifest, no `_wfopen`/`MultiByteToWideChar` anywhere in our code
**[verified]**. tinyfiledialogs returns UTF-8. A project or sprites in a Cyrillic/CJK
folder fail to open/save/export on stock Windows. For a tool whose owner and a large
audience use non-ASCII paths, this is a first-contact breaker.

**Fix:** a single `tp_file_open(path, mode)` / `tp_write_file_atomic(path, buf, len)`
pair in core (UTF-8→UTF-16 + `_wfopen` on Windows; temp-then-rename for atomicity;
delete-on-short-write); route every writer, both parsers, and stb (via
`stbi_write_png_to_mem`) through it. Also fixes: truncated outputs on failed writes,
silent overwrite, and enables a duplicate-out_path collision check in
`tp_export_run`.

### 3.7 P1 — Engine coupling: the mirrored geometry decode has no handshake

`tp_pack_read` re-implements builder math: UV round-half-up, `transform_point`
("byte-for-byte mirror of nt_builder_atlas_geometry.c:983-1001",
`tp_pack_read_internal.h:15-19`), hull-inflation recovery, alias inference from
layout. Struct-version bumps fail loud; **same-version math changes fail silent**,
guarded only by fixtures. Polygon fixtures assert in ±4px tolerance mode, so a hull
change up to 4px slips through.

**Fix:** (a) upstream issue: expose the decode contract as a public engine header
(pairs with #285/#286 work); until then (b) pin an engine-format fingerprint (hash of
a known fixture's packed bytes) in a test so any silent math change breaks CI; (c)
covered by the fuzz packet. Also fix the stale `#282` comments in
`packer/CMakeLists.txt:5-7,25-28` that contradict `apps/gui/CMakeLists.txt:33`
(the GUI links `tp_build` now — which is exactly what *unblocks* moving the bridge
into core).

### 3.8 P1 — Persistence details that undermine "years" (all in `tp_project.c`)

- **Sparse save floats on engine defaults:** omitted knobs mean "whatever
  `nt_atlas_opts_defaults()` returns today" (`tp_project.c:214-215,851-904,1236`);
  an engine default bump silently changes every existing project's output. Pin the
  defaults snapshot (or a `defaults_version`) in the file.
- **`tp_migrate` cannot migrate:** it receives only the version int, never the parsed
  tree (`tp_project.c:1315-1326`) **[verified]** — the first real schema bump forces
  a load-path redesign under pressure. Pass `cJSON *root`, land a proven v1→v2 case.
- **The deterministic writer is physically duplicated:** `tp_project.c:641-731` vs
  `tp_sb.h` — byte-identical copies of the escaper/float formatter that can drift
  invisibly. Delete the private copy; include `tp_sb.h`.
- **God-file:** schema + path library + mutation API + serializer + loader + bridge
  in one 1519-line TU. Split into `tp_path.c` / `tp_project_model.c` /
  `tp_project_json.c` (the §3.3 schema table lands alongside).
- **Version policy tension:** hard-reject on `version > current` plus
  unknown-keys-ignored conflict — additive changes that bump the version lock out
  older builds that could read them. Decide a major/minor split.

### 3.9 P1 — Exporter layer: ecosystem mechanics missing

- **No per-target options mechanism** (`tp_project_target` = id/path/enabled only):
  no Defold `extrude_borders`, no pretty-print, no template variables; the
  `premultiply` parameter of `tp_export_write_pages` is dead — hard-coded `false` at
  both call sites **[verified]**. Name munging is per-atlas, shared by all targets —
  two targets needing different name policies cannot coexist
  (`tp_export_run.c:24-38`).
- **Missing caps axes** for the next 10 formats: y-axis origin, trim/crop modes,
  page-naming scheme (hard-coded `"%s-%d.png"`, `tp_export_png.c:35`), per-format
  name transforms. Append-only cap bits + a sparse per-target `options` map (the
  schema already tolerates unknown keys) is the low-debt path.
- **Registry lifetime:** `g_registered[32]` borrows pointers, no unregister — fine
  for builtins, insufficient for the Phase 7 template/discovery loader.
- Duplicated `is_rect_quad` + implicit "hull bbox min == (0,0)" dependency in both
  writers — hoist into a shared prepared-model helper.

### 3.10 P1 — GUI: no seams for a "set of tools" product

The decomposition is real at the compile layer (include discipline holds) but there is
no registration seam on any product axis:

- Adding a feature costs the same 5–7 hand-wired touch points every time: a
  `gui_state.h` extern, a `s_pending_*` flag in the ~130-line `apply_pending`
  if-ladder (`gui_actions.c:1040-1172`), an action fn, a hand-placed `declare_*` in
  `frame()`, a branch in `handle_canvas_input`, a hand-numbered selftest phase.
  Traced costs: canvas tool ≈ 5 files/6 sites; new panel ≈ 7 files/9 sites (worst:
  `compute_panel_widths` hardwires exactly two side panels); overlay ≈ 4–5 files.
- The advertised "views declare-only" rule is not the rule that exists: the settings
  view mutates the model inline ~30× (`gui_view_settings.c:300-828`) — defensible,
  but unstated and ungated; the enforced grep gate only proves actions don't draw.
- Frame ordering in `frame()` is load-bearing and implicit (clamp_selection →
  build_rows → slice9 sync → update_preview → handle_canvas_input); reorder and
  selection/overlays break silently.
- `gui_pack.c` lacks a single `job_dispose()` — the free discipline is copy-pasted
  across ~8 error/terminal paths; one missed field on a future job type leaks.
- Theme is copy-paste constants (scale, by contrast, is structural); ~20 color
  literals baked in `ensure_ids`; "add a light theme" touches ~5 files.
- Ids are ad-hoc string literals with no collision registry; selftest is a
  hand-numbered 16-phase god-driver; `preview_target_result()` branches on a view
  width tier (`gui_actions.c:845`) — a view concept inside the binding layer.

**Fix (packet D):** one explicit registry per axis — `gui_command` enum + handler
table (replaces the pending-bool ladder), an overlay-toggle table shared by View
menu/canvas/selftest, a `gui_tool` descriptor for canvas gestures; write down the
model-write rule and add the reverse grep gate (views call `gui_project_*` only);
name the frame pipeline and assert its invariants; `job_dispose()`; `theme_t` table;
selftest phase table. All fixed-size arrays of structs — engine-idiomatic.

### 3.11 P2 — Smaller structural items

- Silent fixed caps: `tp_relativize` drops path components past 256; page-name
  buffers truncate silently (`tp_pack_read.c:420,487`) — against fail-early.
- O(n²) spots: duplicate-name validation, alias grouping, per-add dedupe — fine at
  hundreds of sprites, flag at thousands.
- `tp_sprite` uses TexturePacker vocabulary (`spriteSourceSize`) inside the canonical
  model — cosmetic leakage.
- The Defold goldens are self-referential (no bob.jar in CI — known backlog); a
  change that breaks bob's importer while keeping our bytes stable ships green.
- Docs drift: AGENTS.md describes `apps/cli` in the present tense; ROADMAP Phase 6
  claims a CLI-parity success criterion that is untestable today; smoke app covers
  toolchain only (accurate to its claim, zero core coverage).

---

## 4. Enforceable boundary rules (proposed CI gates, like the zero-Clay gate)

1. No sprite-name normalization outside tp_core: `grep -rnE "strrchr\([^,]+, '\.'\)" apps/` → 0 (today: 3 hits — fails).
2. No exporter/target id string literals in frontends (registry or named constants only): `grep -rnE '"(json-neotolis|defold)"' apps/` (excl. selftest) → 0 (today: fails).
3. Frontends gather sprites only via the core API; no `tp_pack_sprite_desc` assembly in `apps/`.
4. `tp_normalize` rejects dangling animation frames (negative test pins it).
5. Public `tp_*.h` never names `nt_*` types: `grep -nE '\bnt_[a-z_]+_t\b' packer/include/tp_core/*.h` → 0 (today: passes — keep).
6. New project/pack-model capability in `apps/` requires a `packer/tests/` test first (review gate).
7. AGENTS.md may not describe `apps/cli` in the present tense while `test -d apps/cli` fails.

## 5. Parity inventory (what moves to core, what stays)

| Capability | Today | Verdict |
|---|---|---|
| Folder scan + image-ext policy | `gui_scan.c` | **→ core** (injected FS walker) |
| Sprite-desc assembly + override→engine encoding | `gui_pack.c:111-223` | **→ core** |
| Sprite-name/override-key normalization | 4 copies | **→ core, single fn** |
| Natural frame sort + anim id derivation | `gui_rows.c` | **→ core** |
| Anim frame identity validation | nowhere | **→ core (new)** |
| Degradation prediction text | `gui_pack.c:1036` | **→ core** (`tp_export_predict_loss`) |
| Default target seeding | `gui_project.c:115` | **→ core, registry-driven** |
| Pack-stale detection (memcmp) | `gui_pack.c` | stays GUI (interactive-only) |
| Undo/redo snapshots | `gui_history.c` | stays GUI (uses core save/load primitives) |
| Path relativization, caps clamp | core | already correct |

## 6. Sequenced consolidation plan (each packet = one verifiable landing)

- **A. Op-layer consolidation (before anything else, strictly before CLI):**
  `tp_sprite_export_key` + `tp_source_scan` + `tp_project_atlas_build_descs` +
  `tp_anim_frames_sorted` + anim-frame validation in `tp_normalize` +
  `tp_export_predict_loss` + registry-driven default target. GUI collapses to calls.
  Kills §3.1, §3.2, most of §5.
- **B. Schema single-source + persistence hardening:** X-macro knob/override table;
  split `tp_project.c`; delete the duplicated writer; real `tp_migrate(root)` with a
  proven case; pin defaults in the file. Kills §3.3, §3.8.
- **C. Degradation contract + notices sink:** centralized drop pass; notices persisted
  on job result; GUI notices panel (backlog C2) as the sink; per-target `options`
  map + missing caps axes; revive `premultiply`. Kills §3.4, most of §3.9.
- **D. GUI seams:** command table, overlay/tool registries, model-write rule + reverse
  gate, named frame pipeline, `job_dispose`, `theme_t`, selftest phase table.
  Kills §3.10.
- **E. Safety nets:** `scripts/check.sh` + CI lint; fuzz harnesses + malformed corpus +
  ASan/UBSan CI job; frozen goldens; headless `gui_logic` ctest (+ TSan-able);
  cross-OS determinism hash-diff job; release checksums. Kills §3.5.
- **F. Files/I-O:** `tp_file_open`/`tp_write_file_atomic` (UTF-16 on Windows, atomic,
  collision check); `setlocale` guard or locale-free float formatting + a fractional
  golden. Kills §3.6, locale risk in §3.5.

A and F are the user-facing/most-urgent pair; B–E can interleave with feature work.
Only after A lands should CLI Phase 4 start — it then becomes genuinely thin.

## 7. Strategic annex — distance to "best in the world" (2026-07 web-verified)

- **The lane is empty:** every OSS competitor is dormant or dead (free-tex-packer
  2021/domain parked, ShoeBox ~2016, crunch 2017, gdx-texture-packer-gui 2023/2024).
  TexturePacker 8.0.3 is the only maintained rival: ~45 exporters, 20 texture output
  formats, KTX2/Basis as its 8.0 flagship, $49.99 + paid CI subscription.
- **We already win on:** packing math (concave NFP + full D4 — unique), per-target
  capability packing (unique), determinism as a contract, explicit animations, free/MIT.
- **We lose on:** no CLI (the thing TP actually sells — pipelines/CI), 2 formats
  (neither Phaser/Pixi/Godot/Unity/libGDX/Spine — ~95% of the atlas-consuming world
  can't use our output), Defold identity-only (engine#285 nullifies the density edge
  for our one real target), crash on oversized sprite (engine#287), unsigned
  binaries, no package managers, no web presence.
- **Five 6-month bets (ranked):** 1) CLI + `--watch` + machine-readable JSON build
  report (unclaimed by any competitor); 2) lingua-franca formats via the template
  system (TP json-hash/array, libGDX/Spine .atlas, Godot) — reverses SUMMARY §6 Q3,
  which was right for Defold-first v1 and wrong for "best in the world";
  3) WASM browser playground (engine is wasm-first — uniquely cheap for us, proven
  funnel pattern, empty lane); 4) KTX2/Basis output (engine vendors basisu; the
  encoder is one flag from KTX2 — `nt_basisu_encoder.cpp:49`) + engine#285;
  5) trust/distribution (SignPath + notarization, itch.io, winget/brew, docs site
  with JSON Schema for json-neotolis, honest TP comparison page).
- **Explicit non-goals:** 45-exporter parity (top ~8 + templates + contributor guide
  covers >95%), HDR/EXR (no demand evidence), AI features (no user problem), content
  encryption, PSD/SWF deep import, per-sprite pixel formats, Steam-first, packing
  heuristic zoo.

---

## Appendix A — Lead verification log

Claims re-checked directly in code before accepting: `strip_ext` vs
`strip_ext_keep_folder` divergence on dot-basenames (gui_rows.c:58 / gui_pack.c:83);
`desc_add` +1 shape re-encoding and effective-shape rule (gui_pack.c:141-166);
`build_animations` copies frames verbatim with no sprite-set validation
(tp_normalize.c:89-134) and `commit_sprite_rename` does not remap frames; async
export frees notices unread (gui_pack.c:377,387); `tp_migrate(int, tp_error*)` has no
tree access (tp_project.c:1315); `premultiply` hard-coded false at both
`tp_export_write_pages` call sites; ANSI `fopen` everywhere with no UTF-8
manifest/`_wfopen` in packer or apps; project schema version + migration chain +
newer-version refusal exist (tp_project.c:205,942,1313-1388); `.ntpack` read-back
version handshakes exist (tp_pack_read.c:140,445,580); exporter registry = builtins
array + runtime `tp_exporter_register` (tp_export.c:109-197). Six audit reports
cross-agreed independently on §3.1 (three audits converged on the GUI-resident
bridge as the dominant debt).
