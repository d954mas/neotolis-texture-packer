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

## Shared exporter boundary

Current exporter descriptors declare:

- stable exporter ID and display name;
- primary extension;
- capability flags;
- write callback;
- optional output-list callback.

Settings clamping, loss notices, normalization, and safe publication are shared
export-layer behavior around the descriptor. The current descriptor does not
carry format-version metadata.

Built-in `json-neotolis` and `defold` descriptors and runtime-registered C
exporters use the same export orchestration. This is not yet the target unified
package descriptor or Import/Export IR.

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
| Async inspect job | not applicable | not implemented | not implemented |
| Async validate job | not applicable | not implemented | not implemented |

The table describes the typed core capability matrix, not deployed transports.
There is no current MCP or Dev API server.

## File CLI

Ordinary CLI commands are one-shot saved-file workflows. Inspect and validate
load immutable snapshots without taking the writer lease. Mutations open a
short-lived writable session, submit the same typed operations, and save through
the same persistence contract. Pack/export remain synchronous commands with
versioned machine payloads.

The CLI is not a back door into an already-open GUI session. A conflicting live
writer produces `project_live`.

## Native GUI

The GUI owns one small session host. Views submit intents; actions own draft
conflict rules, the thin mutation adapter submits one typed operation batch,
and the host owns `update + borrowed view`, task completion, and active/candidate
lifecycle cutover. There is no in-process transport/client mirror.

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
