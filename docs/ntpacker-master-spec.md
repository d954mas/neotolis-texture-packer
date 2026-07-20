# Neotolis Texture Packer — Master Product and Architecture Specification

**Version:** 2.1

**Date:** 2026-07-12

**Status:** active consolidated master specification; product decisions integrated; remaining implementation contracts listed in §60
**Repository:** `d954mas/neotolis-texture-packer`

---

**Consolidation basis:** architecture review and decision discussion completed on 2026-07-12.

Decision language used in this document:

- **Invariant** — must remain true across implementations.
- **Target design** — required end-state behavior that may be delivered in slices.
- **Initial policy** — the selected first implementation; may change without changing the product model.
- **Open contract** — intentionally deferred and listed in §60.

---

## 1. Purpose

Neotolis Texture Packer is an open, deterministic texture-atlas tool built around
a concave NFP/Minkowski packing core.

The product should compete on three axes:

1. **Smarter packing**
   - true silhouette packing;
   - concave polygons;
   - exact D4 transforms;
   - deterministic output;
   - per-target capability-aware packing.

2. **Open interoperability**
   - multiple export formats;
   - atlas import;
   - atlas splitting;
   - user-defined format packages;
   - conversion between runtimes without rewriting the game loader where possible.

3. **Automation and AI**
   - complete CLI;
   - one canonical operation layer;
   - machine-readable contracts;
   - live GUI control through MCP;
   - shared Undo/Redo and project state between human and agent.

GUI polish remains important, but it is not the primary product-development
priority for the next phase.

---

## 2. Current baseline

The repository already provides:

- one packing core;
- native GUI and CLI frontends;
- `.ntpacker_project` files;
- multiple atlases per project;
- file and folder image sources;
- live source refresh;
- packing settings and per-region overrides;
- animations;
- full-fidelity Neotolis JSON export;
- Defold export;
- per-target capability-aware repacking;
- deterministic output;
- machine-readable CLI output;
- complete CLI project editing;
- dry-run, inspect, and validate;
- runtime exporter registration;
- raw RGBA pack inputs.

The next architecture must preserve these properties.

---

## 3. Product roadmap structure

There are **two product epics** and one shared engineering foundation.

```text
Shared foundation
├─ stable persistent IDs
├─ typed operation layer
├─ transactions and revisions
├─ semantic Undo/Redo
├─ tagged source model
└─ versioned canonical IRs

Epic A — Live automation and AI
├─ CLI/API boundaries
├─ live session
├─ Dev API
├─ MCP mode
├─ ownership and recovery
└─ authorization

Epic B — Format ecosystem and atlas interoperability
├─ format packages
├─ exporters
├─ atlas importers
├─ linked atlas sources
├─ Extract Sprites
└─ Lua extension runtime
```

The shared foundation is not a third user-facing epic. It is the common
implementation layer required by both.

---

# Part I — Shared foundation

## 4. Architectural invariants

### 4.1 One business-logic layer

All persistent project changes and pack/export behaviors belong below GUI, CLI,
MCP, and Dev API.

```text
GUI controls
CLI verbs
MCP tools
Dev API calls
      |
      v
tp_operations / tp_session
      |
      v
project model / source resolution / pack / import / export
```

Frontends may shape commands differently, but must not duplicate mutation,
validation, naming, capability, or Undo rules.

### 4.2 Equal capability, different interfaces

Equal capability does not require identical public shapes.

- CLI may have many human/script-friendly commands.
- MCP should have a compact tool catalog.
- Dev API may expose a small number of transactional endpoints.
- GUI invokes operations through the same operation/session layer.

CLI and MCP remain separate modes. Ordinary CLI is file-oriented; MCP is
session-oriented.

### 4.3 Determinism

Given the same:

- project state;
- normalized source content;
- selected target format, data-format version, profile, and options;
- installed format-package implementation;
- packer version and deterministic algorithm profile;

the outputs and structured reports must be reproducible.

No timestamps, random ordering, locale-dependent floats, pointer-derived values,
or hidden UI state may affect persistent output.

The project does not pin `package_version`. The export report records the exact
Neotolis version, format package ID/version/origin, selected data-format version,
and algorithm profile used. Updating Neotolis or a global format package is a
toolchain update and may intentionally change output. Project-local packages are
normally pinned by the project's own version control.

### 4.4 Explicit target adaptation and loss reporting

A target-format limitation is visible on the specific export target before
export, repeated in export preflight, and included in the structured report.
Targets are declared at the project level and resolved per atlas as effective
targets (§31, §61.1); loss is reported against the effective target for each atlas.

The user and agent must be able to inspect:

- what packing behavior was adapted;
- what capability caused the adaptation;
- whether runtime meaning was preserved;
- which metadata cannot be represented;
- which sprite or target is affected.

A compatible repack such as disabling unsupported flips is a notice, not a
failure. Unsupported metadata such as pivot or 9-slice is a visible warning/loss.
Export remains allowed. Only a condition that prevents creation of a valid
output artifact is a blocking error.

### 4.5 No hidden project choice

No interface silently chooses a project based only on:

- focused window;
- last-opened window;
- arbitrary registry order;
- installation order.

Automatic selection is allowed only when unambiguous.

### 4.6 Serialized live-session mutation

All live project-model changes are applied through one serialized session queue.
GUI actions, MCP requests, Undo/Redo, and Refresh never mutate the live model
directly from arbitrary threads.

Heavy work may run on workers using immutable inputs. Worker completion is
published back through the session queue. Worker threads cannot directly change
revision, history, dirty state, or the authoritative preview selection.

### 4.7 Ownership boundaries

Each mutable responsibility has one owner:

| Owner | Owns | Does not own |
|---|---|---|
| `tp_model` | mutable project state, revision, semantic saved identity and dirty state, history, live idempotency retention, attached best-effort recovery recorder and health state | project path, exact saved-file fingerprint, dialogs, job scheduling, frontend state |
| `tp_session` | the sole live `tp_model`, session identity/path, exact saved-file fingerprint, admission and ordering, runtime generations, event sequence, job handles | model validation, recovery codec, filesystem/lock backends, Pack/Export algorithms, GUI or protocol formatting |
| `tp_session_snapshot` | one immutable owned read view plus revision/model/source/event generations | mutation authority or borrowed aliases into the live model |
| `tp_recovery_store` | injected recovery root and backend, bounded orphan scan, orphan-slot handles | live model semantics, project Save policy, controller handoff |
| `tp_recovery_live` | one session's live-slot path, OS liveness handle, metadata attachment, clean-close lifecycle | model semantics, Save policy, orphan resolution |
| `tp_recovery_claim` | exclusive right to inspect, recover, or discard one orphan slot | canonical project authority |
| `tp_project_lease` | OS-backed reservation of one canonical project identity while a writer has authority | orphan-journal lifecycle or controller handoff policy |
| frontend adapter | intent capture, native dialogs, presentation and typed result mapping | business rules, persistence, history, recovery policy, mutable model aliases |

The semantic dirty baseline exists only in `tp_model`. The exact on-disk
fingerprint exists only in the session persistence state.

`tp_session` is an orchestration boundary, not a service locator or a second
business-logic layer. Responsibilities with independent invariants or fault
matrices remain in their owning modules and are invoked through narrow typed
contracts. A session implementation must not absorb model validation, journal
encoding/decoding, filesystem or lock backends, Pack/Export algorithms, GUI
state, dialogs, protocol JSON, or transport error formatting.

## 5. Stable persistent identities

There is no persistent `project_id`.

Saved structural entities receive random persistent 128-bit IDs at creation:

- atlas;
- source;
- animation;
- export target.

Recommended shape:

```text
atlas_<128-bit-id>
source_<128-bit-id>
anim_<128-bit-id>
target_<128-bit-id>
```

IDs survive rename, reorder, save/reload, ownership transfer, Undo/Redo, and GUI
view changes.

### 5.1 Project identity

For a saved project, identity is the canonical normalized path of the
`.ntpacker_project` file.

A copy at another path is another project. `Save As` therefore creates another
project identity automatically. Moving or renaming the project file changes its
identity. An unsaved session uses a temporary runtime session ID until first
save.

Recovery records, live-session claims, and local integration permissions are
keyed by canonical project path. They are not embedded in the project file.

GUI/CLI argument paths and every `tp_core` filesystem path are strict UTF-8.
On Windows, clients read the UTF-16 process command line, reject malformed
UTF-16, and convert once at ingress. The core filesystem boundary converts to
UTF-16 and uses only W-suffixed Win32 APIs, including controlled extended drive
and UNC paths for long-path I/O.

Saved source paths are relativized only when project and source share the same
explicit root: POSIX `/`, Windows drive, or the complete UNC `server/share`
tuple. Different roots preserve the absolute spelling. Component processing is
re-entrant and streaming; path depth has no smaller hidden limit than the
canonical 4095-byte path bound.

### 5.2 Derived sprite identity

Every resolved sprite has a unique deterministic `sprite_id`, including sprites
without persisted metadata.

```text
sprite_id = stable_hash(source_id + normalized source-local key)
```

Examples:

```text
folder source:
source_id + relative/path/button.png

atlas source:
source_id + importer region key
```

A logical/export rename does not change `sprite_id`. An external source-file
rename changes the source-local key and therefore represents deletion of the old
sprite and creation of a new sprite.

A normal derived sprite does not require a serialized record. A sparse record is
stored only when the sprite carries persisted metadata, overrides, a logical
name, or another persistent reference.

### 5.3 Source-key normalization

Persistent source-local keys use:

- UTF-8;
- `/` as separator;
- Unicode NFC;
- normalized `.` components and repeated separators;
- preserved letter case;
- paths relative to the source root.

A key must never escape its source root through `..`, become absolute, or depend
on OS-specific inode/realpath identity.

Case-folding is used only for portability validation. `Button.png` and
`button.png` are not silently merged; they are reported as a cross-platform
collision.

Neotolis does not rename physical source files. If files are renamed outside the
application, the old sprite becomes missing and the new path becomes a new
sprite. Persisted metadata for the missing sprite remains orphaned and becomes
active again if the old source key returns.

### 5.4 Selectors and canonical targeting

Canonical operations, history entries, animation references, and persistent
references target IDs.

CLI and MCP may accept names, paths, or compound selectors for convenience. A
selector must resolve to exactly one ID before transaction validation. An
ambiguous selector produces an error and candidate list; it is never guessed.

### 5.5 Canonical project schema

The project file has one accepted representation: schema v5. The loader accepts
exactly v5 and returns a structured version error for v1-v4 or a future version;
it never rewrites an unsupported file. Missing or malformed version data is a
malformed-project error. The production core contains no legacy project converter.

Every persisted atlas, source, animation, and target has a non-nil, kind-correct,
unique structural ID. Sources are tagged records. Sprite overrides and animation
frames use only the canonical `{source, key}` identity, where `key` is the
normalized source-local key with its extension preserved. Name-only pending forms
are not valid project states. `sprite_id` remains derived and is never persisted.

Each source record additionally carries a write-once `added_at` stamp, set when
the source is added and never rewritten (it drives "recently added" sorting and a
`new` badge). No continuously-updated per-asset timestamp is ever persisted: a
source file's `mtime` is read live from the filesystem for display and sorting
only. Persisting churning timestamps is the TexturePacker SmartUpdate anti-pattern
(needless VCS diffs); "who changed what, when" inside the project is the
transaction history's job, not a stored field.

New private candidates may temporarily contain missing IDs. The writable session
adoption boundary assigns all missing IDs atomically and validates the complete
graph before publication. Saves, checkpoints, and externally visible snapshots
must already satisfy the canonical invariant. See decision 0016.

### 5.6 Validation

Validation reports:

- missing, malformed, wrong-kind, or duplicate persistent IDs;
- references to unknown IDs;
- duplicate exact source keys;
- case-insensitive and platform-name collisions;
- invalid normalization or traversal;
- a sprite override or animation frame that is not canonical `{source, key}`;
- canonical records whose source entity remains in the graph but whose physical
  source is unavailable or source-local key is absent, reported as orphaned model
  state rather than migrated or silently discarded. An unknown source ID is a
  graph-integrity error and cannot pass adoption, save, or checkpoint publication.

