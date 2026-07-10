# Roadmap — Standalone Texture Packer toward v1

Draft, recommendation-first. Decisions and rationale live in
`docs/research/SUMMARY.md`; this file is the phased build plan. Each phase is
small and independently shippable. "Parallel" tags mark phases with no ordering
dependency once their prerequisites are met.

Guiding invariants (AGENTS.md): capability lands in `tp_core` first, CLI + GUI
are thin over it; exporters are data-driven plugins over one canonical placement
model; the engine tree is read-only — every engine change ships as an upstream
issue + PR.

---

## Dependency order (critical path in bold)

```
Phase 0 (done)
   │
   ├─► Phase 1a  .ntpack parse-back reader ──┐
   │                                         ▼
   └─────────────────────────────►  Phase 1b  tp_core skeleton + canonical model
                                          │
                        ┌─────────────────┼─────────────────┐
                        ▼                 ▼                 ▼
                   Phase 2  PNG      Phase 3  project   Phase 7  mustache
                   + JSON exporters  file [parallel      exporters  [parallel]
                        │            with 2; feeds 4/6]
                        └────────┬────────┘
                                 ▼
                            Phase 4  CLI
                                 │
                        ┌────────┴────────────────┐
                        ▼                          ▼
                  Phase 5  Defold+demo      Phase 8  watch/incremental
                  [parallel]                [parallel after 4]

              Phase 6  GUI MVP  (fed by 2 + 3 directly, NOT 4)  [parallel with 5/7]
                        │
                        └───────────────► Phase 9  variants + secondaries + release polish
                                          (needs 2, 4, 6 +7 if built)
```

**Parallelizable once prerequisites land:** 5, 6, 7 (all after 2, with 4/3 as noted); 8 after 4.
The single serial bottleneck is **1a → 1b → 2** — all in-repo, nothing waits on upstream engine
review (owner decision, `SUMMARY.md §5g`).

---

## Phase 0 — Environment & CI (DONE)

Already in the repo. Recorded here as the baseline.

- Engine submodule wired (`external/neotolis-engine`), `nt_builder` pulled in
  natively (`CMakeLists.txt`), WASM builds hard-errored out.
- CMake presets `native-debug` / `native-release` (Ninja + Clang, C17).
- CI `ci.yml`: build + `ctest` on Linux/Windows/macOS, ccache, LFS pull.
- `release.yml`: tag-driven (`v*`) 3-OS archive attach to a GitHub Release.
- `apps/smoke` — packs generated RGBA discs through `nt_builder` into an
  `.ntpack` with debug PNGs; proves toolchain + submodule + builder pipeline.

---

## Phase 1a — `.ntpack` parse-back reader (`tp_pack_read`)  ★ critical path

**Goal:** `tp_core` reads placements, geometry and page pixels back from a
freshly built `.ntpack` — the engine stays untouched (`SUMMARY.md §5g`).

**Deliverables**
- `tp_pack_read` module: parse the pack container
  (`shared/include/nt_pack_format.h`), the atlas blob (`nt_atlas_format.h` v6)
  and texture-asset mip0 → pages (dims + straight-alpha RGBA8) and regions
  (frame rect from u16 UVs, D4 transform, trim, pivot, slice9, polygon
  verts/indices, aliases).
- Name reverse-map: engine `nt_hash64_str(raw sprite name)` → name string (the
  atlas blob hashes the RAW name, not the normalized resource id — see
  `docs/plans/phase-1.md §2`; tool knows all inputs; collision = error).
- Export-friendly pack settings profile used for export/preview passes:
  `premultiplied=false`, `compress=NULL`, `gen_mipmaps=false`.

**Acceptance**
- Golden round-trip test: pack a fixture set covering rotated, flipped,
  trimmed, polygon, slice9, alias and multi-page sprites → parse back →
  frame rects, transforms, trim, pivots, slice9 and polygons match the
  expected placement exactly.
- UV→pixel rect recovery is exact for all page sizes up to 4096 (property
  test over sizes and offsets).
- Zero modifications under `external/neotolis-engine`.

