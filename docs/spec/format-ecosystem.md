# Format ecosystem and atlas interoperability

**Status:** Target contract; the unified registry and IRs are not implemented.

The current built-in/runtime-C exporter surface remains documented in
[`../architecture/engine-and-client-boundaries.md`](../architecture/engine-and-client-boundaries.md).
This contract defines the future common system for built-in, template, and
sandboxed Lua formats.

## One package descriptor

Every format uses one descriptor and registry vocabulary regardless of
implementation driver. A package may provide:

- export;
- import;
- detection/probe;
- one or more data-format versions and compatibility profiles;
- declared artifacts and companion-file rules;
- deterministic fixtures and tests.

Stable format identity is separate from package version, package API version,
manifest version, and data-format version. Projects store the selected format
ID/data version/profile/options, not the installed package implementation
version.

Duplicate IDs and incompatible manifest/API versions fail explicitly. Adding a
format must not modify Pack or Export orchestration.

Exact manifest fields, capability tokens, archive manifest, and public schemas
remain open contracts until fixed by executable fixtures.

## Canonical IR boundary

Exporters receive immutable, versioned Export IR. Importers return immutable,
versioned Import IR. Neither receives mutable project internals.

Export IR represents normalized, final names and:

- pages and pixel/artifact references;
- frames, trim/source size, pivots, 9-slice;
- polygon vertices/indices;
- full D4 transforms;
- aliases and explicit animations;
- selected data-format/profile/options;
- structured capability adaptations and notices.

Import IR represents the corresponding foreign atlas facts plus source
provenance and companion-file diagnostics. Materialization into project state
is a separate validated transaction.

The exact schemas and limits are open release contracts. Both IRs require
version tags, count/byte bounds, deterministic ordering, and graceful rejection
of unknown incompatible versions.

## Capability model

One exact capability vocabulary covers built-in and external handlers:

- rotation and full D4 flips;
- trim and source-size recovery;
- polygons;
- pivot;
- 9-slice;
- multipage;
- aliases;
- explicit animations;
- import/export direction and delivery modes.

Target adaptation happens before writing. A representable repack is a notice.
Unrepresentable metadata is reported per target/sprite. Only inability to create
a valid artifact is blocking.

## Package origins and delivery

The registry supports bundled, user/global, and project-local packages. Origin
and resolved implementation version are reported in export results.
Project-local packages are normally pinned by the project's version control.

Packages may load from a directory or a packaged archive. Export delivery may
target a directory or a deterministic archive with a versioned artifact
manifest. Traversal, absolute names, case collisions, undeclared files, and
partial publication are rejected.

## Detection

Detection is ranked evidence, never an irreversible hidden choice:

1. extension filtering;
2. exact markers and declarative signatures;
3. optional bounded probe;
4. visible candidate list when ambiguous;
5. explicit operator override.

The selected format ID is stored on a linked source and never silently changes
on Refresh. A validated Change Format operation is explicit.

## Linked atlas sources

A future project schema adds a read-only linked-atlas source record. It stores
the selected format and descriptor/companion reference; regions are
materialized on demand through Import IR.

Linked sources share the source-runtime boundary with file/folder sources:

- verify on open, explicit Refresh, Pack, Export, and Extract;
- future watchers may invalidate runtime state;
- external changes do not change project revision, dirty state, or Undo;
- Pack consumes canonical raw RGBA through the existing source/packer path;
- a failed refresh keeps the project record and reports structured runtime
  status.

This requires a project version newer than 5 and an explicit migration
contract.

## Project-level export targets

The future model declares export targets at project scope, with per-atlas
participation and optional path override. Export resolves effective targets
before packing/writing.

Canonical v5 continues to store per-atlas targets. Project-level targets are not
backported or auto-lifted into v5; they require a newer schema and defined
migration.

## Extract Sprites

Extraction operates on a linked source in its current atlas:

1. re-read and materialize the current foreign atlas;
2. compute all final region names and paths;
3. validate traversal, absolute names, platform collisions, and conflicts;
4. write and verify every full-source-size PNG in sibling staging;
5. publish the output files;
6. replace the linked source with the folder source;
7. transfer recoverable names, pivots, 9-slice, animations, and aliases;
8. commit project changes as one semantic transaction.

Default conflict policy is fail-without-changes. Overwrite must be explicit.
Undo restores project state but does not delete already published files.
A separate PNG is written for every visible alias name, even when bytes are
identical.

## Template and sandboxed scripting

Templates are for deterministic text projections with escaping, ordered
iteration, loops, conditions, optional fields, separator helpers, declared
artifacts, and structured diagnostics. The template language must remain small.

Complex parsing, import, probes, binary output, arithmetic, or multi-file logic
uses the sandboxed Lua driver. The sandbox exposes immutable IR, bounded
companion-file access, declared staging, JSON/text/binary helpers, paths,
logging, and structured errors.

It does not expose unrestricted OS/file I/O, networking, process execution,
native modules, package/debug libraries, or arbitrary filesystem traversal.
Each invocation uses protected calls, an isolated allocator/state, memory and
instruction/output limits, and cooperative cancellation hooks.

Installed and project-local Lua/template handlers execute automatically without
per-package trust prompts. The sandbox is the security boundary: automatic
execution is allowed only after the adversarial sandbox suite passes. A global
disable-external-formats mode and diagnostic safe mode may be provided.

Failure discards the invocation's IR/staging, leaves the project unchanged, and
cannot replace a successful preview or output.

The accepted runtime is PUC Lua 5.5, pinned to the latest patch available when
the Lua package slice is implemented and carrying applicable official bug
fixes. Binary chunks are disabled; handlers load as text only. If the release
spike finds an active defect stream in the used GC, debug-hook, or allocator
areas, the fallback is the latest PUC Lua 5.4.x without changing the sandbox
API.

## Package verification

Built-in and external packages run equivalent descriptor-level tests:

- deterministic declared artifacts;
- malformed input and missing companions;
- transform/capability coverage;
- multipage, aliases, and animations;
- import/export round trips where meaningful;
- path safety and cancellation/resource limits.
