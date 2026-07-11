# Plan: Op-Layer Consolidation + AI-First CLI

Status: rev 2 — APPROVED (lead + 2 adversarial reviews incorporated; both reviewers
returned APPROVE-WITH-CHANGES, all BLOCKER/MAJOR amendments folded in below).
Inputs: `docs/design/architecture-review-2026-07-11.md` (packet A), `docs/design/ai-first.md`
(owner ruling), AGENTS.md hard invariants, `docs/design/ux.md` §4 (superseded in part, see L-1).

## Lead rulings (resolved review blockers)

- **L-1 Exit codes:** this plan's table supersedes `ux.md §4.4` (3-code table) —
  `ai-first.md` item 3 is the newer owner ruling. B5 rewrites `ux.md §4`.
- **L-2 Contract item 8 is PARTIAL in this packet:** malformed project / missing
  source / dangling anim frame fail with structured errors; an oversized sprite
  still aborts inside the engine builder (engine#287 — engine-repo fix, already
  filed, tracked). A tp_core pre-decode guard cannot distinguish definite
  failures once trim is in play; partial guessing violates keep-it-simple.
  Done criteria and CLI docs state this honestly.
- **L-3 Renames are unwired at export TODAY (verified):** `build_norm_opts`
  (`tp_export_run.c:24-38`) never populates `tp_normalize_opts.overrides`; the
  only writers are unit tests. Sprite renames are silently dropped by every
  export. A4 is therefore an output-changing bugfix (renames start applying),
  not "status text only" — reported to the owner at landing.
- **L-4 Dangling anim frame** (frame key with no matching sprite — happens when a
  sprite is deleted while an animation references it; renames do NOT dangle
  under the key→final model) = hard `TP_STATUS_INVALID_ARGUMENT` at export with
  anim+frame context (fail early), and a validate-time finding in CLI
  `validate` so agents catch it pre-export.
- **L-5 `tp_project_create` stays target-free.** Seeding is an explicit core
  helper both frontends call (protects 10+ existing core-test call sites).

## Goal

1. **Packet A:** move the "project → pack input" op layer from `apps/gui/` into
   `tp_core`: sprite identity, source scanning, desc assembly + override
   encoding, effective-shape rules, animation frame contract (+ wire renames),
   degradation prediction, default-target seeding.
2. **CLI:** ship `apps/cli/` (`ntpacker`) as a thin client satisfying the
   AI-first contract: `--json` with versioned schemas, stable exit codes,
   structured errors/notices, dry-run, inspect/validate, stdout/stderr
   discipline.
3. **Prove parity:** ctest asserts byte-identical export output CLI vs the GUI's
   code path (both routed through the same core entry points). CLI e2e tests
   need no GL → first real CI coverage of the op layer on 3 OS.

## Non-goals

- Packets B/C/D/E/F of the review remain sequenced after, EXCEPT the pieces
  explicitly pulled in: boundary grep-gates (A6), `setlocale` at CLI entry +
  structured notice fields (prereqs of the CLI contract), structured export
  report (B3 needs it).
