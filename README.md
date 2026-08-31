# Neotolis Texture Packer

A standalone native texture/atlas packer built on
[neotolis-engine](https://github.com/d954mas/neotolis-engine). One shared core
owns project semantics, packing, export, persistence, history, and recovery;
the native GUI, saved-file CLI, and agent mode are thin clients over those
contracts.

## What works today

- strict canonical-v5 projects with multiple atlases;
- folder and individual image-file sources;
- deterministic Pack and Export through a contained builder worker;
- atlas and per-sprite packing settings;
- stable structural IDs and explicit animations;
- semantic revision, dirty state, Undo/Redo, durable Save, and best-effort
  recovery;
- a native preview-oriented GUI;
- complete saved-file CLI editing, inspection, validation, Pack, Export, dry
  run, versioned JSON reports, and stable exit codes;
- initial agent mode for new headless sessions, with structured discovery,
  snapshots, transactions, history, Undo/Redo, and recovery preservation;
- full-fidelity `json-neotolis` export;
- target-neutral Export IR and one runtime format catalog shared by CLI and GUI;
- bundled Defold and Phaser exports with capability adaptation and structured
  loss notices.

The current product does not yet provide filesystem watchers, linked foreign-atlas
sources, Import IR, or a Dev API transport. Agent mode cannot yet open saved
projects, Save, Pack, Export, or attach to GUI sessions.

Start at [`docs/README.md`](docs/README.md) for current product, architecture,
format, and explicitly labeled target contracts.

## Download

Prebuilt binaries for Windows, Linux, and macOS are published on the
[Releases](https://github.com/d954mas/neotolis-texture-packer/releases) page.
Release archives contain `ntpacker-gui`, `ntpacker`, and the example catalog.

## Build

Requires CMake 3.25+, Ninja, Clang, Python 3.8+ for process-contract tests, and a
recursive clone:

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

## Agent mode

Use ordinary CLI commands for saved-file workflows. To create a live headless
session, start `ntpacker agent --new` and exchange JSON lines through stdin/stdout.
`ntpacker agent --help` describes the supported commands and their schemas; the
running process also accepts `help` and `capabilities` requests. No SDK or special
agent integration is required. See the [agent wire contract](docs/formats/agent-v1.md)
and [approved design](docs/spec/agent-mode-v1.md).

This first packet supports new unsaved sessions only. EOF closes the session
without Save; dirty edits rely on best-effort recovery journals. Degraded
recovery is reported and may lose unsaved edits on exit. Recovery candidates
are currently resolved through the GUI; terminal recovery commands remain
future work.

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
| `apps/agent/` | agent protocol and headless session host |
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
