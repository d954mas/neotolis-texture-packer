# Agent / Dev API wire v1

**Status:** Current contract for the first packet described below; all other
commands and IPC/handoff sections are target contracts with explicit
prerequisites. Companion to the
[agent mode design](../spec/agent-mode-v1.md). Existing operation/transaction,
project and CLI payload schemas keep their independent version numbers.

## First implementation packet

The first packet supports only `help`, `capabilities`, `operations.list`,
`session.bind` with `{new:true}`, `session.status`, `session.close`,
`project.snapshot`, `project.apply`, `history.list`, `history.undo`, and
`history.redo`. Startup supports bare agent mode or `--new`; `--project` is
rejected as unavailable, without opening a file. Other commands below are
future capabilities and must not be advertised yet. The initial project uses
the same default project/target constructor as GUI.

P1 owns no background task, GUI connection or IPC endpoint. Its single-threaded
process reads one request and writes one response synchronously. A slow reader
can delay this process only. Job/GUI pumping under backpressure becomes a
requirement when those capabilities are added, not a claim about this packet.

`help` returns `{schema:1,commands:[{command,description,host_generation,
params_schema,result_schema}]}`. Command schemas use JSON Schema draft 2020-12;
closed object properties/required lists describe wire shape, while transaction
admission remains the core's responsibility. Named project/transaction schemas
retain their own versions. The input transaction envelope schema is projected
by the core and included directly in `help` with an agent-only prohibition on
caller-supplied authorship. The operation schema is obtained from
`operations.list`, not copied into a second operation table.

The private policy file is `<user-app-data>/automation/permissions.json`:
`{schema:1,mode:disabled|ask|allow_all,projects:[{path:absolute-string,
decision:allow|deny}]}`. All fields are required and objects are closed. Missing
file means Ask; malformed, inaccessible, duplicate-key or over-1-MiB input
refuses admission. P1 reads the global mode before creating a session and before
each bound project command. Path decisions and policy writing arrive in P2.
Status and preserve-close remain available after refusal. No policy is written
by P1. Agent mode requires the user app-data location; the GUI's executable-local
fallback is not used for agent policy or recovery.

## Framing and request envelope

UTF-8 JSON, one object per LF-terminated line. Readers also accept CRLF. No BOM,
JSON arrays as requests, duplicate keys, embedded NUL, NaN/Infinity, or unknown
members. Blank lines are ignored. Strings use JSON escaping. Stdout is protocol
only; diagnostics go to stderr. There is no JSON-RPC envelope or platform tool
registration exchange.

```json
{"schema":1,"id":"q1","command":"session.status","params":{}}
```

All four fields are required. `id` is an opaque ASCII token of 1..64 characters
from `[A-Za-z0-9_.-]`. It identifies this request, not a durable operation.
`command` is a closed token from the command table. `params` is always an object.
`host_generation`, when present, is a lowercase hex32 token. It is additionally
required on bound project commands, but not
on discovery, status, initial bind, or close-preserve. Wrong generation returns
`host_changed` before any effect. Local IPC adds `token` after authorization;
stdio never requires the external agent to manage IPC tokens.

The client waits for one complete response before sending its next request.
The host admits requests in line order and processes at most one foreground
command at a time; background tasks and GUI continue. An event/job wait lasts
at most 1000 ms so it cannot obstruct another explicit agent action indefinitely.

Inbound line limit is 2 MiB excluding delimiter, checked before materialization.
The nested transaction still has its existing 1 MiB / 4096-operation limits.
Unknown command/argument/version yields a structured rejection. Malformed JSON
gets `id: null` if a valid request ID cannot be obtained without guessing.
An oversized line terminates the connection with `request_too_large`; it is not
drained into an unbounded buffer. Valid rejected requests leave the stream open.

## Responses and large values

```json
{"schema":1,"id":"q1","ok":true,"result":{"state":"unbound"},"notices":[]}
{"schema":1,"id":"q2","ok":false,"error":{"code":"not_bound","message":"Bind a project first","details":{}},"notices":[]}
```

These are complete response objects. They have exactly `schema`, `id`, `ok`,
`notices`, and either `result` or `error`. Notices are structured objects with
`code`, `message`, and `details`. An error has the same fields. Existing core
codes retain their spelling; protocol codes are listed below. Error prose is
never a machine discriminator.

