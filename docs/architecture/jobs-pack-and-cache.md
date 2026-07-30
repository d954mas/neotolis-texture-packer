# Jobs, Pack, and result cache

**Status:** Current architecture.

Pack is an explicit derived job. Export is an explicit external side-effect
command that shares the session's runtime ordering, progress, and cancellation
handle; it is neither a derived job nor a model transaction. Project edits,
external source refresh, Undo/Redo, and cache misses never start Pack
automatically.

## Immutable job input

Pack admission captures an immutable input token containing exactly the model
generation and source-runtime generation. The request envelope separately
captures the session instance generation, request ID, atlas ID, preview target,
canonical serialized model, and resolved source paths. Source bytes are read by
the worker after admission rather than copied into the envelope; the completed
result's `pack_input_hash` identifies the decoded pixels actually consumed.
Completion sequence is result ordering state, not part of the input token.

Later model/source-generation changes do not automatically reject a terminal
receipt. Instance, request, and target identity still gate adoption. The core
then compares input tokens for freshness: a changed token keeps the receipt
observable but marks it `STALE` with `TOKEN_CHANGED`, so it cannot be presented
as a current preview.

## Shared build workflow and containment

Saved-file CLI Export and the live session worker both construct the same
immutable export snapshot job. That executor owns pack-input assembly,
effective settings, normalization, target publication, and structured
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

The session runtime admits at most one concrete Pack job or Export command at a
time. A second start returns `busy`. The small GUI project host owns only the
active/candidate session pair and explicitly cancels and drains an old task
before replacement. A superseded completion is never presented as current.

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
captured model, effective settings, pack core, normalization, and target
writers, then publishes external files.

Each target's declared output set is all-or-none. A command spanning multiple
targets can nevertheless finish with `partial_publication`, and a direct-writer
failure can report `publication_uncertain` when the writer cannot prove that it
left no artifacts. Ordinary project Save does not export, and Export does not
silently mutate project configuration.