## 6. Typed operations

All persistent model mutation is represented by typed semantic operations.

Examples:

```text
project.defaults.set
atlas.create
atlas.remove
atlas.rename
atlas.settings.set
atlas.settings.clear
atlas.visible.set
source.add
source.remove
source.replace
sprite.override.set
sprite.override.clear
sprite.name.set
animation.create
animation.remove
animation.frames.set
animation.settings.set
target.create
target.remove
target.set
target.participation.set
note.create
note.remove
note.set
note.reparent
atlas.board.move
workspace.tidy
```

Export targets (`target.*`) are **project-scope** operations; per-atlas
participation is the atlas-scope `target.participation.set` operation (§61.1,
§31). Text objects (`note.*`) and atlas board placement (`atlas.board.move`,
`workspace.tidy`) are ordinary model operations over the workspace section
(§61.2).

**Field-presence SET operations.** The mask-carrying `*.set` operations -- `atlas.settings.set`,
`sprite.override.set`, `animation.settings.set`, `target.set`, `target.participation.set`, and
`note.set` -- are *field-presence*: an
operation changes only the fields it actually carries, and its presence mask is derived from which
fields are present (including in the JSON form). Omitted fields are left unchanged; an operation that
names no field is rejected. This keeps a serialized operation a faithful round-trip -- the canonical
encoder emits only the masked fields and the decoder reconstructs the same mask -- which the recovery
journal relies on to replay committed operations without re-introducing fields that were never sent.

**Settings inheritance and provenance.** Packing/atlas and sprite settings
resolve along project -> atlas -> sprite. The project `defaults` block holds
atlas-level setting defaults (padding, max page size, shape, ...); an atlas holds
sprite-level setting defaults (pivot, ...); a sprite override wins at the leaf.
For every field the core exposes the **effective value** and its **origin**
(`default | project | atlas | sprite`) so clients can render non-default markers
and a "modified only" filter. Reverting a field is a `*.clear` / `clear_override`
operation at the level that set it -- not a re-assignment to the inherited value.
Validation always runs on effective values. See §61 for the inspector treatment.

**Display-only visibility.** An atlas carries `visible: bool` (default true).
`atlas.visible.set` is a named, undoable model operation recorded in history and
marks the project dirty. Semantics are **display-only**: a hidden atlas still
packs and exports. A hidden atlas is not a disabled atlas; a `disabled` concept
(excluded from pack/export) is a possible future feature and must never be
conflated with `visible` (§61.1).

**Palette-ready operation registry (hard requirement on F2).** Every typed
operation carries a human-readable **label template** (used for history entries
and undo toasts, e.g. "Set padding 4 (3 sprites)") and a machine-readable
**argument schema** (name, type, range/enum). A command palette and machine
clients can therefore index and invoke parameterized operations
("set max page size 1024") without per-command UI code. This is a requirement on
the operation engine, not an optional client convenience.

Operations target stable IDs, not array indexes or mutable names. A frontend may
accept a human-friendly selector, but it resolves that selector to an ID before
building the canonical operation.

### 6.1 Model operations, session commands, and jobs

These categories are distinct.

**Model operations** change persistent project state, increment revision, and are
Undoable as part of a transaction.

**Session commands** act on the live session but are not model operations:

```text
save
save_as
discard
undo
redo
```

**Derived jobs** compute results without changing project model:

```text
pack
inspect
validate
```

**External side-effect commands** may write or install files and require their
own staging/commit rules:

```text
export
extract_sprites
format.install
format.uninstall
```

**Runtime events** update non-project observations without changing project
revision or semantic dirty state by themselves. Examples include source refresh
status and diagnostics.

`Pack` and `Export` are not session commands. A session may coordinate their
ordering, immutable inputs, cancellation, and result publication, while their
job and side-effect contracts remain distinct from session-command atomicity.

`Save` is not a semantic operation. It does not increment revision and is not
Undoable.

### 6.2 Why not raw JSON Patch

Raw JSON Patch is not the canonical mutation protocol because:

- array indexes shift;
- file schema paths are not semantic contracts;
- rename/reference updates become unclear;
- validation messages lose domain meaning;
- Undo labels become poor;
- schema migrations break paths.

JSON Patch may be used internally for debugging, but not as the product operation
model.

## 7. Transactions

A model transaction contains:

```text
transaction_id
expected_revision
label
author/client metadata
ordered operations[]
```

The recorded author is one of `human` or `agent(<external-controller identity>)`
(§18). Author identity is committed with the transaction and exposed to every
client so the GUI can render AI/ME authorship badges and a live agent-activity
surface. Authorization is decided once at the connection level (§23); there are
no per-action confirmations.

Rules:

- resolve all selectors to IDs before validation;
- validate the full batch first;
- apply atomically through the session queue;
- rollback the entire batch on failure;
- increment revision once;
- create one Undo entry;
- emit one committed change event.

Example:

```text
Create enemy animations
├─ animation.create enemy_idle
├─ animation.frames.set [...]
├─ animation.create enemy_walk
└─ animation.frames.set [...]
```

One agent request creating 100 animations should normally be one transaction and
one Undo entry.

### 7.1 Unified commit and recovery recording

All model transactions use one commit contract regardless of origin:

- GUI;
- MCP / Dev API;
- Undo;
- Redo.

A transaction reaches its irreversible model commit point after it:

1. was fully validated;
2. was applied atomically to the authoritative session;
3. received a new revision and history position.

The session then publishes one ordered committed event/result. Recovery
recording is deliberately **not** part of the commit point and cannot change its
outcome. The compact TXN or HISTORY record is attempted after the irreversible
model commit; the attempt may occur before the ordered event/result is emitted.
Recovery-only allocation, encoding, admission, append, or sync failure never
rejects the transaction: every such failure enters the same degraded-recovery
path.

The initial target keeps the existing synchronous per-record durability barrier
(`fflush` plus `fsync`/`_commit`) but moves it after the model commit point. This
is simpler and stronger than the 5-second RPO when benchmark latency is
acceptable. Only measured user-visible stalls justify batching. If batching is
introduced, a runtime-owned wake mechanism independent of further edits, Save,
or shutdown must **complete** the durability barrier no later than 5 seconds
after the oldest undurable commit.

If append or durability sync fails, the committed model, revision, event, and
Undo/Redo position remain published. Recovery enters a sticky degraded state,
surfaces the persistent structured notice `recovery_degraded` (first cause,
last durable revision/time, sticky/cleared transition), and stops recording
later dependent diffs until it is re-established from a fresh checkpoint.
Editing continues. A successful Save is never blocked by recovery failure; a
fresh recovery checkpoint after project publication clears degradation and
resumes recording, while checkpoint failure preserves old evidence and the
notice.

For operations with external side effects, every fallible preparation happens
before model commit. Required post-commit publication is infallible or
idempotently recoverable and completes before the success event/result. Recovery
recording is neither the atomicity mechanism nor compensation for side effects.

### 7.2 Idempotency

External transaction IDs are idempotent within a defined retention window.
In the v1 contract, retrying a retained committed transaction ID returns the
structured `duplicate_id` result with the current revision and does not apply the
payload a second time. Duplicate detection precedes the revision precondition,
so a retry remains a duplicate even after later commits advanced revision.

The live retained transaction-ID set is authoritative for the current session.
Recovery persists the retained IDs covered by its latest durable watermark, and
Save-time compaction preserves that covered set. A host restart may lose the
same unsynced tail as project recovery; a client detecting a new host/session
generation must resnapshot and reconcile instead of assuming every pre-crash ID
remains retained.

Durable replay of the original result is an optional additive transport upgrade,
not a v1 model requirement. A client that needs the exact original response after
losing it must use a surface that explicitly advertises durable result replay;
otherwise it rebuilds from the returned current revision and a fresh snapshot.

## 8. Revisions, history position, and dirty state

Every live session has a monotonically increasing revision.

Every committed project-model transition, including Undo and Redo, receives a
new revision. Revision never moves backward.

`Save` does not change revision.

Mutation includes `expected_revision`:

```text
expected == current
→ validate and commit

expected < current
→ revision_conflict

expected > current
→ invalid_revision
```

No automatic field-level merge or CRDT is part of v1. The caller inspects current
state, rebuilds its intended transaction, and retries.

Revision, Undo/Redo history position, and saved state are separate concepts.
Dirty state is not calculated from revision numbers.

```text
dirty = current semantic state identity != saved baseline identity
```

Therefore:

```text
edit -> save -> edit -> undo
```

returns to a clean state even though revision has continued to increase.

## 9. Semantic Undo, Redo, and visible history

The target Undo model stores semantic transaction diffs with enough before/after
data for exact reverse application.

Examples:

```text
field update:
before = 2
after  = 4

remove entity:
before = complete removed entity + ordering position

move entity:
before index
after index
```

### 9.1 Atomicity

Undo or Redo applies an entire transaction atomically through the session queue.
Undo and Redo create new monotonically increasing revisions.

History is shared by:

- all GUI views of the session;
- human edits;
- MCP edits;
- Dev API edits.

A new model transaction after Undo discards the Redo branch.

Each visible history entry carries its transaction author (`human` or
`agent(<id>)`, §7). Undo/Redo behave identically regardless of author. The GUI
uses this to badge AI vs. human authorship and to drive a live agent-activity
feed; it never gates individual actions on confirmation (§23).

### 9.2 Save checkpoints in the History panel

A successful Save may appear in the visible History panel as a non-Undoable
checkpoint marker containing the current revision/state identity and path.

It is not a semantic transaction, does not increment revision, and is skipped by
Undo/Redo. A failed Save creates no checkpoint and does not change the saved
baseline.

### 9.3 Session lifetime

Undo/Redo history exists only for the current live session.

- GUI/MCP ownership transfer preserves the history.
- Reconnecting to the same still-live session preserves the history.
- Crash recovery restores the latest valid durably flushed recovery prefix; it
  may lose the most recent tail and is not required to restore the full
  Undo/Redo stack.
- Normal close and reopen starts a new history.

The visible History panel may also contain non-Undoable runtime information such
as source refresh events. These entries are session-local and are not a
persistent audit log.

### 9.4 Snapshot role

Full project snapshots are not the normal per-edit history model.

Snapshots remain useful for:

- initial attach;
- process ownership transfer;
- resynchronization;
- crash-recovery checkpoints;
- history compaction;
- exact-inverse and codec tests;
- debugging.

### 9.5 Exactness oracle

Production Undo/Redo stores semantic diffs. Checkpoint serialization is a test
oracle and recovery primitive, not a production Undo stack.

For each new semantic operation:

```text
A
-> forward operation
-> B
-> reverse operation
-> byte-identical A
```

The restored project must serialize byte-identically to state A.

## 10. Pack results, freshness, and memory cache

Pack output is derived state, not a project mutation. Pressing Pack without
changing the project is not Undoable.

Pack runs on an immutable snapshot. The user may continue editing settings,
performing Undo/Redo, saving, and navigating while it runs.

### 10.1 Required UX

Workflow:

```text
change settings
pack
see worse result
Ctrl+Z
```

After Undo, the previous packed preview should appear immediately when its cached
result is available.

A completed Pack result is not discarded merely because project inputs changed
while it ran. It is shown as the latest completed result selected for preview and
is clearly marked out of date when it no longer matches current inputs.

```text
preview.pack_input_hash == current.pack_input_hash
→ preview is current

preview.pack_input_hash != current.pack_input_hash
→ preview is out of date; show Pack again
```

Project dirty state and preview freshness are independent.

### 10.2 Pack-input hash

The content-addressed key includes all normalized inputs that can affect the
result:

- ordered sprite identities and logical names where relevant;
- semantic image hashes;
- source/import materialization results;
- effective settings and sprite overrides;
- target format capabilities, selected data version, profile, and target options;
- packer algorithm/version profile.

For raster sources:

```text
semantic_image_hash = hash(width + height + canonical RGBA8 pixels)
```

The raw PNG/WebP/JPEG bytes, `mtime`, compression settings, and non-rendering
container metadata are not semantic Pack inputs. They may be used for change
detection and avoiding unnecessary decoding.

