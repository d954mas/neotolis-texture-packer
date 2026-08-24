# Documentation

Start with the smallest document that owns the question.

## Current product and architecture

- [`spec/product.md`](spec/product.md) — implemented product contract.
- [`architecture/overview.md`](architecture/overview.md) — current component
  and ownership map.
- [`architecture/model-operations-and-session.md`](architecture/model-operations-and-session.md)
  — mutation, revision, history, observation, and session ownership.
- [`architecture/persistence-and-recovery.md`](architecture/persistence-and-recovery.md)
  — writer leases, Save publication, and recovery.
- [`architecture/jobs-pack-and-cache.md`](architecture/jobs-pack-and-cache.md)
  — explicit Pack jobs, Export commands, and result selection.
- [`architecture/sources-and-raster.md`](architecture/sources-and-raster.md)
  — source identity, Refresh, scanning, and image ingress.
- [`architecture/engine-and-client-boundaries.md`](architecture/engine-and-client-boundaries.md)
  — engine, exporter, GUI, CLI, and live-headless boundaries.
- [`spec/format-ecosystem.md`](spec/format-ecosystem.md) — current Lua runtime
  and bundled Defold/Phaser packages; Reload Formats remains target behavior.

## Wire and file formats

- [`formats/project-v5.md`](formats/project-v5.md)
- [`formats/cli-report.md`](formats/cli-report.md)
- [`formats/json-neotolis.md`](formats/json-neotolis.md)
- [`formats/defold-tpinfo.md`](formats/defold-tpinfo.md)
- [`formats/ntpack-binary.md`](formats/ntpack-binary.md)
- [`formats/lua-package-v1.md`](formats/lua-package-v1.md) — current package,
  Lua sandbox, diagnostics, and fixed-limit contract.
- [`formats/phaser-3-multiatlas.md`](formats/phaser-3-multiatlas.md) — exact
  current Phaser consumer wire contract.

## Approved target contracts

These describe intended future behavior and are not claims about current
availability:

- [`spec/automation.md`](spec/automation.md) — Dev API and MCP.
- [`spec/ux.md`](spec/ux.md) — two-level canvas and future workspace model.

Repository-wide working rules remain in [`../AGENTS.md`](../AGENTS.md).
Plans, research logs, reviews, and superseded implementation records are not
durable product authority.