A response is one complete JSON object followed by LF, including large results.
Ordinary partial OS reads/writes do not create another wire envelope or chunk
assembly protocol. Do not truncate a response or interleave its bytes with another
response. Incomplete output means unknown command outcome, never success or an
automatically retryable error.

One immutable response owns its bytes until delivery. The core prepares the
transaction and required publication resources before committing. Encoding
the returned result or writing its bytes can still fail after commit; either
failure closes the stream with an uncertain delivery, never a rejected commit. Do not keep
a process-wide replay cache or silently impose a new project-size limit. Large
read responses may fail with `oom`, like existing snapshot/serialization APIs.
The host must continue pumping GUI/jobs while a slow reader applies backpressure.

## Startup and state DTOs

The first output is one unsolicited ready record, only after successful startup:

```json
{"schema":1,"type":"ready","app_version":"<build version>","controller_id":"11111111111111111111111111111111","state":"unbound"}
```

The only other unsolicited record is terminal lifecycle notification when the
GUI completes Close/New/Open without transfer. Finish or reject the current
request first, then emit `{schema:1,type:closed,session_id:hex32,reason:host_closed}`
and close the stream. The proxy forwards this record and exits 0. An interrupted
delivery does not prove normal closure; report host loss instead. Explicit
`session.close` uses its correlated reply, without an extra closed record.
There are no unsolicited progress, project-change or event records.

For `--new` or `--project`, successful ready has `state: bound` and `session`
containing the Snapshot DTO below, including the initial project and observation.
Startup failure emits one error response with
`id: null`, then exits. Use existing CLI exit categories: 2 invalid argv, 3 bad
project, 8 lease/file/authorization refusal, 1 infrastructure failure. After
ready, command errors do not set a pending process exit failure. Clean close
exits 0; broken framing/I/O or unconfirmed dirty preservation exits 1. Complete
request errors and process-lifecycle errors are distinct.

`session.status` returns either `{state: unbound}` or a Status DTO:

```text
state: bound | authorization_pending | host_lost | closing | closed
session_id: hex32
host_generation: hex32
host_kind: gui | headless
canonical_path: string | null
revision: nonnegative JSON-safe integer
dirty: bool
event_sequence: decimal u64 string
model_generation: decimal u64 string
source_generation: decimal u64 string
authorization: allowed | pending | denied | disabled
controller_id: hex32
recovery: {available:bool, degraded:bool, code:string|null}
job: null | Job DTO
```

Unreachable-host status uses last observed values plus `observed: false`; reachable
status uses `observed: true`. Cached fields never authorize a write. Runtime u64
counters use canonical decimal strings without leading zeroes except `"0"`.
Transaction revisions retain existing numeric schema semantics; values outside
the exact JSON integer range are rejected rather than rounded. Structural IDs
retain the existing prefixed kind-specific representation.

Snapshot DTO is `{status:Status,project:canonical-v5-object}` from one immutable
host cut. Its observation token is the tuple of `session_id`, `host_generation`,
`revision`, `event_sequence`, `model_generation`, and `source_generation` already
present in Status; it is not a second separately advancing counter. Every successful
initial bind and handoff/resynchronization supplies this full DTO.

## Commands

All parameter sets are closed. `?` marks optional members; omitted values use
only the defaults explicitly stated here. `generation` below means the request
envelope's mandatory `host_generation`. Paths supplied over the protocol must be
absolute, normalized through existing core path identity; startup argv paths may
be relative to launch cwd. Source paths inside project operations retain the
current project-relative semantics.

An unsaved `--new` session has no source base. Absolute source paths work;
relative ones stay unresolved until a base exists. First Save preserves their
relative spelling under the existing core rules; it does not silently reinterpret
them against launch cwd. Save As of an established project preserves source
identity through the current core rebasing behavior.

