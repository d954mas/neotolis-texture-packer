# Architecture overview

**Status:** Current architecture.

Neotolis Texture Packer is one native application with a shared core and thin
clients:

```text
saved project / live operator
              |
      CLI or native GUI
              |
     typed operations + session
              |
 project model / history / recovery
              |
 source scan + pack/export jobs
              |
       neotolis-engine builder
```

## Ownership layers

- `packer/tp_core` owns the project model, canonical identity, validation,
  operation catalog, transactions, session, snapshots, persistence, history,
  recovery, source scanning, exporter descriptors, and pack-result metadata.
- `packer/tp_build` owns fallible builder execution, worker-process transport,
  packing, parse-back, and export orchestration.
- `apps/cli` parses saved-file commands and renders human or versioned JSON
  responses. It does not own live-session policy.
- `apps/gui` owns native presentation, drafts, intent scheduling, and the
  process host around one shared session. It does not implement a second project
  mutation engine.
- `external/neotolis-engine` is a read-only dependency. The packer uses public
  engine APIs and shared format headers.

Clients never bypass core validation, canonical naming, transaction,
revision/dirty, persistence, or Undo rules.

## Current and target boundaries

The current product has a file-oriented CLI, a native live GUI, and an
in-process live-headless session capability shape. MCP and Dev API transports
are not implemented. Their target contract is in
[`../spec/automation.md`](../spec/automation.md).

Current format support is two built-in exporters plus runtime C exporter
registration. The unified package registry, Import/Export IR, linked atlas
sources, templates, and sandboxed Lua are target architecture, documented in
[`../spec/format-ecosystem.md`](../spec/format-ecosystem.md).

## Primary durable contracts

- [Project format v5](../formats/project-v5.md)
- [Model, operations, and session](model-operations-and-session.md)
- [Persistence and recovery](persistence-and-recovery.md)
- [Jobs, pack results, and cache](jobs-pack-and-cache.md)
- [Sources and raster ingress](sources-and-raster.md)
- [Engine and client boundaries](engine-and-client-boundaries.md)

Plans, research logs, review history, and old implementation decisions are not
runtime authority. Code and executable tests prove the implemented state; the
current and target product specs define intended behavior.
