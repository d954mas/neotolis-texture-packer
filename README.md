# Neotolis Texture Packer

A standalone texture/atlas packer built on [neotolis-engine](https://github.com/d954mas/neotolis-engine):
**one packing core, two equal frontends** (native GUI + CLI), pluggable export
formats. The packer is the engine's NFP/Minkowski **concave** vector packer —
it nests true silhouettes (not just rectangles), tries all 8 D4 orientations
(4 rotations × 2 flips), trims, deduplicates, and packs deterministically.

**Status: in active development.** The GUI is a working project editor with
live packing; the CLI (Phase 4) and the Defold exporter (Phase 5) are next.
See `docs/ROADMAP.md` for the full plan and current phase.

## What works today

- **GUI (`ntpacker-gui`)** — create/open/save `.ntpacker_project` files; add
  image files and live-linked folders; multiple atlases per project; rename
  atlases/regions (inline, file on disk untouched); undo/redo (Ctrl+Z/Y);
  refresh sources (F5) with add/remove/change detection; missing files are
  flagged, never fatal; per-monitor DPI scaling; in-process packing with the
  packed atlas rendered on a zoom/pan canvas.
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

## Planned (roadmap order)

- `ntpacker` CLI: `ntpacker pack project.ntpacker_project` — byte-identical
  to GUI export (tool-parity invariant).
- Defold exporter: `.tpinfo`/`.tpatlas` for
  [extension-texturepacker](https://github.com/defold/extension-texturepacker)
  + native `.atlas` fallback; in-repo demo project built headless by bob in CI
  (`examples/defold-demo/` already carries the comparison assets).
- Animation editor + preview player (explicit assembly, Defold-style; no
  name-based auto-detection by design — see `docs/research/animation-grouping.md`).
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

## License

MIT. Bundled third-party bits keep their own licenses
(`apps/gui/deps/tinyfiledialogs` — zlib; `examples/defold-demo/UPSTREAM-LICENSE` — MIT).