| Command | Params | Result / effect |
|---|---|---|
| `help` | `{command?:string}` | Versioned command parameter/result schemas; omission lists all currently supported commands. |
| `capabilities` | `{}` | `{agent_schema:1,project_schema:5,transaction_schema:1,commands:[string],operations:[string],limits:object}`; only implemented features. |
| `sessions.list` | `{}` | `{sessions:[Discovery DTO]}` sorted by canonical path then session ID; no live project content. |
| `session.bind` | `{path?:string,session_id?:hex32,new?:true}` | Exactly one selector; binds once. Path attaches to existing owner or opens headless; session ID attaches only; new creates unsaved. Returns Snapshot DTO. |
| `session.status` | `{}` | Current Status; usable while unbound, pending or disconnected. |
| `session.close` | `{decision:preserve|discard,expected_revision?:integer}` | Preserve needs no revision; discard requires generation and expected revision and headless ownership. Drain then `{closed:true,preserved:bool}`. No implicit Save. |
| `project.snapshot` | `{}` + generation | Snapshot DTO; read-only, no Refresh. |
| `project.inspect` | `{}` + generation | Existing inspect schema-4 object inside `{status:Status,report:object}`; live snapshot rather than disk. |
| `project.validate` | `{}` + generation | Existing validate schema-2 object inside `{status:Status,report:object}`; count/truncation semantics unchanged. |
| `operations.list` | `{}` | `{schema:1,operations:[Operation Descriptor]}` generated from current shared catalog; reserved unimplemented kinds excluded. |
| `project.apply` | `{transaction:TxnEnvelope,dry_run?:bool}` + generation | Apply Result below, nesting existing transaction result schema 1; dry_run defaults false and uses snapshot preview with no ID retention/Save/history. |
| `project.save` | `{expected_revision:integer}` + generation | Save Receipt; unsaved project returns `save_path_required`. |
| `project.save_as` | `{expected_revision:integer,path:string,overwrite:bool}` + generation | Save Receipt; false uses create-only publication, true normal core Save As safeguards. |
| `history.list` | `{}` + generation | `{status:Status,entries:[History Entry]}` from one host cut. |
| `history.undo` / `history.redo` | `{expected_revision:integer}` + generation | Status after shared Undo/Redo; no automatic retry. |
| `sources.refresh` | `{}` + generation | `{job:Job DTO}`; explicit whole-session Refresh. |
| `pack.start` | `{atlas_id:AtlasID,preview_exporter_id?:string}` + generation | `{job:Job DTO}`; omitted exporter uses native atlas settings. |
| `export.start` | `{atlas_id?:AtlasID,target_exporter_id?:string,out_dir?:string,dry_run?:bool}` + generation | `{job:Job DTO}`; omitted filters mean whole project/all enabled targets; dry_run defaults false. |
| `job.status` | `{job_id:string}` + generation | Job DTO plus typed terminal result when complete. Never returns another job's outcome. |
| `job.wait` | `{job_id:string,timeout_ms?:integer}` + generation | Same result as status; timeout 0..1000, default 1000; running remains running, not an error. |
| `job.cancel` | `{job_id:string}` + generation | `{accepted:bool,job:Job DTO}`; existing core cancellation race decides accepted. |
| `events.next` | `{after:string,limit?:integer,timeout_ms?:integer}` + generation | `{events:[Event],next:string,resync_required:bool,status:Status}`; limit 1..64 default 64, timeout 0..1000 default 0. |
| `formats.list` | `{}` + generation | Existing formats schema-1 report from authoritative host catalog, not the proxy executable's catalog. |
| `view.reveal` | `{atlas_id?:AtlasID,source_id?:SourceID,animation_id?:AnimationID,target_id?:TargetID,sprite?:{source:SourceID,key:string}}` + generation | `{revealed:true}` or `view_unavailable`; no model change. Empty focuses project; at most one subordinate target and its atlas. |

Pack/Export admission additionally takes an `expected_input` object in params:
`{model_generation:string,source_generation:string}`. Values must match the
current core input token before admission. This prevents operating on a different
model/source projection than the agent inspected without inventing new freshness
policy. Changes after admission use existing core stale-result semantics.

An Operation Descriptor contains `op`, `effect`, `target_kind`, `label`,
`label_template`, `input_schema`. The latter is a draft-2020-12 JSON Schema for
the complete canonical operation, including `op` as a constant, closed
properties and required fields. `target_kind` is `atlas`, `source`, `animation`,
or `target`. Property metadata includes `title` and applicable `x-group`,
`x-clear`, `x-inherit`, `x-cap-key`, and `x-enum-tokens` values. Numeric wire enums
remain numeric. The owning catalog contains typed SET-family fields and
addressing/create/frame arguments. Both wire admission and descriptor
projection reuse that vocabulary; there is no adapter-owned operation table.
Reserved core operations are excluded from both discovery and agent admission.

