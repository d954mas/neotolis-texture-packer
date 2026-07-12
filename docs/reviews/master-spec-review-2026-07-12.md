# Review: `ntpacker-master-spec.md`

**Date:** 2026-07-12

**Reviewer role:** architecture / product decomposition

**Scope:** `docs/ntpacker-master-spec.md` as the new product and architecture source of truth, checked against the current repository baseline.
**Decision:** approve as the master direction, require the corrections and pre-implementation decisions below before execution starts.

## Verdict

The master specification is complete enough to become the primary product and architecture document. It has a coherent product thesis, explicit invariants, two measurable epics, failure behavior, non-goals, acceptance criteria, and a proposed delivery order (`docs/ntpacker-master-spec.md:21`, `docs/ntpacker-master-spec.md:80`, `docs/ntpacker-master-spec.md:1392`, `docs/ntpacker-master-spec.md:2519`, `docs/ntpacker-master-spec.md:2875`).

It is **not yet safe to copy section 54 directly into an implementation roadmap**. Three dependencies must be corrected:

1. minimal semantic transactions and Undo must precede the project-mutating `Extract Sprites` slice;
2. a minimal recovery journal must exist before GUI mutations switch to the unified “committed only after journal append” contract;
3. identity, schema migration, canonical path, semantic-state identity, and capability vocabulary need executable contract spikes before public structs and schemas are frozen.

Recommended disposition:

- keep the master spec unchanged as the product contract;
- replace the old roadmap with a dependency-correct implementation roadmap;
- keep format documents as subordinate executable format contracts;
- mark old design/research documents as historical where they conflict with the master spec.

## Strengths and completeness

### Product direction is focused

The three differentiators are concrete and mutually reinforcing: silhouette packing, open format interoperability, and complete automation/live AI (`docs/ntpacker-master-spec.md:21-50`). The spec correctly prioritizes interoperability before broad GUI polish and recognizes that the current CLI already captures much of the ordinary automation value (`docs/ntpacker-master-spec.md:2724-2759`).

### Architectural boundaries are explicit

The strongest invariant is one business-logic layer beneath GUI, CLI, MCP, and Dev API (`docs/ntpacker-master-spec.md:119-150`). The separation among model operations, session commands, derived jobs, and external side effects is especially useful because it prevents Save, Pack, Export, and Extract from being forced into one false transaction model (`docs/ntpacker-master-spec.md:359-395`).

### State semantics are unusually well specified

The document separates revision, history position, saved baseline, preview freshness, and runtime source generation (`docs/ntpacker-master-spec.md:480-518`, `docs/ntpacker-master-spec.md:608-695`, `docs/ntpacker-master-spec.md:752-802`). That removes the most common ambiguity in GUI/agent collaboration systems.

### Failure behavior is part of the product contract

Atomic batch validation, idempotent retries, recovery-journal acknowledgement, explicit stale preview, staged extraction, and sandbox failure isolation all have defined outcomes (`docs/ntpacker-master-spec.md:411-478`, `docs/ntpacker-master-spec.md:1234-1268`, `docs/ntpacker-master-spec.md:2211-2241`, `docs/ntpacker-master-spec.md:2307-2334`). This is sufficient to derive fault-injection tests rather than relying on prose-only expectations.

### The epics have real completion tests

Epic A is complete only when human and AI edit the same unsaved live state and one Undo restores the previous state and preview. Epic B is complete only when a foreign atlas can be inspected, linked, extracted, repacked, and converted (`docs/ntpacker-master-spec.md:2893-2906`). These are good product-level UAT gates.

## Capability baseline matrix

