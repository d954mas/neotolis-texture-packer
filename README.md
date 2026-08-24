# Neotolis Texture Packer

A standalone native texture/atlas packer built on
[neotolis-engine](https://github.com/d954mas/neotolis-engine). One shared core
owns project semantics, packing, export, persistence, history, and recovery;
the native GUI and saved-file CLI are thin clients over those contracts.

## What works today

- strict canonical-v5 projects with multiple atlases;
- folder and individual image-file sources;
- deterministic Pack and Export through a contained builder worker;
- atlas and per-sprite packing settings;
- stable structural IDs and explicit animations;
- semantic revision, dirty state, Undo/Redo, durable Save, and best-effort GUI
  recovery;
- a native preview-oriented GUI;
- complete saved-file CLI editing, inspection, validation, Pack, Export, dry
  run, versioned JSON reports, and stable exit codes;
- full-fidelity `json-neotolis` export;
- target-neutral Export IR and one runtime format catalog shared by CLI and GUI;
- bundled Defold and Phaser exports with capability adaptation and structured
  loss notices.

The current product does not yet ship filesystem watchers, linked foreign-atlas
sources, Import IR, MCP, or a Dev API transport.

Start at [`docs/README.md`](docs/README.md) for current product, architecture,
format, and explicitly labeled target contracts.

## Download

Prebuilt binaries for Windows, Linux, and macOS are published on the
[Releases](https://github.com/d954mas/neotolis-texture-packer/releases) page.
Release archives contain `ntpacker-gui`, `ntpacker`, and the example catalog.

## Build

Requires CMake 3.25+, Ninja, Clang, and a recursive clone:

```bash
git clone --recursive https://github.com/d954mas/neotolis-texture-packer.git
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug
```

The release equivalents are:

```bash
cmake --preset native-release
cmake --build --preset native-release
ctest --preset native-release
```

The GUI executable is under `build/apps/gui/<preset>/`; the CLI is under
`build/apps/cli/<preset>/`. Run `ntpacker help` for the command catalog or read
the [CLI machine-report contract](docs/formats/cli-report.md).

## Examples

- `examples/showcase/` — a ready-made project over 60 CC0 animal sprites.
- `examples/defold-demo/` — Defold integration and export comparison.
- `examples/projects/` — deterministic user-openable test and performance
  projects.

## Repository layout

| Path | Ownership |
|---|---|
| `packer/` | shared `tp_core`/`tp_build`, project model, operations, session, packing, export, history, recovery, and tests |
| `apps/cli/` | file-oriented `ntpacker` frontend |
| `apps/gui/` | native `ntpacker-gui` frontend |
| `apps/common/` | shared application utilities |
| `apps/smoke/` | toolchain smoke test |
| `docs/` | current and target product contracts, architecture, and wire formats |
| `examples/` | executable and user-openable examples |
| `external/neotolis-engine/` | read-only engine submodule; fixes ship upstream |

Repository invariants and contributor workflow are in [`AGENTS.md`](AGENTS.md).

## Feedback and license

Please [open an issue](https://github.com/d954mas/neotolis-texture-packer/issues)
for incorrect output or missing behavior. Reproducible project and source
fixtures are especially useful.

The project is MIT licensed. Bundled third-party components retain their own
licenses.