Live inspect/validate use the bound session's captured project/source projection.
The current CLI inspect orchestration also probes and scans files; it cannot be
reused wholesale and labeled with an older observation token. Extract only pure
payload encoders. A query never implicitly Refreshes or reports a new filesystem
scan as the already-observed source generation.

`TxnEnvelope` is the unchanged core shape, nested rather than translated:

```json
{"schema":1,"transaction":{"id":"22222222222222222222222222222222","expected_revision":0,"label":"Rename atlas","operations":[{"op":"atlas.rename","atlas_id":"atlas_33333333333333333333333333333333","name":"characters"}]}}
```

Agent requests omit `author`; an explicit one is rejected with `invalid_argument`.
Host admission assigns `agent(<controller_id>)` before invoking the shared typed
transaction boundary. Apply Result is exactly
`{mode:apply|dry_run,transaction_result:CoreTxnResult}`. The nested value preserves
the entire core `{schema:1,result:...}` object, compacted onto one physical line.
For dry run, its verdict describes the hypothetical snapshot application, never
a commit to the live session or a reservation for a later call. A semantic no-op
retains core `status:no_change`, including its empty `errors` array.

Example successful apply and semantic no-op:

```json
{"schema":1,"id":"q4","ok":true,"result":{"mode":"apply","transaction_result":{"schema":1,"result":{"operations":[{"op":"atlas.rename","atlas_id":"atlas_33333333333333333333333333333333"}],"revision":1,"status":"committed","transaction_id":"22222222222222222222222222222222"}}},"notices":[]}
{"schema":1,"id":"q5","ok":true,"result":{"mode":"apply","transaction_result":{"schema":1,"result":{"errors":[],"revision":1,"status":"no_change","transaction_id":"66666666666666666666666666666666"}}},"notices":[]}
```

A dry-run success has the same nested core result and `mode:dry_run`; its returned
revision is hypothetical, so it must not replace the client's observed revision.
Rejected typed transaction results use `ok:false` with
`error.details:{phase:admission,mode:apply|dry_run,transaction_result:CoreTxnResult}`.
Request decoding can fail before a typed transaction/result exists: then details
are `{phase:decode,field:string|null}`, with the decoder's `tp_error` code/message.
Do not fabricate an empty transaction result or promise collect-all structural
errors. `tp_txn_request_decode_n` returns its first structural failure; admission
preserves the shared result's actual diagnostic collection.

Save Receipt has `saved`, `target_path`, `file_fingerprint`,
`file_durability_degraded`, `file_durability_code`, `recovery_degraded`,
`recovery_code`, and `status:Status`. Nullable codes are null when healthy.
Successful publication is `ok:true` even when durability/recovery is degraded.
There is no automatic Export, retry or Undo record for Save.

History Entry is the current `tp_session_history_entry` value projection:
`kind`, `revision`, `label`, `author`, `transaction_id`, `state_identity`, `path`,
`undoable`, `undone`. Kinds are `edit`, `save_checkpoint`, `runtime_refresh`.
Non-applicable strings/identity use null. Label and author retain current display
bounds; these strings must not be used for controller authentication or replay.

Event projects the existing six session event kinds into `model_committed`,
`undone`, `redone`, `saved`, `discarded`, `source_runtime_changed`, with `sequence`,
`revision_before`, `revision_after`, `admission_sequence`, `model_generation`,
`source_generation`, `transaction_id`, `label`, `author`. Counters use decimal
strings; optional identity/text fields use null. `resync_required:true` returns
no events and `next` equal to the current high-water mark. History and events
are not interchangeable or sufficient to reconstruct a project snapshot.

Job DTO contains `job_id` (opaque runtime token), `kind:pack|export|refresh`,
`state:running|succeeded|failed|cancelled`, `status_code`, `message`,
`input_token:{model_generation,source_generation}`, `result` (null until terminal).
Pack result has atlas ID, sprite/missing-source counts, pages `{index,w,h}`, input
hash, core freshness and reason. Export result embeds current schema-2 report
plus `partial_publication` and `publication_uncertain`. Refresh result has
`added`, `removed`, `changed`, `unavailable`. All report policy stays in core;
shared pure payload encoders may be extracted from CLI, never invoked by
running a second saved-file command.

## Dev API discovery and binding

