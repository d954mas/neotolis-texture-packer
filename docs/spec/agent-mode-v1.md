# Agent mode v1 design

**Status:** Target contract. The user approved the release choices and work on
the first implementation packet on 2026-08-31. This is not a claim of shipped
behavior or authorization to merge. The accepted direction remains
[automation.md](automation.md).

This design fixes the first release over canonical project v5. It does
not introduce Import, workspace fields, a new project schema, or an AI runtime.
The public wire contract is [agent-v1.md](../formats/agent-v1.md).

The current implementation is P1: new headless sessions, command/operation
discovery, snapshots, transactions, history and close. Saved-project binding,
Save, jobs, IPC, GUI attachment and handoff remain future packets. This document
describes the full target; P1 is not a completed v1 release.

## Decisions carried forward

- One executable has ordinary saved-file CLI and a long-lived agent mode.
- Agents use process input/output tools and documented commands. No specialized
  agent-platform integration is required.
- One agent process binds to one project. It never silently switches projects.
- GUI and headless hosting use the same typed operations and session. GUI is
  the normal host when it has the project open.
- A saved canonical path has one cooperating writer. Unsaved sessions have
  runtime identity. Neither receives a persistent project UUID.
- One external controller is allowed; human GUI edits may continue.
- Save, Pack, Export, Refresh, and model edits retain their distinct effects.
- Transactions produce one revision transition and one Undo entry. Invalid
  external input produces structured errors; internal invariants still assert.
- Authorization remains binary per canonical path, outside the project, with
  Disabled / Ask for each project / Allow all projects. Default is Ask.
- Clean host handoff preserves model, dirty baseline, revision, and semantic
  Undo/Redo. Crash recovery does not promise restoration of that history.

## Approved release choices

These choices were approved in this discussion, not recovered from old plans.

1. **Pull interface.** UTF-8 JSON lines; commands receive replies. Events and
   task progress are queried explicitly. There is no unsolicited event stream,
   subscription system, HTTP listener, or detached daemon.
2. **EOF ends the agent process.** EOF never means Save or discard. A proxy
   detaches from GUI; a headless owner drains work and preserves dirty recovery
   before exit. If recovery was degraded, acknowledged unsaved edits may be
   lost on exit; this is reported, never hidden behind a successful Save.
3. **Headless administration.** The user can manage the same path-consent policy
   through GUI settings or ordinary one-shot CLI commands. Unknown saved paths
   in Ask mode return `authorization_required` until that decision exists;
   there is no self-approval command in the agent protocol. Existing recovery
   list/resolve actions also receive an ordinary CLI surface, so recovery after
   headless exit does not require launching GUI. Their exact administration
   command payloads must be frozen before that packet is built.
   P1 already freezes and reads the private permission-store schema in the wire
   contract, without adding a policy-writing command.
4. **No automatic crash promotion.** A broken connection does not promote a
   cached client snapshot. Reconnect to a proven live host or use explicit
   recovery; only clean handoff transfers a live session in v1.

Choice 3 adds terminal access to existing product decisions, not a second
permission model or a background process. Choice 4 keeps the current recovery
contract instead of reviving the
old, superseded recovery-mirror design.

## Operator scenarios

| Scenario | Required result |
|---|---|
| Agent uses ordinary inspect on an open project file | Saved bytes only, explicitly `state_source: saved_file`; no controller connection. |
| Agent edits a GUI-open project | Connect to that exact session, one transaction, visible author, one Undo. No project-file shadow write. |
| Agent opens an allowed saved path without GUI | Agent process creates and owns the one live session. Edits stay in RAM until Save. |
| Agent starts `--new` | New unsaved runtime session; absolute source paths resolve, relative sources have no base before first Save. No implicit cwd binding. |
| Another agent selects the same project | `controller_busy`; no second writer, queued takeover, or silent replacement. |
| Human edits between snapshot and agent apply | Shared core reports revision conflict. Agent reads and decides again. |
| GUI opens a headless-owned project | Explicit clean handoff; GUI receives the unsaved graph and history, not the last saved file. |
| GUI closes while its controller remains | GUI offers transfer to the connected agent or ordinary Save/discard/cancel. Closing a view does not imply discard. |
| Agent input/output pipe closes | Detach if GUI-owned; preserve dirty recovery and terminate if headless-owned. |

