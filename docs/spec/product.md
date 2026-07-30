# Product contract

**Status:** Current contract.

Neotolis Texture Packer is a standalone native texture-atlas tool built on
neotolis-engine. It gives human and machine operators the same project meaning
through a shared core rather than separate GUI and automation models.

## Current capabilities

- multiple atlases in one canonical v5 project;
- folder and individual image-file sources;
- deterministic pack and export;
- atlas and per-sprite packing settings;
- stable structural IDs and explicit animations;
- native preview-oriented GUI;
- complete saved-file CLI editing, inspect, validate, Pack, Export, and dry run;
- full-fidelity `json-neotolis` export;
- Defold export with explicit capability adaptation and loss notices;
- target-capability adaptation with structured loss notices;
- semantic revision, dirty state, Undo/Redo, durable Save, and best-effort
  recovery;
- worker-process containment for fallible builder work;
- GUI-owned semantic Pack-result preview caching.

The current product does not provide filesystem watchers, linked foreign-atlas
sources, Import/Export IR, unified format packages, Lua handlers, MCP, or a Dev
API transport.

## Shared behavior

All operators use one typed operation/session layer. Mutation, validation,
canonical naming, target capability, transaction, revision, dirty, history,
recovery, Pack, and Export rules live below clients.

Invalid input returns structured diagnostics and never intentionally aborts a
client. Every saved-file CLI capability has stable versioned JSON and a
documented exit code.

The ordinary CLI is file-oriented. It does not attach to unsaved GUI state or
act as a hidden live-session transport.

## Determinism

Given the same:

- canonical project state;
- normalized decoded source pixels;
- target exporter/profile and effective settings;
- packer/format implementation versions;

the persistent outputs and structured non-timing report fields are
reproducible. Timestamps, locale-dependent floats, pointers, filesystem
enumeration accidents, and hidden GUI state must not affect output.

Timing measurements and live filesystem status are diagnostic runtime data, not
persistent deterministic output.

## Explicit derived work

Pack is user- or operator-triggered. Edits, Refresh, Undo/Redo, source changes,
and cache misses may mark a preview stale but never start Pack automatically.

Pack updates the live preview/cache. Export writes the configured target
artifacts. Save publishes the project file. These are distinct commands and
none silently performs another's persistent side effects.

## Capability adaptation

Each export target states what it can represent. Effective settings may disable
an unsupported packing transform so the resulting artifact remains valid.
Metadata that cannot be represented is reported before export and in the final
machine report.

A compatible adaptation or metadata loss is a notice, not an export failure.
A condition that prevents a valid artifact is an error.

## Persistent and runtime boundaries

The project file contains authored model state only. Revision, dirty anchor,
history, recovery journals, source availability/mtime, preview cache, GPU
resources, filters, selection, scroll, dialogs, and drafts are runtime state.

External source refresh does not change project revision, dirty state, or Undo
history, and does not start Pack.

Current persistence is exactly [project schema v5](../formats/project-v5.md).
Future workspace, linked-source, project-level-target, or scale-variant fields
require a newer schema and an explicit migration contract.

## Current clients

The native GUI and saved-file CLI are capability-equivalent where their product
shapes overlap, while using appropriate interfaces:

- GUI: one live session, one borrowed current view, drafts, history, recovery,
  and one active task.
- CLI: one-shot queries or mutations over a saved project, with structured
  output and predictable dry runs.

Future live automation is governed separately by
[`automation.md`](automation.md).
