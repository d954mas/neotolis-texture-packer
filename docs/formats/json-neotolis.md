# `json-neotolis` export format

**Status:** Current contract, schema 1.

`json-neotolis` is the full-fidelity built-in interchange format. It represents
all D4 transform values, polygons, pivots, 9-slice borders, multiple pages,
aliases, and explicit animations.

- Exporter ID: `json-neotolis`
- Descriptor: `<base>.json`
- Pages: `<base>-<page>.png`, including `-0` for a single page
- Encoding: UTF-8, LF, two-space indent, trailing newline
- Page pixels: straight-alpha RGBA8

Consumers must ignore unknown optional fields. Removing or changing an existing
field requires a `version` bump.

## Determinism and naming

The output is byte-reproducible for identical inputs. `version` is first and
remaining keys use ASCII order. Pages are ordered by index, sprites by final
export name, and animations by ID. Floats use `%.9g` under the `C` locale.

Page `file` values are basenames and resolve relative to the descriptor.
Sprite rename overrides are resolved before export, so `name`, animation
frames, and `alias_of` always use final names.

## Root object

| Key | Type | Contract |
|---|---|---|
| `version` | integer | Always 1. |
| `animations` | array | Explicit project animations; omitted when empty. |
| `atlas` | string | Atlas display name. |
| `pages` | array | One page record per output PNG. |
| `pixels_per_unit` | number | Atlas scale metadata. |
| `sprites` | array | One record per sprite and alias. |

## Page object

| Key | Type | Contract |
|---|---|---|
| `file` | string | `<base>-<index>.png` basename. |
| `h` | integer | Page height. |
| `premultiplied` | boolean | False for the built-in export profile. |
| `w` | integer | Page width. |

## Sprite object

Coordinates are y-down PNG space.

| Key | Type | Contract |
|---|---|---|
| `alias_of` | string or null | Final name whose placement is shared, or null. Always present. |
| `frame` | `{x,y,w,h}` | Placed trimmed rect. `w/h` are unrotated. |
| `name` | string | Unique final export name. |
| `page` | integer | Page index. |
| `pivot` | `[x,y]` | Normalized over `sourceSize`; omitted at `[0.5,0.5]`. |
| `polygon` | object | Trim-local y-down `verts` and triangle `indices`; omitted for a plain rect. |
| `slice9` | `[l,r,t,b]` | Pixel borders; omitted when all are zero. |
| `sourceSize` | `{w,h}` | Original untrimmed dimensions. |
| `spriteSourceSize` | `{x,y,w,h}` | Recovered content/hull bbox in the original image. |
| `transform` | integer | D4 transform value 0 through 7; omitted for identity. |
| `transformStr` | string | `flipH`, `flipV`, and `diag` tokens joined by `|`; emitted with `transform`. |

The D4 value uses bit 0 for horizontal flip, bit 1 for vertical flip, and bit 2
for diagonal transpose. Apply diagonal, then horizontal flip, then vertical
flip. A diagonal transform swaps the on-page footprint dimensions.

Polygon vertices may extend slightly beyond a decoder's original alpha trim
because the engine builder inflates non-rectangular hulls. Emitted `verts` are
normalized to trim-local bounds `0..w` and `0..h`. `spriteSourceSize.x/y` place
that recovered hull in original-image space, while its `w/h` spans match the
emitted vertex spans.

## Animation object

Animations come only from explicit project animation records. Filename suffixes
such as `walk_01` and `walk_02` never create an animation automatically.

| Key | Type | Contract |
|---|---|---|
| `flip_h` | boolean | Horizontal playback mirror. |
| `flip_v` | boolean | Vertical playback mirror. |
| `fps` | number | Playback rate. |
| `frames` | array of string | Final sprite names in explicit order. |
| `id` | string | Animation logical identifier/name used by the export model. |
| `playback` | integer | Stable playback ID 0 through 6. |

## Sparse fields

| Field | Omitted when | Consumer default |
|---|---|---|
| `pivot` | centered | `[0.5,0.5]` |
| `polygon` | axis-aligned frame quad | frame rect |
| `slice9` | all zero | no 9-slice |
| `transform`, `transformStr` | identity | identity |
| root `animations` | no explicit animations | empty array |

All other root/page/sprite fields listed above are present.