**Notes:** replaces the previously planned engine export-callback PR (kept in
`SUMMARY.md §5g` as an optional later optimization once profiling justifies
skipping the serialize→parse round-trip). No upstream dependency remains.

---

## Phase 1b — `tp_core` skeleton + canonical model

**Goal:** a `packer/` static lib that runs a pack and returns the canonical `tp_result`.

**Deliverables**
- `packer/` (`tp_core`) target: `add_subdirectory(packer)` in root CMake; C17,
  engine warning flags (`nt_set_warning_flags`).
- Canonical `tp_result` / `tp_sprite` / `tp_page` model (`SUMMARY.md §5d`).
- `tp_pack(settings*, arena*) -> tp_result*`: takes a MINIMAL settings struct
  (the full `.ntpacker_project` loader is Phase 3, not a 1b dependency); drives
  `nt_builder` begin/add/end, writes the session `.ntpack`, parses it back via
  `tp_pack_read` into an owned `tp_result` (sorted by name). `arena` is a
  `tp_core`-local bump allocator (`tp_arena` — the engine exposes none).
- No I/O opinions in core: it returns data; frontends write files.

**Acceptance**
- Unit test packs the smoke sprite set, asserts every `tp_result` invariant
  (`SUMMARY.md §5d`), stable sprite ordering, correct `alias_of` on a duplicate.
- Runs green in `ctest` on all 3 OSes.

**Depends on:** 1a.

---

## Phase 2 — PNG page export + JSON exporters

**Goal:** produce real files from a pack: page PNGs + the ecosystem JSON formats.

**Deliverables**
- PNG page writer (vendored `stb_image_write` / `fpng`), straight-alpha default
  with an optional premultiply toggle.
- Hardcoded C exporter over the canonical model: **`json-neotolis`** (own
  full-fidelity schema: D4 transform mask, polygons, pivot, slice9, pages —
  the reference target that supports everything). Schema documented in
  `docs/formats/json-neotolis.md` as part of this phase.
  (TexturePacker-compatible `json-hash`/`json-array` dropped from scope —
  `SUMMARY.md §6 Q3`; possible later as Phase 7 templates.)
- Exporter **registry** + capability-flag struct (even for hardcoded ones) so
  Phase 7 templates and the GUI/CLI reuse one interface.
- Test-only capability-restricted exporter descriptor (e.g. rot90-only,
  no-flip) registered in the test suite — proves per-target packing (§5h)
  without waiting for Phase 5's Defold target.
- Per-target packing (`SUMMARY.md §5h`): each export target packs with
  `project settings ∩ target capabilities` (unsupported features silently not
  used for that target); targets with identical effective settings share one
  pack run. Informational notices only for genuine metadata loss (dropped
  pivot/polygon/slice9); hard error only for impossibilities.
- Alias semantics: exporters emit EVERY aliased name as its own entry pointing
  at the shared frame (json-neotolis lists all names; matches .tpinfo alias
  behavior).
- Core normalization pass (`prepareData` equivalent): trim-`crop` rewrite, name
  munging (ext strip / folder prefix), scale multiply, numeric-suffix
  animation auto-grouping (feeds Phase 5 `.tpatlas`; explicit `animations[]`
  from the Phase 3 project schema overrides) — done before any exporter.
- Determinism: canonical sprite sort key is the FINAL munged export name (not
  the raw builder name).

**Acceptance**
- Golden-file test: exported `json-neotolis` matches its documented schema for
  a fixture set covering rotated, flipped, trimmed, polygon, slice9, alias and
  multi-page sprites (full fidelity — nothing dropped).
- Per-target packing proven: same project exported to `json-neotolis` (full D4)
  and to a rotation-less target produces different, correct layouts.
- Exported PNG decodes to expected page dims; a parse-back reconstructs frames.
- Byte-identical output on re-run (determinism).

**Depends on:** 1b.

**Non-goals (out of v1 scope):**
- Common-divisor / align-to-grid / `multipleOf4` size constraint.
- Alpha-bleed / dilation post-process (straight-alpha PNG halos under linear
  filtering — revisit if reported).
