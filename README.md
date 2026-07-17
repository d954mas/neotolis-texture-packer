# Neotolis Texture Packer

A standalone texture/atlas packer built on [neotolis-engine](https://github.com/d954mas/neotolis-engine):
**one packing core, two equal frontends** (native GUI + CLI), pluggable export
formats. The packer is the engine's NFP/Minkowski **concave** vector packer —
it nests true silhouettes (not just rectangles), tries all 8 D4 orientations
(4 rotations × 2 flips), trims, deduplicates, and packs deterministically.

**Status: v0.2.0.** The native GUI and the `ntpacker` CLI both ship: headless
pack/export, project inspection/validation, and full project editing from the
command line sit alongside the GUI's packing, settings, and animations.
The shared core now owns typed operations, transactions, revisions, semantic
history and recovery. Project files use one strict canonical schema (v5); this
pre-release build intentionally does not load or migrate v1–v4 projects.
Undo/Redo acknowledgement uses compact durable history records, and project
Save uses synced sibling-temp atomic publication. Windows arguments and all
core filesystem paths cross one strict UTF-8 boundary.

[`docs/ntpacker-master-spec.md`](docs/ntpacker-master-spec.md) is the normative
product and architecture specification. [`docs/ROADMAP.md`](docs/ROADMAP.md)
is its derived execution tracker, and
[`docs/plans/master-spec-implementation-plan.md`](docs/plans/master-spec-implementation-plan.md)
is the task-level handoff for implementation.

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
- **CLI (`ntpacker`)** — headless `pack`/`export`, byte-identical to the
  GUI's export output (pinned by a CLI==core byte-parity ctest);
  `inspect`/`validate` for machine-readable project inspection; full project
  editing from the command line (`new`/`add`/`remove`/`set`, `sprite
  set`/`unset`, `anim create`/`add-frame`/`move-frame`/…, `target`/`atlas`
  add/remove/set) so a script or an AI agent can build and edit a project
  from nothing; `--json` on every verb with a stable, versioned per-verb
  schema; a frozen exit-code contract (0 ok … 6 partial success … 7 validate
  `--strict` findings); `--dry-run` reports what a pack would write and every
  predicted metadata loss without touching disk. This is the saved-file
  automation interface for humans and agents. Live editing of the same unsaved
  GUI session belongs to the planned Dev API/MCP surface (master spec Part II).

## Known limitations

- Packing already runs on a GUI worker, and the shared session owns mutation,
  semantic history and recovery. Canonical pack-input hashes, a multi-result
  memory LRU and the full stale/current preview behavior are still planned.
- The Defold target packs without rotations until the engine grows a
  rotation-only transform mode
  ([engine#285](https://github.com/d954mas/neotolis-engine/issues/285));
  9-slice is dropped for Defold with a notice.
- Duplicate frames are only merged when the source files are identical;
  identical-after-trim dedup is engine work in progress
  ([engine#286](https://github.com/d954mas/neotolis-engine/issues/286)).
- Known user-controlled builder preconditions (including an impossible sprite
  footprint) are rejected as structured errors. The engine builder is still
  in-process, so allocation, codec/output I/O, and an unknown engine assertion
  can terminate GUI or CLI until the private builder-worker boundary in H0.3-H0.5
  is complete ([engine#287](https://github.com/d954mas/neotolis-engine/issues/287)).
- Polygon hulls on anti-aliased edges can be noisy — raise the alpha
  threshold or lower max vertices
  ([engine#288](https://github.com/d954mas/neotolis-engine/issues/288)).
- The window's X button bypasses the unsaved-changes prompt (engine gap);
  window size / recent projects are not remembered yet.
- The application-owned project/image paths use the strict UTF-8/Win32-wide
  boundary, but the engine's native resource loader still opens the bundled GUI
  pack through a narrow path. A Unicode or extended-length installation
  directory can therefore prevent UI resource loading until the engine public
  filesystem API is fixed upstream.
- CLI: no `--verbose` and no machine-readable progress stream yet (needs an
  engine log-writer opt-out so live progress can coexist cleanly with
  `--json`); `--json` payloads themselves are complete — only in-flight
  progress is stderr-only text for now.

## Next development sequence

1. Complete H0.3-H0.5: isolate the engine builder in a bounded private process,
   keep UTF-8/publication in the parent, and pin crash/cancel/full-disk parity.
2. Native Neotolis atlas import, inspection, materialization, and
   `atlas detect`/`inspect`/`extract`.
3. Linked read-only atlas sources and transactional Extract Sprites.
4. Session/Pack hashes and stale/current behavior, format packages, sandboxed
   Lua/templates, then live
   Dev API/MCP collaboration.

Watchers update runtime source state and mark previews stale; they do **not**
auto-pack in the current target design. See the roadmap for dependencies and
acceptance gates.

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

### Running the CLI

The exe lands at `build/apps/cli/<preset>/ntpacker(.exe)`. One-liner:

```bash
ntpacker pack examples/showcase/showcase.ntpacker_project --dry-run --json
```

Run `ntpacker help` for the full verb/flag list, or see
`docs/formats/cli-report.md` for the machine payload schemas.

## Repository layout

| Path | What |
|---|---|
| `packer/` | `tp_core` (canonical project I/O, operations/session, semantic history/recovery, validation, normalization and exporters) + `tp_build` (drives the engine builder) + tests |
| `apps/cli/` | `ntpacker` — headless CLI frontend over `tp_core` |
| `apps/common/` | shared version header (`ntpacker_version.h`) used by both frontends |
| `apps/gui/` | `ntpacker-gui` (engine `nt_ui` frontend) |
| `apps/smoke/` | toolchain smoke test |
| `examples/defold-demo/` | Defold project with real assets (from [extension-texturepacker](https://github.com/defold/extension-texturepacker), MIT) for the three-way atlas comparison: Defold native / TexturePacker / ntpacker |
| `examples/showcase/` | ready-made project over 60 CC0 animal sprites ([Kenney](https://kenney.nl)) — open it and press Pack |
| `docs/` | master specification, derived roadmap, implementation plan, format contracts, and research history |
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