### 10.3 Result selection and concurrent jobs

Every Pack request has a job identity and input hash. All successful results may
enter the cache.

A Pack job that was requested earlier must not silently overwrite a newer active
preview after the newer request has completed. The user may explicitly select a
cached older result, and Undo may make an older hash current again.

Changing source files or settings does not automatically start a new Pack.
Neotolis updates current inputs and displays `Preview out of date` until the user
runs Pack, except where a future explicit auto-pack mode is introduced.

### 10.4 Memory-only pack-result cache

The cache exists only for the live session. It is not written into the project or
to a disk cache in the first implementation.

Inactive Pack results are stored in a compressed representation containing
layout/metadata/notices and compressed atlas pages or the normal serialized
runtime artifact where practical. They do not retain GPU textures or independent
copies of source images.

The active result is pinned and may hold decompressed/GPU resources. Inactive
results use a separate byte-budget LRU. Concrete budgets and compression details
are implementation policy.

The pack-result store must also serve cheap downscaled page thumbnails (mip
levels or a cached downscale) for the Project overview canvas at the owner's real
scale (30+ atlases / 5000+ sprites) without forcing full-resolution page
residency: full-res pages stay in the LRU and the overview never pins them
(§52.3, §61.1).

On cache miss, project state is restored immediately and the existing preview
is marked out of date. Undo/Redo never starts Pack automatically. The user runs
Pack explicitly when a fresh result is required.

### 10.5 Raw RGBA ownership boundary

Raster decoders normalize supported source formats to canonical RGBA8. Pack-job
input memory is temporary and released after the job according to the ownership
contract of `tp_build -> neotolis-engine`.

Whether the engine copies, adopts, trims, or retains raw RGBA during packing is an
engine-builder decision to be fixed by API contract and profiling, not by this
session specification.

### 10.6 Fallible builder containment

Ordinary source, settings, output-path, allocation, codec, and filesystem
failures must not terminate a GUI, CLI, MCP, or Dev API host. The shared core
decodes path sources to bounded canonical RGBA8 and validates every known
user-controlled builder precondition before invoking the engine. This preflight
is a safety boundary, not a substitute for a fallible builder API.

While the engine builder exposes aborting assertions or narrow path-based output,
the production integration runs it in a private worker process. The worker
receives a versioned bounded request containing validated settings and raw pixels,
uses only private ASCII relative staging names, and returns a versioned result.
The parent owns cancellation, timeout, crash detection, UTF-8 filesystem reads,
and final publication. A worker exit, signal, malformed response, or missing
artifact becomes a structured `builder_crashed`/`builder_failed` result and cannot
replace the last successful preview.

An upstream fallible memory/sink builder API may replace the process boundary
after its error, ownership, cancellation, and UTF-8 contracts are executable-test
pinned. Clients never call the engine builder directly, and this replacement does
not change public operation/session semantics.

## 11. Tagged source model and live source state

The current path-only source list evolves into source records.

```text
Source
├─ kind: path
│  ├─ image file
│  └─ recursively scanned folder
│
└─ kind: atlas
   └─ foreign atlas descriptor
```

Conceptual record:

```json
{
  "id": "source_...",
  "kind": "path | atlas",
  "path": "relative/path",
  "format_id": "atlas kind only",
  "options": {}
}
```

`path` sources preserve current behavior. `atlas` sources are specified in Epic B.

### 11.1 Canonical raster input

PNG, WebP, JPEG, and future raster formats are decoded to one canonical image
representation before analysis and packing:

```text
RGBA8
rows top-to-bottom
channels R,G,B,A
straight alpha
no row padding in hashed data
orientation already applied
```

JPEG and other formats without alpha use `A = 255`. Exact color-management rules
must be deterministic across platforms.

### 11.2 File watchers and refresh

An open live session observes file, folder, and linked-atlas sources.

Watchers are invalidation hints, not the source of truth:

```text
filesystem event
-> debounce
-> rescan/re-read affected source
-> update runtime source state and thumbnail
-> update source generation and current pack hash
-> mark packed preview out of date
```

The implementation should watch parent directories where atomic-save/rename
patterns could invalidate a direct file handle. Folder sources are rescanned
recursively. Linked atlas sources watch their descriptor and importer-declared
companion files.

A full source verification also occurs at project open, manual Refresh, before
Pack, before Export, and after watcher overflow or uncertainty.

Watcher changes never automatically start Pack in v1.

### 11.3 External source changes

External source-file changes are not project-model mutations:

- they do not increment project revision;
- they do not create Undo entries;
- they do not make `.ntpacker_project` dirty;
- they update a runtime `source_generation` and the current `pack_input_hash`;
- they may appear as informational session-history events.

A physical rename outside Neotolis is remove-old plus add-new. Neotolis does not
attempt hash-based rename recovery or sidecar identity.

Live `mtime` observed by the watcher is used for runtime display and the
"recently modified" sort key only; it is never written to the project file (§5.5).

### 11.4 Runtime source errors

If watcher/Refresh cannot read or decode a current source, the live session keeps
the last successfully decoded thumbnail only for visual reference and displays a
runtime warning.

The error is not saved in the project, does not change revision, and is replaced
by the result of the next watcher, Refresh, Pack, or Export read attempt.

Pack and Export remain invokable. They retry current source loading while
building their immutable input. If loading still fails, the command returns an
error and leaves the last successful packed preview unchanged. A new Pack never
silently uses last-known-good full source pixels.

### 11.5 GUI source previews

GUI thumbnails are derived temporary resources, not project data or Pack inputs.

- preserve aspect ratio;
- maximum side 256 physical pixels;
- do not upscale smaller images;
- load lazily for visible items plus a small viewport margin;
- pin visible items;
- evict invisible items by a dedicated CPU/GPU LRU;
- key by semantic image hash and thumbnail generation profile.

The selected source may load a full-resolution preview on demand. Full decoded
source images are not retained in a general RAM LRU in the first implementation.
Pack jobs decode their own current inputs and release them after completion.

# Part II — Epic A: Live automation and AI

## 12. Epic objective

Allow a human and one AI integration to work on the same live project while
sharing:

- state;
- revision;
- history;
- preview;
- jobs;
- save status.

The AI must not edit a second hidden copy of the project when the GUI is open.

---

## 13. Terminology

### Project

Persistent user data stored in `.ntpacker_project`.

A saved project's identity is its canonical project-file path. There is no
persistent project UUID.

### Project session

One live in-memory project state:

```text
model
saved baseline / dirty state
revision
Undo/Redo
source runtime state and generation
preview/cache state
active Pack job
connected views
external controller
```

### GUI view

A window or tab displaying a session. Any number of views may show one session.

### Session host

The process currently owning the authoritative in-memory session.

Possible hosts:

- GUI;
- `ntpacker mcp` in headless mode.

### `ntpacker mcp`

A long-lived mode of the normal `ntpacker` executable that speaks MCP to an AI
host. It is not a separate user-facing application.

### Dev API

The private local protocol used when MCP connects to a GUI-owned live session.

### External controller

The one active external integration controlling a project session.

## 14. Process topology

### 14.1 Same binary, different modes

```text
ntpacker <cli-command>
ntpacker mcp
ntpacker-gui
```

CLI and MCP are not the same mode.

### 14.2 CLI

Normal CLI is:

- one-shot;
- file-oriented;
- based on saved state;
- process exits after the command.

```text
start
load project file
execute
optionally save/export
return structured result
exit
```

Read-only commands explicitly report `state_source: saved_file` when a live
session exists.

A mutating CLI command detects an active live session for the canonical project
path and fails with `project_live` rather than editing a second file-oriented
copy. A deliberate emergency/debug override such as `--offline-force` may bypass
this protection.

Before saving, GUI/headless live hosts compare the project-file fingerprint with
the version loaded or last saved. External file modification produces
`file_changed_externally`; it is never overwritten silently.

Project-file Save writes canonical bytes to an exclusively created sibling
temporary file, durably syncs and closes it, rechecks any expected fingerprint
immediately before publication, and atomically creates or replaces the
destination. A pre-publication failure leaves the destination unchanged. A
post-publication parent-directory sync failure returns
`file_durability_uncertain`; the published bytes and returned fingerprint remain
authoritative, so clients surface a warning and do not retry as if no write
occurred.

Machine validation findings preserve exact UTF-8 context strings and include
the applicable stable atlas/source/animation/target IDs. Presentation adapters
may omit absent contexts but must not truncate or substitute them. Reports have
a hard byte/count budget and end with a deterministic structured truncation
finding when the complete set does not fit.

**Machine contract.** The CLI is a first-class machine interface. The as-built
JSON payloads are pinned in [`formats/cli-report.md`](formats/cli-report.md);
this section states the contract, that document freezes exact values, and the two
must not contradict:

1. **`--json` everywhere.** Every command has a machine-readable mode whose
   payload carries a per-verb versioned `"schema": N`; JSON is never lossy
   relative to the human text.
2. **Exit codes are a contract.** A stable, test-pinned table -- `0` ok, with
   distinct codes for internal error, usage, project load/parse, pack failure,
   export failure, partial success, strict-validate findings, and typed
   pre-publication file I/O failure. The exact numbers are frozen in
   `cli-report.md` / `cli_exit.h`; reference them rather than re-numbering.
3. **Errors and notices are structured data.** An error carries a stable string
   `id` (a `tp_status` token) alongside its message; a degradation notice carries
   `{sprite, field, target, reason-id}`. Prose is derived from the data, never the
   reverse.
4. **stdout/stderr discipline.** stdout is the requested payload only; stderr
   carries diagnostics and progress, so an agent can pipe stdout straight into a
   parser.
5. **Dry-run / explain.** `pack`/`export --dry-run` and mutation `--dry-run`
   report what would happen -- pages, occupancy, effective settings, and every
   predicted degradation -- without writing files.
6. **One-run query surface.** `inspect` and `validate` report *all* findings in a
   single run as structured data, so an agent fixes them in one edit cycle rather
   than one per run.
7. **Graceful failure, never a crash.** Bad input (oversized sprite, malformed
   file, missing source) returns a structured error and a non-zero exit code; an
   aborting process would break an agent loop.
8. **Published schemas.** JSON Schema is published for the `.ntpacker_project`
   file and for export/report payloads; the formats are documented as the API.
9. **Stability policy.** Field additions are non-breaking; a removal or rename
   bumps the verb's `schema` number, mirroring the project-file discipline.

snake_case field names and dot-decimal floats (`LC_NUMERIC` pinned to `C`) are
the authoritative as-built details recorded in `cli-report.md`.

### 14.3 MCP

MCP is:

- long-lived;
- session-oriented;
- able to attach to unsaved GUI state;
- able to host a headless live session.

### 14.4 One MCP process per project

One running `ntpacker mcp` process is bound to exactly one project session.

```text
MCP process A -> Project A
MCP process B -> Project B
```

Several projects require several MCP processes.

After binding, normal project tools do not require `session_id`.

---

## 15. MCP startup and project selection

Supported forms:

```text
ntpacker mcp
ntpacker mcp --project <path>
ntpacker mcp --new
```

There is no `--attach` shortcut.

### 15.1 Unbound startup

Plain:

```text
ntpacker mcp
```

starts unbound and exposes only discovery/binding capabilities:

```text
projects_list
project_attach
project_open
project_new
version/capability resources
```

### 15.2 Selection rules

1. Explicit project/path in the user request wins.
2. One suitable open project may be selected automatically.
3. Several projects with one unambiguous semantic match may be selected.
4. Several plausible projects require a user question.
5. Last-focused window is not hidden authority.
6. The process never silently switches projects after binding.

### 15.3 Explicit path

```text
ntpacker mcp --project game.ntpacker_project
```

- attaches to a matching live session if one exists;
- otherwise opens it headlessly;
- never creates a duplicate cooperating writable session.

### 15.4 New project

```text
ntpacker mcp --new
```

creates one memory-only session and binds immediately.

---

## 16. Single live session per project identity

One canonical saved project path maps to one cooperating live session.

Identity resolution uses:

- canonical normalized project-file path for saved projects;
- a temporary runtime session ID for unsaved projects.

There is no persistent `project_id`.

