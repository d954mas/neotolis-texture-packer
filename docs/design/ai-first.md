# AI-First Operation — owner ruling 2026-07-11

Owner: *"CLI ships out of the box. From the very core we design the tool to be
operated by both humans and AI. Human and AI are equal first-class users."*

This is a standing design principle, ranked with the Hard Invariants in
AGENTS.md. It reframes tool parity: **one op layer, N clients — GUI (human),
CLI (human + machine), and machine contracts (AI agents / pipelines)**. An AI
operator is not a special mode; it is the same CLI + file formats used
programmatically. What follows is the concrete contract every capability must
satisfy, and what it changes in the plan.

## Why this is cheap for us and hard for competitors

- Deterministic byte-identical output (already a contract) means an agent can
  *verify* its own edits by hashing outputs — no screenshot-diffing needed.
- The project file is versioned JSON with a canonical writer: the file itself
  is an API surface an agent can read and edit; a re-save is byte-stable, so
  agent edits produce clean diffs.
- Per-target capability packing already computes "what will this format drop"
  (`tp_export_effective_settings` + notices) — exactly the feedback loop an
  agent needs, once notices are delivered instead of freed (review §3.4).
- No competitor offers any of this: TexturePacker's CLI is human-shaped
  (prose errors, no JSON reports, paid CI licensing).

## The contract (applies to every capability, enforced at review)

1. **Headless reachability.** Any operation the GUI can do — create/open
   project, add/remove sources, set any knob or override, assemble
   animations, pack, export, rescan folders — must be callable from the CLI.
   (This is the existing parity invariant; AI usage makes it non-negotiable.)
2. **`--json` everywhere.** Every CLI command has a machine-readable output
   mode with a **versioned schema** (`"schema": N` in the payload, mirroring
   the project-file convention). Human-readable text is the default; JSON is
   never lossy relative to it.
3. **Exit codes are a contract.** 0 = ok, distinct stable codes for: invalid
   arguments, project errors, pack failure, export failure, partial success
   (some targets failed). Documented, test-pinned.
4. **Errors and notices are structured data.** `tp_error` grows a stable
   error id (string enum) alongside the message; degradation notices carry
   {sprite, field, target, reason-id} — not only prose. Both surface in
   `--json` output. Prose is derived from data, never the other way around.
5. **stdout/stderr discipline.** stdout = the requested payload only;
   stderr = diagnostics/progress. An agent can pipe stdout straight into a
   parser.
6. **Dry-run / explain.** `pack --dry-run` and `export --dry-run` report what
   would happen — pages, occupancy, per-target effective settings and every
   predicted degradation (`tp_export_predict_loss`, review packet A/C) —
   without writing files.
7. **Query surface.** `inspect` / `validate` verbs: dump project state
   (atlases, sources, sprites with resolved overrides, animations, targets)
   and validate a project file, reporting *all* problems as structured
   findings in one run (an agent fixes them in one edit cycle, not one per
   run).
8. **Graceful failure, never a crash.** Bad input (oversized sprite —
   engine#287, malformed file, missing source) must return a structured
   error. An aborting tool breaks an agent loop; this elevates #287 and the
   parser-fuzz work (review §3.5) from hardening to product requirements.
9. **Machine-consumable docs.** Published JSON Schema for
   `.ntpacker_project` and the json-neotolis export; a docs page an agent can
   read (quick-start, verb reference, error-id table). The formats are the
   API; document them like one.
10. **Stability policy.** JSON field additions are non-breaking; removals/
    renames require a schema bump. Same major/minor discipline the review
    (§3.8) asks of the project file.

## Non-goals (for now)

- A bespoke "AI mode" or natural-language interface — the CLI contract *is*
  the AI interface.
- An MCP server ships later as a thin wrapper over the same op layer once the
  CLI JSON contract exists (it is mechanical at that point and a strong
  differentiator; not before).

## Plan impact

- **CLI moves from backlog to critical path** (Phase 4, AI-first shape per
  this contract), immediately after review packet A (op-layer consolidation
  into tp_core) — packet A is the prerequisite that makes the CLI thin and
  guarantees GUI/CLI byte-parity instead of a second implementation.
- Review packet C (centralized degradation pass + notices as data) feeds
  requirement 4/6 directly; packet F (structured errors on bad paths/files,
  atomic writes) feeds 8.
- The JSON build report (competitive bet #1) becomes the flagship of
  requirement 2: pages, occupancy %, per-sprite placements, savings,
  notices — machine-diffable in CI.
- engine#287 (graceful error instead of assert on oversized sprite) is
  promoted: required for the AI contract, not just polish.
