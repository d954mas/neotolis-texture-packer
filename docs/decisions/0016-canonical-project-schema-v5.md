# 0016 — Canonical project schema v5 and removal of legacy migrations

**Status:** accepted
**Date:** 2026-07-17
**Scope:** `.ntpacker_project`, structural identity, sprite/frame references, session adoption
**Supersedes:** 0007, 0008, 0009 and every project-schema migration rule derived from them

## Context

The packer is pre-release and has no external projects or compatibility obligations.
Keeping v1–v4 loaders, deterministic synthetic IDs, lazy name-to-key migration and
mixed pending/canonical records would make transitional states part of the permanent
core. That cost is larger than requiring old internal fixtures to be regenerated.

The product needs one representation that every GUI, CLI, MCP and Dev API client can
trust without running a migration state machine.

## Decision

1. The only accepted project format is schema v5. The loader rejects v1–v4 and
   versions newer than v5 with the structured `TP_STATUS_BAD_VERSION` status.
   A missing, non-integer or otherwise malformed version is a bad project.
2. The core does not contain an old-project converter or a compatibility loader.
   Unsupported files are not rewritten, partially adopted or deleted.
3. Atlas, source, animation and target records carry non-nil, kind-correct,
   globally unique structural IDs. IDs loaded from disk are authoritative and are
   never silently replaced.
4. New in-memory entities may be created with nil IDs inside a private candidate.
   The writable session-adoption boundary assigns all missing IDs atomically,
   validates the complete graph, and publishes it only on success. Save,
   checkpoint and externally visible snapshots require the canonical invariant.
5. A source is always a tagged object with `id`, `path` and an optional `kind`
   (`folder` is the sparse default; `file` is explicit). Bare source strings are
   invalid in v5.
6. Persisted sprite overrides and animation frames are always addressed by the
   canonical pair `{source, key}`. A name-only or string frame reference is invalid.
   `key` is the normalized source-local key with its extension preserved.
7. `sprite_id` is never persisted. It is derived from the source structural ID and
   normalized source-local key. Logical/export rename does not change identity.
8. A canonical `{source, key}` record may be orphaned only when its source entity
   and source ID remain in the project graph but the physical source is currently
   unavailable or the source-local key is absent. This is a validation/runtime
   finding, not a migration state; the record becomes active again if the same key
   returns. An unknown source ID is a graph-integrity error, not a saveable orphan.
9. The writer always emits v5. It refuses a noncanonical graph before any file,
   journal checkpoint or save publication is replaced.
10. A target `exporter_id` is a non-empty strict-UTF-8 machine token of at most
    255 bytes. Registry, direct model mutation, load/adoption, typed operations,
    jobs and clients share that bound and never truncate the ID.

## Validation contract

Load/adoption/save validation rejects malformed, nil, wrong-kind or duplicate
structural IDs; duplicate exact source paths; malformed canonical references; and
references that violate graph integrity. A source entity referenced by a canonical
record cannot be removed until those references are removed. Files missing on disk
and canonical records whose known source or source-local key is physically absent
remain reportable model states rather than parser failures.

Save-time path relativization recognizes POSIX, drive and full UNC server/share
roots. Different roots retain an absolute spelling; component depth is bounded
only by the canonical path-byte limit and processing is re-entrant.

## Consequences

- There is one project state space below every client and no `id_synthetic`,
  legacy-hash, lazy re-key or pending name-only branch.
- Checked-in valid projects and examples are canonical v5 fixtures.
- Developers with old pre-release fixtures must recreate or externally convert them;
  the production packer does not carry conversion code.
- Decisions 0010–0015 remain implementation history. They are not normative where
  they describe a legacy/pending project form or contradict this decision and the
  master specification.