- MCP server; engine changes (#285/#287 stay engine-repo work).
- GUI behavior changes beyond the three sanctioned fixes: dot-basename key fix
  (A1), renames-apply-at-export + dangling-frame error (A4), single-seeded
  default target invariant (A5). Each is reported to the owner at landing.

## Current state (verified evidence)

- Bridge in GUI: `assemble()`/`desc_add()` `gui_pack.c:83-223`; scanning
  `gui_scan.c` (recursion, ext whitelist, **entries already sorted by rel path**
  — `gui_scan.c:184-186`; entries carry `size`/`mtime` consumed by F4 refresh
  diff in `gui_actions.c:665-743`).
- 4 divergent name-normalization copies (`gui_rows.c:48,58`, `gui_pack.c:83`,
  `tp_normalize.c:34`); dot-basename divergence → override silently no-ops.
  File sources are pre-stripped with `base_name()` at `gui_pack.c:215` — row
  key and pack key agree today only because `path_stem` folder-strips too.
- Renames unwired at export (L-3). Anim frames stored in override-key space,
  consumed verbatim as final names, unvalidated (`tp_normalize.c:89-134`).
- Atlas-level "non-RECT → extrude=0" clamp copy-pasted at **five** sites
  (`gui_pack.c:461,875,885,1055,1195`) and **missing from the export path**
  (`tp_export_run` gets unclamped `tp_project_atlas_to_settings` output — a
  saved CONCAVE+extrude>0 project hard-rejects at export, inconsistent with
  pack/preview). Per-sprite effective-shape rule in 3 more places.
- Preview diff duplicated GUI-side (`gui_pack.c:1015-1098`); default target
  literal `"json-neotolis"` in `gui_project.c:115,341`; third op-layer copy in
  `packer/tests/tp_demo_export.c:43-77` (.png-only scan, own sort).
- `apps/cli` does not exist; release.yml packages ONLY the GUI dir
  (`release.yml:65-67` — "gui now, cli when it lands").
- No `setlocale` anywhere in packer/ (non-C `LC_NUMERIC` corrupts every float
  in JSON/protobuf output — CLI is the exposure).
- **Gate blindness:** showcase project has no dotfiles/renames/anims/overrides —
  the draw-hash gate and existing goldens are blind to A1/A4/A5 behavior. The
  NEW dedicated tests are the governing gate for those steps; the hash gate
  only guards GUI pixels.

## Target architecture

```
tp_core additions:
  tp_names.h/.c   tp_sprite_export_key(raw, out, cap)  (folder-keeping ext strip,
                  dotfile-keeping — same semantics as tp_normalize final_name step)
                  tp_nat_cmp(), tp_names_common_prefix()
  tp_scan.h/.c    tp_scan_dir(abs) -> entries {rel, abs, size, mtime}, sorted by
                  rel; image-ext policy owned here (single source)
  tp_input.h/.c   tp_pack_input_build(project, atlas_i, &input, err) /
                  tp_pack_input_free(&input)
                  input = { descs, count, missing_sources }
                  owns: raw-name policy (folder child = rel WITH ext; file source
                  = basename WITH ext), override mapping via
                  tp_sprite_export_key(path_last() for file sources), per-sprite
                  effective shape/extrude, engine encoding (+1 shape); preserves
                  per-source-then-sorted-within-source desc order (NOT a global
                  sort — layout depends on input order)
  tp_project.c    atlas-level clamp (non-RECT -> extrude=0) lands in
                  tp_project_atlas_to_settings (fixes the export-path
                  inconsistency; pack/preview/export all see clamped settings);
                  tp_project_sprite_effective() = per-sprite rule only;
                  tp_project_atlas_seed_default_target(a) helper (L-5)
  tp_export.h     TP_EXPORTER_ID_JSON_NEOTOLIS constant;
                  tp_export_notice gains {sprite?, target, field_id, reason_id}
                  (msg stays; tp_export_notice_addf zeroes the slot — realloc
                  does not); tp_export_predict_loss(project, atlas_i, caps,
                  opt_prepared, &notices, err): project-knowable axes
                  (transform, polygon->rect, slice9, pivot) from the project;
                  alias/multipage axes only when opt_prepared != NULL
                  (dry-run path packs and passes it; GUI chip passes NULL)
  tp_normalize.c  build_norm_opts populates overrides from a->sprites[].rename
                  keyed via tp_sprite_export_key -> raw-name map; frames
                  resolved key->final through the same index; dangling frame ->
                  TP_STATUS_INVALID_ARGUMENT naming anim+frame (L-4)
  tp_export_run   grows an optional structured report out-param
                  (tp_export_report: per-target {exporter_id, out_path,
                  written_files[], notice range}, per-page {w,h,occupancy});
                  CLI report and (follow-up, not gated here) GUI stats consume it

apps/gui: gui_pack.c assemble/desc_add + 5 clamp sites deleted -> core calls;
  gui_scan = cache/F4-diff wrapper over tp_scan (needs size/mtime — kept);
  gui_rows/main.c/gui_view_settings route through tp_names/effective helpers;
  preview diff -> tp_export_predict_loss(NULL prep).
apps/cli: ntpacker — verbs below, zero deps, hand-rolled args, JSON via tp_sb.h.
packer/tests/tp_demo_export.c routed through tp_input (kills the third copy;
  makes it a valid parity oracle).
```

### CLI v1 contract (pinned)

Verbs v1: `pack` (alias `export`) `[--atlas <name>] [--target <id>]
[--out-dir <dir>] [--dry-run] [--json] [--quiet]`; `inspect [--json]`;
`validate [--json] [--strict]`; `new <path>`; `version [--json]`.
Wave 2 (B4): `add <atlas> <paths...>`, `remove <atlas> <source>`,
`set <atlas> <key>=<value>...`, `sprite set <atlas> <name> <field>=<value>...`
(with `field=inherit` to clear; `slice9=l,r,t,b`), `sprite unset`,
`anim create/remove/list/add-frame/remove-frame/move-frame/set`,
`target add/remove/set`, `atlas add/remove/rename`.
Every B4 verb maps to an existing `tp_project_*` mutator; a missing trivial
mutator (e.g. atlas rename) may be added to tp_core; anything non-trivial is
logged and deferred, not improvised. After B4 the only GUI capabilities not
CLI-reachable are: undo/redo (interactive-only by design), explicit rescan
(unneeded — `pack` re-expands folders from disk every run; documented).

`set`/`sprite set` key vocabulary = the project-file JSON keys; the key→field
dispatch table lives in `apps/cli` for now and is explicitly earmarked to be
absorbed by the packet-B X-macro schema table later.

Exit codes: 0 ok · 1 internal · 2 usage · 3 project load/parse error · 4 pack
failure · 5 export failure · 6 partial success (some atlases/targets failed) ·
7 validate findings under `--strict` · 8+ reserved. `validate` without
`--strict` exits 0 when the file parses and validation runs; findings live in
the payload (agents' normal path). Supersedes ux.md §4.4 (L-1).

JSON: snake_case field names everywhere (no camelCase leakage); every payload
carries `"schema": N` (per-verb schema numbers, independent of project/export
format versions); floats via the deterministic tp_sb writer;
`setlocale(LC_NUMERIC,"C")` at main() entry. stdout = payload only; stderr =
diagnostics/progress. Error payloads carry `tp_status_id()` — a NEW stable
machine token per status (enum-name style, test-pinned; `tp_status_str` prose
stays for humans). `pack --json` build report: per-atlas pages {w,h,
occupancy_pct}, sprite count, per-target {exporter, out, written files,
notices[] (structured)}, timings_ms (masked in golden tests). `--dry-run` =
same report, no writes, predict_loss with the packed prep (full axes).
`version --json` = schema manifest: app version, project schema version, each
verb's payload schema version, json-neotolis/defold format versions, exporter
ids + caps. Doc note: CLI `pack` ≡ GUI "Export All" (packs + exports enabled
targets), not the GUI's preview-only Pack button.

## Steps

Execution regime (as gui-decomposition): one packet in flight; agents
move+compile+test only, NO commits, no background waits; lead runs the full
battery; per-step commit by path + push + CI 3-OS green; base-SHA re-derived
per packet; engine submodule read-only.

- **A1 — tp_names.** New module; port `final_name`'s strip semantics as
  `tp_sprite_export_key` (byte-compat pinned by existing goldens); move
  `nat_cmp`/`names_common_prefix`. Routing: `tp_normalize.c` (its strip step
  calls the shared fn), `gui_rows.c` (`strip_ext` deleted; **file-source rows
  key = `tp_sprite_export_key(path_last(src))`** — must keep matching the pack
  path's `base_name` pre-strip), `gui_pack.c:83` deleted, `main.c` slice9-sync
  key, `gui_actions.c` anim-id derivation. A/B corpus test (old vs new fns
  over: plain, folder child, dotfile bare, dotfile-in-folder, multi-dot, file
  source WITH folder component — the caller composition, not just the fn)
  runs before the old fns are deleted. New `test_names.c` in ctest.
  Behavior change (sanctioned, report to owner): dot-basename rows/keys now
  consistent. Gate: battery + hash 5/5 identical + goldens.
- **A2 — tp_scan.** Move recursion + ext whitelist; entries
  `{rel, abs, size, mtime}` sorted by rel (matches today's `entry_cmp` — a
  behavioral no-op; verify key equivalence). `gui_scan.c` keeps cache, F4
  size/mtime diff (`fp_collect`), exists/is_dir wrappers. Fixture-tree test in
  packer/tests. Gate: battery + hash 5/5 + determinism suite.
- **A3a — tp_input core.** `tp_pack_input_build/free`,
  `tp_project_sprite_effective`, atlas clamp into
  `tp_project_atlas_to_settings` (fixes export-path clamp hole — new test:
  CONCAVE+extrude>0 project exports cleanly), `test_input.c` (file+folder
  sources, override mapping incl. dotfile, effective matrix, missing count,
  OOM, per-source order preservation on a 2-source fixture). No GUI changes.
- **A3b — GUI flip.** `assemble`/`desc_add` + five clamp sites
  (`gui_pack.c:461,875,885,1055,1195`) + `gui_view_settings.c:558` routed;
  `tp_demo_export.c` routed through tp_input. Gate: battery + hash 5/5
  identical + full ctest.
- **A4 — renames + anim frame contract (output-changing bugfix, L-3/L-4).**
  `build_norm_opts` populates overrides from `sprites[].rename` (key→raw map
  via tp_names); frames resolved key→final via the same index; dangling frame
  → hard error with context. New goldens: rename-through-`tp_export_run`
  (json + defold), rename feeding an anim (frames follow), dangling frame →
  error, no-rename project byte-identical to pre-A4 output. Selftest phase:
  rename → export → assert renamed names + anim frames. GUI export error path
  surfaces the message (existing status route). Report to owner at landing.
  Gate: battery + hash 5/5 (draw unchanged) + old-output-byte-parity test for
  rename-free projects.
- **A5 — predict_loss + notices + default target.** Notice struct
  `{sprite?, target, field_id, reason_id, msg}` + slot zeroing in `addf`;
  `tp_export_predict_loss` per Target-architecture signature; GUI chip
  reimplemented over it (NULL prep; chip strings pinned by selftest phase 11);
  consistency test NARROWED to project-knowable axes (alias/multipage excluded
  until packet C centralizes writer notices — noted dependency).
  `TP_EXPORTER_ID_JSON_NEOTOLIS`; `tp_project_atlas_seed_default_target`
  called by GUI new-project path (+ CLI `new` later); `gui_project.c` literals
  removed; selftest asserts `target_count == 1` on New. Gate: battery + hash
  5/5 identical; core tests keep `tp_project_create` target-free.
- **A6 — boundary gates.** `scripts/check_boundaries.sh`: no `strrchr('.')`
  name-stripping in `apps/`, no exporter-id literals in `apps/` outside
  selftest, no `tp_pack_sprite_desc` assembly in `apps/`, no `nt_*` types in
  public tp headers, AGENTS.md/`apps/cli` present-tense guard. CI step runs
  it. Prove: passes on HEAD, fails on a seeded violation. Fix stale `#282`
  comments in `packer/CMakeLists.txt`.
- **B1 — CLI skeleton.** `apps/cli/` (`ntpacker`, console exe, links tp_build);
  hand-rolled args, verb dispatch, `--json` plumbing (tp_sb), exit-code table
  (`cli_exit.h`), `setlocale(LC_NUMERIC,"C")`, `tp_status_id()` in core
  (test-pinned tokens), `version` verb incl. `--json` manifest. ctest: usage →
  exit 2; version --json parses; locale test (set de_DE if available, assert
  dot-decimal output).
- **B2 — read verbs.** `inspect` (project dump; human format explicitly
  cosmetic — only `--json` is contract), `validate` (all findings one run:
  load errors, missing sources, dangling frames via A4 checker, knob range
  warnings; `--strict` → exit 7). ctest: golden JSON on fixture (schema-stable
  fields), exit-code matrix.
- **B3a — pack/export + report.** `tp_export_report` out-param on
  `tp_export_run` (or `_ex` sibling — GUI async/sync callers pass NULL or
  adopt); CLI `pack`: tp_input → tp_pack → export per enabled target,
  `--target`/`--out-dir` filters, creates target parent dirs (GUI does at
  `gui_pack.c:1258` — CLI must too), partial failure → exit 6.
  `docs/formats/cli-report.md` (snake_case, per-verb schema, timings_ms
  masked-in-goldens policy). ctest e2e: files exist, report golden (timings
  masked), exit codes.
- **B3b — dry-run + parity.** `--dry-run` (pack, no writes, predict_loss with
  prep). Parity ctest: same fixture project exported via (1) CLI `pack` and
  (2) direct core calls exactly as the GUI wires them (tp_input →
  tp_export_run — same entry points by construction after A3b, no GL needed);
  assert byte-identical trees. Fixture MUST include: nested relative
  out_path, a rename feeding an anim, a dotfile override, a fractional pivot
  (locale/float canary). Runs in CI 3 OS.
- **B4 — mutation verbs (wave 2).** Verb list per contract above; each 1:1 to
  existing mutators (+trivial-only additions, else defer+log). ctest per
  verb: mutate → save → reload → assert; byte-stable re-save; `inherit`/
  `unset` clears overrides.
- **B5 — docs + release wiring.** README (CLI section + Known-limitations
  refresh: "no CLI" line dies, #287 limitation stated); `ux.md §4` rewritten
  to this contract (L-1; `info`→`inspect` reconciled, `--save` superseded by
  `set`/`sprite set` — noted); ROADMAP Phase 4 updated; AGENTS.md present
  tense now true; release.yml Package step **edited** to also copy
  `build/apps/cli/native-release/` (verified: today it copies gui only) and
  the Package bash block run locally against a built tree as the acceptance
  check (tag-driven workflow — no tag burned to test it).

## Risks & mitigations

- **R1 byte-drift in tp_normalize (A1/A4):** goldens + the A/B corpus test
  (incl. caller composition) + A4's explicit rename-free-project byte-parity
  test.
- **R2 desc order (A2/A3):** scan already sorted; tp_input preserves
  per-source-then-sorted order (global sort forbidden — layout-changing);
  2-source order test pins it. Showcase (1 source) can't catch it — the unit
  test governs.
- **R3 double-seed (A5):** seeding via explicit helper; GUI's own seeding
  deleted same commit; selftest `== 1`.
- **R4 CLI scope creep:** B4 = existing mutators (+trivial-only); new core
  capability → log + defer.
- **R5 Windows unicode paths:** unchanged in this packet (packet F); CLI docs
  state the limitation (same as GUI today).
- **R6 tp_core gains OS dir-walk code:** sanctioned (tp_core already does file
  I/O; AGENTS.md bans UI/CLI parsing, not FS). Injected-scanner alternative
  rejected: it re-creates per-frontend walkers — the duplication this packet
  kills.
- **R7 tp_export_run signature change (B3a):** additive out-param defaulting
  to NULL keeps all existing callers/tests compiling (prefer `_ex` wrapper if
  churn is high).

## Verification battery (lead-run, per step)

1. Build native-debug + native-release — zero warnings.
2. ctest both presets — green.
3. GUI selftest (isolated dir) — green.
4. Draw-hash gate 5/5 vs BASELINE.md — identical for pure moves; sanctioned
   deltas re-recorded + reported. NOTE: hash gate guards GUI pixels only; the
   new unit/golden tests are the governing gate for A1/A4/A5 behavior.
5. `scripts/check_boundaries.sh` (from A6 on).
6. Commit by path, push, CI 3 OS green before next step.
7. Owner-report note in the landing message for every sanctioned behavior
   change (A1 keys, A4 renames/dangling-frame, A5 single-seed).

## Done criteria (packet exit)

- Zero name/desc/effective-shape logic in `apps/` (grep-gated); GUI
  byte-identical except the three sanctioned fixes.
- Renames apply at export; dangling anim frame fails loudly with context and
  is a `validate` finding.
- `ntpacker`: v1 + wave-2 verbs pass e2e ctest on 3 OS; parity test green;
  build report schema-versioned + documented; exit codes + `tp_status_id`
  test-pinned; `version --json` manifest works.
- ai-first contract: items 1-7 satisfied for the shipped surface (item 1
  residual: undo/rescan — documented as by-design); **item 8 PARTIAL** —
  structured errors everywhere except oversized-sprite abort (engine#287,
  tracked); items 9-10 partially (cli-report.md + manifest verb; JSON Schema
  publication stays in the distribution packet).
- release.yml packages both exes (locally verified Package block).