A project copied to another path is another project. A bound MCP process never
silently changes paths or switches projects.

The system must prevent two cooperating authoritative writable live copies for
the same canonical path.

Ordinary CLI remains file-oriented. Read-only commands may inspect saved state;
mutating commands are blocked with `project_live` while a cooperating live
session exists unless an explicit offline-force override is used.

## 17. GUI windows

A project session may have any number of GUI views.

All views share:

- project model;
- revision;
- Undo/Redo;
- dirty state;
- preview;
- jobs;
- connected integration.

View-local state remains local:

- zoom;
- pan;
- selection;
- filters;
- panel layout.

A single-instance GUI process is preferred so a second app launch asks the
primary process to open/focus the project.

---

## 18. One external controller

A session accepts at most one external MCP controller in v1.

The human may continue editing through GUI while the controller is connected.

A second MCP process receives:

```text
session_busy
```

It must not:

- load another writable cooperating copy;
- silently replace the current controller;
- queue for automatic takeover.

### 18.1 Explicit replacement

The GUI may offer:

```text
Keep current connection
Replace connection
```

Replacement sequence:

1. finish or reject the current atomic transaction;
2. invalidate the old token;
3. notify old controller when possible;
4. reject all later old-controller operations;
5. send current state/revision to the new controller;
6. assign the controller slot.

Already committed edits remain in history.

### 18.2 Reconnect

The same still-running MCP instance may reconnect using its stable runtime
controller instance ID.

A new process has a new ID and is treated as a competitor.

### 18.3 Dead controller

A new controller may take over only after the previous claim is proven stale.

A short timeout alone is not proof.

---

## 19. Session ownership

### 19.1 GUI open

GUI is the authoritative session host.

```text
AI host
-> MCP
ntpacker mcp
-> local Dev API
ntpacker-gui
-> session
```

MCP is a Dev API client and recovery mirror.

### 19.2 GUI closed

MCP may host the authoritative session directly:

```text
AI host
-> MCP
ntpacker mcp
-> tp_session/tp_operations
```

### 19.3 GUI opens a headless-owned project

Ownership transfers to GUI. There is no merge and no second session.

Transfer includes:

- project snapshot;
- revision;
- current live-session semantic history;
- dirty/saved baseline;
- runtime source status required for resynchronization;
- pack-result cache metadata/results that can be moved or reconstituted safely.

Runtime worker/thread state is never transferred between processes.

If Pack is running, it is cancelled on the old host. The new host uses an
available result from the memory cache or starts Pack again when requested or
required by the chosen workflow.

Other commands are expected to be short and must finish or reach a defined safe
boundary before the authority cutover. In particular, a file-publication commit
is not interrupted halfway through.

After cutover, the old host may not publish a Pack result or accept model
mutations. GUI is authoritative and MCP becomes client/mirror.

### 19.4 GUI closes while MCP continues

Ownership may transfer back to MCP under the same rules. The user must not lose
committed state during a clean handoff.

## 20. Dev API

### 20.1 Transport

Local IPC only by default:

- named pipe on Windows;
- Unix domain socket on Linux/macOS.

No TCP port is required for v1.

### 20.2 Protocol

Versioned JSON-RPC-style messages are acceptable.

Required classes:

```text
discovery/handshake
session attach
snapshot/resync
query/inspect
transaction apply
save/save-as/discard
pack/export/job status/cancel
Undo/Redo
view focus/reveal
events
```

### 20.3 Canonical transaction endpoint

The Dev API may expose a compact endpoint such as:

```text
project.apply(transaction)
```

The public endpoint count does not need to match the internal operation count.

---

## 21. Synchronization and mirror

When MCP attaches to a GUI-owned session:

1. GUI sends a full initial snapshot.
2. GUI sends ordered committed transaction events.
3. Every event includes before/after revision.
4. MCP updates its recovery mirror.
5. On a revision gap, MCP requests changes-since or a new snapshot.

These synchronization events are internal to MCP and GUI.

They must not automatically flood the LLM context.

The agent queries current state only when needed.

---

## 22. Failure and recovery

### 22.1 Committed MCP transactions

MCP retains pending transaction IDs and can retry idempotently while the same
live host/session generation remains authoritative. A success response follows
the shared model commit and ordered event contract; it does not claim that the
recovery durability watermark already covers the transaction. After a detected
host restart, MCP resnapshots and reconciles before retrying an uncertain
mutation. No full project save is required.

### 22.2 GUI crash

MCP may promote its mirror only after proving the GUI host is dead and atomically
acquiring the session claim.

The point at which ownership authority changes must be singular: both processes
must never accept writes for the same session simultaneously.

### 22.3 Local recovery journal and checkpoints

The authoritative host maintains a bounded local version-4 journal. It contains
one attach checkpoint, ordered TXN operation records, compact HISTORY
transitions, and optional metadata. The journal is not written into the project
repository.

Recovery is an additional best-effort safety net, not a transactional database
or a precondition for editing. Its minimum policy is:

- under a healthy backend, the power-loss and process-crash recovery-point
  objective is at most 5 seconds;
- recovery reconstructs the latest valid durably flushed ordered prefix, so a
  few recent edits may be lost;
- minutes or hours of healthy-session work must not be lost merely because the
  project was not manually saved;
- full Undo/Redo history does not have to survive a crash;
- successful normal Save publishes the project independently, then attempts to
  replace recovery with one fresh checkpoint; checkpoint failure does not fail
  Save.

There is no periodic full-project snapshot: the normal stream remains compact
version-4 TXN/HISTORY diffs because a large project snapshot can cost tens or
hundreds of milliseconds. An unsupported or oversized recovery transition marks
recovery degraded and waits for explicit Save/reattach to establish a fresh
checkpoint; it does not take a surprise per-edit full snapshot.
Version mismatch, replay limits, and record-size limits fail closed for
recovery without blocking live edits. Torn or corrupt input recovers only its
valid prefix, preserves the original journal for diagnosis/retry, and is never
automatically deleted as though fully consumed.

## 23. Authorization

The Dev API is open to any compatible local integration.

The packer does not verify “official Claude” identity and does not trust a
self-reported display name.

Authorization is binary per project path:

```text
external integrations allowed
or
external integrations denied
```

No granular capability scopes in v1.

### 23.1 Global modes

```text
Disabled
Ask for each project
Allow all projects
```

Default:

```text
Ask for each project
```

### 23.2 Project decision

In Ask mode:

- first connection to an existing canonical project path prompts Allow/Deny;
- permission is stored locally by canonical project path;
- permission is not written into the project repository;
- reopening the same path does not prompt again;
- moving/renaming the project file changes identity and prompts again;
- each connection gets a new temporary token.

A new unsaved project created through the integration is allowed for that runtime
session. Its path-based permission is recorded after first save according to the
normal Ask-mode policy.

### 23.3 Disconnect and revoke

Disconnect:

- ends the current connection;
- invalidates current token;
- keeps path permission.

Revoke:

- ends connection immediately;
- removes path permission;
- next Ask-mode connection prompts again.

### 23.4 Security boundary

Full project permission grants only the public packer API:

- inspect;
- edit;
- save;
- pack;
- export;
- Undo/Redo;
- minimal view focus.

It does not grant arbitrary OS, memory, process, network, or other-application
control.

## 24. MCP surface

The exact MCP tool catalog is intentionally deferred.

The stable principle is:

- internal operations may be numerous;
- CLI verbs may be numerous;
- MCP tools should remain compact;
- capability parity does not require one tool per CLI verb.

Likely categories:

```text
project discovery/binding
project inspect/query
project validate
project apply transaction
save/save-as/discard
pack/export/job control
Undo/Redo
minimal view reveal/focus
schema/report resources
```

The full operation vocabulary is provided as a versioned schema/resource.

---

## 25. Epic A non-goals

Not part of v1:

- several simultaneous MCP writers on one project;
- CRDT collaboration;
- field-level automatic merge;
- distributed Undo;
- arbitrary UI clicking/input injection;
- screenshot-driven automation as the primary API;
- mandatory daemon;
- TCP server;
- live-session behavior for ordinary CLI;
- persistent enterprise audit log;
- fine-grained authorization scopes.

---

## 26. Epic A acceptance criteria

### Session

- one canonical live session per canonical project path;
- no persistent project UUID;
- any number of GUI views;
- one external controller;
- second controller receives `session_busy`;
- explicit replacement is atomic;
- one MCP process serves one project;
- all model mutations pass through one serialized session queue.

### Startup and CLI boundary

- plain MCP can list projects;
- one unambiguous project may be selected automatically;
- ambiguous choice asks;
- `--project` attaches to an existing live session for the canonical path;
- bound process cannot silently switch;
- ordinary CLI remains file-oriented;
- mutating CLI returns `project_live` while a live session owns the path;
- GUI refuses silent overwrite after external project-file modification.

### Transactions

- stable ID targeting;
- CLI/MCP name selectors resolve to one ID or fail as ambiguous;
- atomic batches;
- revision conflict reporting;
- idempotent retry;
- GUI/MCP/Undo/Redo share one model commit point independent of recovery I/O;
- one batch equals one Undo entry.

### Revision and Undo

- revision is monotonically increasing;
- Undo/Redo create new revisions;
- Save does not change revision;
- dirty state uses saved semantic baseline;
- semantic forward/reverse tests;
- mixed GUI/MCP history;
- remove/restore exact entity data;
- successful Save appears only as a non-Undoable history checkpoint;
- history survives ownership transfer but not normal close/reopen.

### Pack and sources

- Pack uses an immutable snapshot;
- editing is allowed while Pack runs;
- a completed stale result remains viewable with `Preview out of date`;
- preview freshness is derived from `pack_input_hash`;
- pack cache is memory-only;
- inactive Pack results are compressed and LRU-evicted;
- ownership handoff cancels only running Pack work;
- source watchers update runtime source state/thumbnails but never auto-pack;
- external source changes do not change project revision or dirty state;
- source read failures remain runtime-only and are retried by Pack/Export.

### Recovery

- GUI crash mirror promotion only after proven death;
- clean ownership transfer;
- pending transaction retry;
- local journal/checkpoint recovery of the latest valid durable prefix;
- no requirement to restore crash-time Undo stack.

### Authorization

- global modes;
- persistent local decision by canonical project path;
- visible connected state;
- disconnect and revoke;
- untrusted display name grants no rights.

## 27. Epic A implementation slices

### A0 — operation foundation

- stable IDs;
- typed operations;
- per-operation label templates + argument schema (palette-ready registry, §6);
- transactions with committed author identity (`human` / `agent(<id>)`, §7);
- revisions;
- semantic Undo/Redo;
- tests against snapshot oracle.

### A1 — in-process session

- one session object;
- multiple GUI views;
- job model;
- pack cache;
- state events.

### A2 — local Dev API

- local transport;
- discovery;
- snapshot/resync;
- transaction apply;
- events;
- authorization.

### A3 — MCP mode

- unbound startup;
- project discovery;
- one-project binding;
- compact tools/resources;
- headless ownership.

### A4 — ownership/recovery

- GUI/MCP handoff;
- recovery mirror;
- stale-controller claims;
- local journal/checkpoints.

---

# Part III — Epic B: Format ecosystem and atlas interoperability

## 28. Epic objective

Make adding and using formats simple for both the packer maintainer and external
developers.

The system must support:

- built-in exporters;
- user-defined exporters;
- atlas importers;
- format detection;
- linked foreign atlas sources;
- exact sprite extraction;
- conversion/repacking into another format.

A project importer for another packer's settings/project files is not required.

---

## 29. Terminology

### Format

The complete contract for an atlas representation:

- input/output artifacts;
- data structure;
- coordinate conventions;
- transforms;
- page references;
- supported metadata;
- detection;
- import/export options;
- losses.

### Schema

A structural validation schema for one artifact.

Schema is part of a format contract, not the whole format.

### Format package

A versioned package registering one format identity and optional handlers:

```text
exporter
atlas importer
probe
options
tests
documentation
```

### Export IR

Stable immutable canonical model passed to format exporters.

### Import IR

Stable immutable canonical model returned by atlas importers.

---

## 30. Unified format-package contract

