# AGENTS

## Repository Role

Standalone texture/atlas packer built on neotolis-engine. The current baseline
has one shared packing/export core and two shipped clients: the CLI and native
GUI app (engine `nt_ui`). The target architecture adds one typed
operation/session layer for capability-equivalent GUI, CLI, MCP, and Dev API
clients, plus stable structural IDs, tagged sources, a unified format registry,
and canonical Import/Export IRs.

`docs/ntpacker-master-spec.md` is the normative product and architecture source.
`docs/ROADMAP.md` and `docs/plans/master-spec-implementation-plan.md` are
derived execution documents and must not introduce decisions that contradict it.
Older plans, reviews, and research are non-normative history unless the master
spec explicitly retains them as an executable format or schema contract.

The tool is operated by humans AND machines/AI agents as equal first-class
users, designed so from the core up (consolidated in the master spec): every
capability has an appropriate headless contract. CLI output is machine-readable;
errors and notices are structured data; bad input yields a graceful structured
error — never a crash.

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
- `packer/` — the current shared project/packing/export core
  (`tp_core`/`tp_build`). Target operation/session, import, source-runtime,
  format-registry, and canonical-IR modules also belong here as they land. No
  UI, transport, or CLI parsing here.
- `apps/cli/` — `ntpacker` file-oriented CLI frontend. `apps/gui/` — native
  GUI frontend. Live headless sessions belong to `ntpacker mcp`, not ordinary
  CLI. Each client stays thin over the common core contracts.
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
- Tool parity: one typed operation/session layer, capability-equivalent clients.
  GUI, CLI, MCP, and Dev API may expose different interface shapes, but mutation,
  validation, naming, capability, transaction, and Undo rules live below them.
- Human + AI operators: every capability has an appropriate headless contract
  (ordinary CLI for saved-file workflows; MCP/Dev API for live sessions).
  Every CLI command supports `--json` with a stable versioned schema, exit
  codes are a contract, errors/notices are structured data (id + context, not
  only prose), and destructive/lossy outcomes are predictable via dry-run.
  Invalid user input must produce a structured error, never an abort/crash.
- C17, engine warning flags (`nt_set_warning_flags`) on all our targets.
- Target source model: sources are tagged records. Path files/folders and linked
  atlases share one runtime source/status boundary; external refresh never
  mutates project revision, dirty state, or Undo history and never auto-starts
  Pack.
- Target format model: built-in, template, and sandboxed Lua formats share one
  package descriptor, exact capability vocabulary, and versioned Import/Export
  IRs. Adding a format must not require touching pack orchestration.

## Build

Native only (the tool is a desktop app). Requires CMake 3.25+, Ninja, Clang.

```bash
cmake --preset native-debug
cmake --build --preset native-debug
```

Outputs land in `build/<area>/<target>/<preset>/`. Tests run via
`ctest --preset native-release` (or `native-debug`).

## CI / Releases

GitHub Actions (`.github/workflows/`): `ci.yml` builds and tests on
Linux/Windows/macOS for every push/PR; `release.yml` is tag-driven — pushing a
semver tag (`v0.1.0`) builds all three platforms and attaches archives to a
GitHub Release. The engine's own CI covers Linux native only, so macOS
breakage is possible and belongs upstream (issue/PR to the engine repo).
