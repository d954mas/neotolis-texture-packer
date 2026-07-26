# UI/Session Architecture Comparison

**Date:** 2026-07-26
**Status:** Decision-support research; non-normative
**Decision target:** `docs/design/ui-session-architecture-spec.md`

## 1. Question

Which presentation and concurrency architecture best fits a C17 native
immediate-mode texture packer where:

- one authoritative live session is shared by a human GUI and one external MCP
  controller;
- every mutation uses typed operations, optimistic revision checks, shared
  history, and Undo/Redo;
- GUI views must remain thin and filesystem/business rules belong below them;
- multiple GUI views may observe the same session;
- uncommitted human gestures must survive concurrent agent commits without
  hidden merge behavior?

## 2. Qualitative decision matrix

The labels below are architectural judgments for this product, not measured
scores.

| Approach | Shared-session correctness | Fit for immediate-mode C | Cost and main risk | Decision |
|---|---|---|---|---|
| Ports & Adapters + atomic session observer + Supervising Controller | Defines one authority, ordering, resync, and own-event echo | Direct snapshot reads remain possible; targeted projections fit the frame loop | Moderate; requires a precise observation/lifecycle contract | **Adopt** |
| Session-observing MVP with one broad Presenter | Can be correct if the Presenter uses the same observer and submit port | Natural fit | Moderate; the Presenter tends to become the next GUI god object | Adopt only after partitioning its responsibilities |
| Full Passive View for every panel | Can be correct with a session observer | Mechanically testable, but duplicates simple snapshot-to-widget mappings | High ongoing adapter/DTO volume | Use only at policy-heavy or identity-sensitive seams |
| Classic MVC | Does not by itself define ordering, gaps, or draft conflicts | Plausible | Low initial structure, high interpretive ambiguity | Insufficient as the governing contract |
| MVVM/data binding | Can observe shared state, but needs another synchronization graph | Poor without a retained binding framework | High machinery and lifetime risk | Reject |
| Redux/Elm for the whole application | Gives disciplined transitions but risks copying project authority | Technically possible | High migration and duplicate-state risk | Borrow local reducers/state machines only |
| Full actor framework | Strong single-owner model | Adds asynchronous plumbing to synchronous in-process calls | High shutdown/cancellation migration cost | Borrow ownership semantics only |
| Complete event sourcing | Strong ordering/replay if every event is durable | Poor fit with the existing canonical file/history/recovery model | Very high schema and migration cost | Reject |

## 3. Adopted composition

No single named UI pattern solves the product boundary. The recommended
composition is:

1. **Ports and Adapters** at product level. GUI, MCP, Dev API, CLI, and tests
   drive the same typed operation/session ports.
2. **Single-writer session semantics** inside the authoritative host. Existing
   synchronous locked admission supplies ordering; no actor runtime is needed.
3. **Session-observing Supervising Controller** inside the GUI. A frame pins one
   atomic observation. Simple views query its immutable snapshot directly;
   feature controllers prepare projections only for derived, virtualized,
   identity-sensitive, or policy-bearing presentation.
4. **Small feature-local reducers/state machines** own drafts, selection, and
   conflict transitions. Passive View boundaries are used selectively where
   they create a real ownership or testability seam.

Primary references:

- Alistair Cockburn, [Hexagonal Architecture][cockburn]
- Martin Fowler, [Passive View][passive-view]
- Martin Fowler, [Presentation Model][presentation-model]

## 4. Immediate-mode GUI lessons

Dear ImGui describes IMGUI as an API arrangement in which application code owns
its data and remains the single source of truth while the UI system retains
minimal application state. It explicitly distinguishes immediate-mode API from
the internal rendering implementation.

Applied here:

- `tp_session_snapshot` is committed application truth;
- selection, camera, filters, and drafts are explicit application-owned
  view state;
- `nt_ui` owns only widget interaction mechanics;
- view declarations should not create a second retained domain model;
- projections are disposable and rebuilt from explicit generations.

Immediate mode does not justify placing application logic in the draw function.
It removes widget synchronization machinery; it does not remove the need for
domain/presentation separation.

References:

- [Dear ImGui FAQ: IMGUI paradigm][imgui-faq]
- [Dear ImGui README: minimizing duplicated state][imgui-readme]

## 5. Blender lessons

Blender is a useful mature-editor comparison:

- buttons, shortcuts, and Python commonly converge on operators;
- operators have typed properties and explicit poll/invoke/execute/modal
  lifecycles;
- modal operators retain gesture-local state until finish/cancel;
- Undo participation is declared at the operation boundary;
- dependency-graph updates and window-manager notifications are distinct from
  editor drawing.

Applicable lessons:

- keep mutations as typed semantic operations;
- keep a gesture draft separate until one commit boundary;
- publish a notifier/event after authoritative change;
- keep derived/runtime refresh separate from model mutation and Undo;
- let multiple frontends call the same operation contract.

Rejected Blender traits:

- an ntpacker core operation must not depend on window, region, active widget,
  selection context, or mutable GUI globals;
- no Data-API path may bypass the transaction/session boundary;
- no global UI context is allowed to become hidden mutation input.

References:

- [Blender Operator API][blender-operator]
- [Blender window-manager operator/Undo declarations][blender-wm-types]
- [Blender event/depsgraph/notifier stages][blender-event-system]
- [Blender Sequencer data-storage split][blender-storage]

## 6. VS Code lessons

VS Code separates workbench UI, platform services, common editor code, and the
extension host. Its Custom Editor contract is especially relevant: edits update
the authoritative document. For text-backed custom editors, the custom view
listens to the same text-document change path for:

- its own edits;
- Undo/Redo;
- edits from another editor;
- edits from another extension.

Custom binary editors instead own a `CustomDocument` contract and explicitly
coordinate edits, backup, save, and multiple webviews. The exact API is
therefore not a universal template for ntpacker.

Applied here:

- GUI must observe its own session commit as an event echo;
- `transaction_id` identifies acknowledgement but does not bypass the normal
  reducer path;
- MCP/Dev API cannot directly manipulate GUI state;
- external commit visibility cannot depend on GUI wrapper invalidation;
- update-loop prevention must be explicit.

The transferable lesson is one shared document/session authority plus a common
observation path. VS Code does not imply that every native GUI panel needs a
copied DTO or a pure reducer.

VS Code's layered source organization also supports keeping platform-specific
implementations behind a narrow boundary.

References:

- [VS Code Custom Editor API][vscode-custom-editor]
- [VS Code Extension Host][vscode-extension-host]
- [VS Code source organization][vscode-source]
- [VS Code Extension API event guidelines][vscode-events]

## 7. Unity and Cocoa document lessons

Unity editor tooling exposes an explicit draft/apply boundary through
`SerializedObject`: `Update` refreshes serialized data from authoritative
objects, while `ApplyModifiedProperties` commits pending property edits. Unity
also warns that refreshing can discard unapplied local changes.

The useful lesson is that a local inspector draft and the committed object are
different states. The ntpacker rule is stricter for concurrent writers: a
foreign revision preserves and marks the draft conflicted instead of silently
discarding or rebasing it.

Cocoa's document architecture centralizes a document's reading, writing,
change tracking, Undo, and window-controller coordination. That validates a
session/document owner with multiple observing views. It does not require
putting widget presentation inside that owner.

References:

- [Unity `SerializedObject.Update`][unity-update]
- [Unity `SerializedObject.ApplyModifiedProperties`][unity-apply]
- [Cocoa Document Architecture][cocoa-document]

## 8. Godot lessons

Godot's editor centralizes history through `EditorUndoRedoManager`. Its
documentation warns that editor scripts which mutate objects directly may
bypass Undo/Redo and dirty-state behavior.

Applied here:

- GUI and MCP must not mutate the project directly;
- session admission is the only history/dirty/revision boundary;
- presentation adapters do not infer Undo grouping after mutation;
- one agent batch remains one explicitly formed transaction.

Godot's automatic history selection by object is unnecessary here. One project
session has one explicit history.

Godot supports the invariant that editor mutations go through an Undo-aware
operation boundary. It does not establish that all view computation must use
reducers or copied presentation DTOs.

References:

- [Godot EditorUndoRedoManager][godot-undo]
- [Godot warning about direct editor mutations][godot-editor-code]

## 9. Elm and Redux lessons

Elm formalizes:

```text
Model -> View -> Message -> Update -> Model
```

Redux guidance emphasizes:

- single state ownership;
- reducers without side effects;
- state machines for conditional transitions;
- feature-oriented slices rather than one giant reducer;
- meaningful event names;
- batching one conceptual transaction rather than publishing invalid
  intermediate states.

Applied here:

- use feature-scoped typed actions where a presentation seam needs them;
- model `gui_edit_state` explicitly as a state machine;
- split reducers by owned state, not widget type;
- keep I/O and session submits outside pure reducers;
- submit one transaction per gesture.

Not adopted:

- no immutable global GUI store containing a copy of project state;
- no global catch-all intent union;
- no middleware framework;
- no replay of GUI intents as model authority;
- no Redux-style replacement of existing `tp_session`, history, or operations.

References:

- [The Elm Architecture][elm]
- [Redux Style Guide][redux-style]
- [Redux data flow][redux-flow]

## 10. Why MVVM is not selected

MVVM is effective when the UI framework supplies retained controls, observable
properties, change notification, commands, and binding. Microsoft describes
the View's primary dependency as bindings to a ViewModel and relies on
notification interfaces and observable collections.

`nt_ui` is immediate-mode C. Recreating MVVM would require:

- an observable-property system;
- manual change notification;
- binding lifetime management;
- a second retained ViewModel state graph;
- synchronization between that graph and session snapshots.

The desired separation is valid, but a session-observing controller with
targeted projections obtains it with less machinery and fits the frame loop
directly.

Reference:

- [Microsoft MVVM architecture][mvvm]

## 11. Why classic MVC or one broad MVP Presenter is insufficient

MVC separates presentation from domain but does not define:

- the only writer for a shared live session;
- ordering between human and MCP commands;
- optimistic revision conflicts;
- event gaps and resynchronization;
- own event echoes;
- draft behavior under concurrent commits.

An honest MVP alternative can include the same session observer and submit
port; omitting them would be a strawman comparison. That design is close to the
selected Supervising Controller. Its remaining risk is concentrating all
presentation, observation, draft, I/O, and lifetime behavior in one Presenter,
creating a new `gui_actions` god object. Presenter-like behavior is therefore
partitioned into:

- session client;
- feature-local draft/view reducers;
- targeted projections;
- side-effect coordinator.

Simple views may still read the pinned immutable snapshot through read-only
queries. Passive View is a selective seam, not a universal panel mandate.

References:

- Trygve Reenskaug, [original MVC paper][mvc]
- Martin Fowler, [organizing presentation logic][fowler-ui]

## 12. Why no actor framework

The session has actor-like ownership requirements: one owner serializes
commands and publishes results. The existing implementation already provides
this with a synchronous gate and admission sequence.

A dedicated actor runtime/thread would add:

- asynchronous request lifetime and shutdown complexity;
- response queues for currently simple in-process calls;
- another scheduler and cancellation boundary;
- transport-independent backpressure machinery;
- difficult migration of synchronous tests and APIs.

Actor systems also do not automatically define a total order across independent
senders. The authoritative host must still assign admission order.

The project adopts the single-owner mental model, not an actor framework.

Reference:

- [Erlang signal ordering guarantees][erlang-order]

## 13. Why no complete event sourcing

Complete event sourcing would make an append-only event log the durable model
authority. It requires:

- permanent versioned event schemas;
- complete replay for every historical behavior version;
- side-effect isolation;
- snapshot/compaction policy;
- migration of Undo/history/recovery semantics;
- durable result reconciliation.

The product already has a canonical project file, semantic operations, bounded
live events, Undo history, and a separate recovery journal. Turning live events
into permanent model authority solves no current requirement.

The event stream remains a bounded observation and resynchronization mechanism.

Reference:

- Martin Fowler, [Event Sourcing][event-sourcing]

## 14. Conflict strategy comparison

| Strategy | User data | Semantics | Complexity | Decision |
|---|---|---|---:|---|
| Preserve draft; explicit Apply/Discard | preserved | explicit | low | **Adopt** |
| Cancel draft on any foreign commit | lost | explicit | very low | Reject |
| Automatic rebase/retry | may overwrite same/deleted/grouped fields | hidden | medium/high | Reject |
| Gesture lock/lease | preserved | blocks controller | high | Reject |
| Last writer wins | silently lost | hidden | low | Reject |
| Field merge/CRDT | potentially preserved | complex | very high | Reject |

