# 0018 — Fallible builder containment

**Status:** accepted
**Date:** 2026-07-17
**Scope:** `tp_build -> neotolis-engine` packing boundary

## Context

The engine builder currently has two properties that cannot form a production
error boundary: user-reachable conditions can reach `NT_BUILD_ASSERT`, and output
is opened through a narrow fixed-size path. Core-side image decoding and complete
preflight remove known bad-input assertions, but they cannot contain allocation,
codec, write, or future assertion failures. An assertion in the GUI or CLI process
terminates the host and violates the structured-error invariant.

The engine submodule is read-only here. A future engine issue/PR can add a fallible
memory/sink API, but the packer architecture cannot make its safety depend on an
unreleased upstream change.

## Decision

The production builder call is isolated behind a private worker process until a
fallible upstream API is available and proven.

- The parent core performs bounded UTF-8 reads, canonical RGBA8 decode, semantic
  validation, name/capability resolution, and final artifact publication.
- The worker protocol is versioned and bounded. It carries validated settings,
  raw pixels, dimensions and sprite metadata; it never receives arbitrary source
  paths.
- Builder-visible files use generated ASCII relative names inside a private
  staging directory. The parent accesses that directory through the shared UTF-8
  filesystem backend.
- The parent owns lifecycle, timeout/cancellation and, on Windows, a Job Object so
  cancellation or parent death cannot strand a worker tree.
- Worker crash/abort, malformed reply, timeout, missing artifact and non-zero exit
  become structured builder diagnostics. The last successful preview remains
  authoritative.
- The ordinary clients and future live clients use the same `tp_build` contract;
  none may invoke `nt_builder` directly.

An upstream fallible API may replace the worker only when it provides explicit
error returns for all user/resource/I/O failures, a memory or caller-owned sink
for output, bounded ownership, cancellation, and strict UTF-8 path behavior where
paths remain. That replacement is internal and does not change client contracts.

## Executable evidence required to close the slice

- a deliberately crashing worker returns `builder_crashed` while the host and
  prior result survive;
- malformed/truncated/oversized protocol messages fail closed;
- Unicode and long parent paths pack through ASCII relative worker staging;
- cancellation terminates the worker and removes unpublished staging;
- output open/write/full-disk failures are structured and publish nothing;
- normal in-process baseline fixtures remain byte-identical through the worker.

## Consequences

There is one extra process launch and a bounded raw-pixel transfer on a Pack job.
Pack is already explicit, heavy work, so correctness and host survival take
priority; measurements decide whether shared memory is needed. Until the worker
or a qualifying upstream API lands, the repository must report builder output
failure containment as an open foundation blocker rather than claiming a
crash-proof core.
