# Engine and client boundaries

**Status:** Current architecture.

## Engine boundary

The project uses public neotolis-engine builder, hashing, UI, filesystem, and
shared binary-format APIs. `external/neotolis-engine` is read-only.

Packer-specific behavior lives above the engine:

- canonical project and operation model;
- source identity and validation;
- capability-based effective settings;
- worker-process containment;
- `.ntpack` parse-back into the canonical result;
- exporter normalization and publication;
- session/history/recovery/client semantics.

If a defect originates in the engine, the durable fix belongs in an upstream
engine issue and PR. Local guards may validate inputs or contain failures but
must not fork a private engine implementation.

The builder is fallible and is treated as such even where engine internals use
assertions. Production job boundaries convert malformed input, worker crash,
timeout, protocol error, and invalid binary output into `tp_status` diagnostics.

## Format and native export boundary

The immutable native handler table is fixed to built-in formats. It is the
always-present prefix of an explicit refcounted `tp_format_catalog`; there is no
mutable active registry. Production does not accept runtime C exporters.
Public clients enumerate catalog rows and descriptor metadata only; native
serializer bindings, the publisher, and raw project/sprite orchestration remain
internal boundaries.

The shared scanner receives an explicit absolute root derived from the real
executable image. It performs bounded no-follow package reads, strict API-v1
descriptor/source admission, exact-byte fingerprints, deterministic duplicate
handling, and owned primary-plus-report diagnostics. Missing `formats/` is a
successful native-only catalog; broken packages remain unavailable rows and a
catalog/root limit fails closed to native-only. Candidate descriptors that need
Lua compilation are a separate scanner handoff. The compile controller
uses the existing self-reexec worker boundary and accepts a catalog batch only
after the exact `REQUEST -> ANNOUNCE -> RESULT` sequence for every row followed
by `END -> COMPLETE`. Results remain separate from the scan until that terminal
frame; a global protocol/process/budget failure makes the whole attempt
ineligible, so a validated prefix cannot be published.

Lua 5.5 state ownership and the Lua C API exist only in worker-side `tp_build`.
Each compile or runtime attempt gets a fresh bounded state. The runtime kernel
accepts prepared projected Export IR, prepared host facts, declared documents,
diagnostic/notice sinks, and cancellation; it owns no filesystem probing, path
planning, pixel access, capability policy, PNG work, or publication. A
standalone bounded binding codec captures exact admitted package snapshots and
unavailable/absent references without reopening `formats/`. Shared CLI/GUI
startup now invokes isolated compilation and atomically installs either the
complete eligible catalog or the native fallback. Export admission sends those
exact bindings through the outer job protocol; only the worker can execute Lua.
Each built-in binds two separate pieces:

- a descriptor with stable ID, display name, exact D4 transform mask, other
  capabilities, and declared document artifacts;
- a memory-only serializer for those declared documents.

The shared layer materializes and validates target-neutral immutable Export IR
v1 with final names, page facts, geometry, transforms, aliases, and explicit
animations. Raw page pixels are not serializer-visible. One IR is built for each
distinct effective pack result and reused by every compatible target. The format
descriptor is bound separately into the artifact plan, which is the sole
authority for concrete filenames. Serializers consume the IR and plan and return
memory documents; they do not write files or enumerate outputs.
Common host-fact preparation may perform a bounded, read-only upward probe for
`game.project` when a descriptor declares the Defold project-resource fact. Lua
receives only the final normalized resource string, never filesystem or path
access.

After full preflight, the serializer produces and validates the complete owned
document batch without publication side effects. Wet execution then acquires
non-blocking OS-backed leases for the plan's destination files in stable path
order immediately before publication. A partially overlapping Export therefore
returns `export_busy` after serialization but before staging or artifact writes;
disjoint output sets remain independent. Dry execution never acquires a lease.
Permanent `.ntpacker-export.lock` sidecars provide rendezvous names,
while ownership is the live OS handle and therefore ends automatically when a
process exits. Sidecars are coordination infrastructure, never artifact content,
and are not removed or inferred from later.

The core writes every planned document and page PNG into sibling staging,
verifies the complete planned set, and publishes it with rollback for handled
failures. Abrupt process termination may leave private staging/backup entries;
later exports do not infer ownership of or clean them. Apart from the permanent
lease sidecars, files outside the current plan remain untouched. Capability
adaptation and loss reporting also have one shared owner: the project boolean
expands to an internal D4 mask, the format mask is intersected before pack
grouping, the exact effective mask reaches neotolis-engine, and dry/wet exports
analyze the same effective settings and IR. A descriptor that declares
single-page output rejects a multi-page IR before its serializer runs;
serializers do not reimplement capability policy.

Native `json-neotolis` and the bundled `defold-tpinfo-2` and
`phaser-3-multiatlas` Lua packages all use this path. The production catalog has
no generic native registration API; test binaries may build a private immutable
catalog for capability and failure coverage.

## Client shapes

The core names product client shapes independently of transport:

| Capability | File CLI | Native GUI | In-process live headless |
|---|---|---|---|
| Transaction | not applicable | available | available |
| Persistence | available | available | available |
| Events | not applicable | available | available |
| History | not applicable | available | available |
| Recovery | not applicable | available | available |
| Pack job | not applicable | available | available |
| Export command | available | available | available |
| Source Refresh job | not applicable | available | available |
| Async inspect job | not applicable | not implemented | not implemented |
| Async validate job | not applicable | not implemented | not implemented |

