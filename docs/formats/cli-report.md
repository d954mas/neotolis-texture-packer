# ntpacker CLI — machine payloads (`--json`)

**Status:** active as-built CLI schema contract. Exact future operation, Dev API,
MCP, Import/Export IR, and format-manifest schemas remain open contracts in
[`../ntpacker-master-spec.md`](../ntpacker-master-spec.md) §60. Existing fields
must not be renamed before the matching code, schema version, and golden tests
migrate together.

Conventions (master spec §4 and the historical ai-first ruling): every payload is a single JSON
object on **stdout** (stderr carries diagnostics only), field names are
**snake_case**, and every payload carries a per-verb `"schema": N` (independent
of the project-file and export-format schema versions — those are reported by
`version --json`). Field additions are non-breaking; removals/renames bump the
verb's schema. Floats are dot-decimal always (`LC_NUMERIC` pinned to `C`).
Errors with `--json` are also stdout payloads:
`{"schema":1,"error":{"id":"<tp_status_id>","message":"..."}}`; the exit code
is the authoritative machine signal (see `cli_exit.h`: 0 ok · 1 internal ·
2 usage · 3 project load/parse · 4 pack failure · 5 export failure · 6 partial ·
7 validate --strict findings · 8+ reserved).

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
reported separately as `project.schema_version`.

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
