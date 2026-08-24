# Sandboxed Lua export formats

**Status:** Mixed contract. The generic sandboxed Lua runtime is current; the
bundled Defold and Phaser packages in this document remain target behavior.

The current product runs strict API-v1 Lua packages through the native Export
IR, capability projection, artifact planner, PNG writer, and rollback-backed
publisher without format-specific packing or publication branches. The target
work here adds the first bundled packages.

The exact package and script contract is
[`../formats/lua-package-v1.md`](../formats/lua-package-v1.md). The first
runtime format is `defold-tpinfo-2`; `phaser-3-multiatlas` then proves the host
is generic. The Phaser bytes are governed by
[`../formats/phaser-3-multiatlas.md`](../formats/phaser-3-multiatlas.md).

## Product boundary

- `json-neotolis` remains a native, always-present reference format.
- Runtime formats are direct child directories of the portable executable's
  `formats/` directory. There is no installer, system share directory,
  environment override, project-local root, recursive search, watcher,
  marketplace, archive import, or in-app code editor.
- A runtime package contains exactly `format.json` and `export.lua`. It has one
  integer API version and no package semantic version, dependencies, modules,
  settings, or dynamic output declarations.
- Projects persist only a stable exporter ID. They do not pin package bytes,
  fingerprints, settings, or an API version. An unavailable ID remains authored
  state and becomes usable again when a matching package returns.
- Import, detection, extraction, foreign-atlas ingestion, and round trips are
  outside this epic and have no implied package contract.
- There is no Template driver. A custom format uses sandboxed Lua; a trusted
  native exporter remains a product-owned built-in.

The portable release contains:

```text
<real executable directory>/
  ntpacker[.exe]
  ntpacker-gui[.exe]
  formats/
    defold-tpinfo-2/
      format.json
      export.lua
    phaser-3-multiatlas/
      format.json
      export.lua
```

The root is derived from the real executable image, never the process working
directory. Both clients use the same resolver and receive the root explicitly;
there is no public format-root option.

## Shared export ownership

Native and Lua handlers consume the same immutable Export IR v1 and exact
capability vocabulary. Capability adaptation, transform-mask intersection,
loss notices, pack grouping, PNG generation, concrete filenames, dry-run
semantics, staging, leases, rollback, and publication remain common-core
responsibilities.

Lua receives a target-projected read-only view. Unsupported metadata is removed
and diagnosed before the handler runs. A handler sees no raw model, pack result,
page pixels, mutable pointer, concrete output directory, or filesystem API. It
can write only the descriptor's fixed text documents through bounded host-owned
writers. PNG pages remain core-owned.

Serialization and publication become two explicit stages:

1. `serialize_and_validate_documents` executes the native or Lua handler in
   memory and validates the complete declared document set;
2. `publish_documents` polls cancellation, obtains destination leases, writes
   staging entries, verifies the set, and performs the existing rollback-backed
   swap.

Dry-run executes stage 1 and discards the documents. It creates no output lease,
staging entry, PNG, or metadata file, while returning the same artifact plan,
notices, and handler diagnostics as a wet run up to publication.

## Catalog and ownership

The target replaces the fixed process-global format lookup with an opaque,
owned, immutable catalog. One CLI invocation owns one catalog. The GUI owns one
active generation and a temporary candidate during Reload. Sessions retain the
generation used for validation and admission. There is no mutable global active
catalog.

Catalog creation is two-phase:

1. the client host performs bounded, handle-based discovery and strict
   descriptor/source byte admission without loading Lua;
2. the existing self-reexec worker transport performs protected text-only Lua
   compilation and returns deterministic per-row diagnostics.

CLI and GUI processes never create a `lua_State`. A catalog becomes eligible
only after complete discovery and compile validation. An eligible catalog is
installed atomically. An incomplete scan, host OOM, unidentified worker reply,
or exhausted catalog-level validation budget retains the native-only startup
baseline or the previous active GUI generation.