| Capability | Current repository baseline | Delta required by master spec | Evidence |
|---|---|---|---|
| Shared core | `tp_core` owns project/scanning/export logic; `tp_build` owns pack orchestration | Add `tp_operations` and `tp_session`; remove remaining frontend mutation/history policy | `packer/CMakeLists.txt:1-12`, `packer/CMakeLists.txt:50-63` |
| Project persistence | Schema v1; atlas arrays; path-only `char **sources`; sprites/animations/targets addressed mostly by names/indexes | Schema migration to structural IDs, tagged sources, target format version/profile/options, ID references | `packer/include/tp_core/tp_project.h:40-64`, `packer/include/tp_core/tp_project.h:69-127`, `packer/include/tp_core/tp_project.h:129-137` |
| CLI machine interface | Versioned `--json`, inspect/validate, full mutation family, dry-run tests exist | Preserve behavior while routing mutations through typed transactions; add stable ID selectors and live-session conflict | `apps/cli/main.c:48-71`, `apps/cli/main.c:180-207`, `apps/cli/cli_cmds.h:19-35`, `apps/cli/cli_mutate_family.cmake:67-115` |
| Export registry | Built-in plus runtime registration exists; exporter lookup is centralized | Replace the current C-only descriptor with unified format-package manifest/driver/versions/origin contract | `packer/src/tp_export.c:162-249`, `packer/include/tp_core/tp_export.h:251-288` |
| Capability-aware packing | Effective settings and structured loss prediction exist | Replace coarse booleans with exact public modes such as D4 mask, geometry and alias/animation modes | `packer/src/tp_export.c:127-140`, `packer/src/tp_export.c:279-335`, `packer/include/tp_core/tp_export.h:41-57` |
| Raw RGBA input | Core already accepts pathless raw RGBA8 sprites | Fix ownership/lifetime contract with the engine; reuse this path for imported atlas regions | `packer/include/tp_core/tp_pack.h:50-58`, `docs/ntpacker-master-spec.md:697-705` |
| Source scanning/refresh | Recursive sorted scan is in core; GUI keeps a small scan cache | Add tagged source runtime state, generations, deterministic image hashes, real watchers, linked-atlas companion tracking | `packer/include/tp_core/tp_scan.h:23-43`, `packer/src/tp_scan.c:148-188`, `apps/gui/gui_scan.c:46-70` |
| GUI Pack jobs | GUI Pack/Export runs on worker threads | Move job authority to session; immutable input hash, out-of-order completion rules, result LRU, ownership transfer | `apps/gui/gui_pack.c:349-366`, `apps/gui/gui_pack.c:459-506`, `apps/gui/gui_pack.h:57-65` |
| Undo/dirty/stale | GUI serializes pre-mutation snapshots; dirty/stale are GUI globals | Semantic transaction diffs, shared session history, monotonic revisions, saved semantic baseline | `apps/gui/gui_history.h:4-17`, `apps/gui/gui_project.c:161-183` |
| Import / linked atlases | No Import IR, importer registry, atlas source, or materializer in the current core target list | Entire Epic B native-import foundation and linked-source slice | `packer/CMakeLists.txt:8-12`, `packer/CMakeLists.txt:53-59` |
| Lua/template packages | Runtime C exporter registration exists, but no package loader or script runtime is built into `tp_core` | Manifest discovery, `.ntformat`, constrained template engine, sandboxed Lua and package tests | `packer/src/tp_export.c:201-249`, `docs/ntpacker-master-spec.md:1678-1757` |
| Live session / Dev API / MCP | No session/IPC/MCP modules in current core/build target list | Entire Epic A session, IPC, authority, authorization, mirror and recovery stack | `packer/CMakeLists.txt:8-12`, `packer/CMakeLists.txt:53-59`, `docs/ntpacker-master-spec.md:1014-1508` |

The baseline is therefore stronger than a prototype: packing, project editing, CLI contracts, raw RGBA, exporters, adaptation notices, async GUI work, and snapshot Undo already exist. The implementation is still a substantial model migration, not a thin layer over finished abstractions.

## Confirmed contradictions and gaps

### 1. Legacy ID migration conflicts with “IDs survive Save”

The invariant says IDs survive save/reload (`docs/ntpacker-master-spec.md:232-233`). The migration section says an old project receives deterministic temporary IDs, then the next Save persists normal random IDs (`docs/ntpacker-master-spec.md:307-315`). As written, first Save changes every migrated structural ID and can invalidate references, selectors, history entries, and connected mirrors.

Required resolution: define one atomic migration rule. Preferred: assign final random structural IDs once when the legacy model enters a writable session, retain them for that session, and persist those exact IDs on Save. A repeated read-only load may use deterministic synthetic IDs, but a writable attach must explicitly promote them before any external transaction is accepted.

### 2. `Extract Sprites` depends on work scheduled after it

Extraction must replace the linked source and transfer metadata as one semantic transaction (`docs/ntpacker-master-spec.md:2211-2228`). The recommended order places Extract in Phase 1, while semantic diffs, serialized session mutation, and History migration are in Phase 2 (`docs/ntpacker-master-spec.md:2807-2828`). Section 7 also says every model transaction creates one Undo entry (`docs/ntpacker-master-spec.md:423-444`).

