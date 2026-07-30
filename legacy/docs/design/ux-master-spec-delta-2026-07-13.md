# UX → Master-Spec Delta (2026-07-13)

**Status: REVIEW-READY (final consolidation of the 2026-07-13 UI/UX session).** Owner-approved
process: to be merged into `docs/ntpacker-master-spec.md` (section A) and `docs/design/ux.md`
(section B) **after** the in-flight master-spec implementation agent finishes its current work.
Until merged, this file is the single source for UX-session decisions that touch normative
contracts. Covers: export model (A1, A7, A8), settings inheritance (A2), timestamps (A3), atlas
visibility (A4), palette-ready ops (A5), history authorship (A6), overview thumbnails (A9),
workspace layout + text objects (A10), session state (A11), interaction contract (B), rejected
list (C). Companion docs: [`ux-vision-2026-07-13.md`](ux-vision-2026-07-13.md) (rationale), [`../plans/ux-epics-2026-07-13.md`](../plans/ux-epics-2026-07-13.md) (execution), [`mockups/ntpacker-fakeshots.html`](mockups/ntpacker-fakeshots.html) (visual reference).

---

## A. Master-spec amendments (core model / schema / contracts)

### A1. Export targets move to project level; atlases hold participation
**Current spec/impl:** targets are per-atlas records (`tp_project_add_target/...`); new projects
seed a default target per atlas; GUI edits targets in two places.
**Change:** export rules (targets) are declared once on the **project**:
`target = { id, exporter_id, enabled, path_template, image_opts, scale_variants }`.
Per-atlas state is a **participation record**: `{ target_id → enabled_override?, path_override? }`.
Effective targets for an atlas = project rules ∘ participation. Path templates support
`{atlas}`, `{page}`, `{scale}`, `{scale_suffix}`.
**Consequences:** target CRUD = project-scope typed operations; participation toggles =
atlas-scope operations; CLI `--json` schemas and export orchestration resolve effective targets;
migration for existing per-atlas-target project files (auto-lift identical per-atlas targets to
one project rule).
**Why:** 30 atlases × identical config was the #1 owner-reported confusion; single editing surface.

### A2. Settings inheritance: project → atlas → sprite with value provenance
**Change:** project holds atlas-setting defaults (padding, max page size, shape, …); atlas holds
sprite-setting defaults (pivot, …). Core exposes **effective value + origin**
(`default | project | atlas | sprite`) per field so UIs can render non-default markers and
"modified only" filters, and revert = "clear override at this level".
**Consequences:** operation vocabulary gains `clear_override`; validation runs on effective
values; project file gains a `defaults` block.

### A3. Source records: write-once `added_at`
**Change:** each source (file/folder/linked atlas) records `added_at` once at add time. **No
continuously-updated per-asset timestamps are ever written to the project file** (VCS-churn rule —
the TexturePacker SmartUpdate anti-pattern). File mtime is read live from the FS, never persisted.
"Who/when changed what" remains the transaction history's job.

### A4. Atlas `visible` flag as a model operation
**Change:** atlas gains `visible: bool` (default true). Toggling is a **named, undoable typed
operation** ("Hide atlas ui"), recorded in history, marks project dirty (owner decision —
overriding a view-state design). **Semantics: display-only.** Hidden ≠ disabled: a hidden atlas
still packs and exports. (A separate `disabled` concept — excluded from pack/export — is a
possible future feature and must not be conflated.)

### A5. Operation registry must be palette-ready
**Requirement on the typed-operation layer (F2):** every operation carries a human-readable label
template (for history entries and undo toasts: "Set padding 4 (3 sprites)") and a
machine-readable argument schema (name, type, range/enum) so a command palette can index and
invoke parameterized operations ("set max page size 1024") without per-command UI code. Likely
already implied by F2 design — this makes it a hard requirement.

### A6. History entries carry author identity
**Requirement (aligns with Epic A):** each transaction records its author
(`human | agent(<controller identity>)`); exposed to clients so the GUI can render AI/ME
authorship badges and a live agent-activity surface. No per-action confirmations (owner decision:
allow-all/deny at connection level).

### A7. Export delivery: ZIP mode + manifest
**Change:** export orchestration supports a delivery mode `archive`: same staging pipeline, but
outputs land in one ZIP with canonical layout `{atlas}/{target}/…` (per-target path templates
ignored) plus `manifest.json` (versioned schema: tool version, project name, timestamp, file
list with sizes/hashes). CLI: `ntpacker export --archive out.zip`.
**Horizon (not v1):** `Package project` (project file + sources, relative paths) is a separate
future command, distinct from result archives.

### A8. Scale variants: schema slot reserved, implementation deferred
**Change:** `scale_variants: [{factor, suffix}]` (default `[{1.0, ""}]`) reserved in the target
schema now. Each variant is **its own pack** (sources scaled before packing so padding/extrude
stay honest) — interacts with the pack-result LRU. Implementation deferred (Defold-first path
does not need it); schema stability is the point.
Image options v1: PNG only (+premultiply per exporter caps). WebP / PNG quantization deferred.