Built-in and external formats use the same conceptual descriptor and registry.

A package may provide:

- export only;
- import only;
- both;
- optional probe;
- options/presets;
- tests.

Conceptual package directory:

```text
texturepacker-json/
├─ format.json
├─ export.lua
├─ import.lua
├─ probe.lua          optional
├─ README.md
└─ tests/
```

A built-in package may contain only metadata and tests because its handler is
compiled into the application.

---

## 31. Package identity and versions

Each package declares:

```text
stable format ID
display name
manifest version
package version
format API version
supported data-format versions and profiles
handler entries
capabilities
extensions/signatures
options
```

These versions have separate meanings.

### Manifest version

Integer version of the `format.json` structure: field names, handler
declarations, capabilities, signatures, options, and artifact declarations.

It changes only for an incompatible manifest-contract change. Adding optional
backward-compatible metadata does not require a new version.

### Package version

Version of the concrete `.ntformat` implementation. It changes for bug fixes,
new supported data versions, improved probes, documentation, or implementation
changes.

The project does not pin `package_version`. The currently installed compatible
package is used, and its version/origin is recorded in inspect/export reports.

### Format API version

Version of the runtime contract between Neotolis and handlers: Lua/template host
services, Import/Export IR, diagnostics, and writer APIs.

Each Neotolis release supports one current major `format_api_version` and one
current `manifest_version`. Built-in formats are updated together with the
application. An external package with a different version is rejected with an
explicit migration/update error. Neotolis does not retain compatibility layers
for obsolete package API or manifest versions.

### Data-format version and profile

Version of the external atlas representation, such as PixiJS 1.3 or 1.4. A
single format package represents one logical format family and may support
multiple import/export data versions and compatibility profiles.

Export targets are declared once at the **project** level (§61.1). When creating
a target the user selects a concrete supported data version and optional profile.
Each project target stores:

```text
target = {
  id,
  format_id,
  data_version,
  profile,
  options,
  enabled,
  path_template,   # expands {atlas} {page} {scale} {scale_suffix}
  image_opts,      # v1: PNG (+premultiply per exporter caps)
  scale_variants   # default [{factor: 1.0, suffix: ""}]
}
```

A target does not store package, manifest, or Format API versions. Existing
targets do not automatically move to a newer data-format version when package
support is added.

Per-atlas state is a **participation record**, not a second target editor: for
each project target an atlas may carry `{ enabled_override?, path_override? }`.
An atlas's **effective targets** = project target rules composed with its
participation overrides. Target CRUD is project-scope (`target.*`); a
participation toggle is the atlas-scope `target.participation.set` operation.
CLI `--json` schemas, export preflight, and export orchestration always resolve
effective targets before writing (§50).

`scale_variants` reserves a schema slot only. Each variant would be its own pack
(sources scaled *before* packing so padding/extrude stay honest), interacting
with the pack-result LRU; the Defold-first path does not need it, so
implementation is deferred and schema stability is the point. Non-1.0 variants,
and the WebP / PNG-quantization `image_opts`, are deferred and not part of v1.

**Migration.** A project file that still carries identical per-atlas targets is
auto-lifted on load: identical per-atlas targets collapse to one project target
rule, and any per-atlas divergence becomes a participation override.

A new `format_id` is used for a materially different representation or semantic
contract, not for every compatible version increment.

`format_id` is a non-empty strict-UTF-8 machine token of at most 255 bytes
(256-byte storage including NUL), normally reverse-DNS for external packages.
The registry, project loader/model, typed operations, jobs, and clients enforce
this one bound; an ID is either preserved exactly or rejected, never truncated.

## 32. Handler implementation

A format identity is separate from the handler implementation.

Supported drivers:

```text
builtin
template
lua
```

### Builtin

Compiled C code shipped with `ntpacker`.

Used for:

- canonical Neotolis format;
- critical complex formats;
- formats requiring native libraries;
- reference implementations.

External DLL/SO plugins are not part of v1.

### Lua

Sandboxed external handler suitable for text, JSON/XML, multi-file, binary,
import, export, and optional content probes.

### Template

A constrained deterministic text-template engine for simple exporters. It is not
used for importers and must not grow into a general-purpose language. Complex
handlers use Lua.

### Automatic sandboxed execution

Installed and project-local Lua/template handlers execute automatically without
per-package trust prompts.

The sandbox is the security boundary. If it is not safe enough for automatic
execution, it must be fixed rather than delegating code review to the user.

A global `Disable external formats` mode and a diagnostic safe mode may be
provided. Package syntax/runtime/resource-limit failures are ordinary structured
errors and cannot partially mutate project state or publish partial output.

## 33. Format package storage

### Development form

An unpacked directory.

### Distribution form

```text
format-name.ntformat
```

`.ntformat` is a ZIP-compatible archive with `format.json` at the root.

### Discovery locations

```text
embedded built-ins
user-installed formats
<project>/.ntpacker/formats/
explicit --format-path
```

Project-local packages make Git and CI reproducible.

### Duplicate IDs

Duplicate format IDs are errors by default and report all origins.

Silent search-path shadowing is forbidden.

An explicit development override may be added separately.

---

## 34. Suggested format CLI

```text
ntpacker format list
ntpacker format inspect <id>
ntpacker format validate <path>
ntpacker format test <path>
ntpacker format install file.ntformat
ntpacker format uninstall <id>
ntpacker format rescan
```

Format origin and package version must appear in inspect output.

---

## 35. Export IR

Exporters receive a versioned immutable representation, not mutable project or
builder structures.

Required content:

```text
atlas metadata
pages
final ordered sprite names
frame placement
source size
trim
exact D4 transform
optional polygon vertices/indices
pivot
9-slice
aliases
animations
target options
structured notices
```

The IR must define:

- coordinate system;
- ordering;
- number representation;
- defaults;
- transform conventions;
- page naming semantics.

New optional fields may be append-only within a major IR version.

---

## 36. Format capabilities

Capabilities must express what the output format can represent.

They are used before export to calculate:

```text
project pack intent
∩ builder capabilities
∩ target-format capabilities
= effective target pack
```

### 36.1 Packing capabilities

Examples:

```text
exact allowed D4 members
rect/polygon geometry
multipage
```

A vague boolean `rotation: true` is insufficient.

Internally, exact transform support may be represented as an 8-bit D4 mask.

### 36.2 Metadata capabilities

Examples:

```text
pivot
9-slice
alias mode
animation frame list
animation fps
playback mode
animation flips
```

Some values are modes rather than booleans:

```text
aliases:
native
expanded
none
```

### 36.3 Canonical conventions

Handlers consume one canonical IR.

Do not force every manifest to describe arbitrary coordinate conversion rules
that the core does not need before packing.

The handler is responsible for mapping the canonical IR into its format.

### 36.4 Status

The exact public capability vocabulary is not yet frozen.

It must be derived from real fixtures for:

- Neotolis JSON;
- Defold;
- TexturePacker JSON Hash/Pixi.

---

## 37. Export behavior

A format exporter:

1. receives normalized immutable Export IR;
2. receives the target's selected data-format version, profile, and options;
3. creates only declared artifacts;
4. reports written artifacts;
5. reports target adaptations and metadata loss;
6. produces deterministic output for the current toolchain.

Multi-file formats are first-class. An exporter is not limited to one primary
extension.

### 37.1 Target-specific preview and diagnostics

A logical atlas participates in several project-level export targets with
different capabilities (§31, §61.1). The project retains the richest shared model;
each effective target computes its own effective pack and diagnostics.

GUI displays target-specific status when a target is selected and as a summary
badge in the target list. Export preflight and the final structured report repeat
the same information.

Examples:

- unsupported flips used only for placement -> compatible repack without flips;
- unsupported polygon placement -> compatible rectangle repack;
- unsupported pivot or 9-slice -> visible metadata warning/loss.

These conditions do not prohibit export. A blocking error is reserved for a
case where a valid declared output cannot be produced.

### 37.2 Delivery modes

Export orchestration first resolves each atlas's effective targets (project
target rules composed with per-atlas participation, §31) and then delivers the
same staged output in one of two modes:

- **directory** (default): each effective target writes to its resolved
  `path_template` location.
- **archive**: the staged output lands in one ZIP with a canonical
  `{atlas}/{target}/...` layout (per-target path templates are ignored) plus a
  versioned `manifest.json` (schema tag, tool version, project name, timestamp,
  and the file list with sizes/hashes). CLI: `ntpacker export --archive out.zip`.

Because staging is already atomic, archive mode reuses it; export scope and
dry-run behave identically in both modes. `Package project` -- handing an
editable project (project file plus sources, paths rewritten relative) to another
person -- is a distinct future command, not a result archive.

## 38. Atlas import scope

Only produced atlases are imported:

```text
descriptor
+ page texture(s)
+ related metadata files
```

The system does not attempt to restore another packer's project settings.

An importer reads a foreign atlas and returns canonical Import IR.

It does not mutate a project directly and does not write extracted PNGs directly.

---

## 39. Import IR

Conceptual structure:

```text
AtlasImport
├─ pages[]
├─ regions[]
├─ animations[]
└─ notices[]
```

A region contains what is available:

```text
stable importer-local key
name
page reference
packed frame/footprint
exact D4 transform
source_size
trim/source rect
optional polygon vertices/indices
pivot
9-slice
alias relation
```

The importer normalizes foreign conventions into the canonical Neotolis
coordinate/transform model.

---

## 40. Import detection and user selection

File extension is a candidate filter, not sufficient identity for shared
extensions such as `.json`.

Detection pipeline:

```text
extension candidates
-> exact markers / declarative signatures
-> optional bounded probe
-> ranked suggestions
-> visible user-selectable format field
```

### 40.1 Declarative signatures

Possible checks:

- magic bytes;
- text prefix;
- JSON root type;
- required JSON paths;
- forbidden JSON paths;
- exact marker values;
- related-file patterns.

### 40.2 Probe

An optional `probe.lua` belongs to one format package. It is a small read-only
recognizer, not an importer and not an authority.

It receives bounded input such as filename, extension, limited prefix bytes/text,
file size, and a safely parsed JSON root when applicable. It has no output-file
access and cannot mutate the project. It returns match/confidence/reason.

Probe failures are candidate diagnostics, not application failures.

### 40.3 Selection

- explicit `--format <id>` wins;
- an exact unique identity marker or only one installed compatible candidate may
  preselect the format;
- heuristic signatures/probes rank and preselect suggestions but do not hide the
  choice;
- the user can always choose another installed importer before import;
- after a failed import, `Change Format` remains available without removing and
  recreating the source;
- a format change is persisted only after the new importer successfully validates
  and reads the source;
- close candidates require explicit selection;
- installation order is never authority.

Once a linked atlas is accepted, its `format_id` is stored in the source and is
not silently changed on Refresh or reopen.

CLI returns candidates for an ambiguous format and requires an explicit
`--format`. MCP may present/rank candidates but must not silently commit an
ambiguous selection.

## 41. Neotolis format marker

The canonical JSON should include an explicit identity marker:

```json
{
  "format": "neotolis-atlas",
  "version": 1
}
```

`generator` information may be included but is not format identity.

This improves unambiguous import detection.

---

## 42. Atlas source

A foreign atlas may be added to the current Neotolis atlas as a source.

Project record:

```json
{
  "id": "source_...",
  "kind": "atlas",
  "path": "legacy/ui.json",
  "format_id": "org.neotolis.texturepacker-json",
  "options": {}
}
```

Only the source definition and explicit selected format/options are persisted.
Derived regions are not serialized into the project.

### 42.1 Read-only rule

Linked atlas sources are read-only in v1.

The user may:

- inspect regions;
- repack them;
- export them;
- extract them;
- change the selected importer through an explicit validated `Change Format`.

The user may not persist per-region edits directly on the linked source. To edit
the images normally, use Extract Sprites. This avoids stale override
reconciliation.

### 42.2 Refresh and watcher behavior

The foreign source is re-read:

- at project open;
- after relevant watcher invalidation/debounce;
- on Refresh;
- before Pack;
- before Export;
- before Extract Sprites.

The current external files are authoritative. The importer reports companion
files to watch. A changed descriptor may replace that companion set.

