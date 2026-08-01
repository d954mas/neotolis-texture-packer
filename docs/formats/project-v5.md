# Canonical project format v5

**Status:** Current contract.

`.ntpacker_project` is UTF-8 JSON. Production accepts exactly schema version 5
and always writes version 5. Older and newer versions are rejected with
`bad_version`; there is no compatibility loader, implicit upgrade, or in-place
migration.

The executable references are
[`tp_project.h`](../../packer/include/tp_core/tp_project.h),
[`tp_project_parse.c`](../../packer/src/tp_project_parse.c),
[`tp_project_write.c`](../../packer/src/tp_project_write.c), and the
[`project_v5_rich.golden`](../../packer/tests/fixtures/project_v5_rich.golden)
fixture.

## Canonical encoding

- `version` is emitted first. Remaining object keys use ascending ASCII order.
- Indentation is two spaces, line endings are LF, and the file ends with a
  newline.
- Floats use round-trip-stable `%.9g` formatting under the `C` locale.
- Fields equal to their documented defaults are omitted.
- Unknown keys, wrong types, malformed UTF-8, and non-canonical graphs are
  errors. The writer validates before replacing any published file.
- Saving an unchanged canonical project is byte-identical.

Paths are stored project-relative where possible and use `/`. Absolute paths
remain absolute when they cannot be expressed relative to the project file,
including paths on a different drive or UNC share.

## Document shape

```json
{
  "version": 5,
  "atlases": [
    {
      "id": "atlas_<32 lowercase hex digits>",
      "name": "atlas1",
      "sources": [],
      "sprites": [],
      "animations": [],
      "targets": []
    }
  ]
}
```

`atlases` is required and is an array, including for an empty project. Atlas
packing fields are sparse and use the defaults returned by
`tp_pack_settings_defaults`:

| Field | Default | Admitted value |
|---|---:|---|
| `max_size` | 2048 | integer 1 through 4096 |
| `padding` | 2 | integer 0 through effective `max_size` |
| `margin` | 0 | integer 0 through effective `max_size` |
| `extrude` | 0 | integer 0 through effective `max_size`; nonzero only with rectangle shape |
| `alpha_threshold` | 1 | integer 0 through 255 |
| `max_vertices` | 8 | integer 1 through 16; values below 4 adapt to 4 for the current engine |
| `shape` | 2 | 0 rectangle, 1 convex hull, 2 concave contour |
| `allow_transform` | true | boolean |
| `power_of_two` | true | boolean |
| `pixels_per_unit` | 1 | positive finite number |

Export targets are currently owned by an atlas. Project-level targets are a
future schema change and must not be written into v5.

## Structural identity

Atlas, source, animation, and target records carry persistent, non-nil,
kind-correct, globally unique IDs:

- `atlas_<32 hex>`
- `source_<32 hex>`
- `anim_<32 hex>`
- `target_<32 hex>`

IDs loaded from disk are authoritative and are never silently regenerated.
Fresh private candidates may start with nil IDs; the writable session-adoption
boundary assigns all missing IDs atomically before publication.

Sprite identity is not persisted as a separate ID. It is derived from the
source structural ID plus the canonical source-local key. Rename and collection
reordering do not change that identity.

## Sources

Every source is a tagged record:

```json
{ "id": "source_<32 hex>", "path": "art/characters" }
{ "id": "source_<32 hex>", "kind": "file", "path": "art/icon.png" }
```

`kind` is `folder` or `file`; `folder` is the omitted default. Bare source
strings are invalid. Linked-atlas sources are reserved for a future schema and
are not valid v5 records.

The stored kind is used when disk state is unavailable. Runtime scanning may
still report a stored path whose current filesystem type or availability has
changed; external source state is not a parser migration.

## Sprite overrides

`sprites` records carry identity plus any sparse overrides. Operation-produced
projects normally create a record when at least one override exists, but v5 also
admits and preserves an identity-only `{source,key}` record. Every record is
addressed by:

```json
{
  "key": "walk/frame_01.png",
  "source": "source_<32 hex>"
}
```

`key` is the NFC-normalized, source-local path with its extension preserved.
Optional fields are `allow_rotate`, `extrude`, `margin`, `max_vertices`,
`origin`, `rename`, `shape`, and `slice9`.

Defaults are centered origin `[0.5, 0.5]`, no rename, zero slice borders, and
inherited packing settings. Origin components are finite, slice borders are
integers 0 through 65535, and explicit `max_vertices` is 1 through 16. Values
below 4 adapt to 4 at the current engine boundary while remaining unchanged in
the saved project.
Explicit `margin` and `extrude` are 1 through 255 and cannot exceed the atlas
`max_size`; nonzero effective extrude requires rectangle shape. `shape` is 0
through 2. The only explicit `allow_rotate` value admitted by v5 is 0; absence
means inherit.

An unknown source ID is a graph-integrity error. A known source whose file or
key is temporarily missing remains a reportable runtime/validation state and
may become active again when the same key returns.

## Animations

Animations carry structural `id`, human `name`, and an explicit ordered
`frames` array. Frames use the same `{source, key}` identity as sprite
overrides:

```json
{
  "frames": [
    { "key": "walk_01.png", "source": "source_<32 hex>" }
  ],
  "id": "anim_<32 hex>",
  "name": "walk"
}
```

Optional fields are `flip_h`, `flip_v`, `fps`, and `playback`. Defaults are
false, false, 30, and playback ID 0. Playback IDs 0 through 6 are stable:
once-forward, loop-forward, once-backward, loop-backward, once-ping-pong,
loop-ping-pong, and none.

Filename suffixes never create project animations automatically.

## Export targets

```json
{
  "exporter_id": "json-neotolis",
  "id": "target_<32 hex>",
  "out_path": "out/atlas"
}
```

`enabled` defaults to true and is written only when false. `exporter_id` is a
non-empty strict-UTF-8 machine token of at most 255 bytes. Duplicate enabled
output paths are admitted by canonical project I/O; validation reports the
`duplicate_out_path` warning so an operator can resolve the publication
collision before Export.

## Validation and publication

Load, adoption, checkpoint, and save reject:

- nil, malformed, wrong-kind, or duplicate structural IDs;
- duplicate source paths after slash normalization;
- malformed `{source, key}` references or unknown source IDs;
- invalid settings, names, exporter IDs, and malformed target paths;
- any shape that cannot be serialized canonically.

Missing files and missing keys under a known source do not make the JSON
unparseable. They are reported by validation and source-runtime diagnostics.

A future project format may add project-level targets, workspace state, linked
sources, or scale variants. Such changes require a version newer than 5 and an
explicit migration contract; v5 remains frozen.