Required resolution: move the minimal transaction kernel, inverse data for source replacement, saved baseline, and serialized apply path ahead of project-mutating Extract. Standalone `atlas inspect` may ship before this; project replacement may not.

### 3. Unified commit acknowledgement depends on a later journal

The spec requires GUI, MCP, Undo, and Redo to become visible only after recovery-journal append (`docs/ntpacker-master-spec.md:446-464`). The proposed order delays journal semantics until live-AI Phase 4 (`docs/ntpacker-master-spec.md:2838-2844`). A GUI converted to typed operations in Phase 0/2 would therefore violate the settled commit contract.

Required resolution: introduce a minimal append-only local journal with transaction ID, before/after revision, and replay payload before switching GUI mutation authority. Advanced checkpoint cadence, compaction, corruption policy, and cross-process claims may remain later work.

### 4. `Save As` has no bound-session identity rule

`Save As` creates another project identity because identity is canonical path (`docs/ntpacker-master-spec.md:235-246`). A bound MCP process may not silently change path or project (`docs/ntpacker-master-spec.md:1025-1026`), yet `save_as` is a public session command (`docs/ntpacker-master-spec.md:366-374`).

Required resolution: select one contract before Dev API schema work. Recommended: successful Save As changes the authoritative session identity only after acquiring the destination claim; the existing MCP connection receives an explicit `session_identity_changed` event and must acknowledge/rebind. It must never happen as an unreported path mutation.

### 5. Pack concurrency is underspecified

The session model lists one active Pack job (`docs/ntpacker-master-spec.md:847-861`), while result selection discusses concurrent jobs and out-of-order completion (`docs/ntpacker-master-spec.md:666-677`). Ownership transfer also speaks of a singular running Pack (`docs/ntpacker-master-spec.md:1156-1164`).

Required resolution: define whether v1 permits one running Pack with supersession/cancellation or multiple concurrent Packs. Recommended initial policy: one running Pack per session plus one latest requested intent; old successful results remain cacheable, but a superseded job cannot select itself as preview.

### 6. Canonical project-path identity lacks a cross-platform algorithm

Session uniqueness, permissions, recovery, CLI blocking, and Save As all depend on canonical path (`docs/ntpacker-master-spec.md:235-246`, `docs/ntpacker-master-spec.md:1014-1033`, `docs/ntpacker-master-spec.md:1301-1314`). The spec does not settle Windows case/UNC/short names, symlinks/junctions, nonexistent destination paths, or normalization before first Save.

Required resolution: create a focused path-identity contract and cross-platform fixture suite before session registry or authorization storage is implemented.

### 7. Project-local reproducibility and duplicate-ID rejection need an operating policy

Project-local packages are presented as the Git/CI reproducibility mechanism (`docs/ntpacker-master-spec.md:1740-1749`), but any duplicate format ID across embedded/user/project locations is an error (`docs/ntpacker-master-spec.md:1751-1757`) and projects do not pin package versions (`docs/ntpacker-master-spec.md:1635-1643`). This is safe but can make a valid repository fail only because a user-installed package exists.

Required resolution: retain “no silent shadowing,” but define a reproducible invocation policy. Recommended: CI uses an explicit isolated format search mode; diagnostics list every conflicting origin; development override is explicit and report-visible.

### 8. Epic B success needs a fixture matrix, not one universal fixture

The success statement combines trim, rotation, polygons, pages, and animations in a TexturePacker/Pixi or Neotolis import (`docs/ntpacker-master-spec.md:2901-2906`). The TexturePacker JSON Hash/Pixi reference scope does not claim polygon and multipage coverage (`docs/ntpacker-master-spec.md:2445-2467`).

Required resolution: define separate reference fixtures by capability. Neotolis is the full D4/polygon/multipage oracle; TexturePacker/Pixi covers its actual profiles; cross-format tests assert explicit loss/adaptation rather than pretending every format represents every field.

### 9. Several release-blocking contracts are not named explicitly in section 60

Section 60 lists journal, authority, cache, template syntax, schemas, extraction recovery, and color management (`docs/ntpacker-master-spec.md:3034-3055`). It does not explicitly list project schema migration, canonical path identity, semantic-state identity/hash, selector grammar, or Pack supersession. These cannot remain accidental implementation choices because they affect persisted data and public concurrency behavior.

Required resolution: track them as pre-implementation decision records even if the master spec remains unchanged.

## Corrected critical path

The implementation roadmap should use the following dependency order.

### F0 — executable contract spikes