No last-known-good region snapshot is project state. A last successful thumbnail
or preview may remain visible only as runtime feedback after a read error.
Temporary in-process caching is allowed only as an optimization.

External atlas changes update source generation and Pack freshness but do not
increment project revision or create Undo entries.

### 42.3 Errors

Examples:

```text
descriptor missing
page missing
format package unavailable
manifest/API incompatibility
malformed descriptor
invalid region
duplicate region name
```

These are ordinary runtime source errors analogous to missing or unreadable image
files. They are retried by Refresh/Pack/Export/Extract and are not persisted in
the project.

## 43. Region materialization

When Neotolis packs a linked atlas source, every region is materialized into a
canonical RGBA image.

### 43.1 Rectangular region

```text
load page
select packed footprint
inverse D4 transform
restore trim into source_size canvas
```

### 43.2 Polygon region

A polygon region may have an AABB overlapping another region's AABB.

The imported polygon/triangles define which page texels belong to the sprite.

```text
load page
select footprint
apply triangle mask
inverse D4 transform
restore trim into source_size canvas
```

The imported polygon is a decode mask only.

It is not reused as the polygon for the new Neotolis pack.

### 43.3 Pixel-art correctness

D4 transforms are exact integer texel permutations:

```text
identity
90/180/270 rotations
horizontal/vertical flips
diagonal transforms
```

No interpolation is required.

Pixel art is not blurred or resampled.

### 43.4 New pack

After materialization:

```text
RGBA source
-> Neotolis alpha analysis
-> Neotolis trim
-> Neotolis hull generation
-> Neotolis packing
```

The existing raw-RGBA pack-input path should be used.

No temporary PNG is required.

---

## 44. Extract Sprites

`Extract Sprites` is a first-class operation on a linked atlas source. It always
operates in the same current Neotolis atlas. There is no destination project or
destination atlas selector.

### 44.1 Primary UI

```text
Atlas source: legacy/ui.json
[Extract Sprites...]

Output folder:
sprites/ui/

[Extract]
```

The only required user choice is the final output folder.

Suggested default:

```text
<project>/sprites/<source-name>/
```

### 44.2 Preflight, staging, and commit

Extraction uses this order:

1. Re-read the current foreign descriptor/pages.
2. Materialize all regions and compute final output names.
3. Validate traversal, absolute paths, platform collisions, and existing-file
   conflicts for the complete operation.
4. Write and verify every PNG in a temporary staging directory beside the final
   destination.
5. Publish staged files into the output folder.
6. Replace the linked atlas source with the output folder source.
7. Transfer recoverable pivot, 9-slice, animations, names, and alias metadata.
8. Commit project-model changes as one semantic transaction.

If preparation fails, staging is removed and neither the final output nor project
model changes. The exact crash-recovery manifest for a failure during final file
publication remains an implementation contract.

### 44.3 Existing files

The default is fail-without-changes when any output path conflicts.

GUI may offer an explicit `Replace conflicting files` confirmation. CLI/MCP must
pass an explicit `overwrite: true`. Automatic suffix renaming is forbidden
because it changes sprite names and references.

### 44.4 Undo

Undo restores project state only. Created/published files are not automatically
deleted by Undo because deletion could destroy later user edits.

### 44.5 Image size

Extracted PNGs always restore the full available `sourceSize`. Neotolis performs
trim again later when packing.

This preserves animation-frame alignment, original canvas, pivot coordinate
meaning, and 9-slice coordinate meaning.

If the foreign format does not contain source size, use the best recoverable
bounds and emit a notice. No `Original size / Trimmed` selector is required in
the primary workflow.

### 44.6 Paths and aliases

Extraction must reject traversal, normalize separators, prevent absolute output
names, detect case-insensitive collisions where relevant, and report the complete
region-to-file mapping.

A separate PNG is written for every visible region name, including aliases.
Identical bytes are intentional. The extracted folder remains understandable
without a sidecar, and normal packer deduplication may merge identical runtime
content later.

## 45. Lua external handlers

Lua is the full scripting runtime for importers, probes, binary formats, and
complex exporters.

Handlers receive:

- immutable IR/input views;
- safe structured helpers;
- bounded companion-file access;
- declared staged output access.

Not exposed:

```text
os
unrestricted io
package
debug
network
process execution
arbitrary filesystem access
native module loading
```

External packages execute automatically inside this sandbox; no per-package trust
prompt is required.

### 45.1 Host services

```text
JSON parse/encode
text writer
binary reader/writer
safe relative file resolution
path helpers
structured errors/notices
ordered iteration
logging
```

### 45.2 Failure isolation and cancellation

Each handler invocation uses protected calls and an isolated Lua state with a
custom allocator.

Execution may run on a worker thread for GUI responsiveness, but arbitrary OS
thread termination is not the cancellation mechanism. A Lua instruction/count
hook checks cancellation and execution budgets and raises a controlled Lua error.

Syntax, runtime, cancellation, memory, instruction, input, output, and traversal
failures:

- return structured diagnostics with package/handler/file/line context;
- discard Import/Export IR from the failed invocation;
- remove staged output;
- do not mutate project model;
- do not replace a successful preview/output;
- cannot crash the core through ordinary handler errors.

### 45.3 Resource limits

- custom memory allocator/budget;
- instruction/time budget enforced by hook;
- input/output limits;
- protected calls;
- bounded related-file traversal;
- deterministic locale and formatting;
- stricter small budgets for probes.

### 45.4 Binary formats

Provide a host binary API:

```lua
local w = output:binary("atlas.bin")
w:u32le(count)
w:string_u16(name)
```

This is preferable to requiring every author to implement bounds and endianness
rules manually.

## 46. Template and Lua handlers

External format packages support both:

```text
template
lua
```

Builtin C handlers remain available for bundled reference and complex formats.

### 46.1 Template handler

Templates are intended for simple deterministic text exporters where output is
primarily a projection of Export IR:

- JSON;
- XML;
- YAML;
- text atlas formats;
- JavaScript or source-code descriptors;
- simple multi-file text formats.

A template handler must provide:

- safe string/JSON/XML escaping;
- deterministic iteration;
- loops;
- conditions;
- optional-field handling;
- stable comma/separator helpers;
- declared output artifacts;
- structured diagnostics.

Templates must not receive mutable project internals.

The template runtime must remain deliberately small. Complex calculations,
parsing, binary output, non-trivial related-file logic, or import behavior belong
in Lua.

### 46.2 Lua handler

Lua is used for:

- atlas importers;
- complex exporters;
- content probes;
- binary formats;
- multi-file formats with non-trivial logic;
- transformations that exceed the template runtime.

### 46.3 Mixed package

One format package may use different drivers in each direction:

```json
{
  "export": {
    "driver": "template",
    "entry": "export.tpl"
  },
  "import": {
    "driver": "lua",
    "entry": "import.lua"
  },
  "probe": {
    "driver": "lua",
    "entry": "probe.lua"
  }
}
```

### 46.4 Boundary rule

Do not evolve the template language into a general programming language.

When a format needs custom arithmetic, parsing, state, binary encoding, or
complex branching, the package should switch that handler to Lua rather than
adding more template syntax.

Both runtimes use the same versioned Export IR, package manifest, capability
model, artifact declarations, diagnostics, deterministic rules, and package
tests.

---

## 47. Reference formats

### 47.1 Neotolis JSON

- builtin export;
- builtin import;
- full-fidelity reference;
- explicit format marker;
- first D4/polygon/multipage extraction oracle.

### 47.2 TexturePacker JSON Hash / PixiJS

Treat the data format separately from the consuming engine.

One package may expose compatibility profiles:

```text
generic TexturePacker JSON Hash
PixiJS
Phaser
```

This should be the first external Lua reference package.

It exercises:

- JSON detection;
- trim;
- 90-degree rotation;
- anchor/pivot;
- 9-slice borders;
- animations;
- import and export.

### 47.3 Defold

Keep built-in initially.

Import entry points:

```text
.tpatlas -> complete import with animations
.tpinfo  -> layout-only import
```

The importer resolves related files through the safe companion-file API.

---

## 48. Format-package testing

A package should support self-contained fixtures:

```text
tests/
├─ export/
├─ import/
└─ detect/
```

Test categories:

- deterministic output;
- declared artifact list;
- malformed input;
- missing companion files;
- all supported transforms;
- capability notices;
- multipage;
- aliases;
- animations;
- import/export round trip where meaningful.

Commands:

```text
ntpacker format validate <path>
ntpacker format test <path>
```

Built-in packages run through the same descriptor-level tests.

---

## 49. Epic B acceptance criteria

### Package system

- built-in and external formats share descriptor/registry contract;
- unique stable format IDs;
- separate manifest/package/API/data version concepts;
- one current manifest/API version per Neotolis release;
- incompatible external packages are rejected explicitly rather than run through
  compatibility layers;
- one package may expose several data-format versions and profiles;
- project stores `format_id`, selected data version/profile/options, not package or
  API versions;
- directory and `.ntformat` loading;
- project-local packages;
- automatic sandboxed execution without trust prompts;
- duplicate IDs rejected;
- package origin/version reported.

### Detection

- extensions filter;
- exact markers and declarative signatures;
- optional bounded probe as a suggestion/ranking mechanism;
- visible user-selectable importer;
- explicit format override;
- ambiguous JSON produces candidate list and requires selection;
- user can change format later through validated `Change Format`;
- selected `format_id` never silently changes on Refresh;
- Neotolis marker detected unambiguously.

### Export

- immutable Export IR;
- exact capabilities;
- target stores concrete data-format version/profile;
- targets are declared at project scope; each atlas carries a participation
  record, and orchestration resolves effective targets before writing (§31, §61.1);
- directory and `--archive` delivery, the latter with versioned `manifest.json`
  (§37.2);
- deterministic artifacts for the current toolchain;
- multi-file output;
- target-specific adaptations and metadata losses shown before export and in
  reports;
- unsupported capabilities warn/adapt but do not prohibit export;
- external Lua/template exporter.

### Import

- immutable Import IR;
- current source re-read;
- rectangular and polygon regions;
- all D4 transforms exact;
- trim/source-size recovery;
- multipage;
- structured runtime source errors.

### Linked sources

- one persisted atlas source record;
- regions derived on demand;
- read-only;
- watcher refresh plus open/Refresh/Pack/Export/Extract verification;
- raw RGBA passed to normal packer;
- external source changes do not change project revision.

### Extract Sprites

- only output folder prompt;
- same current atlas;
- full preflight and staging;
- conflict default is fail; overwrite is explicit;
- linked source replaced by folder only after files are prepared/published;
- full sourceSize PNGs;
- metadata transferred;
- files survive Undo;
- path and collision safety;
- one PNG for every visible alias name.

### Lua

- no OS/network/process/native-module access;
- memory/instruction/output limits;
- hook-based cooperative cancellation;
- JSON and binary host APIs;
- deterministic execution;
- package errors cannot mutate project or publish partial output;
- sandbox is sufficient for automatic execution.

## 50. Epic B implementation slices

### B0 — native import foundation

- Import IR;
- native Neotolis importer;
- materializer;
- all D4/polygon tests;
- `atlas detect/inspect/extract`.

### B1 — project atlas sources

- canonical tagged-source extension with linked-atlas descriptors;
- linked atlas source;
- read-only behavior;
- refresh;
- raw RGBA ownership/cache;
- Extract Sprites transaction.

### B2 — package registry

- manifest descriptor;
- embedded built-in manifests;
- directory discovery;
- `.ntformat`;
- versions/origins;
- signatures;
- project-level targets + per-atlas participation; export orchestration resolves
  effective targets (project rules composed with participation), directory and
  `--archive` delivery with versioned `manifest.json` (§31, §37.2).

### B3 — Lua

- sandbox;
- bindings;
- JSON/binary services;
- probe;
- package tests.

### B4 — reference formats

- TexturePacker JSON Hash/Pixi;
- Defold import;
- libGDX candidate;
- manual grid-sheet importer candidate.

---

# Part IV — Product review and prioritization

## 51. Are the two epics complete enough to implement?

Yes, at the architectural level.

Both epics now have:

- product purpose;
- core data model;
- process model;
- security boundary;
- major workflows;
- failure behavior;
- acceptance criteria;
- staged implementation.

They do not yet have every final public JSON schema or every command signature.
That is appropriate.

Those details should be fixed through executable fixtures and tests, not invented
fully before implementation.

---

## 52. Remaining design details

The major product decisions are accepted. The following contracts remain to be
finalized through C structs, schemas, fixtures, prototypes, and golden tests.

### 52.1 Public capability vocabulary

Finalize exact transform, geometry, alias, animation, and metadata modes from
real Neotolis JSON, Defold, and TexturePacker JSON/Pixi fixtures.

### 52.2 Manifest and API field names

Finalize:

- `format.json` schema under the selected `manifest_version`;
- Export IR schema;
- Import IR schema;
- Dev API methods;
- MCP tools/resources;
- CLI JSON schemas.

These names must be tested as public contracts before release.

### 52.3 Cache budgets and representation

Architecture is fixed as:

- memory-only compressed Pack-result LRU;
- separate lazy thumbnail CPU/GPU LRU;
- pack-result page thumbnails (mip or cached downscale) served to the Project
  overview at 30-atlas / 5000-sprite scale without full-resolution page residency;
- no general persistent full-resolution decoded-source cache in v1;
- selected full source preview on demand;
- temporary Pack input memory governed by the engine-builder ownership contract.

Concrete byte budgets, compression representation, and GPU residency thresholds
remain implementation policy.

### 52.4 Template syntax

Choose or implement the constrained template syntax and escaping/helper set. The
accepted product decision is that templates exist and are export-only; exact
syntax is not yet fixed.

### 52.5 Authority state machine

Finalize stale-controller proof and the singular authority cutover state
machine. Journal framing and local compaction remain version-4 contracts;
cross-host mirror/cutover behavior must preserve ordered best-effort recovery,
the 5-second healthy durability target, and an explicit recovery watermark
without turning it back into a model commit gate.

## 53. Critical review

### 53.1 Strongest existing advantage

The product already has a technically differentiated packing core and deterministic
AI-capable CLI.

The risk is building infrastructure that users do not immediately feel.

### 53.2 Highest near-term user value

Epic B provides a direct visible value proposition:

```text
open an existing atlas
inspect it
split it
use it as a source
repack it
export it for another engine
```

This is easier to demonstrate and broader than live MCP control.

### 53.3 AI epic value

Epic A is strategically strong, but the existing CLI already enables many agent
workflows.

The most valuable unique part of Epic A is not “Claude can run a command.”
It is:

```text
Claude and the human edit the same unsaved project
with shared Undo and immediate GUI feedback
```

That should remain the standard for considering the epic complete.

### 53.4 Main architecture risk

Both epics can trigger large refactors simultaneously.

Do not implement:

- source-model extension;
- semantic Undo;
- Dev API;
- Lua;
- package manager;

in one long branch.

Deliver vertical slices with tests and user-visible outputs.

### 53.5 GUI priority

**Owner priority override (2026-07-20):** with no users yet, code order is
optimized for low rework, not demo value. The canvas/workspace UI refactor is now
a first-class phase U (see §54.6 and §61), sequenced before both epics. This
supersedes "GUI visual polish is secondary" and "avoid unrelated GUI redesign"
below for the canvas/workspace surfaces; cosmetic polish as a standalone goal and
a full GUI rewrite remain non-priorities.

GUI visual polish is secondary, but minimal GUI hooks are still needed:

- linked source display;
- source error state;
- Extract Sprites action;
- connected integration indicator;
- authorization prompt;
- Undo labels.

Avoid unrelated GUI redesign during either epic.

---

## 54. Recommended execution order

The target architecture is intentionally long-lived. The phases below are an
implementation order, not a reduction of the final design.

### Phase 0 — shared identifiers and boundaries

1. Remove persistent project ID assumptions; canonical path identity.
2. Persistent random IDs for atlas/source/animation/target.
3. Deterministic sprite IDs and selector resolution.
4. Typed model operations separated from session commands/jobs.
5. Transaction/revision/dirty-state tests.
6. Canonical tagged path-source schema; no legacy project-file migration.
7. Preserve deterministic save behavior.

### Phase 1 — source runtime and Epic B immediate value

1. Canonical path and RGBA normalization.
2. Runtime source status/generation and watchers.
3. Lazy thumbnail LRU and source-error UX.
4. Import IR and native Neotolis import.
5. Atlas inspect and Change Format workflow.
6. Atlas Extract Sprites with preflight/staging/conflict safety.
7. Polygon/D4 tests.
8. Linked atlas source and raw-RGBA materialization.

This creates a marketable interoperability workflow without requiring Lua or MCP.

### Phase 2 — semantic history and Pack session behavior

1. One serialized session queue.
2. Semantic diff types and forward/reverse tests.
3. Monotonic revision and semantic saved baseline.
4. Shared History presentation and Save checkpoints.
5. Immutable async Pack jobs.
6. `pack_input_hash`, stale-preview UX, memory-only compressed Pack-result LRU.
7. Ownership contract tests with `neotolis-engine` raw RGBA inputs.

### Phase 3 — format package system

1. Manifest with explicit manifest/package/API/data version separation.
2. Built-in descriptors.
3. Detection suggestions and explicit selection.
4. Sandboxed automatic Lua/template execution.
5. TexturePacker JSON Hash/Pixi family with multiple data versions/profiles.

### Phase 4 — live AI

1. In-process session abstraction.
2. Local Dev API and recovery-health/watermark semantics.
3. MCP mode.
4. Authorization keyed by canonical path.
5. Ownership/handoff/recovery and mirror promotion.

### Phase 5 — breadth

- Defold importer;
- libGDX;
- optional explicit auto-pack/watch mode if later desired;
- additional compatibility profiles;
- template runtime refinement;
- GUI refinements.

### 54.6 Execution-order revision (2026-07-20 owner override)

The Phase 0-5 decomposition above remains the historical work breakdown. With no
users yet, the owner re-sequenced delivery to minimize rework. The revised spine
is:

```text
Base (H0 + F2 + F3) -> Phase U -> Epic B (B0 -> B1 -> B2 -> B3) -> Epic A -> Breadth
```

- **Base** is the shared engineering foundation: H0 (fallible builder
  containment, decision 0018), F2 (typed operations, transactions, minimum
  journal), and F3 (live session, semantic history, pack-result behavior).
  M0-M5 are already done.
- **Phase U** is the canvas/workspace UI refactor (§61) -- promoted to a
  first-class phase and built *once*, before either epic.
- **Rationale:** both epics are GUI-heavy, and their core is a thin adapter over
  owned snapshots; building the canvas once on top of the finished Base avoids
  reworking it twice. This is why the UI refactor precedes the epics rather than
  trailing them.
- **B before A is preserved:** Epic A sits on Epic B's package runtime, so B
  ships first.
- **F3 prerequisite relaxed** from `{F2, B1, H0}` to `{F2, H0}`. The "Extract
  Sprites composes with History" concern that motivated the old B1 dependency
  moves to B1's own gate; F3 no longer waits on B1.
- **B0 (pure-core import, no GUI) may run in parallel with Phase U**, since it
  touches no canvas surface.

This is a re-sequencing plus targeted additions, not a reduction of the target
architecture: every Part II AI/Dev-API/MCP contract and the Part III
format-package/target/Export-IR design survive unchanged, and determinism remains
a core property with no dedicated UI.

## 55. Product positioning after both epics

A concise positioning statement:

> Neotolis Texture Packer is an open, deterministic atlas tool that packs true
> silhouettes, imports and splits existing atlases, exports to extensible runtime
> formats, and lets humans and AI agents edit the same live project safely.

The product is then differentiated by the combination, not one isolated feature:

```text
better packing
+ format interoperability
+ open extension system
+ complete automation
+ live human/AI collaboration
```

---

## 56. Final non-goals

The consolidated plan does not require:

- replacing the current GUI;
- importing TexturePacker project files;
- preserving a foreign polygon for new packing;
- continuously synchronizing changed linked-atlas regions;
- persistent derived atlas regions in project JSON;
- several AI writers per project;
- external native DLL plugins;
- plugin marketplace;
- arbitrary filesystem/process access for plugins;
- immediate support for dozens of formats;
- external native DLL plugins or a plugin marketplace.

Clarifier (2026-07-20): "replacing the current GUI" stays a non-goal. Evolving the
canvas/workspace surfaces *within* the current GUI stack (phase U, §61) is in
scope; a from-scratch GUI replacement is not. Rejected canvas/UX directions are
listed in §61.4.

---

## 57. Definition of success

### Epic A succeeds when

A user can open a project in GUI, connect one AI agent, ask it to perform a
multi-step edit, inspect the changes live, press Undo once, and restore the exact
previous state and preview without the AI editing a hidden second copy.

### Epic B succeeds when

A user can import a TexturePacker/Pixi or Neotolis atlas containing trim,
rotation, polygons, pages, and animations; inspect it; use it directly as a
read-only source; extract full-size PNGs into a selected folder in the same
current atlas; repack them with Neotolis; and export to another supported format.

---

## 58. Source-of-truth documents after this consolidation

This document supersedes the accumulated decision-history versions for product
and architecture planning.

Implementation should additionally remain grounded in:

- current repository code;
- format fixtures;
- per-format specifications;
- operation schemas;
- executable acceptance tests.

The 2026-07-13 UX consolidation adds these companion documents (their normative
canvas/workspace content is folded into §61):

- [`design/ux-vision-2026-07-13.md`](design/ux-vision-2026-07-13.md) -- vision and
  rationale;
- [`design/ux-master-spec-delta-2026-07-13.md`](design/ux-master-spec-delta-2026-07-13.md)
  -- schema/contract basis, consolidated into §61;
- [`plans/ux-epics-2026-07-13.md`](plans/ux-epics-2026-07-13.md) -- execution
  breakdown;
- [`design/mockups/ntpacker-fakeshots.html`](design/mockups/ntpacker-fakeshots.html)
  -- visual reference;
- [`design/ux.md`](design/ux.md) -- living interaction contract (normative source
  is §61).

Old draft files remain useful only as research history.

---

## 59. Settled implementation-policy decisions

The following decisions are final for this specification unless explicitly
reopened.

### Identity and project/session boundaries

1. Saved project identity is canonical project-file path.
2. Persistent `project_id` is removed entirely.
3. Unsaved projects use a temporary runtime session ID.
4. Atlas, source, animation, and export target receive random persistent 128-bit
   IDs.
5. Every sprite has a deterministic ID from `source_id + normalized source key`.
6. CLI and MCP may use ID or a human-friendly selector; canonical operations use
   the uniquely resolved ID.
7. Neotolis never renames physical source files. External rename is remove + add.
8. Source keys use UTF-8, `/`, NFC, preserved case, and portability validation.

### Transactions, revision, history, and CLI

9. Ordinary CLI remains one-shot and file-oriented.
10. Mutating CLI is blocked with `project_live` while a live session owns the
    canonical path; explicit offline force may bypass it.
11. GUI detects external modification of the project file before save.
12. Undo/Redo create new monotonically increasing revisions.
13. Save does not change revision.
14. Dirty state compares current semantic state with saved baseline.
15. Save is a session command, not a model operation or Undo entry.
16. Successful Save may appear as a non-Undoable History checkpoint.
17. Undo/Redo history is live-session-local, survives ownership transfer, and
    resets after normal close/reopen.
18. All live-model mutation passes through one serialized session queue.
19. GUI, MCP, Undo, and Redo share one model commit contract. Recovery recording
    follows the ordered committed stream but is not a visibility gate; a healthy
    backend advances its durability watermark within 5 seconds, while degraded
    recovery is persistent and explicit. A full project save is not required.

### Pack, sources, and memory

20. Pack runs on an immutable snapshot while the user continues editing.
21. A completed result remains viewable even when stale; freshness is derived
    from `preview_hash == current_pack_input_hash`.
22. Project dirty and preview freshness are independent.
23. Pack does not automatically start after source watcher changes,
    project edits, or an Undo/Redo cache miss; stale preview remains visible until
    the user explicitly runs Pack.