Launch forms are `ntpacker agent`, `ntpacker agent --project <path>`, and
`ntpacker agent --new`. Bare startup is unbound. Exactly one explicit bind may
succeed in that process; closing the binding finishes the process. Multiple
projects use separate processes. There is no hidden last-focused selection.

`--help`/`--json` use the existing CLI conventions. Agent protocol output is
always JSON; `--json` is accepted but redundant. `--quiet` affects stderr only.
Other file-command flags are rejected as usage errors; their behavior belongs
to explicit agent requests.

## State, identity, and ownership

| State/resource | Single owner |
|---|---|
| Authored graph, revision, dirty baseline, retained transaction IDs, semantic history | Existing `tp_model` inside `tp_session`. |
| Project path/lease, source projection, ordered events, active task | Existing `tp_session`, driven on one host thread. |
| Controller binding, consent, IPC endpoint and host generation | Shared application session-host code used by GUI and agent mode. |
| stdin/stdout framing and request correlation | Agent frontend. It owns no second mutable project. |
| GUI drafts, selection, camera, GPU resources and preview cache | Existing GUI owners. |
| Query snapshot or response bytes | One request-owned immutable value; released after delivery. |
| Terminal Pack receipt | Host-owned detached metadata; heavy core result moves exactly once to GUI preview or is destroyed headlessly. |
| Terminal Export/Refresh receipt | One host-owned result; GUI and request encoding borrow it only for a valid lifetime. |
| Recovery journal and live-slot lock | Existing recovery modules attached by the authoritative host. |

The host assigns random 128-bit `session_id`, `host_generation`, and
`controller_id`. IDs are runtime data, not filenames or persistent project IDs.
`session_id` survives a clean handoff and Save As. `host_generation` changes on
handoff or host restart. A new agent process gets a new `controller_id`.

Every state-sensitive request carries the last observed host generation and
revision or observation precondition required by the wire contract. This guards
against applying a request to a replaced session whose revision happens to match.

The host alone advances `tp_session_update`. Transport I/O never calls a raw
session pointer from its own thread. GUI integrates requests at its existing
actions/controller boundary, including lifecycle and draft precedence. The
headless loop drives the same semantic admission; it does not emulate GUI ticks.

## Commands, retries, and observation

The operation catalog and transaction schema already exist. The transport
decodes into those types and uses `tp_session_apply`; it does not replay CLI
verbs or call `tp_model_apply_json` around the session boundary.

The host assigns transaction authorship from the controller binding. Requests
cannot claim `human` or another controller by supplying an `author` string.
User labels remain labels, not identity or authority.

Request IDs correlate replies only. Existing committed transaction IDs are
retained in the core's bounded 4096-ID window. Retrying an ID may return
`duplicate_id`; that is not a replay of the previous successful response.
No transport response cache or second idempotency database is introduced.

Save, Undo, Redo, Export, and cancellation are not made idempotent merely by
giving a request an ID. After a lost response, query state/job outcome and
reconcile. Never automatically replay a side effect or retry across a changed
host generation. `publication_uncertain` and successful-but-uncertain Save
notices remain explicit.

A query pins one immutable observation at the host boundary. Its project,
revision, event cursor, source generation, task and recovery facts describe that
cut. GUI frame views remain borrowed; only a real response lifetime retains an
owned snapshot. Events describe transitions, not complete replayable model diffs.
A cursor gap requires a new snapshot. Neither model revision nor event sequence
alone represents every runtime change.

## Task lifecycle and results

Pack, Export and Refresh keep the existing single task slot. Start returns a
runtime `job_id`; a second start returns `busy`. Queries and permitted edits
remain possible during work. Poll/wait do not block the host's update loop.
There is no automatic job queue or automatic Pack.

