# Agent mode and local Dev API

**Status:** Target contract; the first headless agent packet is implemented.
Local Dev API and the rest of the release remain future work.

The current `ntpacker agent` command exposes a subset of the in-process
live-headless session shape: new unsaved sessions, discovery, snapshots, typed
transactions, history, Undo/Redo, and recovery preservation. It cannot yet
open saved projects, Save, run jobs, attach to GUI, or transfer ownership.

The approved v1 design is [agent-mode-v1.md](agent-mode-v1.md). The
[wire contract](../formats/agent-v1.md) identifies the 11 implemented commands
and the separate prerequisites for later packets. The remainder of this
document describes the full target, not current capability parity.

## Product boundary

- Ordinary CLI remains one-shot and saved-file-oriented.
- Agent mode is long-lived and session-oriented, in the same `ntpacker`
  executable as the ordinary CLI.
- The Dev API is the local transport used by agent mode and other compatible
  tools to reach the authoritative session in another process.
- GUI, file CLI, agent mode, and Dev API never implement project rules
  independently.
- An agent must not edit a second hidden project copy while a GUI session is
  authoritative.
- A read-only ordinary CLI may inspect the saved file while a live session
  exists, but its machine report identifies `state_source: "saved_file"` so it
  cannot be mistaken for live unsaved state.
- A mutating ordinary CLI receives `project_live` while a cooperating live
  session owns the path. A deliberate emergency/debug override may bypass that
  cooperating lease, but it is never the ordinary workflow. Its exact CLI
  spelling, safeguards, and machine-report field remain open release contracts;
  it does not authorize silently overwriting an externally changed file.

Equal capability does not require identical endpoint catalogs. A compact
transaction endpoint may expose the same internal operation vocabulary that the
CLI presents as many human-friendly verbs.

## Agent-facing interface

An external agent is instructed to use the ordinary CLI for saved-file work or
agent mode for live work. It launches and drives the executable through ordinary
process input/output tooling. No agent-platform protocol, specialized integration
client, or vendor SDK is required. The packer does not run an AI model itself.

Agent mode exposes the packer's own versioned structured commands, responses,
and events. Removing an external protocol dependency does not remove any live
capability, change session ownership, or turn agent mode into one-shot file
commands. Command discovery and argument metadata come from the shared core
catalogs; clients do not maintain a second operation vocabulary.

The first packet's input/output framing and command schemas are defined and
tested in [agent wire v1](../formats/agent-v1.md). Later commands retain their
explicit prerequisites before implementation.

## Local transport

The default Dev API is local IPC:

- named pipe on Windows;
- Unix-domain socket on Linux and macOS.

No network listener is required. Messages and errors are versioned structured
data. Exact method names, discovery records, authentication fields, and JSON
schemas remain open release contracts and must be fixed by prototypes and
golden tests before shipping.

## Session binding

One agent-mode process binds to exactly one project session. After binding,
normal project commands do not require a session ID.

Supported target startup shapes are:

```text
ntpacker agent
ntpacker agent --project <path>
ntpacker agent --new
```

Plain startup is unbound and exposes only discovery, capability/version, and
project binding. Selection must be explicit or unambiguous; focus history,
registry order, and installation order are never hidden authority. A bound
process never silently switches projects.

For a saved path, agent mode attaches to the existing cooperating live session
or opens one headlessly. It never creates another authoritative writable copy.
Unsaved sessions use runtime identity only; no persistent project ID is added
to the project file.

## Ownership and controllers

A canonical saved project identity has one cooperating writable live session.
At most one external controller owns the controller slot in the first
release, while human GUI edits may continue through the same session.

A second controller receives a structured busy result. Replacement is explicit:

1. finish or reject the current atomic transaction;
2. revoke the old controller token;
3. reject later requests from it;
4. send current snapshot and revision to the replacement;
5. assign the controller slot.

A controller is not considered dead merely because a short timeout elapsed.
Takeover requires proof that the old authority is stale.

## Host handoff

The GUI normally hosts the live session. The agent-mode process may host it
headlessly while no GUI owns it. Opening the project in the GUI transfers
authority rather than merging two copies.

Handoff preserves committed project state, revision, semantic history,
dirty/saved baseline, and enough source/recovery/cache state to resynchronize.
Worker threads and OS process handles are not transferred. Running Pack work is
cancelled at the old host; a result may be reconstituted from safe cached state
or a new Pack may be requested.

The authority cutover is singular. After it, the old host cannot accept
mutations or publish derived results.

## Required capability classes

Agent mode and the Dev API must cover:

- discovery and handshake;
- attach/open/new;
- full snapshot and resynchronization;
- inspect and validate;
- schema-driven transaction apply;
- Save, Save As, and discard/close decisions;
- Pack, Export, job state, and cancellation;
- Undo and Redo;
- ordered events and history;
- GUI focus/reveal where a GUI view exists;
- typed capability/version queries.

Inspect/validate may initially remain synchronous snapshot operations; the
current capability matrix explicitly marks asynchronous inspect/validate jobs
as not implemented.

## GUI presence

When automation is present, the GUI exposes connection and authorization state,
the external controller identity, Disconnect/Revoke, transaction authorship in
History, and ownership-handoff/recovery progress. These surfaces observe the
same controller/session state as the transport; they do not infer presence from
display names or maintain a second authority.

Visible authorship distinguishes human and external-controller transactions.
The GUI also provides a live activity surface for the controller's current
transaction/action with a route to the affected project object when one exists.
Its exact banner, badge, and recent-highlight presentation remains a GUI design
choice.

Per-action confirmation dialogs and arbitrary UI injection are not required by
this transport contract.

## Synchronization

On attach, the client receives a complete immutable snapshot and an observation
token. Later committed events are ordered and carry the revision transition.
A gap or expired event window requires a new snapshot; clients never infer
missing state.

Internal mirroring events are transport synchronization, not automatic LLM
context. The agent queries only the state it needs.

Transactions use stable idempotency IDs and expected revisions. Retry is safe
only while the same session generation remains authoritative. After host
restart or uncertain ownership, the client resnapshots before deciding whether
to retry.

## Authorization

Connection and controller ownership are explicit, visible session state. The
Dev API is open to compatible local integrations; it does not verify a claimed
vendor identity.

Authorization is binary per canonical project path: external integrations are
allowed or denied. There are no granular capability scopes in the first
release. The global modes are:

- `Disabled`;
- `Ask for each project`;
- `Allow all projects`.

The default is `Ask for each project`. In that mode, the first connection to an
existing canonical path prompts Allow/Deny. The decision is stored locally by
canonical path, outside the project and repository. Reopening the same path
reuses it; moving or renaming the project changes identity and prompts again.
Every connection receives a new temporary token.

An integration-created unsaved project is allowed for that runtime session.
After first Save, its path decision follows the normal Ask-mode policy.
Disconnect invalidates the current token but retains the path decision. Revoke
also removes the path decision, so the next connection prompts again.

Permission is connection-level, not a per-operation confirmation dialog. It
grants only the public packer API—inspect, edit, Save, Pack, Export, Undo/Redo,
and minimal view focus—not arbitrary OS, memory, process, network, UI-injection,
or other-application control. Sensitive filesystem effects still obey the same
typed preflight, capability loss, dry-run, and publication contracts as human
operations.

Exact handshake field names and wire schemas remain open release contracts.

## End-to-end acceptance

With a project open in the GUI, one external controller can submit one
multi-operation transaction through the live transport. The GUI observes one
History entry with the controller author, and one Undo restores the exact prior
project meaning. No saved-file shadow copy, delayed merge, or client-specific
validation path participates in that flow.