24. Running Pack is the only job cancelled/restarted for ownership transfer;
    short commands finish at a safe boundary.
25. Pack-result cache is memory-only; inactive results are compressed and managed
    by a dedicated byte-budget LRU.
26. GUI thumbnails use a separate lazy CPU/GPU LRU, preserve aspect ratio, and
    have a maximum side of 256 physical pixels.
27. Full decoded source images are not retained in a general RAM LRU in v1.
28. Raster formats normalize to deterministic RGBA8; semantic image hash uses
    width, height, and RGBA pixels.
29. Raw file bytes/mtime are only change/decode-cache inputs, not Pack semantics.
30. Raw RGBA lifetime during Pack is governed by the `tp_build -> neotolis-engine`
    ownership contract and later profiling.
31. Watchers update source runtime state and thumbnails automatically but do not
    change project revision, dirty state, or Undo history.
32. Source errors are runtime-only. Pack/Export retry current reads and fail
    explicitly rather than silently using old full source pixels.

### Format packages and interoperability

33. External format packages support constrained templates and sandboxed Lua.
34. Importers and probes use Lua; templates are export-only.
35. Sandboxed Lua/template packages execute automatically without trust prompts.
36. Sandbox uses immutable inputs, staged outputs, memory/instruction/output
    limits, protected calls, and hook-based cooperative cancellation.
37. `.ntformat` declares separate `manifest_version`, `package_version`, and
    `format_api_version`.
38. Each Neotolis release supports one current manifest/API version; no old API
    compatibility layer is required.
39. One package represents a logical format family and may support several
    external data-format versions and compatibility profiles.
40. Export targets are declared at **project** level and store `format_id`,
    concrete `data_version`, optional profile, options, `enabled`,
    `path_template`, `image_opts`, and a reserved `scale_variants` slot; they do
    not store package/API/manifest versions. Each atlas holds a per-target
    participation record (`enabled_override?`, `path_override?`); an atlas's
    effective targets = project rules composed with participation. Identical
    legacy per-atlas targets auto-lift to one project rule on load (§31, §61.1).
41. Package version is not pinned in project; reports record the used toolchain.
42. Probe/signature results are suggestions. Exact unique identity may preselect,
    but users can always change the importer.
43. Ambiguous import requires explicit selection. Once accepted, source
    `format_id` does not silently change.
44. Target limitations are shown in GUI before export and in reports. Compatible
    adaptation and metadata loss do not prohibit export; only invalid output is a
    blocking error.
45. Linked atlas sources remain read-only and are refreshed by watchers plus
    open/Refresh/Pack/Export/Extract verification.
46. Extract Sprites uses complete preflight and staging.
47. Existing-file conflicts fail by default; overwrite must be explicit; no
    automatic suffix rename.
48. Extract Sprites writes one PNG for every visible region name, including
    aliases.
49. Extracted files survive Undo; Undo restores project model only.

### Recovery and implementation discipline

50. Ownership transfer preserves current live-session history and committed
    state, but never transfers runtime worker/thread state.
51. Crash recovery restores the latest valid durable recovery prefix, may lose
    the bounded recent tail, and need not restore the full Undo stack.
52. Exact public schema/method names are finalized through implementation,
    fixtures, and golden contract tests.
53. The final architecture remains complete and long-lived; vertical slices are
    implementation order, not removal of target behavior.
54. Invalid builder input, output/filesystem failure, and a builder worker crash
    return structured diagnostics and cannot terminate a client host or replace
    its last successful preview.

---

## 60. Open decisions after consolidation

The following items were not decided in the discussion and must not be treated as
settled:

1. **Ownership state machine.** Exact process-claim mechanism, proof that a host
   is dead, and the singular authority-cutover protocol.
2. **Cache budgets.** Exact CPU/GPU memory budgets, compressed Pack
   representation, and eviction thresholds.
3. **Template language.** Concrete syntax and the final mechanically enforced
   boundary between template and Lua.
4. **Capability vocabulary and public schemas.** Exact field names and modes for
   manifests, Import/Export IR, Dev API, MCP resources/tools, and CLI JSON.
5. **Extraction publication crash recovery.** Exact manifest/cleanup behavior if
   the process fails during final staged-file publication.
6. **Color-management profile.** Exact deterministic ICC/gamma/orientation
   normalization rules for all supported raster decoders.
7. **Note style tokens.** The fixed swatch-palette color tokens (10-12) and the
   exact note-style schema field names (§61.2).
8. **Workspace schema field names.** The `workspace` section field names -- board
   positions and note records (§61.2).
9. **App-state file location.** The per-platform location of the view-state /
   app-state file that lives outside the project (§61.3).
10. **Path-template grammar.** The exact template grammar and escaping for
    `{atlas}`, `{page}`, `{scale}`, and `{scale_suffix}` (§31, §61.1).

Deferred, not open: non-1.0 scale-variant image options (WebP, PNG quantization)
and arbitrary (non-token) note colors are settled as out of v1 (§31, §61.2), not
awaiting a decision.

These are open contracts, not missing product direction. They should be closed by
focused prototypes, fixtures, and acceptance tests before the corresponding API
is released.

---

## 61. Canvas UI, workspace, and interaction model (2026-07-13 UX consolidation)

This section is the normative home for the canvas/workspace contracts settled in
the 2026-07-13 UX session (consolidated from
[`design/ux-master-spec-delta-2026-07-13.md`](design/ux-master-spec-delta-2026-07-13.md)).
Full interaction detail lives in [`design/ux.md`](design/ux.md); where the two
overlap, this section governs.

### 61.1 Two-level canvas and unified project tree

**Two canvas levels.** The workspace has exactly two canvas levels, Project and
Atlas, and no third "page" level:

- **Project level** shows every atlas as a group-card: name, health badges
  (fill % / outdated / not packed / warning), a visibility eye, and the last
  pack's page thumbnails inside the card. Pages of different atlases never mix in
  one flow; only whole atlas cards are neighbors, and a very large atlas shows the
  first N pages plus a "+N pages" tile. Batch actions ("Pack stale (N)",
  "Export all") live here.
- **Atlas level** is the working level: all of one atlas's pages are laid out
  spatially at once. Page tabs are removed; double-clicking a page zooms the
  camera to it (a camera move, not a mode change), down to texels. Sprites are
  selectable directly at this level with the usual tree/inspector synchronization.

Breadcrumbs (`Project > <atlas>`) ascend; double-clicking a Project group-card
descends. A single continuous canvas and a third page level are rejected (§61.4).

**Unified project tree.** The left panel is one project tree with a clickable
**project root node**; selecting the root shows project settings and export rules
in the inspector (the project is a tangible object, not a hidden mode). The tree
is root -> atlases -> folders/sprites -> a per-atlas Animations node, replacing the
separate ATLASES / SPRITES / ANIMATIONS sections. A project-wide filter (Ctrl+F)
matches anywhere and shows hits under their atlases. Cross-atlas drag works in the
tree; pages are packer output and are never drag targets.

**Sorting is view state, not data.** Four keys -- name (default), size (packed
area), file `mtime` (read live), and `added_at` -- each with two directions
(re-clicking the active key flips it), plus an independent "warning on top"
checkbox that overlays any sort. A key applies within each tree level. Atlas order
in the project and animation frame order stay manual (undoable operations);
filesystem-derived folder/file order mirrors disk and is never manually reordered.

**Thumbnails are the default.** Sprite rows and atlas rows always show previews
from the async thumbnail cache; hovering shows a large 1:1 preview. The sprite
area additionally has list and grid display modes (a header toggle, view state).
The atlas "cards view" is the Project canvas level, not a panel mode. Overview
thumbnails are served from the pack-result store without full-resolution
residency (§10.4, §52.3).

**Export UX.** Export rules are edited only at the project root; the atlas
inspector shows a compact participation block (per-target enable plus optional path
override, §31). Ctrl+E opens preflight only -- it never edits targets: scope is the
current selection (nothing selected at Project level = all), and it shows a dry-run
file list, amber overwrites, degradation notices, a "Copy CLI command" action, and
an optional "Export as ZIP" (§37.2). Repeat-last-export is Ctrl+E then Enter.

**Scale calibration.** UX performance budgets are validated against a
30-atlas / 5000-sprite bench fixture (the owner's real scale): sub-100 ms
interactions, non-blocking refresh, virtualized lists, and a bounded thumbnail
cache.

### 61.2 Workspace schema: board positions and notes

The project file gains a `workspace` section holding board positions and text
objects. It is freeform with **no auto-reflow**.

**Board positions.** Every atlas board has a stored position on the Project
canvas, auto-assigned to free space when the atlas is created. The tool never
moves boards on its own: hiding an atlas leaves its spot empty (no compaction; an
"N hidden" chip restores it), board growth after Pack wraps pages inside the board
(huge atlases show a "+N pages" tile) and never pushes neighbors, and shrinking
leaves the space. Collisions render as legal overlap (z-order, last-touched on
top) and are non-semantic: dropping an atlas onto an atlas changes no data.
"Move atlas" (`atlas.board.move`) is a named undoable operation; "Tidy up"
(`workspace.tidy`) is the only tool-initiated movement and is user-invoked.

**Notes (text objects).** A note is an ordinary model object -- a typed operation
with a named history entry ("Add note ... on enemies"), undoable -- not a drawing.
It is content + parent + **per-object style** (no mixed-run rich text in v1):
`{ size: S|M|L|XL, bold, italic, text_color: token, bg_color: token|none, align }`,
where color tokens come from a fixed swatch palette (10-12); `bg=none` renders as a
plain label, a background renders as a sticker. Parenting is spatial and automatic
(one object type, no "kind" choice at creation): a note dropped on an atlas board
is parented to that atlas (board-relative coordinates; it moves with the board,
hides with the atlas eye, and is deleted with the atlas -- the delete confirmation
states "and N notes", undo restores); a note dropped on empty canvas is parented to
the project (absolute coordinates). Re-parenting is a drag between board and canvas
(`note.reparent`). In v1 notes live on the Project level only (there is no stable
anchor inside a regenerated atlas pack).

Notes are visible everywhere a model object is: in the tree (attached notes as a
collapsed "Notes (N)" group inside their atlas, free notes as a "Notes" group under
the project root after the atlases; click zooms the canvas to the note), in the
inspector (editable), in the project-wide filter (Ctrl+F searches note text), and
over MCP / Dev API (content plus tokens are agent-filterable). The canvas has a
"show notes" overlay toggle (view state, like the pivot/outline toggles); there is
no separate notes mode. Notes are written only on explicit user action, so they
never churn VCS.

Deferred, not rejected: arbitrary (non-token) note color (needs an `nt_ui`
color-picker widget) and mixed-run rich text. Recorded future idea, not planned:
sprite-attached notes (a badge on a model object, not a canvas object -- pack
regeneration gives a sprite no stable canvas anchor). A freeform drawing layer
(arrows/rectangles/free draw) is out of scope on the far horizon.

### 61.3 View-state vs app-state boundary

Window geometry, recent projects, panel view modes (list/grid), sort keys, and the
canvas level/camera are **view state**: they live in an app-level settings file
**outside** the project. They never enter the project file, never dirty it, and
never enter version control. The `workspace` section (§61.2) is the deliberate
exception -- board positions, notes, and atlas `visible` are model state that
belongs to the project and is undoable. The per-platform location of the app-state
file is a non-normative detail (§60).

### 61.4 Rejected canvas/UX directions

The following were considered and explicitly rejected in the 2026-07-13 session;
do not resurrect them without owner sign-off:

- Auto-pack on edit; an animation timeline editor (flipbook only).
- Canvas modes "Pack Explain" / "Pack Diff" and a "why here" card. Determinism
  stays a core property (byte-identical output for identical inputs) with **no
  dedicated UI**.
- Per-action AI confirmations (permission is connection-level, §23).
- Manual reordering of filesystem-derived rows (a smart folder mirrors disk).
- Continuously-updated per-asset timestamps in the project file (§5.5).
- Editing export targets inside the Ctrl+E modal (preflight is read-only, §61.1).
- A single continuous canvas, and a third "page" canvas level (§61.1).