The host retains the latest controller-visible terminal receipt until another
task is admitted or the session closes. A forgotten ID returns
`result_expired`; no silent rerun. GUI-initiated tasks share this rule and slot;
they may replace the last receipt, so `job.status` must identify the requested
job exactly.

Pack replies contain detached metadata and the core freshness verdict, not raw
page pixels. Build that receipt before transferring the heavy result's unique
owner to the GUI preview cache; headlessly, destroy the heavy result afterward.
The core result has no public retain operation: a shallow copy cannot make a
second owner. Export/Refresh receipts retain their one owned result and release
it through the existing compact/destroy API. Headless Pack does not create a
second preview cache. Export uses the common artifact plan, dry run, report,
destination leases, and publication rules.
Refresh changes runtime source state only. Cancellation reports the actual
terminal result and does not undo files already published.

## Closing and failure

| Trigger | GUI owns session | Agent process owns session |
|---|---|---|
| Explicit detach / stdin EOF / stdout failure | Revoke this connection; preserve GUI edits and jobs. | Drain/cancel owned work, preserve dirty recovery, release leases, exit. |
| Explicit close with preserve | Detach; GUI stays open. | Same preservation path as EOF; no Save. |
| Explicit close with discard | Reject: agent closure cannot silently close the human editor. | Shared discard decision then drain/close; project file unchanged. |
| Save then close | Separate acknowledged Save, then close. | Separate acknowledged Save, then close. |
| Host connection breaks | No writes until the same host is verified again. | Not applicable to in-process dispatch. |
| Host crashes | Report host loss; no automatic mirror promotion. | Recovery may retain a durable prefix; no invisible daemon is spawned. |

If the human completes Close, New, or Open without transfer, the old binding
ends. Finish/reject the pending request, send the terminal `closed` record,
revoke its token/resume secret, and release its controller slot. The proxy then
exits. This is a normal close, distinct from `host_lost`; lost terminal delivery
remains unconfirmed host loss. The next GUI project is a new session; a
controller must start another process to bind it. A lifecycle Cancel
leaves the old session and controller intact.

Normal protocol rejection keeps the stream usable. Broken framing, over-limit
input, or broken output closes that connection without touching other clients'
state. EOF with a partial JSON line never applies that partial request.

A healthy headless host enables recovery using the shared application-data
location and current recovery policy. Recovery failure does not reject committed
edits, but status and every mutation response carry the degraded notice. Exit
does not pretend that preservation succeeded if it could not be confirmed.
The current destructor already preserves dirty/non-discarded or degraded slots;
do not add another recovery store or implicit Save-on-exit.

Headless recovery is an ordinary file-oriented workflow over
`tp_recovery_scan_root` and `tp_recovery_resolve_journal`: list candidates, then
explicitly discard, Save Original, or Save As. It restores a recoverable project
to disk, not a live session with the old Undo/Redo stack. Existing store-root,
liveness, claim, fingerprint and destination-lease checks remain in core. No
resolving an active journal or passing an arbitrary file as a recovery journal.
Destructive/lossy resolution needs dry-run preflight and structured publication
outcomes; any missing shared core support must be added there, not reimplemented
in the CLI. Exact CLI names, schemas and acceptance fixtures remain a prerequisite
of the persistence packet, not an implicit expansion of the agent wire table.

## Consent and discovery

Connection consent is not a permission prompt for every operation. Disabled
publishes no externally usable endpoint; existing connections are revoked.
It also rejects in-process agent creation, including `--new`; it does not disable
ordinary saved-file CLI or local permission administration.
Ask-mode unknown paths produce one GUI Allow/Deny decision. Denied paths are not
prompted repeatedly until the user changes policy. Display names are untrusted
labels. Direct Dev API clients obey exactly the same gate as agent mode.

An integration-created unsaved project receives a runtime grant. A GUI-created
unsaved project in Ask mode requires one Allow/Deny decision keyed by `session_id`;
this temporary decision expires with the session and is never a persistent UUID.
Allow-all permits both kinds. First Save applies the new path's normal policy;
neither runtime grant silently becomes a saved-path grant.