- Pack-effort knob (Fast/Good/Best).

---

## Phase 3 — Project file (`tp_project` load/save)

**Goal:** persist all settings + inputs in a versioned, portable JSON project.

**Deliverables**
- Schema v1 per `SUMMARY.md §5a` (cJSON at `deps/cjson`): `version` integer,
  `app` info, `sources` (folders/files/ignore), `atlases[]`, sparse `sprites`
  overrides. Relative paths, stable identifiers, sorted deterministic output.
- Per-atlas `animations[]` schema per `SUMMARY.md §5a` (id, ordered frames,
  playback, fps, flip_h/v).
- Load with newer-version refusal + a migration hook (`v1→…`) even if empty.
- Folder rescan on load; absolute-path-on-load → relativize-on-save.

**Acceptance**
- save → load → save is byte-identical; relative paths resolve from the project dir.
- Newer schema version is refused with a clear message.
- A folder source picks up a newly added file on reload.

**Depends on:** 1b (model). Parallel with 2.

---

## Phase 4 — CLI (`apps/cli` over `tp_core`)

**Goal:** headless, CI-friendly `ntpacker pack game.ntpacker_project`.

**Deliverables**
- Arg parser; `pack <project>` loads project → `tp_pack` → writes exporter files.
- Flags mirror project fields with **CLI-overrides-project** semantics; `--save`
  rewrites the project (TP pattern). `--force`, `--quiet`, `--version`.
- Exit codes: 0 success / non-zero on failure (missing sprites, doesn't fit,
  bad exporter). Errors to stderr.

**Acceptance**
- CLI packs the smoke project headless and writes PNG + JSON; exit-code contract holds.
- A CLI flag override changes output vs the project's stored value.
- CLI output byte-matches the `tp_core` library test for the same input.

**Depends on:** 2, 3.

---

## Phase 5 — Defold exporter + in-repo demo (bob.jar in CI)  [parallel]

**Goal:** prove a `.tpinfo` from our packer renders in a stock Defold build.

**Deliverables**
- Hardcoded `.tpinfo` exporter (protobuf-text) per `defold.md §(b)` checklist
  (rotation corner rule, quad fallback, pivot, vertices, all `required` fields);
  optional `.tpatlas` starter; `.atlas`+loose-PNG **fallback** preset.
- `.tpatlas` generation from the project's `animations[]` + auto-grouped
  animations (`SUMMARY.md §5a`, Phase 2 normalization).
- `examples/defold-demo/`: `game.project` (dependency pinned to a matching
  extension tag), generated `.tpinfo` + page PNG, `.tpatlas`, one collection
  with a sprite (incl. a rotated + trimmed sprite and one flipbook animation).
- CI job: install JDK, download `bob.jar` (version-matched), `bob resolve` +
  `bob build --variant headless`; assert green.
- Version pinning: demo + CI pin the newest extension-texturepacker release
  and the MATCHING bob.jar/Defold version (lock-step requirement); floating
  "latest" is not used in CI — bumping the pin is a deliberate small PR.

**Acceptance**
- CI bob build is green (parse + compile prove `.tpinfo` correctness).
- Our `.tpinfo` diffs cleanly against the extension's `examples/rotate` +
  `examples/anim_trim` field conventions.
- Rotated/trimmed/animated sprites all present in the demo.

**Depends on:** 2 (page pixels + model), 4 (invoke path). Parallel with 6, 7.

---

## Phase 6 — GUI MVP on `nt_ui`  [parallel]

**Goal:** a usable single-window packer GUI; thin over `tp_core`.

**Deliverables**
- `apps/gui`: GLFW+GL window via `nt_app_run`; toolbar / central canvas / two
  toggleable panels layout (reference `external/neotolis-engine/examples/ui_showcase/main.c`).
- Open/save project; add folders/files via OS-native dialog (vendor
  tinyfiledialogs or Win32/GTK/Cocoa — none is in the engine deps yet).