Missing `formats/` is a successful native-only catalog. Broken children remain
unavailable rows. Native IDs are reserved; two runtime packages claiming the
same ID make both unavailable. Discovery order never affects row order or which
package wins.

## Exact job binding and containment

Pack-preview and every Export target capture either an exact immutable format
binding or an exact unavailable result at admission. Bindings contain handler
kind, descriptor, API, content fingerprint, and the admitted Lua source bytes;
they are deduplicated by `(format ID, fingerprint)`. Worker code never reopens
`formats/` or resolves a later package by ID.

Saved-file CLI dry and wet Export use the same outer self-reexec job boundary as
the GUI. The existing inner builder worker remains. Runtime topology is:

```text
CLI or GUI host -> isolated Pack/Export job worker -> inner builder worker
```

The outer worker owns all Lua C API use, panic termination, cancellation, and
the existing five-minute process timeout. There is no second nested Lua worker
and no Lua-specific wall-clock option. Memory, instruction, host-call, output,
notice, diagnostic, protocol, and restart ceilings are fixed in the Lua package
contract and cannot be raised by a descriptor.

## Reload Formats

Reload is a GUI runtime action, not a project operation. It changes no project
revision, dirty/Save state, transaction, or Undo history and never starts Pack.

The Reload FSM:

1. coalesces repeated requests and blocks new Pack/Export admission;
2. cancels any active Pack or Export once and pumps it to a real terminal
   receipt; an Export beyond its publication boundary finishes normally;
3. neither cancels nor waits for an independent Refresh;
4. builds and compile-validates a complete candidate catalog;
5. atomically swaps only an eligible candidate, updates the session reference,
   and recomputes validation/presentation;
6. unblocks admission and publishes one bounded summary.

A successful Reload mirrors disk: deleted packages disappear and no old package
bytes remain as fallback. Authored target IDs remain. A failed candidate keeps
the active generation unchanged.

## Diagnostics and client equivalence

`tp_status` plus value-owned `tp_error` remains the primary C-boundary result.
An additive owned bounded format-diagnostic report carries severity, stable
code, phase, format ID when known, logical package path, optional line/column,
message, sanitized Lua-only frames, and truncation. Discovery errors persist on
catalog rows; runtime failures affect only that target invocation.

`ntpacker formats` and `ntpacker formats --json` are the detailed catalog
authority. The JSON payload uses schema 1; Pack/Export reports use schema 2.
The planned GUI Formats surface and Reload action must consume the same core
rows and reports without duplicating parsing or policy.

## First formats and proof

`defold-tpinfo-2` is implemented first. The current native `defold` serializer
remains a transition oracle until Lua matches its full byte and notice matrix in
the isolated worker and real CLI/GUI flows. The native row, old ID, symbols, and
fallback are then removed; unchanged goldens continue as Lua-only tests.

`phaser-3-multiatlas` targets Phaser `>=3.70 <4`, with the example pinned to
3.90.0. It supports identity and clockwise 90-degree rotation, pivots, 9-slice,
multipage, and aliases. Polygons and format-level animations are unsupported and
are visibly diagnosed rather than emulated.

One canonical public project produces committed PNG and metadata goldens for
native, Defold, and Phaser consumers using the same art and scenario. Ordinary
CI validates exporters, bytes, paths, and consumer structure without launching
Defold, Bob, Node, a browser, or a graphical viewer.

## Acceptance

- Adding a valid package changes no pack, IR, PNG, or publication orchestration.
- Missing, malformed, duplicate, newer-API, crashing, or resource-exhausting
  packages produce structured bounded results and never abort a client.
- Every job executes the exact admitted package bytes, even if Reload or an
  external editor changes the directory while it runs.
- Lua has no filesystem, OS, network, process, native-module, bytecode, pixel,
  clock, random, locale, or unrestricted dynamic-loading path.
- Dry and wet runs execute the same pure serializer; handled handler failure
  publishes nothing.
- CLI and GUI expose the same catalog meaning and diagnostics.
