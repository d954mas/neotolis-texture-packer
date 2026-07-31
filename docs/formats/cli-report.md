# CLI machine payloads

**Status:** Current contract.

Every saved-file CLI capability supports versioned JSON. With `--json`, stdout
contains exactly one JSON object and stderr is reserved for diagnostics.
Field names are `snake_case`, floats are dot-decimal under the `C` locale, and
the process exit code is the authoritative success signal.

The exact executable contracts live in `apps/cli/` and its golden/contract
tests. Payload schema numbers are independent of
[project schema v5](project-v5.md) and exporter format versions.

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Requested work completed successfully. |
| 1 | Unexpected internal failure. |
| 2 | Invalid invocation or arguments. |
| 3 | Project load, parse, or model error. |
| 4 | Pack or normalization failure. |
| 5 | All requested exports failed. |
| 6 | Partial success. |
| 7 | `validate --strict` found one or more error-severity findings. |
| 8 | Typed project-file failure before publication. |

Values 9 and above are reserved.

JSON errors have the stable envelope:

```json
{
  "schema": 1,
  "error": {
    "id": "bad_project",
    "message": "..."
  }
}
```

`error.id` uses stable machine tokens. Most are from the append-only
`tp_status` vocabulary; CLI-owned failures such as usage errors add their own
stable tokens. File I/O errors may also include `phase`, `path`, and
`native_code`.

## Schema manifest

`version --json` uses manifest schema 2 and advertises the project format,
export formats, and every CLI payload schema. Query verbs map directly to a
number. Mutation families advertise separate variants:

```json
{ "apply": 1, "dry_run": 2 }
```

`anim list` advertises query schema 4 because it shares the inspect shape.
`help --json` and `--help --json` return the same schema-1 command catalog,
global options, and exit-code mapping.

## Pack report: schema 1

```json
{
  "schema": 1,
  "dry_run": false,
  "atlases": [{
    "name": "animals",
    "sprite_count": 60,
    "missing_sources": 0,
    "pack_runs": 1,
    "pages": [{
      "index": 0,
      "w": 1024,
      "h": 512,
      "occupancy_pct": 87.3
    }],
    "targets": [{
      "exporter_id": "json-neotolis",
      "out_path": "C:/project/out/animals",
      "status": "ok",
      "pack_run": 0,
      "written_files": [
        "C:/project/out/animals.json",
        "C:/project/out/animals-0.png"
      ],
      "notices": []
    }]
  }],
  "totals": {
    "targets_ok": 1,
    "targets_failed": 0,
    "files_written": 2
  },
  "timings_ms": { "total": 812.4 }
}
```

`occupancy_pct` counts original placed frame area; aliases do not double-count
shared pixels. Targets with identical effective settings reuse a pack run.
Timings are intentionally environment-dependent; the rest of the report is
deterministic.

A failed target has `status: "failed"` and `error`; other targets continue.
After all requested atlases are processed, any success combined with any
failure exits 6. With no successful target, pack/normalization failure exits 4;
otherwise an all-export-failed run exits 5.

Skipped atlases retain a human `note` and structured atlas notice. Stable skip
IDs are `no_usable_images` and `no_enabled_targets`; both are successful.
Input failures remain typed errors and are not collapsed into these skips.

### Dry run

`pack --dry-run --json` uses the same schema with `dry_run: true`.
It creates or changes no published project/export output path. Pack may use and
clean up its private scratch request directory and temporary `.ntpack`:

- `written_files` is empty;
- `files_written` is zero;
- successful targets include `would_write`;
- notices are predicted capability losses.

`--atlas` selects one atlas. `--target` filters by exporter ID; filtering all
targets away is a successful empty result. `--out-dir` re-roots relative target
paths under the supplied directory while leaving absolute paths unchanged.

## Inspect: schema 4

`inspect --json` reports the canonical v5 graph, including structural IDs,
tagged sources, `{source,key}` sprite/frame identity, animations, and per-atlas
targets. `project.schema_version` reports the project wire version separately
from the payload schema.

`anim list --json` shares the animation shape and schema 4. It is read-only and
rejects `--dry-run` as usage error 2.

## Validate: schema 2

Each materialized validation finding returns exact, non-truncated context
strings:

```text
severity, code, message,
atlas?, atlas_id?, source?, source_id?, sprite?,
anim?, animation_id?, frame?, target?, target_id?
```

Counts are reported as `counts.error` and `counts.warning`. Finding codes are
the append-only `TP_VALIDATION_CODE_*` tokens from
[`tp_validate.h`](../../packer/include/tp_core/tp_validate.h).
The materialized report is bounded to 2048 findings and 4 MiB. When findings
are omitted, a synthetic `validation_truncated` finding records that fact while
the error/warning counts continue to include omitted findings.

Findings normally remain in the payload with exit 0. With `--strict`, one or
more error-severity findings change the exit to 7; warnings alone still exit 0.

## Mutation apply: schema 1

```json
{
  "schema": 1,
  "ok": true,
  "verb": "set",
  "count": 1
}
```

Successful mutations may include notices. `file_durability_uncertain` means the
canonical bytes were published but storage durability could not be confirmed;
clients must surface it and must not retry as if publication failed.
`recovery_degraded` similarly reports local crash-recovery authority without
reversing a successful project save.

## Mutation dry run: schema 2

Mutation dry runs report `command`, `dry_run`, `would_change`,
`operation_count`, `revision_before`, `revision_after`, `affected_ids`,
`generated_ids`, and structured `notices`. They do not save or publish.

For `new --dry-run`, `generated_ids` is empty and
`generated_ids_semantics` is `assigned_on_apply`; previews never expose IDs
that a later apply cannot reuse.

Removals or field renames require the corresponding payload schema bump.
Additive optional fields keep the current version.