### A9. Pack-result thumbnails for project overview
**Requirement:** the pack-result store must serve downscaled page thumbnails (mip levels or
cached downscale) cheap enough for a project-overview canvas at **30+ atlases / 5000+ sprites**
(owner's real scale). Full-res pages stay in the LRU; overview must not force full-res residency.

### A10. Canvas workspace data: atlas board positions + notes
**Change (owner 13.07; final revision — no auto-reflow):** project file gains a `workspace`
section: every atlas board has a stored position on the Project canvas (auto-assigned to free
space when the atlas is created; "Move atlas" is a named undoable operation; "Tidy up" is an
explicit re-grid command — the ONLY tool-initiated movement). **The tool never moves boards on
its own:** hiding leaves an empty spot (no compaction), board growth after Pack wraps pages
inside the board (huge atlases show a "+N pages" tile) and never pushes neighbors — collisions
render as legal overlap (z-order, last-touched on top); shrinking leaves the space. Overlap is
non-semantic: dropping an atlas onto an atlas changes no data (semantic board drops are reserved
for notes parenting and V2 sprite/folder moves; atlas merge, if ever, is an explicit command) and **text objects (notes)** — the owner's
annotation tool first (agent access is a bonus property). A text object = content + parent + **per-object style** (no mixed-run rich text in v1).
**Parenting is spatial and automatic (Figma idiom, owner-settled 13.07)** — one object type, no
"kind" choice at creation: dropped on an atlas board → parented to that atlas (board-relative
coords; moves with the board, hides with the atlas eye, deleted with the atlas — delete confirm
states "and N notes", undo restores); dropped on empty canvas → parented to the project
(absolute coords). Re-parent by dragging between board and canvas (drop target highlights).
Notes live on the Project canvas level only in v1 (no anchoring to regenerated pack content
inside an atlas). Style:
`{ size: S|M|L|XL, bold, italic, text_color: token, bg_color: token|none, align }` — color
tokens come from a fixed swatch palette (10–12); `bg=none` renders as a plain label, with bg as
a sticker. Bold/italic require bold/italic font faces (engine asset work). Text objects are
ordinary model objects: typed operations with named history entries ("Add note … on enemies"),
undoable; **visible in the tree** — attached notes as a collapsed "Notes (N)" group inside their
atlas, free notes as a "Notes" group under the project root (after the atlases), re-parenting on
canvas moves them between the two, click → canvas zoom-to; editable in the inspector; found by
project-wide filter (Ctrl+F searches note text); exposed via MCP/Dev API (content + tokens
agent-filterable). Canvas gets a "show notes" overlay toggle (view state, like pivot/outline
toggles) — no separate mode. Deferred, not rejected: arbitrary color (needs an nt_ui
color-picker widget), mixed-run rich text. Written only on explicit
user action → no VCS churn. **Recorded future idea (not planned):** sprite-attached notes —
annotations on model objects (badge in tree/inspector), not canvas objects; pack regeneration
gives sprites no stable canvas anchor. A freeform drawing layer (arrows/rects) is explicitly
out of scope (far horizon).

### A11. Session/app state file (non-normative note)
Window geometry, recent projects, panel view modes (list/grid), sort keys, canvas level/camera:
an app-level settings file **outside the project** (never in VCS, never dirties the project).
Location conventions per platform; project files must not absorb any of it.

## B. ux.md amendments (interaction contract)

- **Two-level canvas:** Project (atlas group-cards: name, health badges fill%/outdated/not
  packed/⚠, eye, page thumbnails inside; freeform positions per A10 — auto-placed at creation,
  never moved by the tool, "Tidy up" re-grids on demand; hiding leaves the spot empty, an
  "N hidden" chip restores) ⇄ Atlas
  (working level: ALL pages laid out spatially; page tabs removed; double-click page = camera
  zoom, not a mode; sprites selectable). Breadcrumbs `Project ▸ <atlas>` ascend. Rejected: a
  single continuous canvas (pack regeneration makes the space drift; fit/inspector ambiguity;
  VRAM culling) and a third "page" level.
- **Left panel = one project tree** with a clickable **project root node** (root selection →
  project settings + export rules in the inspector): root → atlases → folders/sprites →
  per-atlas Animations node. Replaces ATLASES/SPRITES/ANIMATIONS sections. Filter (Ctrl+F) is
  project-wide, matches shown under their atlases. Cross-atlas drag works in tree and (V2) on
  the Project canvas; pages are packer output — never drag targets.
- **Sorting is view state:** four keys (name default / size / file mtime / added_at) × two
  directions (re-click flips) + independent "⚠ on top" checkbox that overlays any sort. Keys
  apply within each tree level; atlas order in the project stays manual (an undoable operation).
- **Thumbnails are the default:** sprite rows and atlas rows always show previews (async
  thumbnail cache); hover → large 1:1 preview. SPRITES area has list ⇄ grid display modes
  (header toggle, view state). Atlas "cards view" is the Project canvas level, not a panel mode.
- **Export UX:** rules edited only at project root; atlas inspector shows a compact
  participation block. Ctrl+E opens **preflight only** (scope = current selection; nothing
  selected at Project level = all; dry-run file list; overwrites amber; degradation notices;
  "Copy CLI command"; optional "Export as ZIP"). Repeat-last-export = Ctrl+E → Enter.
- **Scale calibration:** UX perf budgets are validated against a 30-atlas / 5000-sprite bench
  fixture (owner's real scale): <100 ms interactions, non-blocking refresh, virtualized lists,
  bounded thumbnail cache.

## C. Explicitly rejected (do not resurrect without owner)

- Auto-pack on edit; animation timeline editor (flipbook only).
- Canvas modes "Pack Explain" / "Pack Diff"; "why here" card. Determinism stays a core property
  (byte-identical output for identical inputs) with **no dedicated UI**.
- Per-action AI confirmations (permission is connection-level).
- Manual reorder of FS-derived rows (smart folder mirrors disk).
- Continuously-updated per-asset timestamps in the project file.
- Editing export targets inside the Ctrl+E modal (preflight is read-only).
