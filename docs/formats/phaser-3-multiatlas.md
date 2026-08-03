# Phaser 3 Multi-Atlas export

**Status:** Target export contract; not implemented by the current product.

The `phaser-3-multiatlas` runtime package emits one Phaser Multi-Atlas JSON Array
document plus the common core-owned PNG pages. It targets Phaser `>=3.70 <4`;
the executable example and independent consumer oracle pin Phaser 3.90.0.

The consumer authority is Phaser 3.90.0's
[`MultiAtlasFile`](https://github.com/phaserjs/phaser/blob/v3.90.0/src/loader/filetypes/MultiAtlasFile.js#L204),
[`TextureManager.addAtlasJSONArray`](https://github.com/phaserjs/phaser/blob/v3.90.0/src/textures/TextureManager.js#L860-L891),
and
[`JSONArray`](https://github.com/phaserjs/phaser/blob/v3.90.0/src/textures/parsers/JSONArray.js#L38-L48).
`MultiAtlasFile` reads the root `textures` array and each entry's `image`, then
explicitly dispatches those page objects through `addAtlasJSONArray`;
`JSONArray` consumes each page's `frames` array, trim, rotation, pivot, and
`scale9Borders` members. Phaser's
[`Frame.setScale9`](https://docs.phaser.io/api-documentation/3.90.0/class/textures-frame#setScale9)
defines those four values as the center rectangle. A JSON Hash `frames` object
is not a valid MultiAtlas page for this loader.

## Package descriptor

```json
{
  "api_version": 1,
  "id": "phaser-3-multiatlas",
  "display_name": "Phaser 3 Multi-Atlas",
  "capabilities": {
    "transforms": ["identity", "rotate_90_cw"],
    "polygons": false,
    "pivot": true,
    "slice9": true,
    "multipage": true,
    "aliases": true,
    "animations": false
  },
  "outputs": [
    {"id": "multiatlas", "suffix": ".json"}
  ]
}
```

Polygons are rectangle-packed by common capability projection and reported as
loss. Explicit format-level animations are hidden and reported as unsupported;
the example may animate already exported frame names in game code. No custom
emulation hides either limitation.

## JSON document

The document is Unicode-scalar UTF-8 without BOM or NUL, uses two-space
indentation and LF, and ends in one LF. Object members occur in the order shown
here. Page entries are ascending by page index. Frame entries within a page are
ascending by final UTF-8 name bytes.

```json
{
  "textures": [
    {
      "image": "atlas-0.png",
      "format": "RGBA8888",
      "size": {"w": 1024, "h": 512},
      "scale": 1,
      "frames": [
        {
          "filename": "hero",
          "frame": {"x": 10, "y": 20, "w": 32, "h": 48},
          "rotated": false,
          "trimmed": true,
          "spriteSourceSize": {"x": 4, "y": 2, "w": 32, "h": 48},
          "sourceSize": {"w": 40, "h": 52},
          "pivot": {"x": 0.5, "y": 0.5},
          "scale9Borders": {"x": 8, "y": 6, "w": 24, "h": 38}
        }
      ]
    }
  ]
}
```

There is no root or per-page `meta` object. No timestamp, absolute path,
package fingerprint, host version, or original source path/name enters the
consumer bytes. Integers are unquoted base 10. Pivot values use the common
`C`-locale `%.9g` f32 representation.

## Field mapping

| JSON field | Export IR / plan mapping |
|---|---|
| `textures[i].image` | basename of core page artifact `i`, normally `<base>-<i>.png` with zero-based `i` |
| `format` | fixed string `RGBA8888` |
| `size.w/h` | page dimensions |
| `scale` | fixed integer `1` |
| `frames[j].filename` | final sprite name |
| `frame.x/y` | placed page origin |
| `frame.w/h` | canonical unrotated/pre-D4 width and height |
| `rotated` | true only for `rotate_90_cw` |
| `trimmed` | canonical trimmed flag |
| `spriteSourceSize` | unrotated trim rectangle inside the source |
| `sourceSize` | original untrimmed source dimensions |
| `pivot.x/y` | normalized source pivot, always emitted |
| `scale9Borders` | emitted only for a nonzero slice9; center rectangle described below |

For `[left, right, top, bottom]` and untrimmed source `width, height`:

```text
x = left
y = top
w = width  - left - right
h = height - top  - bottom
```

Common project validation guarantees non-negative center dimensions. Rotation
changes only the page footprint and `rotated`; trim, source, pivot, and slice9
remain in unrotated source space, matching Phaser's JSON Array parser.

An alias receives its own complete frame-array entry with the alias final name,
original page rectangle, and metadata. This preserves every consumer frame key
without a Phaser-specific alias extension.

Phaser's texture creates its own `__BASE` frame and stores frames in an ordinary
JavaScript object whose `has()` method calls `frames.hasOwnProperty`. The exact
reserved final-name set is:

```text
__BASE
__proto__
hasOwnProperty
```

A target containing one of those names fails before document completion with a
structured handler error. Names are never silently rewritten.

## Rotation and UV meaning

The only non-identity admitted D4 value is `rotate_90_cw` (stored value 5).
Its `frame.w/h` remain the unrotated dimensions. The actual on-page footprint
is therefore `frame.h` by `frame.w`. `rotated: true` makes Phaser 3.90's JSON
Array parser call
[`updateUVsInverted`](https://github.com/phaserjs/phaser/blob/v3.90.0/src/textures/Frame.js#L766-L775),
which uses `cutHeight` along atlas X and `cutWidth` along atlas Y. Every other
D4 value is excluded during effective-settings packing and cannot reach the
Lua handler.

## Independent conformance oracle

The producer Lua code is not its own validator. Packet 4 adds an independent C
parser that reads the emitted JSON and proves:

- exact root/page/frame-array member vocabulary and types;
- every planned PNG basename has one page entry and no extra entry;
- final names are unique, sorted in bytes, and avoid the reserved set;
- page bounds using the derived post-rotation footprint, trim/source
  relationship, pivot, alias, and center-rectangle slice9 mapping match the
  canonical Export IR;
- polygons and explicit animations are absent and their common loss notices are
  present when applicable;
- exact output bytes are stable across repeated exports and platforms.

The owning CTest gates are `tp_phaser_export_contract`,
`tp_phaser_capability_matrix`, and `tp_phaser_consumer_validate`.

## Example consumer

The public Phaser consumer pins `phaser` to exactly `3.90.0` and one exact small
development-server dependency in `package.json` plus its lockfile. It loads the
committed document with `this.load.multiatlas`. `node_modules` and generated
bundles are not committed. Manual use is `npm ci` followed by the documented run
command; ordinary CI does not install Node or launch a browser.
