# Neotolis Texture Packer

A standalone texture/atlas packer built on [neotolis-engine](https://github.com/d954mas/neotolis-engine):
**one packing core, two equal frontends** (native GUI + CLI), pluggable export
formats. The packer is the engine's NFP/Minkowski **concave** vector packer —
it nests true silhouettes (not just rectangles), tries all 8 D4 orientations
(4 rotations × 2 flips), trims, deduplicates, and packs deterministically.

**Status: first test release (v0.1.0).** The GUI is a working atlas tool:
live packing, settings, animations, and two export formats. The CLI (Phase 4)
is next. See `docs/ROADMAP.md` for the full plan.

## Download

Prebuilt binaries for Windows / Linux / macOS:
[**Releases**](https://github.com/d954mas/neotolis-texture-packer/releases).
Unzip and run `ntpacker-gui`. Try it on
`examples/showcase/showcase.ntpacker_project` (60 CC0 animal sprites) or the
Defold comparison demo below.

## What works today

- **GUI (`ntpacker-gui`)** — create/open/save `.ntpacker_project` files; add
  image files and live-linked folders; multiple atlases per project; rename
  atlases/regions (inline, file on disk untouched); undo/redo (Ctrl+Z/Y);
  refresh sources (F5) with add/remove/change detection; missing files are
  flagged, never fatal; per-monitor DPI scaling; in-process (threaded)
  packing with the packed atlas on a zoom/pan canvas — hull/frame/trim/pivot
  overlays, per-region vertex readout, pack stats.
- **Settings panel** — all packing knobs (shape, page size up to 16384,
  padding, alpha threshold, rotations, POT…) plus **per-region overrides**
  (shape/rotation/max-vertices/margin/extrude, inherit by default); invalid
  combinations are disabled with a reason, never a crash.
- **Animations** — multi-select sprites → "Create animation from selection";
  frame reorder, fps, the full Defold playback set, flips; preview player
  with scrubber. Explicit assembly only — no name-based auto-detection, by
  design (`docs/research/animation-grouping.md`).
- **Export dialog** (Ctrl+E) — per-target enable + output path, then export.
- **Export: `defold`** — `.tpinfo`/`.tpatlas` for
  [extension-texturepacker](https://github.com/defold/extension-texturepacker)
  (pinned 2.7.0), including animations; see `docs/formats/defold-tpinfo.md`.
- **Export: `json-neotolis`** — full-fidelity JSON (D4 transforms, polygon
  hulls, pivots, slice-9, aliases, multi-page, animations) + page PNGs.
  Schema: `docs/formats/json-neotolis.md`. Deterministic byte-identical output.
- **Per-target capability packing** — each export target repacks with
  `project settings ∩ target capabilities`; features a format can't hold are
  simply not used for that target (notices for genuine metadata loss, never
  blocking errors).
- **`.ntpack`** — the engine's native runtime pack is always produced (it is
  also the GUI preview artifact: what you see is literally the atlas that
  ships).

## Known limitations (v0.1.0)

- No CLI yet (next phase) — exports run from the GUI.
- Packing runs synchronously: the window can stall for a few seconds on very
  large atlases (worker-thread + progress is planned).
- The Defold target packs without rotations until the engine grows a
  rotation-only transform mode
  ([engine#285](https://github.com/d954mas/neotolis-engine/issues/285));
  9-slice is dropped for Defold with a notice.
- Duplicate frames are only merged when the source files are identical;
  identical-after-trim dedup is engine work in progress
  ([engine#286](https://github.com/d954mas/neotolis-engine/issues/286)).
- A sprite that cannot fit the page size aborts the app
  ([engine#287](https://github.com/d954mas/neotolis-engine/issues/287)) —
  keep `max page size` comfortably larger than your biggest sprite.
- Polygon hulls on anti-aliased edges can be noisy — raise the alpha
  threshold or lower max vertices
  ([engine#288](https://github.com/d954mas/neotolis-engine/issues/288)).
- The window's X button bypasses the unsaved-changes prompt (engine gap);
  window size / recent projects are not remembered yet.

## Planned (roadmap order)

- `ntpacker` CLI: `ntpacker pack project.ntpacker_project` — byte-identical
  to GUI export (tool-parity invariant).
- Defold demo built headless by bob in CI; native `.atlas` export preset.
- Async packing with progress; notices panel; list search/filter.
- Watch mode (auto-repack on source changes).

## Build

Requires CMake 3.25+, Ninja, Clang. Clone with submodules:

```bash
git clone --recursive https://github.com/d954mas/neotolis-texture-packer.git
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug
```

Release preset: `native-release`. CI builds and tests Linux/Windows/macOS on
every push; tagged `v*` releases produce binaries for all three platforms.

### Running the GUI

The exe lands at `build/apps/gui/<preset>/ntpacker-gui.exe`. In VS Code just
press **F5** (`.vscode/launch.json` is checked in). Open
`examples/defold-demo/defold-demo.ntpacker_project` for a ready-made project
over real assets.

## Repository layout

| Path | What |
|---|---|
| `packer/` | `tp_core` (model, project IO, `.ntpack` parse-back, normalization, exporters) + `tp_build` (drives the engine builder) + tests |
| `apps/gui/` | `ntpacker-gui` (engine `nt_ui` frontend) |
| `apps/smoke/` | toolchain smoke test |
| `examples/defold-demo/` | Defold project with real assets (from [extension-texturepacker](https://github.com/defold/extension-texturepacker), MIT) for the three-way atlas comparison: Defold native / TexturePacker / ntpacker |
| `docs/` | roadmap, UX design, format specs, competitor research |
| `external/neotolis-engine` | the engine (submodule, read-only here — changes go upstream) |

`AGENTS.md` documents the repository invariants and the agent workflow used to
develop this project.

## Feedback & issues

Found a bug, a wrong export, or missing behavior? Please
[open an issue](https://github.com/d954mas/neotolis-texture-packer/issues) —
I read every report and fix them. Attach the `.ntpacker_project` (and a couple
of source images if the problem is packing/export related): projects are
small, portable, and make reports instantly reproducible.

## License

MIT. Bundled third-party bits keep their own licenses
(`apps/gui/deps/tinyfiledialogs` — zlib; `examples/defold-demo/UPSTREAM-LICENSE` — MIT).
