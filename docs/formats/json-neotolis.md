# `json-neotolis` export format

The **full-fidelity** JSON descriptor for a packed atlas. It is the reference
target: the only JSON schema that can express everything the packer produces —
the D4 transform mask (rotations *and* flips), concave polygons, per-sprite
pivots, 9-slice borders, multiple pages, aliases and animations. Every other
(foreign) exporter is a lossy projection of this model.

- **Exporter id:** `json-neotolis`
- **Output:** one `<base>.json` next to its page images `<base>-<N>.png`
- **Schema version:** `1` (the `version` field; bump on any breaking change)
- **Encoding:** UTF-8, LF line endings, 2-space indent, trailing newline.

Consumers **must ignore unknown fields** — new optional fields may be added
under the same schema `version`; a breaking change bumps `version`.

---

## Files & page naming

The exporter writes, for output base path `<base>` (the target's `out_path`, no
extension):

- `<base>.json` — this descriptor.
- `<base>-<page>.png` — one straight-alpha RGBA8 PNG per page, `<page>` = the
  0-based page index. A single-page atlas still uses the `-0` suffix (uniform,
  so a consumer never special-cases page count).

Inside the JSON, a page's `file` is the **basename only** (e.g.
`"atlas-0.png"`) — page images always sit in the same directory as the JSON, so
a consumer resolves them relative to the descriptor.

Pages are **straight-alpha** by default (RGB not premultiplied); each page
records this in its `premultiplied` flag. (The page writer supports a
premultiply toggle; `json-neotolis` uses straight alpha.)

---

## Determinism

Output is byte-for-byte reproducible for identical input (pinned by a re-export
`memcmp` test):

- `version` is emitted first; every other object key follows in ascending ASCII
  order.
- Sprites are ordered by their **final export name**; animations by `id`; page
  array by page index.
- Floats use `%.9g` (exact round-trip). Locale is fixed to `C`.

## Sparseness

Fields that carry no information are omitted (a consumer applies the documented
default). The rules:

| Field | Omitted when | Default a consumer assumes |
|---|---|---|
| `pivot` | pivot is the center `(0.5, 0.5)` | center `(0.5, 0.5)` |
| `polygon` | the sprite hull is the axis-aligned frame quad | frame rect is the mesh |
| `slice9` | all four borders are `0` | no 9-slice |
| `transform` / `transformStr` | transform is identity (`0`) | identity |
| `animations` (root) | there are none | `[]` |
| `alias_of` | *(never — always emitted, `null` for originals)* | — |