- Live page preview (source & page images via `stb_image` → `nt_gfx` texture);
  settings panel 1:1 with project fields; **Pack** runs `tp_core` on a worker
  thread; region-outline overlay toggle.

**Acceptance**
- Manual smoke: open tool → add folder → Pack → see page preview with region
  outlines → save project.
- The saved project, packed via the CLI, produces byte-identical output (parity).

**Depends on:** 2, 3. Parallel with 5, 7.

---

## Phase 7 — Mustache template exporters + descriptors  [parallel] [post-v1 stretch]

**Goal:** data-driven long-tail formats without recompiling.

**Deliverables**
- Small embedded mustache engine + FTP formatter extension
  (`add/subtract/multiply/divide/offsetLeft/offsetRight/mirror/escapeName`).
- JSON exporter descriptors (id, ext, template, capability flags); flags gate
  GUI/CLI controls and **constrain the pack** (drive the D4→rotation/identity
  restriction from `SUMMARY.md §4`).
- Built-in templates: libGDX `.atlas` (new + legacy dialects), Godot, Phaser3, CSS.
- User-exporter discovery: config dir + project-relative `exporters/`.
- *(Optional, if owner approves `SUMMARY.md §6 Q4)`:* second engine PR adding a
  transform-**policy** enum so rotation-capable foreign formats can use 90°.

**Acceptance**
- Golden-file libGDX `.atlas` matches the `spine-libgdx.md` spec in both dialects
  (top-left `bounds` vs bottom-based `offsets`, rotate/split/pad rules).
- A `supportsRotation:false` descriptor greys out rotation and forces identity packing.
- A user-supplied `.mst` + descriptor renders through both CLI and GUI.

**Depends on:** 2. Parallel with 5, 6.

---

## Phase 8 — Watch mode + incremental  [parallel after 4]

**Goal:** cheap repack; make the packer safe to wire into every build.

**Deliverables**
- Content-hash sidecar (inputs + effective options + template text + tool
  version); no-op when unchanged, `--force` override. Embed the key in JSON
  `meta.smartupdate` too.
- FS watcher (`ReadDirectoryChangesW` / inotify / kqueue), debounce + coalesce,
  feeding the same async pack job; `--watch` in CLI, auto-repack in GUI.
- Decoded+trimmed source-image cache keyed by file hash.

**Acceptance**
- Re-pack with unchanged inputs prints "up to date" and exits 0 without work.
- Touching one source file triggers exactly one repack; add/remove/rename detected.
- Watch loop stable over a burst of edits (no duplicate/dropped repacks).

**Depends on:** 4. Parallel with 5/6/7.

---

## Phase 9 — Scaling variants + secondary channels + release polish

**Goal:** the two "single placement, N outputs" features + a shippable v1.

**Deliverables**
- Scaling-variant cook (`{v}`, per-variant max size, identical-layout from one
  placement result); multipack `{n}` output-name placeholders with validation.
- N named secondary channels (normal/emission/AO) rendered from the same
  placement, per-channel output settings (color space, compression).
- Round-trip **unpacker** (also the exporter test oracle).
- Docs + per-format loader snippets; release archives (GUI + CLI, 3 OSes) via
  `release.yml`; first `v0.x` tag.

**Acceptance**
- Variant and secondary outputs share layout byte-for-byte; only scale/pixels differ.
- `{n}`/`{v}` presence validated; missing placeholder errors early.
- `release.yml` produces all-3-OS archives containing both binaries; unpacker
  round-trips a packed atlas back to per-sprite images.

**Depends on:** 2, 4, 6 (+7 if built) (for GUI/exporter surfaces). Final integration phase.

---

## Sequencing summary

1. **1a (.ntpack parse-back reader)** — in-repo, no upstream dependency.
2. **1b → 2** — core + real outputs (serial spine).
3. **3** — project file (parallel with 2).
4. **4** — CLI (needs 2+3).
5. **5 / 6 / 7** — Defold+demo, GUI MVP, template exporters — run in parallel.
6. **8** — watch/incremental (parallel after 4).
7. **9** — variants, secondaries, release polish — closes v1.
