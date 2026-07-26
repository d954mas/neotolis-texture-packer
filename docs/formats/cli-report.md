# ntpacker CLI — machine payloads (`--json`)

**Status:** active as-built CLI schema contract. Exact future operation, Dev API,
MCP, Import/Export IR, and format-manifest schemas remain open contracts in
[`../ntpacker-master-spec.md`](../ntpacker-master-spec.md) §60. Existing fields
must not be renamed before the matching code, schema version, and golden tests
migrate together.

Conventions (master spec §4 and §14.2, the AI-first CLI ruling): every payload is a single JSON
object on **stdout** (stderr carries diagnostics only), field names are
**snake_case**, and every payload carries a per-verb `"schema": N` (independent
of the project-file and export-format schema versions — those are reported by
`version --json`). Field additions are non-breaking; removals/renames bump the
verb's schema. Floats are dot-decimal always (`LC_NUMERIC` pinned to `C`).
Errors with `--json` are also stdout payloads:
`{"schema":1,"error":{"id":"<tp_status_id>","message":"..."}}`; the exit code
is the authoritative machine signal (see `cli_exit.h`: 0 ok · 1 internal ·
2 usage · 3 project load/parse · 4 pack failure · 5 export failure · 6 partial ·
7 validate --strict findings · 8 typed pre-publication file I/O failure ·
9+ reserved).

`help --json` and `--help --json` emit the same schema-1 object. Its `commands`
and `global_options` arrays are the machine-readable command catalog, and its
`exit_codes` object freezes the symbolic mapping above. The options catalog
includes `--dry-run`; it previews `pack` and mutation commands, while read-only
queries such as `anim list` reject it with structured `usage` and exit 2.

`version --json` emits manifest schema 2. Query verbs map directly to their
payload schema number. Each mutation family maps to a variant object:
`{"apply":1,"dry_run":2}`. `anim` additionally advertises `"list":4` because
`anim list` is a query sharing the inspect schema, not a mutation response.

## `pack` report (schema 1)

```json
{
  "schema": 1,
  "dry_run": false,
  "atlases": [{
    "name": "animals",
    "sprite_count": 60,
    "missing_sources": 0,
    "pack_runs": 1,
    "pages": [{"index": 0, "w": 1024, "h": 512, "occupancy_pct": 87.3}],
    "targets": [{
      "exporter_id": "json-neotolis",
      "out_path": "C:/.../out/animals",
      "status": "ok",
      "written_files": ["C:/.../out/animals.json", "C:/.../out/animals-0.png"],
      "notices": [{"field": "pivot", "reason": "caps_unsupported",
                   "sprite": "round/elephant", "message": "..."}]
    }]
  }],
  "totals": {"targets_ok": 1, "targets_failed": 0, "files_written": 2},
  "timings_ms": {"total": 812.4}
}
```

- `dry_run` — `true` when the report came from `pack --dry-run`; then NO files were
  written (`written_files` is empty on every target and `files_written` is 0), each
  ok target instead carries a `would_write` array (the paths it WOULD produce), and
  its `notices` are the PREDICTED degradations (`tp_export_predict_loss` with the
  packed prep — full axes incl. alias/multipage) rather than writer-emitted ones.
  A dry run creates no directories either.

```json
    "targets": [{
      "exporter_id": "json-neotolis",
      "out_path": "C:/.../out/animals",
      "status": "ok",
      "written_files": [],
      "would_write": ["C:/.../out/animals.json", "C:/.../out/animals-0.png"],
      "notices": [ ... predicted losses ... ]
    }]
```

