# AGENTS

## Repository Role

Standalone texture/atlas packer built on neotolis-engine: one packing core,
two equal frontends — a CLI and a native GUI app (engine `nt_ui`). Exports to
multiple engine formats (Defold, generic JSON + PNG, own `.ntpack`, ...) via a
pluggable exporter layer. A project file format persists all settings.

## Communication

Minimize optional commentary. During execution, send only required notices,
blocking questions, meaningful progress updates, risk/verification notes, and
the final result.

## Agent Roles

The lead agent is the orchestrator: it decomposes work into bounded task
packets, delegates, and verifies results. Claude agent roles live in
`.claude/agents/*.md` (`deep-reasoner` for architecture/research/debugging,
`fast-worker` for mechanical execution). Non-trivial, context-heavy, or
research-heavy work must be delegated to the closest existing role; the lead
executes directly only small local tasks where delegation costs more than the
work itself. Create a new role only when the catalog has no fitting role.

## Structure

- `external/neotolis-engine/` — engine submodule. **Read-only for agents.**
- `packer/` — the packing/export core library (`tp_core`): project model,
  pack orchestration, exporters. No UI, no CLI parsing here.
- `apps/cli/` — CLI frontend. `apps/gui/` — GUI frontend. Thin clients over
  `tp_core`; any capability must be reachable from both.
- `apps/smoke/` — minimal environment smoke test (packs a generated sprite
  set through `nt_builder`).
- `docs/research/` — competitor/format research. `docs/` — specs, decisions.
- `examples/defold-demo/` — Defold project proving exported atlases work.

## Hard Invariants

- Engine boundary: use `external/neotolis-engine` public APIs before custom
  code. The engine working tree is read-only. Suspected engine problem: first
  establish where the ROOT CAUSE lives; if it is in the engine, convince the
  lead the fix is needed — every engine change ships only through an issue and
  PR in the engine repo, never a direct edit.
- Tool parity: one op layer (`tp_core`), two equal clients (CLI and GUI).
  Features land in the core first; frontends only wire them up.
- C17, engine warning flags (`nt_set_warning_flags`) on all our targets.
- Exporters are data-driven plugins over one canonical placement model; adding
  a format must not require touching pack orchestration.

## Build

Native only (the tool is a desktop app). Requires CMake 3.25+, Ninja, Clang.

```bash
cmake --preset native-debug
cmake --build --preset native-debug
```

Outputs land in `build/<area>/<target>/<preset>/`.