The table describes the typed core capability matrix, not deployed transports.
There is no current MCP or Dev API server.

## File CLI

Ordinary CLI commands are one-shot saved-file workflows. Inspect and validate
load immutable snapshots without taking the writer lease. Mutations open a
short-lived writable session, submit the same typed operations, and save through
the same persistence contract. Pack/export remain synchronous commands with
versioned machine payloads, but construct and drain the same immutable build
job used behind live session execution; there is no second CLI pack/export
orchestrator.

The CLI is not a back door into an already-open GUI session. A conflicting live
writer produces `project_live`.

Each CLI invocation owns one catalog derived from
`<real-executable-directory>/formats/`. The native GUI host owns one active
generation, passes it into every replacement session, and temporarily owns one
complete candidate during Reload. Both clients use
the same `apps/common` process-host workflow for executable-path resolution,
scan admission, isolated compilation, and native fallback. Its owned result
keeps the active catalog, explicit active/fallback state, and any root-failure
report; clients do not reconstruct or discard that policy independently. A
complete compile batch is installed atomically; a global scan/compile failure
uses the immortal native baseline. Build staging recreates the
executable-relative root for both apps from one explicit production package
manifest; it currently stages the bundled Defold and Phaser packages. Package
changes require explicit GUI Reload or process restart.

Lua source and the Lua C API are reachable only from the re-executed outer
worker. Admission captures exact descriptor/source bytes plus package identity
from one immutable catalog generation; execution never reopens the formats root
or late-resolves a changed package. Native and Lua serializers produce the same
owned document batch for common validation, dry discard, or rollback-backed
publication. The GUI Formats modal exposes the active rows and a Reload action;
the CLI exposes the schema-1 `formats` query.

## Native GUI

The GUI owns one small session host. Views only declare drafts or call typed
`gui_request_*` ingress from `gui_actions.h`. The host alone includes
`gui_actions_driver.h` and calls `gui_actions_step` once between frames; that
one function drains intents, advances the internal project FSM, consumes typed
task/lifecycle terminals, and reconciles presentation against the newly
borrowed view. Dev/test blocking adapters are isolated in `gui_actions_dev.h`
and also drive this actions step. Cancel is a deferred request, not a direct
view-to-session mutation.

Intent drain is observation-aware: the first mutable session call closes the
borrowed cut and ends that drain pass. Remaining inputs stay owned by the
actions controller, `gui_project_step` publishes the next cut, and a later
controller tick resumes them. This sequencing is internal state, not knowledge
required from views, dev tooling, or other callers.

The New/Open/Exit confirmation flow is a tagged FSM owned by the actions
controller: `idle`, `resolve-draft`, `resolve-dirty`, and `open-dialog`. The
view receives a passive state value and returns one typed choice (`accept`,
`discard`, or `cancel`) only for a resolve phase. The synchronous OS picker is
the terminal input for `open-dialog`; that tagged phase retains exclusive
controller ownership from the Open request through picker selection or cancel.
Startup recovery is a second typed modal FSM (`idle`, `choose`,
`resolving`) with passive row access and one typed recovery choice. Neither flow
has public continuation, modal, or mailbox flags. Host bootstrap, shutdown, and
blocking dev/test operations use action-controller helpers, so they do not
reproduce the begin/step/consume sequence.

An active or pending lifecycle flow owns the whole action-controller tick, not
only its dialog phase. Edit drafts, semantic intents, history requests, file
dialogs, and jobs remain queued until the typed lifecycle choice reaches a
terminal and its resulting observation cut is published. Blocking adapters
likewise wait for a controller step that both enters and leaves with the task
slot idle; an automatic Refresh queued behind another task is admitted first,
and either admission or terminal failure is propagated rather than treated as
quiescence.

A pending lifecycle request also precedes a pending Reload Formats request. An
already-active Reload owns its Pack/Export drain to a real terminal; later
lifecycle requests remain queued. Host shutdown pumps that Reload owner before
starting the shutdown lifecycle, so it never converts an ordinary drain into a
forced teardown.

`gui_project_step` is an internal host primitive and the sole live-session
update driver. The project host owns `tp_session_update`, task completion, and
active/candidate lifecycle cutover. Its explicit states are closed, active,
intent-specific draining, and ready-to-cutover. A caller never assembles
apply/pump/poll/end phases and never reconstructs lifecycle state from pointer
and flag combinations. Architecture gates keep the project step callable only
by the actions controller, session update/admission out of other GUI modules,
and both host-driving headers out of views.

The thin mutation adapter still submits one typed operation batch. Source rows
are rendered from the session-owned immutable runtime projection; the GUI does
not scan or fingerprint source files. There is no in-process transport/client
mirror.

GUI presentation state—selection, filter, scroll, modal state, draft text,
preview playback, GPU resources—is not persisted unless explicitly represented
by the project schema.

## Live headless shape

The in-process live-headless shape uses the same session, events, history,
recovery, pack, and export APIs as the GUI. It exists so future transports can
adapt those contracts without moving business rules into RPC handlers.

Transport discovery, authentication, ownership transfer, consent, method
schemas, and MCP resources are target work described in
[`../spec/automation.md`](../spec/automation.md).
