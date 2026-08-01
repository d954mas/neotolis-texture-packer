# Future template and Lua export formats

**Status:** Target contract for a later epic. Native Export IR, descriptors,
artifact planning, and built-in serializers are current architecture and are
documented in
[`../architecture/engine-and-client-boundaries.md`](../architecture/engine-and-client-boundaries.md).

## Scope split

The current native export core is the foundation, not a package system. It owns:

- target-neutral immutable, versioned Export IR, materialized once per distinct
  effective pack result;
- exact capability projection before packing;
- declared artifacts and concrete artifact planning;
- common PNG writing, staging, verification, and rollback-backed publication;
- the fixed native `json-neotolis` and `defold` handlers.

A later epic may add template and sandboxed Lua handlers over those same
contracts. Import, linked atlases, detection, extraction, project-wide targets,
format archives, installation flows, and compatibility profiles are not part of
the current export epic and have no approved implementation contract here.

There is no production runtime C registration surface. A custom format must use
the future template or Lua driver; adding one must not modify pack or publish
orchestration.

## Shared handler contract

Built-in, template, and Lua handlers use the same logical descriptor vocabulary:

- stable format ID and display name;
- exact D4 transform-value mask and other output capabilities;
- ordered declared document artifacts;
- one serializer invocation over immutable Export IR and the artifact plan;
- structured diagnostics and deterministic output.

Future template/Lua drivers receive a transitively read-only projection of the
IR and plan. The current arena storage used by trusted bundled C serializers is
not itself a plugin ABI or sandbox boundary, and must not be exposed directly to
an untrusted driver.

The artifact plan remains the only owner of concrete filenames. A handler may
reference planned paths in its document contents, but it may not invent or write
additional files. The common core writes all documents and page PNGs and
publishes every current plan entry or leaves the previous contents of those same
destinations intact when the operation returns a handled failure. Abrupt process
termination is not auto-recovered and may leave private staging/backup entries.
Destination ownership is coordinated by permanent `.ntpacker-export.lock`
sidecars whose live OS leases prevent overlapping publications. Other files
absent from the current plan are not owned, scanned, restored, or removed by
Export.

Capability policy is descriptor-driven and owned by the common core. Compatible
metadata loss produces the same structured notices for dry and wet execution;
an artifact-shape mismatch such as multiple pages for a single-page descriptor
is rejected before handler invocation. Handlers encode the admitted projection
and do not recreate capability adaptation rules.

Exact template/Lua descriptor storage and discovery are deliberately deferred.
No `format.json`, archive manifest, package installer, or version matrix is
approved by this contract.

## Template driver

Templates are intended for deterministic text projections. The minimum useful
language supports escaping, ordered iteration, conditions, optional fields,
separator helpers, declared documents, and structured errors. It does not grow
into a general programming language.

## Lua driver

Complex text or binary projections use sandboxed Lua. The sandbox receives only
immutable IR, the read-only artifact plan, bounded helpers, logging, and
structured error construction. It does not expose unrestricted filesystem or OS
I/O, networking, process execution, native modules, or arbitrary module loading.

Memory, instruction, output, and cancellation limits must be enforced before
Lua formats can execute automatically. The exact Lua version and sandbox API are
release decisions for that later epic, based on the then-current supported PUC
Lua release.

## Acceptance for the later epic

The first template and Lua formats must prove that neither driver needs a
format-specific branch in pack grouping, IR construction, artifact planning,
PNG writing, reporting, or publication. Malformed handlers fail with structured
diagnostics and cannot publish a partial set.