The local IPC byte framing, commands and responses are the same as stdio.
There is one semantic dispatcher. Stdio owns launch/open behavior; an IPC
connection can only bind the session hosted at that endpoint, never tell a GUI
to replace its project through `new` or an unrelated path.

One advisory UTF-8 record per host lives under
`<private-app-data>/automation/hosts/<host_generation>.json`:

```json
{"schema":1,"session_id":"44444444444444444444444444444444","host_generation":"55555555555555555555555555555555","host_kind":"gui","canonical_path":"C:/art/game.ntpacker_project","pid":1234,"transport":"named_pipe","endpoint":"<OS-local endpoint>"}
```

`transport` is `named_pipe` on Windows or `unix_socket` on Linux/macOS. Endpoint
spelling is platform-owned; clients use the returned value, not guessed path
hashes. Records contain no secrets. Atomic update follows Save As; graceful
host exit removes only its own record. Dead/stale entries are ignored after
handshake/liveness checks. A failed handshake never authorizes opening a second
writer while the shared project lease remains held.

IPC binding adds `controller:{id:hex32,name:string}` and optional
`resume_secret:hex32` to `session.bind` params. The host validates the target
session/host identity, current-user endpoint access and path consent. Success
returns the Snapshot DTO plus `token:hex32` and `resume_secret:hex32`. Both come from OS
entropy through the existing RNG seam. The secret is process-memory-only and
rotates on a successful resume; explicit disconnect/revoke invalidates it.
Every connection gets a new token. All later IPC commands carry that token;
the stdio adapter retains it privately and never prints it.

On transport failure the host invalidates the connection token but retains the
controller claim and resume secret while the controller process is proven live.
A valid resume rebinds that controller to the same session. A new process may
not impersonate an old controller using a display name or ID. Replacement needs
explicit GUI action or proof the prior process is dead, not a short timeout.
PID reuse and advisory metadata cannot substitute for OS process-lifetime proof.
The platform proof implementation is a required IPC prototype/test, not a new
portable process-management framework.

Ask-mode missing consent returns `authorization_required` and offers the pending
connection in GUI. Bind may be retried after the human decision; no transaction
waits inside a modal or is admitted before approval. A busy controller returns
`controller_busy`. Permission fields on ordinary agent commands cannot approve,
replace or bypass a controller. `session.close` over IPC always means detach;
headless host termination is controlled by its owning agent process, not a
foreign client.

## Error vocabulary and result meaning

Protocol-owned codes: `bad_request`, `unsupported_schema`, `unknown_command`,
`request_too_large`, `not_bound`, `already_bound`, `host_changed`, `host_lost`,
`authorization_required`, `authorization_denied`, `authorization_disabled`,
`controller_busy`, `controller_revoked`, `save_path_required`, `result_expired`,
`view_unavailable`, `handoff_in_progress`, `handoff_uncertain`.
Model, revision, file, job and format errors retain existing `tp_status` codes.
Do not duplicate the core enum in a second business-error registry.

Complete `ok:false` means the command's documented effect did not happen, except
an error explicitly carrying `publication_uncertain` or `handoff_uncertain`.
An Export job may finish failed/cancelled with prior targets published; its full
result is authoritative. A wet transaction is never reported rejected merely
because recovery or response delivery failed after the model commit.

## Contract test transcripts

Packet 1 uses command-schema unit tests and independent real-process assertions for ready/unbound,
invalid framing/UTF-8/version/unknown fields, large single-line responses, `--new`, snapshot,
two-op apply, dry run, semantic no-op, duplicate ID, stale revision, wrong host
generation, Undo/Redo, and dirty EOF. Real process tests must exchange split
reads/writes and prove that rejected bytes do not change model/history/file
state. A snapshot over 1 MiB is returned on one complete line; EOF and broken
stdout recovery are verified by restoring the journal through the shared core
and comparing the complete canonical graph. P1 has no independent GUI/job loop
to keep responsive during a blocked pipe write.

Later packet fixtures cover GUI consent/busy/revoke/reconnect, normal GUI close,
lost responses,
Save As identity, headless EOF during each task kind, event-window resync, large
reports, partial publication, and every handoff failure before/after lease
release. Exact internal handoff codec is a separate release blocker; a recovery
journal or matching happy-path snapshots do not prove live-history transfer.