Terminal permission administration reads and updates the same private store as
GUI: query mode/path decisions, set global mode, allow/deny a canonical path,
or forget its decision. Updates are atomic and serialized, with the host checking
current policy before bound command admission; stale in-memory policy cannot
authorize later commands. No watcher or second settings owner. CLI administration
is an explicit local-user action, not proof that a human rather than a same-user
script invoked it. This policy is not a sandbox against an agent already allowed
arbitrary shell access; agents must not change it without the user's permission.

Discovery uses small advisory records in the private application-data directory,
one per live host. They contain endpoint, runtime identity and canonical path,
never project content or tokens. The endpoint handshake verifies the live host;
record existence or a PID alone never grants writer authority. Reuse the current
project lease and recovery liveness mechanisms where applicable. No broker,
background watcher, installation registry or distributed registry is needed.

Agent mode hides IPC details from the external agent. The same request dispatcher
serves in-process headless commands and authorized local IPC. IPC is restricted
to the current OS user; it is not a sandbox against arbitrary malicious code
already running as that user. No tokens in argv, project files, discovery or logs.

Save As and first Save still publish through the existing core. If successful
publication changes to a path lacking consent, report success plus the new
authorization state, then pause further project operations until that path is
allowed. Status and preservation/close remain usable. Failed Save leaves the
old identity and permission binding intact.

## Clean handoff

This is a separate implementation packet, not a side effect of initial IPC.

1. Serialize ingress; resolve GUI drafts through the existing controller. Drain
   the existing task to a genuine terminal, respecting Export publication.
2. Freeze new mutations. Prepare a validated transfer of model, semantic saved
   baseline, revision, retained transaction IDs, Undo/Redo entries and cursor,
   visible history markers, source projection, recovery binding information,
   controller identity, and the terminal receipt. No raw pointers/OS handles.
3. Destination prepares all fallible model/history resources before taking
   authority. Existing history codecs may be reused; recovery replay is not an
   adequate live-history transfer codec.
4. Old host irrevocably stops admission/publication before releasing authority.
   Destination acquires canonical writer authority and validates saved-file
   fingerprint before becoming writable. If another writer wins the lease gap,
   neither participant may pretend transfer succeeded or overwrite its work.
5. Publish new generation and endpoint, rebind controller with a new connection
   token, force a full snapshot, then retire old ownership/resources.

Before authority release, failure leaves the old host usable. After release,
the old host cannot resume merely because of timeout; it needs proof the
destination is not authoritative, a newly acquired lease, and an unchanged file
fingerprint. Otherwise it preserves recovery and reports `handoff_uncertain`.

GPU resources and full preview-cache residency are not transferred. The new host
may retain/reconstitute a safe result, or show no current preview until explicit
Pack. This never discards model/history or starts Pack automatically. Handoff
must not reconstruct package bindings from unverified discovery metadata.

The internal transfer codec, lease cutover and terminal-result transfer need
their own executable design proof before the handoff packet is approved. This
design does not declare those currently missing core APIs implemented.

## Acceptance

- A plain process-control client can discover commands, bind, inspect, perform
  a two-operation transaction, Undo, and close without a specialized SDK.
- With GUI open, both operators see the same revision and History entry; one
  Undo restores exact previous project meaning, without changing the saved file.
- Invalid/oversized input, stale revisions, wrong generation and revoked tokens
  cannot mutate the session or abort a client.
- EOF while dirty, lost responses, failed Save, uncertain Export publication,
  blocked workers and failed handoff have deterministic, non-loss-concealing outcomes.
- Debug/Release and cross-platform integration tests prove writer exclusion,
  consent and IPC parity; build assertions remain active.
- Product v5 and ordinary CLI wire contracts stay unchanged except explicit
  additive help/version and `state_source` announcements.
- Release is complete only with the full required capability surface, GUI
  authorization/presence and clean handoff. Smaller packets advertise only
  implemented capabilities and are not declared a completed v1 release.
