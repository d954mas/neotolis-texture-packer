# AGENTS

## Project

Standalone native texture/atlas packer built on neotolis-engine. The current
baseline has one shared packing/export core, typed operation/session ownership,
stable structural IDs, canonical-v5 tagged sources, semantic history/recovery,
and thin CLI and native GUI clients. Target work adds canonical Import/Export
IRs, the unified format registry, and capability-equivalent MCP and Dev API
clients.

`docs/spec/product.md` is the current product contract. Approved future behavior
is explicitly labeled `Target contract` under `docs/spec/`. Current ownership
and implementation boundaries live under `docs/architecture/`; serialized and
CLI wire contracts live under `docs/formats/`. Plans, research, review history,
and superseded implementation records are not durable authority.

Humans and machine/AI agents are equal first-class operators. Every capability
needs an appropriate headless contract, structured diagnostics, and graceful
handling of invalid input.

## Read Route

- Start every task with this file, branch/HEAD, `git status`, and the user request.
- Use `docs/README.md` to select the minimum task-relevant documentation.
- Read the current product contract or an explicitly labeled target contract for
  product decisions, and the narrow owning architecture document for boundaries.
- Read `docs/formats/` for an affected serialized or CLI report contract.
- Consult decisions, plans, research, tests, and Git history only as evidence
  required by the task; do not preload the whole documentation tree.
- Treat live code and executable tests as evidence of implemented behavior, not
  as authority to silently override a stated product contract.

## Structure

- `external/neotolis-engine/` — engine submodule; read-only for agents.
- `packer/` — shared `tp_core`/`tp_build` project, operation, session, packing,
  export, history, recovery, and future format/IR code. No UI or CLI parsing.
- `apps/cli/` — file-oriented `ntpacker` frontend.
- `apps/gui/` — native GUI frontend over the same core contracts.
- `apps/smoke/` — minimal builder environment smoke test.
- `docs/` — current and target product contracts, architecture, and wire formats.
- `examples/defold-demo/` — executable Defold export integration example.

## Hard Invariants

- Use public neotolis-engine APIs before custom code. Never edit
  `external/neotolis-engine/`. Establish root cause first; an engine fix ships
  through an engine issue and PR.
- Keep one typed operation/session layer with capability-equivalent clients.
  Mutation, validation, naming, capability, transaction, and Undo rules live
  below GUI, CLI, MCP, and Dev API transports.
- Keep ordinary CLI for saved-file workflows. Live headless sessions belong to
  MCP/Dev API, not to ordinary CLI commands.
- Every saved-file capability has machine-readable CLI output. Every CLI command
  supports stable versioned `--json`; exit codes, errors, and notices are
  structured contracts. Destructive/lossy operations provide predictable
  dry-run behavior. Invalid input returns an error, never an abort.
- Use C17 and `nt_set_warning_flags` on every first-party target.
- Use `NT_ASSERT` and builder-boundary `NT_BUILD_ASSERT` in Debug and Release.
  Never disable them, set `NT_ASSERT_MODE=OFF`, or substitute libc `assert()` for
  required runtime invariants.
- Sources are tagged records. Path files/folders and linked atlases share one
  runtime status boundary. External refresh never changes project revision,
  dirty state, or Undo history and never starts Pack automatically.
- Built-in, template, and sandboxed Lua formats share one package descriptor,
  exact capability vocabulary, and versioned Import/Export IRs. Adding a format
  must not modify pack orchestration.

## Working Rules

- Preserve pre-existing tracked, untracked, and ignored user work. Do not
  overwrite, revert, stage, or commit it unless the user puts it in scope.
- Put one-off probes and temporary experiments under
  `scratchpad/experiments/<name>/`, never in the repository root.
- Keep clients thin and changes at the narrowest owning layer.
- Complete work with evidence: inspect the final diff, run checks proportional
  to risk, verify acceptance criteria, and report anything not validated.
- Update durable docs only when a durable product contract or architecture
  changes. Plans, research logs, review findings, and session state stay local.
- Minimize commentary to blocking questions, material progress, risk/evidence,
  and the final result.

## Execution Economy

- Optimize for delivered value, user time, latency, and agent/tool cost. Process
  supports the work; it is never the deliverable.
- Execute clear bounded tasks directly. Do not add planning artifacts, research
  phases, subagents, or extra approval checkpoints unless they reduce total work
  or address a concrete current risk.
- Do not add hooks, wrappers, validators, gates, roles, or workflow layers for
  hypothetical failures. Reuse the simplest existing mechanism; get user
  approval before introducing process that adds recurring friction.
- Treat reviewer findings as hypotheses, not commands. Verify practical impact
  and reject theoretical or low-value findings instead of converting them into
  ceremony.
- Scale verification to blast radius: structural checks for docs/config,
  focused tests for focused code, and full suites or review swarms only for
  broad/high-risk changes or an explicit user request.
- Prefer deletion and simplification over framework growth. Stop when the user
  goal and acceptance criteria are met; do not continue polishing unasked.

## Agent Workflow

The main conversation is the lead. Use `.claude/skills/packer-work/SKILL.md` for
adaptive task execution. A trivial task needs neither delegation nor `.context/`.

Delegate only when a bounded task is genuinely independent, benefits from fresh
context, or would flood the lead context. Non-triviality alone is not a reason.
Use one concrete packet per agent and never allow nested delegation. The project
roles are `deep-reasoner`, `implementer`, and read-only `reviewer`.

Run `/packer-premerge-review` only when the user explicitly requests the
separate expensive merge gate; never launch it after ordinary implementation.

## Build and Test

Requires CMake 3.25+, Ninja, and Clang.

```bash
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug

cmake --preset native-release
cmake --build --preset native-release
ctest --preset native-release
```

`native-tests-debug` adds the GUI self-test configuration. Outputs land under
`build/<area>/<target>/<preset>/`. Registered ctests include boundary gates.

## Simplification and CI

LOC and complexity are diagnostic inventory, never pass/fail gates. Split only
at a real ownership, dependency, contract, or reuse boundary; do not fragment
cohesive code to reduce a metric.

GitHub Actions build and test Linux, Windows, and macOS. `release.yml` publishes
all three platforms from semver tags. Suspected platform failures rooted in the
engine belong upstream.