1. Canonical project-path identity across Windows/Linux/macOS.
2. Schema-v1 to structural-ID/tagged-source migration fixture.
3. Semantic-state identity and canonical equality inputs.
4. Capability vocabulary derived from Neotolis JSON, Defold, and TexturePacker/Pixi fixtures.
5. `tp_build -> neotolis-engine` raw-RGBA ownership/lifetime test.
6. Minimal journal record/replay prototype.

Exit: each decision has an executable fixture or golden test; no production public schema is frozen from prose alone.

### F1 — persistent model foundation

1. Persistent atlas/source/animation/target IDs.
2. Deterministic sprite IDs and normalized source-local keys.
3. Tagged path-source records.
4. ID-based references plus unique selector resolution.
5. Target migration to `format_id`, `data_version`, `profile`, and options.
6. Deterministic save/reload and legacy migration tests.

Exit: old projects migrate without reference loss; repeated save/reload is byte-stable after migration.

### F2 — transaction and semantic-history kernel

1. Typed model-operation vocabulary.
2. Full-batch validation and atomic apply/rollback.
3. Monotonic revision and semantic saved baseline.
4. Semantic inverse data; snapshot implementation retained as oracle.
5. Serialized local session queue.
6. Minimal journal append before commit visibility.
7. Route GUI mutations through the kernel; then route CLI file mutations through the same operations.

Exit: every migrated GUI/CLI mutation has forward/reverse equality tests; journal failure leaves state/revision/event stream unchanged.

### B0 — standalone native import value

1. Versioned Import IR.
2. Native Neotolis importer and format marker.
3. Rectangular/polygon/D4 materializer.
4. CLI `atlas detect`, `atlas inspect`, and non-project extraction to a staging destination.
5. Full transform, trim, alias, animation, and multipage fixtures.

Exit: a produced atlas can be inspected and losslessly materialized without changing a project.

### B1 — linked atlas source and transactional Extract

1. Atlas source runtime state and companion-file refresh.
2. Read-only linked regions and Change Format validation.
3. Raw-RGBA pack integration.
4. Extract preflight/staging/publication.
5. One semantic source-replacement transaction with metadata transfer.
6. Minimal GUI linked-source/error/extract surfaces plus CLI parity.

Exit: Epic B’s first marketable workflow works with native Neotolis atlases and passes failure injection.

### F3 — Pack/session runtime

1. Source generations and watchers as invalidation hints.
2. Immutable Pack snapshots and explicit supersession policy.
3. Semantic `pack_input_hash`.
4. Stale-preview behavior and memory-only compressed result LRU.
5. Lazy thumbnail CPU/GPU LRU.
6. Visible shared History and Save checkpoints.

Exit: edits continue while Pack runs; stale/out-of-order results never become silently authoritative; Undo restores cached preview when available.

### B2/B3/B4 — package ecosystem

1. Manifest and registry with separate manifest/package/API/data versions.
2. Embedded, user, project-local, archive, and explicit-path discovery.
3. Exact detection signatures and explicit candidate selection.
4. Sandboxed Lua with allocator, hook cancellation, bounded I/O, JSON/binary services.
5. Constrained export-only template runtime.
6. TexturePacker/Pixi reference package, then Defold import and breadth.

Exit: external package errors cannot mutate model or publish partial output; package contract tests run equally for built-in and external descriptors.

### A1/A2/A3/A4 — live AI

1. Generalize the in-process session and multi-view event stream.
2. Local IPC, discovery, snapshot/resync, transaction and job endpoints.
3. MCP unbound/bound modes and compact resources/tools.
4. Authorization by canonical path.
5. Singular authority claim, handoff, mirror, journal checkpoints, stale-controller proof, and crash promotion.

Exit: the exact Epic A UAT in `docs/ntpacker-master-spec.md:2895-2899` passes, including one-step Undo of a multi-operation agent transaction.

## Risks

### Simultaneous model refactors

Stable IDs, tagged sources, semantic Undo, Dev API, Lua, and package registry must not share one long branch. The spec itself warns against this (`docs/ntpacker-master-spec.md:2761-2775`). Each packet must preserve deterministic output and CLI/GUI parity.

### Persisted-schema blast radius

The current schema is version 1 and references mutable names/array positions (`packer/include/tp_core/tp_project.h:40-137`). Identity migration touches project JSON, CLI selectors, animations, export targets, GUI selection, Undo snapshots, tests, examples, and reports. A migration fixture corpus is mandatory.

