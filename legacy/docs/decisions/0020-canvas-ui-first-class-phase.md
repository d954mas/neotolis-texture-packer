# 0020 — Canvas UI/UX refactor as a first-class phase (pre-user code-order priority)

**Status:** accepted
**Date:** 2026-07-20
**Scope:** execution order in `docs/ROADMAP.md` and
`docs/plans/master-spec-implementation-plan.md`; normative status of the 2026-07-13 UX
canvas design
**Supersedes:** master spec §53.5 ("GUI visual polish is secondary" / "avoid unrelated
GUI redesign during either epic") and §54's Phase-5-last placement of GUI refinements,
and the `docs/ROADMAP.md` "GUI polish как самостоятельный приоритет" out-of-scope line
— for the canvas/workspace refactor specifically
**Preserves:** Epic A live-AI/Dev-API/MCP (master spec Part II), Epic B
format-package/Export-IR (Part III), and determinism-without-UI as a core property

## Context

There are no users yet. Both remaining epics — Epic A (live AI/Dev-API/MCP) and Epic B
(format-package/Export-IR) — are GUI-heavy: they add panels, canvas interactions, and
status surfaces on top of whatever GUI structure exists when each starts. The core is
already a thin adapter over owned snapshots (M0–M5 foundation work is done: typed
operations, transactions, semantic history, recovery, durable save; the remaining Base
work is H0 builder containment, the F2 phase gate, and F3). The concrete risk is not "the GUI needs polish" — it is building
non-trivial GUI structure twice: once ad hoc inside whichever epic goes first, and again
when the other epic (or a later pass) needs the same structural surfaces the 2026-07-13
UX session already designed.

That UX session (`docs/design/ux-vision-2026-07-13.md`,
`docs/design/ux-master-spec-delta-2026-07-13.md`,
`docs/plans/ux-epics-2026-07-13.md`, under `docs/design/` and `docs/plans/`) produced a
concrete two-level canvas design (Project ⇄ Atlas), a unified project tree, a
Problems/preflight glass-box surface, and the schema/contract deltas (A1–A11) it depends
on. Master spec §53.5 deprioritized GUI redesign ("avoid unrelated GUI redesign during
either epic") and §54 placed GUI refinements last, in Phase 5 — both written before the
canvas design existed and before the foundation (M0–M5) was complete. Left as written,
they would push a now-designed, already-scoped structural refactor behind two GUI-heavy
epics that will each independently need pieces of it.

## Decision

The canvas/workspace refactor becomes a first-class phase **U**, sequenced immediately
after the shared foundation and before both remaining epics:

```text
Base (H0 + F2 + F3)  →  U  →  Epic B  →  Epic A  →  Breadth
```

- **F3's prerequisite set is relaxed** from `{F2, B1, H0}` to `{F2, H0}` — F3 (semantic
  history and Pack session behavior) no longer waits on B1 (linked atlas sources /
  Extract Sprites), since B1 is not on the critical path to Phase U. The
  Extract-Sprites-with-History composition question that F3's old B1 dependency existed
  to sequence moves to become part of B1's own acceptance gate instead of a
  cross-phase ordering constraint.
- The UX delta items **A1–A11**
  (`docs/design/ux-master-spec-delta-2026-07-13.md`) — export-targets-at-project-level,
  settings-inheritance provenance, write-once `added_at`, atlas `visible` as a model
  operation, palette-ready operation registry, history author identity, ZIP export
  delivery, scale-variant schema slot, pack-result thumbnails, canvas workspace data
  (board positions + notes), and the session/app-state file — are promoted from design
  proposal to **normative master-spec §61**.
- §53.5's "GUI visual polish is secondary" / "avoid unrelated GUI redesign" guidance and
  §54's Phase-5-last placement of GUI refinements are superseded for the
  canvas/workspace refactor specifically: it is no longer "unrelated redesign" or
  cosmetic "polish," it is a scoped, designed, prerequisite structural phase.
- What §53.5/§54 correctly ruled out remains ruled out: cosmetic polish as a standalone
  goal (tracked separately as UX-F, not part of Phase U) and a from-scratch GUI
  replacement are still non-goals. Phase U is the structural canvas/tree/workspace
  refactor the UX session scoped, not an open-ended redesign.
- Epic A's live-AI/Dev-API/MCP surface (master spec Part II), Epic B's
  format-package/Export-IR surface (Part III), and determinism-without-UI as a core
  property are unchanged by this reordering — Phase U changes *when* GUI structure is
  built, not *what* either epic delivers. Agent-presence UI (UX-E, the "Claude —
  connected" presence chip and related live-collaboration surfaces) stays reserved for
  Epic A, not pulled into Phase U.

## Consequences

`docs/ROADMAP.md` gains phase U between the shared foundation and Epic B/Epic A, plus
the relaxed F3→B1 edge becomes F3→{F2,H0} only. `docs/plans/master-spec-implementation-plan.md`
gains task packets U.1–U.14 covering the two-level canvas, unified tree, Problems panel,
and the A1–A11 schema deltas. New open contracts introduced by this work — swatch/color
tokens for notes and provenance markers, `workspace`/note field names in the project
schema, the app-state file's location and shape, and the path-template grammar for
export delivery — are tracked as open items in master spec §60 pending their own
decisions. Because Phase U lands before either epic, both Epic B and Epic A build their
GUI-heavy work on top of the canvas/tree/workspace structure once, instead of each
epic extending or reworking an ad hoc pre-Phase-U GUI independently.