Any model revision change conservatively conflicts an active draft in v1.
If measured usage later shows excessive unrelated conflicts, a future typed
commutativity predicate may be considered. A generic automatic merge remains
forbidden.

LSP versioned edits independently support the same optimistic-version
principle without requiring automatic merge:

- [LSP `TextDocumentEdit` version contract][lsp-version]

## 15. Emerging comparison: Agent Host Protocol

Microsoft's Agent Host Protocol describes a host with multiple clients,
monotonic server sequence, origin tracking, replay buffer, and snapshot
reconciliation after a gap. This independently supports the selected
snapshot/event-cursor shape.

It is under active development and its optimistic action rebasing targets
largely append-oriented chat actions. That assumption does not hold for
destructive property edits and structural texture-project operations, so its
automatic reconciliation behavior is not adopted.

AHP is transport-protocol precedent only. Its design does not replace the
in-process atomic observation cut or define native GUI presentation ownership.

References:

- [Agent Host Protocol overview][ahp]
- [AHP sequencing/actions][ahp-actions]
- [AHP reconciliation][ahp-reconcile]

## 16. Result

The chosen architecture is deliberately smaller than every full framework
considered:

- preserve the existing typed core and session;
- add one atomic GUI session observation contract;
- use immutable snapshots as committed truth;
- use a partitioned Supervising Controller;
- let simple views query the frame-pinned snapshot;
- use local state machines and Passive View/projections only at complex seams;
- enforce dependency boundaries mechanically;
- reject hidden merges and duplicate model ownership.

[cockburn]: https://alistair.cockburn.us/hexagonal-architecture/
[passive-view]: https://martinfowler.com/eaaDev/PassiveScreen.html
[presentation-model]: https://martinfowler.com/eaaDev/PresentationModel.html
[imgui-faq]: https://github.com/ocornut/imgui/blob/master/docs/FAQ.md
[imgui-readme]: https://github.com/ocornut/imgui#how-it-works
[blender-operator]: https://docs.blender.org/api/current/bpy.types.Operator.html
[blender-wm-types]: https://github.com/blender/blender/blob/main/source/blender/windowmanager/WM_types.hh
[blender-event-system]: https://github.com/blender/blender/blob/main/source/blender/windowmanager/intern/wm_event_system.cc
[blender-storage]: https://developer.blender.org/docs/features/sequencer/data_storage/
[vscode-custom-editor]: https://code.visualstudio.com/api/extension-guides/custom-editors
[vscode-extension-host]: https://code.visualstudio.com/api/advanced-topics/extension-host
[vscode-source]: https://github.com/microsoft/vscode/wiki/source-code-organization
[vscode-events]: https://github.com/microsoft/vscode/wiki/Extension-API-guidelines
[unity-update]: https://docs.unity3d.com/ja/2023.1/ScriptReference/SerializedObject.Update.html
[unity-apply]: https://docs.unity3d.com/ja/current/ScriptReference/SerializedObject.ApplyModifiedProperties.html
[cocoa-document]: https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/AppArchitecture/Concepts/DocumentArchitecture.html
[godot-undo]: https://docs.godotengine.org/en/stable/classes/class_editorundoredomanager.html
[godot-editor-code]: https://docs.godotengine.org/en/stable/tutorials/plugins/running_code_in_the_editor.html
[elm]: https://guide.elm-lang.org/architecture/
[redux-style]: https://redux.js.org/style-guide/
[redux-flow]: https://redux.js.org/tutorials/fundamentals/part-2-concepts-data-flow
[mvvm]: https://learn.microsoft.com/en-us/dotnet/architecture/maui/mvvm
[mvc]: https://mvc.givan.se/papers/Models-Views-Controllers.pdf
[fowler-ui]: https://martinfowler.com/eaaDev/OrganizingPresentations.html
[erlang-order]: https://www.erlang.org/doc/system/ref_man_processes.html
[event-sourcing]: https://martinfowler.com/eaaDev/EventSourcing.html
[lsp-version]: https://github.com/microsoft/language-server-protocol/blob/gh-pages/_specifications/lsp/3.18/types/textDocumentEdit.md
[ahp]: https://microsoft.github.io/agent-host-protocol/
[ahp-actions]: https://microsoft.github.io/agent-host-protocol/guide/actions.html
[ahp-reconcile]: https://microsoft.github.io/agent-host-protocol/guide/reconciliation.html