### Engine ownership boundary

Imported-atlas materialization relies on raw RGBA already accepted by `tp_pack_sprite_desc`, but copy/adopt/trim/retain lifetime belongs to the engine contract (`packer/include/tp_core/tp_pack.h:50-58`, `docs/ntpacker-master-spec.md:697-705`). If the root cause requires engine work, it must be handled upstream; do not patch the submodule.

### Automatic external code execution

Project-local Lua/template handlers execute without per-package prompts (`docs/ntpacker-master-spec.md:1714-1724`). The sandbox is therefore a release security boundary, not a convenience feature. Parser/runtime vulnerabilities, decompression bombs, large companion graphs, output fan-out, and cancellation must be tested adversarially.

### Cross-process split brain

MCP/GUI ownership transfer is the highest correctness risk in Epic A. “Timeout expired” is not proof of death (`docs/ntpacker-master-spec.md:1107-1112`). Authority claim and cutover require a small explicit state machine with crash tests before any automatic promotion ships.

### External side-effect atomicity

Project transaction rollback cannot undo published user files. Extraction overwrite mode and crash during final publication require a manifest/recovery design (`docs/ntpacker-master-spec.md:2211-2241`, `docs/ntpacker-master-spec.md:3049-3050`). UI wording must not promise filesystem atomicity that the implementation cannot provide.

### Documentation drift

The pre-consolidation UX required default auto-pack and snapshot Undo, while the
master spec requires explicit Pack in v1 and semantic history
(`docs/ntpacker-master-spec.md:520-606`,
`docs/ntpacker-master-spec.md:675-695`). The pre-consolidation roadmap also
described Mustache-only exporters and a persistent incremental/watch cache.
This review triggered their replacement/classification; current
`docs/design/ux.md` and `docs/ROADMAP.md` are now subordinate to the master spec.

## Decisions required before implementation

The following decisions are gates, in order:

1. **Legacy ID promotion:** when synthetic IDs become final random IDs and how references/history/events are atomically remapped.
2. **Canonical path algorithm:** case, symlinks/junctions, UNC, nonexistent Save As destination, path comparison and permission key encoding.
3. **Semantic state identity:** canonical bytes/hash inputs and exclusion of runtime/view state.
4. **Minimal journal contract:** record fields, append/rollback point, replay validation, transaction-ID retention baseline.
5. **Pack supersession:** one running Pack versus multiple; which request may select preview.
6. **Save As session behavior:** destination claim, identity-change event, MCP rebind/detach behavior.
7. **Public capability vocabulary:** exact D4, geometry, alias, animation and metadata modes from fixtures.
8. **Schema migration map:** source records, structural IDs, animation frame references, target format/data/profile/options, old exporter IDs.
9. **Extraction publication recovery:** staging manifest, overwrite backup/restore policy, crash cleanup and user-visible recovery.
10. **Format search reproducibility:** isolated CI mode, duplicate diagnostics and explicit development override.
11. **Lua host/security envelope:** Lua version, allocator accounting, instruction/time policy, archive limits, companion/output quotas and deterministic libraries.
12. **Raster color profile:** deterministic orientation/gamma/ICC behavior before semantic image hashes become cross-platform contracts.

Concrete cache budgets and template syntax can wait until their corresponding packet, as already allowed by `docs/ntpacker-master-spec.md:2697-2719`. They are not blockers for F1/F2/B0.

## Review acceptance

This review is accepted when the implementation-planning documents satisfy all of the following:

- `docs/ntpacker-master-spec.md` is declared the primary product/architecture source of truth.
- The roadmap uses the corrected dependency order above; `Extract Sprites` does not precede its transaction/Undo prerequisites.
- Minimal journal work precedes any production switch to the unified commit acknowledgement path.
- Every item in “Decisions required before implementation” is assigned either to F0 or to a named packet gate; none is hidden as an unowned open question.
- Each packet identifies exact spec sections, bounded module/file scope, schema/report impact, migration behavior, tests, fault injection, and completion evidence.
- Per-format specifications remain subordinate truth for bytes and conventions and are verified through fixtures/golden tests.
- Old roadmap/UX documents are updated or explicitly marked historical where they conflict with the master spec.
- No planned implementation edits `external/neotolis-engine/`; suspected engine root causes become upstream issue/PR work.
- Epic A and Epic B retain their end-to-end success scenarios as final executable UAT, not only unit-level completion claims.

With these conditions, the specification is approved for decomposition into implementation packets.