Everything else (`frame`, `name`, `page`, `sourceSize`, `spriteSourceSize`,
each page's `file`/`w`/`h`/`premultiplied`) is always present.

> **Note on capability degradation.** A *lossy* target reuses this writer with a
> reduced capability set (see the exporter registry). Then the field it cannot
> hold is dropped and a metadata-loss *notice* is raised — never an error. In a
> full `json-neotolis` export no field is ever dropped.

## Export-name overrides (rename)

Region names derive from source file names, but a project may rename any sprite
(a sparse per-sprite override). Renames are resolved by the normalization pass
**before** this writer runs, so the output is unaffected structurally: `name`
and every `frames[]`/`alias_of` reference are simply the **final** names. There
is no override-specific field in this schema.

---

## Field reference

### Root

| Key | Type | Notes |
|---|---|---|
| `version` | int | Schema version. Emitted first. |
| `animations` | array | Optional; omitted when empty. See **Animation**. |
| `atlas` | string | Atlas display name. |
| `pages` | array | One entry per page. See **Page**. |
| `pixels_per_unit` | number | Source pixels per world unit (atlas scale metadata). |
| `sprites` | array | One entry per sprite/alias, sorted by `name`. See **Sprite**. |

### Page

| Key | Type | Notes |
|---|---|---|
| `file` | string | Page image basename, `<atlasbase>-<index>.png`. |
| `h` | int | Page height in pixels. |
| `premultiplied` | bool | `false` for straight alpha (the default). |
| `w` | int | Page width in pixels. |

### Sprite

Coordinates are **y-down** (PNG space: origin top-left, y increases downward).

| Key | Type | Notes |
|---|---|---|
| `alias_of` | string \| null | Final name of the sprite whose frame this one shares, or `null` if this is the original placement. Every aliased name is emitted as its own entry pointing at the shared frame. |
| `frame` | object `{x,y,w,h}` | Placed **trimmed** rect on the page. `w`/`h` are the **unrotated** trim size — when `transform` has the diagonal bit, the on-page footprint is `h × w`. |
| `name` | string | Final export name (unique). |
| `page` | int | Page index this sprite lives on. |
| `pivot` | array `[x,y]` | Normalized over `sourceSize`, y-down; may lie outside `[0,1]`. Omitted when centered. |
| `polygon` | object | Trim-local hull mesh; omitted for plain rects. `verts`: array of `[x,y]` (trim-local y-down, `(0,0)` = top-left of the trimmed content; may slightly exceed the frame bounds due to hull inflation). `indices`: triangle list into `verts`. |
| `slice9` | array `[l,r,t,b]` | 9-slice border widths in px. Omitted when all zero. |
| `sourceSize` | object `{w,h}` | Original untrimmed image size. |
| `spriteSourceSize` | object `{x,y,w,h}` | Trimmed content rect inside the original image, y-down. When untrimmed: `x=y=0`, `w,h == sourceSize`. |
| `transform` | int | D4 mask: bit0 (`1`) flipH, bit1 (`2`) flipV, bit2 (`4`) diagonal (transpose). `0` = identity, omitted. Apply order: diagonal → flipH → flipV; the diagonal bit swaps `w,h` first. A pure 90° rotation is the diagonal bit composed with one flip. |
| `transformStr` | string | Human-readable decode of `transform`, tokens `flipH`/`flipV`/`diag` joined by `\|` (e.g. `"diag"`, `"flipH\|diag"`). Emitted alongside `transform`. |

### Animation

Flipbook metadata over sprite names, orthogonal to placement. Either explicit
(from the project) or numeric-suffix auto-grouped (`walk_01`, `walk_02` →
`walk`, frames ordered by numeric suffix). All fields are always emitted.

| Key | Type | Notes |
|---|---|---|
| `flip_h` | bool | Play horizontally mirrored. |
| `flip_v` | bool | Play vertically mirrored. |
| `fps` | number | Playback rate (default 30 for auto groups). |
| `frames` | array of string | Final sprite names in explicit playback order. |
| `id` | string | Animation identifier. |
| `playback` | int | Stable playback-mode id (`0` = once-forward). |

---

## Annotated example

A two-sprite atlas where the packer rotated the tall sprite (`transform: 4`,
diagonal) to pack tighter, one page:

```json
{
  "version": 1,
  "atlas": "rotated",
  "pages": [
    {
      "file": "rotated-0.png",
      "h": 24,
      "premultiplied": false,
      "w": 220
    }
  ],
  "pixels_per_unit": 1,
  "sprites": [
    {
      "alias_of": null,
      "frame": { "h": 100, "w": 24, "x": 120, "y": 0 },
      "name": "rot_tall",
      "page": 0,
      "sourceSize": { "h": 100, "w": 24 },
      "spriteSourceSize": { "h": 100, "w": 24, "x": 0, "y": 0 },
      "transform": 4,
      "transformStr": "diag"
    },
    {
      "alias_of": null,
      "frame": { "h": 24, "w": 120, "x": 0, "y": 0 },
      "name": "rot_wide",
      "page": 0,
      "sourceSize": { "h": 24, "w": 120 },
      "spriteSourceSize": { "h": 24, "w": 120, "x": 0, "y": 0 }
    }
  ]
}
```

Reading `rot_tall`: its trim size is `24×100` (`frame.w/h`), but because
`transform` has the diagonal bit it occupies a `100×24` footprint at page
`(120, 0)`. It is centered (no `pivot`), untrimmed (`spriteSourceSize == frame`
dims, offset `0,0`) and a plain rect (no `polygon`).
