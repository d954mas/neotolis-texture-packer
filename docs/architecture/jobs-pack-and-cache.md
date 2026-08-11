# Jobs, Pack, and result cache

**Status:** Current architecture.

Pack is an explicit derived job. Export is an explicit external side-effect
command. Refresh is an explicit source-I/O job that produces a replacement
runtime projection. All three share the session's single task slot, progress,
cancellation, and terminal-transfer contract. Export is neither a derived job
nor a model transaction. Project edits, Refresh, Undo/Redo, and cache misses
never start Pack automatically.

## Immutable job input

Pack admission captures an immutable input token containing exactly the model
generation and source-runtime generation. The request envelope separately
captures the session instance generation, request ID, atlas ID, preview target,
canonical serialized model, and resolved source paths. Source bytes are read by
the worker after admission rather than copied into the envelope; the completed
result's `pack_input_hash` identifies the decoded pixels actually consumed.
Completion sequence is result ordering state, not part of the input token.

Preview admission resolves its exporter ID through the session's retained
format catalog. The in-process immutable Export snapshot job also retains and
uses the snapshot's exact catalog generation for target admission and
capability policy. The outer worker protocol still carries only the two native
handlers. A dormant standalone binding codec now defines the bounded snapshot
needed by a later protocol layer, but it is not part of the active job request
or response and no Lua row is admitted by current clients.

Later model/source-generation changes do not automatically reject a terminal
receipt. Instance, request, and target identity still gate adoption. The core
then compares input tokens for freshness: a changed token keeps the receipt
observable but marks it `STALE` with `TOKEN_CHANGED`, so it cannot be presented
as a current preview.

## Shared build workflow and containment

Saved-file CLI Export and the live session worker both construct the same
immutable export snapshot job. That executor owns pack-input assembly,
effective settings, Export IR materialization, target publication, and structured
diagnostics. The clients differ only in admission and delivery: CLI drains the
job synchronously, while a live session transfers one terminal result through
its task slot.

Two process boundaries remain intentional. The outer session-job worker
isolates the complete cancellable Pack/Export command and its filesystem
publication. The inner builder worker contains fallible engine builder code and
also protects direct saved-file CLI execution, which has no outer session
worker. The boundaries defend different failure domains and must not be
collapsed without preserving both direct-CLI builder crash containment and
whole-job cancellation.

The host:

- serializes a bounded request;
- streams progress and one terminal response;
- validates protocol kind, instance generation, request ID, lengths, and
  filesystem outcomes;
- supports cooperative cancellation followed by bounded forced termination;
- owns cleanup of the worker process and private request directory.

Malformed output, non-zero exit, crash, hang, timeout, and partial artifacts
become structured job failures. They do not abort the GUI or corrupt the live
session.

The session runtime admits at most one Pack, Export, or Refresh task at a time.
A second start returns `busy`. Refresh performs source filesystem I/O on its
worker and the session adopts its stable-ID-keyed immutable projection only if
the captured model generation is still current. The small GUI project host owns
only the active/candidate session pair. Replacement requests cancellation and
drains for a bounded grace period; after the deadline it retires the old
session without waiting indefinitely for a blocked filesystem call. A Refresh
worker owns a private job lease until terminal and may release/detach itself
after its session has retired. Its immutable snapshot and projection never
borrow the live session.

The stored GUI lifecycle state is one of closed, active, intent-specific
draining, or intent-specific ready-to-cutover. One internal project step may
advance draining through ready to active/closed and emits one typed lifecycle
terminal; callers do not assemble those phases. Cutover does not depend on
reconstructing state from pointer and flag combinations. A superseded
completion is never presented as current.

Generic task operations dispatch through the concrete owned-job interface.
Cancellation therefore addresses the Pack, Export, or Refresh owner without a
layout cast. A client retaining a terminal receipt beyond its poll boundary
must compact it first; compaction is concrete-owner-specific and idempotent.

## Pack-input identity

`pack_input_hash` is the deterministic semantic identity of everything that can
change a visible Pack result:

- effective packing settings and selected target profile;
- ordered sprite identities and per-sprite overrides;
- canonical decoded RGBA8 dimensions and pixels;
- packer algorithm/version tags.

File bytes, path timestamps, pointers, and native integer layout do not enter
the hash. Size/mtime and encoded-content fingerprints may skip a re-decode, but
the final hash is over canonical pixels. Re-saving identical pixels therefore
keeps the same semantic Pack identity.

## Result selection

Each result accepted into the GUI cache receives a monotonic publication
sequence. In the absence of an explicit selection, the cache chooses the usable
entry with the greatest accepted-result publication sequence. Superseded
requests are rejected by host/session admission before cache insertion, so an
old receipt cannot re-enter later and replace a newer preview.

Undo/Redo or manual selection may name a cached result by semantic hash.
If the corresponding entry is absent, the current preview remains visible but
stale; no Pack is started.

## GUI preview result cache

The reusable result-cache type is single-owner-thread, but current production
cache ownership is in the native GUI adapter, not `tp_session`. The in-process
live-headless shape has no built-in cache. In the GUI:

- every result and preview edge is keyed by stable atlas ID; list position is
  resolved only by test fixtures and presentation iteration;
- the active result remains pinned and hot;
- inactive hot entries retain raw RGBA pages while background compression runs;
- cold entries retain copied geometry plus compressed page blobs;
- promotion restores page pixels in parallel;
- a byte-budget LRU evicts inactive entries;
- the active and highest-sequence safety entries are retained according to the
  cache contract.

Background compression operates on private adapter state and never touches the
cache from its worker thread. Any operation that could invalidate borrowed
pages cancels and joins the compression job first.

Cold decode failure drops only the bad entry and falls back to another usable
candidate. Cache storage is not persisted to disk.

## Pack versus Export

Pack transfers one terminal result through `tp_session_update`. The GUI may
then adopt a successful completion into its preview/cache. Export uses the same
captured model, effective settings, pack core, Export IR materialization, and target
writers, then publishes external files. The final writer callback publishes an
explicit terminal boundary to the host; cancellation after that boundary cannot
rewrite a successfully published Export into Cancelled.

Each target's declared output set is all-or-none. A command spanning multiple
targets can nevertheless finish with `partial_publication`, and a target
failure reports `publication_uncertain` only when publication rollback cannot
prove that the previous artifact set was restored. Serializer, staging,
preflight, and ordinary writer failures leave it false. Ordinary project Save
does not export, and Export does not silently mutate project configuration.
Concurrent exports acquire destination-file leases after preflight and before
serialization. Any overlap returns `export_busy` with no attempted writer or
artifact publication; non-overlapping targets may proceed independently.

Capability policy includes explicit animations. A target that declares no
animation support receives a borrowed IR projection with zero animations and
one atlas-wide structured `animation` loss notice when the source IR is
non-empty.
