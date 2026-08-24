# Defold export format

**Status:** Current bundled Lua export contract.

The `defold-tpinfo-2` target writes inputs for Defold's TexturePacker extension:

- `<base>.tpinfo`: protobuf-text packed layout, format version `"2.0"`;
- `<base>.tpatlas`: protobuf-text wrapper and explicit animations;
- `<base>-<page>.png`: straight-alpha RGBA8 pages.

Geometry coordinates are integer pixels in y-down, top-left-origin space.
Pivot coordinates may be fractional. Output is UTF-8 with LF line endings and
deterministic ordering.

## Resource paths

Page names inside `.tpinfo` are relative basenames and resolve next to that
file. The `.tpatlas` `file` field is a Defold resource path. The exporter walks
up from the output directory, at most ten levels, looking for `game.project`.
When found it emits a project-absolute `/path/base.tpinfo`. Otherwise it emits
the basename and a notice because Defold may not resolve it.

## Capability projection

| Feature | Current handling |
|---|---|
| Trim | Represented by `source_rect` and `corner_offset`. |
| Polygon | Represented by `vertices` and `indices`. |
| Pivot | Represented in pixels; requires `.tpinfo` 2.0. |
| Multipage | One `pages` block per page. |
| Aliases | One complete sprite entry per alias. |
| 9-slice | Unsupported and dropped with a notice. |
| Full D4 transforms | Unsupported. |
| 90-degree rotation | D4 value 5 is packed and emitted as `rotated: true`. |

The bundled package descriptor declares the exact transform-value mask
`identity + rotate90`. The shared export layer intersects that mask with the
requested atlas mask before pack grouping and passes it unchanged to the engine.
Flips and the opposite rotations therefore cannot enter a Defold result, while
the representable 90-degree rotation remains available to the packer.

## `.tpinfo` sprite fields

| Field | Contract |
|---|---|
| `name` | Final export name after rename resolution. |
| `trimmed` | Informational trimmed flag. |
| `rotated` | True only for representable D4 mask 5. |
| `is_solid` | True when the placed page footprint contains no transparent texel. |
| `corner_offset` | `spriteSourceSize.x/y`. |
| `source_rect` | `spriteSourceSize`; equals the emitted vertex bounding box. |
| `pivot` | Normalized pivot multiplied by untrimmed source dimensions. |
| `frame_rect` | Page placement; footprint dimensions are swapped when rotated. |
| `untrimmed_size` | Original source dimensions. |
| `indices` | Canonical quad indices or polygon triangle list. |
| `vertices` | Unrotated, y-down untrimmed-source coordinates. |

`tp_pack_read` normalizes a recovered polygon so the hull bounding-box minimum
is trim-local `(0,0)` and sets `spriteSourceSize` to that recovered bbox.
The exporter therefore consumes `spriteSourceSize` directly and emits vertices
as `trim-local + spriteSourceSize.xy`. This preserves the invariant
`source_rect == bounds(vertices)` without re-deriving another bbox in the
exporter.

Plain rectangles use vertices in top-right, top-left, bottom-left,
bottom-right order with indices `[1,2,3,0,1,3]`. Polygon vertices and indices
come from the canonical pack result.

## `.tpatlas`

| Field | Contract |
|---|---|
| `file` | Project-absolute `.tpinfo` resource when `game.project` is found. |
| `rename_patterns` | Empty; renames are already baked into names. |
| `animations` | Explicit project animations only. |
| `is_paged_atlas` | False, including multipage input. |

Defold's build pipeline automatically exposes every sprite name as a one-frame
animation. The exporter does not duplicate those in `.tpatlas`; it emits only
explicit flipbooks.

Animation frames retain explicit project order. FPS is rounded to Defold's
integer field. Playback mapping is:

| ID | Defold token |
|---:|---|
| 0 | `PLAYBACK_ONCE_FORWARD` |
| 1 | `PLAYBACK_LOOP_FORWARD` |
| 2 | `PLAYBACK_ONCE_BACKWARD` |
| 3 | `PLAYBACK_LOOP_BACKWARD` |
| 4 | `PLAYBACK_ONCE_PINGPONG` |
| 5 | `PLAYBACK_LOOP_PINGPONG` |
| 6 | `PLAYBACK_NONE` |

Playback IDs outside this table are invalid external input and are rejected at
the common Export IR boundary before the Lua handler runs.

## Notices

Capability loss and path fallback are informational, not hard exporter errors.
Stable situations include dropped 9-slice data, guarded unsupported transforms,
and inability to locate `game.project`.

The executable integration example is
[`examples/defold-demo`](../../examples/defold-demo/). The unified
[`examples/runtime-formats`](../../examples/runtime-formats/) project contains
the shared real-engine browser proof.