- `occupancy_pct` — sum of placed ORIGINAL frame areas (aliases share their
  original's pixels, not double-counted) / page area × 100; deterministic,
  in (0,100] for a non-empty page.
- `pages` are grouped per shared pack run; targets whose effective settings
  coincide reuse one run (`pack_runs`).
- A failed target reports `"status": "failed"` + `"error"`; the run continues
  to remaining targets (exit 6 when some succeeded, 5 when none did). A pack/
  normalize failure aborts the atlas before any target writes (exit 4).
- `timings_ms` values are environment-dependent and are **masked in golden
  tests** — never assert them; everything else in the report is deterministic.
- `out_path` is the resolved absolute output **base**: each exporter appends
  its own extension(s) (`<base>.json`, `<base>-<page>.png`, Defold
  `<base>.tpinfo` + sibling `.tpatlas`).
- A successfully skipped atlas retains the human-readable `note` and adds a
  structured atlas-level `notices` entry with `{id, atlas, message}`. Stable
  skip ids are `no_usable_images` and `no_enabled_targets`; both exit 0.
- An atlas-level input failure adds
  `"error":{"id":"<tp_status_id>","atlas":"<name>","message":"..."}`.
  Missing/unreadable sources therefore remain distinct from the typed
  `no_usable_images` skip and produce a non-zero exit without losing source
  context.

## `pack` flags

- `--atlas <name>` — only that atlas (unknown name = usage error, exit 2).
- `--target <id>` — only targets with that exporter id; filtering everything
  away is OK-with-warning (exit 0, empty targets) so preview-only projects
  don't fail agent pipelines.
- `--out-dir <dir>` — RELATIVE target out_paths are re-rooted under `<dir>`
  (resolved against the CWD) instead of the project dir; absolute out_paths
  are untouched. Parent directories are created.
- `--dry-run` (B3b) — same report, no files written, predicted degradations
  included.

## `inspect` (schema 4) / `validate` (schema 2)

Schema 4 reports a canonical-v5 project: tagged source objects carry stable
structural IDs, sprite overrides and animation frames use `{source,key}`
identity, and each animation carries both opaque structural `id` and human
`name`. The `anim list --json` query shares this schema and animation shape. An
operator branches on the payload `schema` number; project-file schema is
reported separately as `project.schema_version`. Because `anim list` is a
read-only query, `--dry-run` is not valid for it and yields a structured
`usage` error with exit 2.

See `apps/cli/cli_inspect.c` / `cli_validate.c` headers. Validate schema 2 keeps
exact (non-truncated) contexts and adds stable structural identities:
`{severity, code, message, atlas?, atlas_id?, source?, source_id?, sprite?,
anim?, animation_id?, frame?, target?, target_id?}` with
`counts:{error,warning}`. Examples of stable finding codes:
`missing_source, empty_atlas, dangling_anim_frame, duplicate_export_key,
export_name_collision, unknown_exporter, setting_out_of_range,
input_build_failed`. The complete append-only vocabulary is defined by
`TP_VALIDATION_CODE_*` in `packer/include/tp_core/tp_validate.h`; adapters emit
those tokens verbatim.

## Mutation success (schema 1)

Normal success is
`{"schema":1,"ok":true,"verb":"<verb>","count":N}`. A successful
mutation may additionally contain `notices`. In particular:

```json
{
  "schema": 1,
  "ok": true,
  "verb": "set",
  "count": 1,
  "notices": [{
    "id": "file_durability_uncertain",
    "message": "project file was published, but storage durability could not be confirmed",
    "status": "file_durability_uncertain"
  }]
}
```

This is not a failed or absent write: the canonical project bytes were
published and are authoritative. Clients must surface the notice and must not
retry as if no write occurred. `recovery_degraded` is likewise a successful
Save notice about local crash-recovery authority, not project-file publication.

Mutation `--dry-run --json` uses schema 2 instead of the apply schema above. It
reports `command`, `dry_run`, `would_change`, `operation_count`,
`revision_before`, `revision_after`, `affected_ids`, `generated_ids`, and
structured `notices`. Machine clients select this decoder from the mutation
verb's `dry_run` entry in `version --json`.

For `new --dry-run`, `generated_ids` is empty and the append-only field
`generated_ids_semantics` is `"assigned_on_apply"`. The preview therefore does
not expose candidate IDs that a later apply cannot reuse.
