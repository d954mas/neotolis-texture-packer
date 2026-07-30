# Defold Atlas Pipeline & extension-texturepacker — Research

> **Status: non-normative point-in-time research.** Use as format evidence and
> fixture input; current product decisions come from
> [`../ntpacker-master-spec.md`](../ntpacker-master-spec.md).

## Summary

Defold's native `.atlas` is a **protobuf text file** (schema: `atlas_ddf.proto`) listing source PNGs + flipbook animations; bob/the editor packs it into a binary `.texturesetc` (`TextureSet`, schema `texture_set_ddf.proto`) + `.texturec` texture, with compression driven by `.texture_profiles`. The native packer supports margin/extrude/inner-padding, trimming (4–8 verts or polygons), per-image pivots (since Defold 1.9.5), multipage atlases — but **no rotation** and no pre-packed input. The official [extension-texturepacker](https://github.com/defold/extension-texturepacker) adds exactly that: it consumes a **`.tpinfo`** file (protobuf **text** format, `tpinfo.proto`) describing already-packed pages + a `.tpatlas` wrapper for animations, and compiles them to the same engine `TextureSet`, including 90° rotation, trim and polygon geometry. For a third-party packer the cheapest, most robust integration is **emitting `.tpinfo` (+ page PNGs, optionally a `.tpatlas`)** and letting users add the official extension — the format is small, documented below field-by-field, and requires no plugin maintenance from us. A fallback "unpacked" export of `.atlas` + individual PNGs covers users who refuse the extension dependency. A ~7-file demo project built headless with `bob.jar` proves the pipeline.

---

## Defold native atlas format

### Source format: `.atlas` (protobuf text, schema `dmGameSystemDDF.Atlas` in `engine/gamesys/proto/gamesys/atlas_ddf.proto`)

Schema (Defold master, abridged to actual fields):

```proto
message AtlasImage {
    required string image                        = 1;   // project-absolute path, e.g. "/assets/hero.png"
    optional SpriteTrimmingMode sprite_trim_mode = 2 [default = SPRITE_TRIM_MODE_OFF];
    optional float  pivot_x                      = 3 [default = 0.5]; // 0..1, top-left origin (may exceed range)
    optional float  pivot_y                      = 4 [default = 0.5];
}
message AtlasAnimation {
    required string id              = 1;
    repeated AtlasImage images      = 2;
    optional Playback playback      = 3 [default = PLAYBACK_ONCE_FORWARD];
    optional uint32 fps             = 4 [default = 30];
    optional uint32 flip_horizontal = 5 [default = 0];  // 0/1
    optional uint32 flip_vertical   = 6 [default = 0];
}
message Atlas {  // editor format; engine format is texture_set_ddf.proto
    repeated AtlasImage images         = 1;   // single-frame images
    repeated AtlasAnimation animations = 2;
    optional uint32 margin             = 3 [default = 0]; // px between images
    optional uint32 extrude_borders    = 4 [default = 0]; // repeat edge px (bleed fix)
    optional uint32 inner_padding      = 5 [default = 0]; // transparent px around image
    optional uint32 max_page_width     = 6 [default = 0]; // >0 => paged atlas (2D array texture)
    optional uint32 max_page_height    = 7 [default = 0];
    optional string rename_patterns    = 8;               // "search=replace,..."
}
```

Real example — `animation/animation_states/assets/knight.atlas` from `defold/examples` (abridged) plus a rotate-demo atlas from extension-texturepacker (`examples/rotate/rotate_d.atlas`, complete):

```
animations {
  id: "idle"
  images { image: "/assets/images/Idle1.png" }
  images { image: "/assets/images/Idle2.png" }
  playback: PLAYBACK_LOOP_FORWARD
  fps: 15
}
extrude_borders: 2
```

```
images {
  image: "/examples/rotate/a.png"
  sprite_trim_mode: SPRITE_TRIM_MODE_OFF
}
images {
  image: "/examples/rotate/b.png"
  sprite_trim_mode: SPRITE_TRIM_MODE_OFF
}
margin: 0
extrude_borders: 0
inner_padding: 0
max_page_width: 0
max_page_height: 0
rename_patterns: ""
```

With pivots (1.9.5+), an image entry looks like:

```
images {
  image: "/assets/hero.png"
  sprite_trim_mode: SPRITE_TRIM_POLYGONS
  pivot_x: 0.5
  pivot_y: 1.0    # bottom-center; top-left is (0,0), bottom-right (1,1)
}
```

Enums (from `tile_ddf.proto`): `Playback` = `PLAYBACK_NONE(0)`, `ONCE_FORWARD(1)`, `ONCE_BACKWARD(2)`, `LOOP_FORWARD(3)`, `LOOP_BACKWARD(4)`, `LOOP_PINGPONG(5)`, `ONCE_PINGPONG(6)`. `SpriteTrimmingMode` = `SPRITE_TRIM_MODE_OFF(0)`, `_4(4)`, `_5(5)`, `_6(6)`, `_7(7)`, `_8(8)`, `SPRITE_TRIM_POLYGONS(9)`.

### Feature matrix (native `.atlas`)

| Feature | Supported | Notes |
|---|---|---|
| Flipbook animations, fps, playback modes | yes | per-animation `flip_horizontal/vertical` |
| Pivot points | yes, **since Defold 1.9.5** (Nov 2024) | per image, unit space, top-left origin, may go outside 0–1 |
| Trim | yes | `sprite_trim_mode`: rect / 4–8-vertex hull / polygons (transparent pixels culled at compile) |
| Rotation during packing | **no** | there is no rotation option; rotated placement only exists in the *engine* format (used by extension-texturepacker) |
| Multipage | yes | set `max_page_width/height`; compiles to 2D-array texture, sampled via `sprite_paged_atlas.material` |
| Pre-packed input | no | Defold always packs the listed PNGs itself (auto-sized to power-of-two) |

### Compiled output

Bob's builder `com.dynamo.bob.pipeline.AtlasBuilder` is registered as `@BuilderParams(name="Atlas", inExts={".atlas"}, outExt=".a.texturesetc", ...)`. One `.atlas` produces:
- **`<name>.a.texturesetc`** — binary protobuf `dmGameSystemDDF.TextureSet` (`texture_set_ddf.proto`)
- **`<name>.texturec`** — compiled texture (schema `graphics_ddf.proto`, mip/compression per texture profile)

Key engine `TextureSet` fields (what any pipeline ultimately produces): `texture` (path), `width/height`, `animations[]` (`id`, frame `start/end` indices, fps/playback/flips), `image_name_hashes[]`, `frame_indices[]`, `tex_coords` (bytes; 4 UV pairs per frame — comment in the proto: unrotated order `[(minU,maxV),(minU,minV),(maxU,minV),(maxU,maxV)]`, rotated order `[(minU,minV),(maxU,minV),(maxU,maxV),(minU,maxV)]`), `geometries[]` (`SpriteGeometry`: per-image `width/height`, `center_x/y`, `rotated` (90° flag), vertices in local `[-0.5,0.5]` space around sprite center (y-up, unrotated), triangle-list `indices`, `pivot_x/pivot_y` in `[-0.5,0.5]`), `page_indices[]`, `page_count`. So **rotation is a first-class concept in the engine format** even though the native packer never sets it.

### Texture profiles (`.texture_profiles`)

Set in `game.project`: `[graphics] texture_profiles = /all.texture_profiles`. Protobuf text (schema `graphics_ddf.proto`). Real file from `defold/examples`:

```
path_settings {
  path: "**"            # Ant glob, matched top-down against resource path
  profile: "Default"
}
profiles {
  name: "Default"
  platforms {
    os: OS_ID_GENERIC   # or OS_ID_WINDOWS / OS_ID_IOS / OS_ID_ANDROID / ...
    formats {
      format: TEXTURE_FORMAT_RGBA
      compression_level: BEST        # FAST/NORMAL/HIGH/BEST
      compression_type: COMPRESSION_TYPE_DEFAULT   # or BASIS_UASTC, ASTC...
    }
    mipmaps: false
    max_texture_size: 0    # >0 downscales anything larger
    premultiply_alpha: true
  }
}
```

Compression only applies when building with `--texture-compression`. Profiles apply to atlases by path match (e.g. `/gui/**/*.atlas`). **Max texture size**: no hard engine limit — atlas pages grow to the next power of two; the practical cap is the GPU (4096 on old mobiles, 8192/16384 typical today), and `max_texture_size` in a profile is the clamp mechanism.

---

## extension-texturepacker deep dive

Repo: `github.com/defold/extension-texturepacker` (MIT, by the Defold Foundation). Adds two resource types to editor + bob: **`.tpinfo`** (the packed-layout description, generated by CodeAndWeb TexturePacker via a bundled custom exporter) and **`.tpatlas`** (a small Defold-side wrapper adding animations/options). Docs: `defold.com/extension-texturepacker/`, format spec in `README_FORMAT.md`.

### How data flows

1. In TexturePacker, the user installs the custom exporter from `exporter/defold/` (an `exporter.xml` + Grantlee template + `defoldscript.qs` JavaScript) and exports → **one `.tpinfo` text file + N page PNGs next to it**.
2. In the Defold editor, the user creates a `.tpatlas` and points its `file` field to the `.tpinfo`.
3. Bob builder `com.dynamo.bob.pipeline.tp.AtlasBuilder` (`@BuilderParams(name="TexturePackerAtlas", inExts=".tpatlas", outExt=".a.texturesetc")`) parses the `.tpinfo` with `TextFormat.merge` (protobuf **text** parser — so `#` comments and `[1, 2, 3]` list syntax are legal), converts pages/sprites into bob's `TextureSetLayout`, runs the standard `TextureSetGenerator.createTextureSet`, and writes the same `.a.texturesetc` + `.texturec` a native atlas would produce. Page PNGs are loaded verbatim (`TextureUtil.createMultiPageTexture`) — **the extension never repacks pixels**.

### `.tpinfo` format (schema `texturepacker/pluginsrc/tpinfo.proto`, package `dmGameSystemDDF`, text format on disk)

```proto
message Point { required float x = 1; required float y = 2; }
message Size  { required float width = 1; required float height = 2; }
message Rect  { required float x = 1; required float y = 2;
                required float width = 3; required float height = 4; }

// Coordinates are in image space (texels)
message Sprite {
    required string name = 1;
    required bool   trimmed = 2;   // trimmed rect smaller than source
    required bool   rotated = 3;   // rotated 90 degrees (see note below)
    required bool   is_solid = 4;  // no transparent texels
    required Size   untrimmed_size = 5; // original image size
    required Point  corner_offset = 6;  // top-left corner -> trimmed rect
    required Rect   source_rect = 7;    // trimmed rect inside the ORIGINAL image
    required Rect   frame_rect = 8;     // final placement in the PAGE (w/h swapped if rotated)
    repeated Point  vertices = 9;       // source-image space, px, unrotated, y-down
    repeated int32  indices = 10;       // every 3 = one CCW triangle
    optional Point  pivot = 11;         // px from top-left of untrimmed image (since exporter v2.0)
}
message Page  { required string name = 1;   // PNG filename, relative to the .tpinfo
                required Size size = 2;     // page texture size
                repeated Sprite sprites = 3; }
message Atlas { repeated Page pages = 1;
                optional string version = 2;      // e.g. "2.0" (format version)
                optional string description = 3;  // tool that generated it
}
```

Real trimmed sprite from `examples/anim_trim/anim_trim.tpinfo` (128×128 source trimmed to a 34×54 polygon placed at page origin):

```
version: "2.0"
description: "Exported using TexturePacker"
pages {
  name: "anim_trim.png"
  size { width: 128  height: 128 }
  sprites {
    name: "sq9"
    trimmed: true
    rotated: false
    is_solid: false
    corner_offset { x: 13  y: 25 }
    source_rect   { x: 13  y: 25  width: 34  height: 54 }
    frame_rect    { x: 0   y: 0   width: 34  height: 54 }
    untrimmed_size { width: 128  height: 128 }
    vertices { x: 41  y: 79 }
    vertices { x: 13  y: 51 }
    vertices { x: 47  y: 25 }
    indices: [0, 1, 2]
  }
}
```

Rotated sprite from `examples/rotate/rotate.tpinfo` (source 130×80 placed as 80×130):

```
sprites {
  name: "c"
  trimmed: false
  rotated: true
  is_solid: true
  corner_offset { x: 0  y: 0 }
  source_rect   { x: 0  y: 0  width: 130  height: 80 }
  frame_rect    { x: 170  y: 0  width: 80  height: 130 }   # w/h swapped!
  untrimmed_size { width: 130  height: 80 }
  vertices { x: 130 y: 80 }  vertices { x: 0 y: 80 }
  vertices { x: 0 y: 0 }     vertices { x: 130 y: 0 }
  indices: [1, 2, 3, 0, 1, 3]
}
```

**Exact conventions (verified against `AtlasBuilder.createSprite` / `Atlas.getTriangles` and the reference exporter `defoldscript.qs`):**

- **Origin/units**: everything is pixels, top-left origin, y-down (TexturePacker convention).
- **`frame_rect`** = placed rect in page coordinates; when `rotated: true` its width/height are the *rotated* (as-drawn) dimensions.
- **Rotation encoding**: `rotated: true` means the sprite bitmap was physically rotated 90° when blitted into the page such that **the source image's top-left corner ends up at the top-right corner of `frame_rect`** (the Java loader literally computes `corner_x = frame_rect.x + frame_rect.width + offset.y`, with the comment "When rotated, the 'top left' of the image is now the top right"). Viewed on screen (y-down) that is a 90° clockwise rotation of the image content. Ignore the proto comment saying "ccw" — CW/CCW flips with the y-down vs y-up frame; match the corner rule above and validate against `examples/rotate/`.
- **`corner_offset`** = `(source_rect.x, source_rect.y)` — offset from the untrimmed image's top-left to the trimmed rect. Redundant with `source_rect`, but the loader uses `corner_offset` (it reads `name`, `rotated`, `untrimmed_size`, `frame_rect`, `corner_offset`, `pivot`, `vertices`, `indices`; `trimmed`/`is_solid`/`source_rect` are informational — but all `required` fields must still be present).
- **`vertices`** are in **untrimmed source-image space** (px, y-down, never rotated even for rotated sprites); `indices` is a flat triangle list, CCW winding, may repeat vertices. If both lists are empty, a rectangle is auto-generated. The reference exporter always writes a quad for non-polygon sprites: vertices in order TR, TL, BL, BR of `source_rect`, `indices: [1, 2, 3, 0, 1, 3]`.
- **`pivot`** (format version "2.0", extension ≥ 2.0): px offset from the untrimmed image's top-left; where the sprite's runtime center will be. If absent, center of the image is assumed (backwards compatible with v1.1 files).
- **Pages**: `name` is resolved relative to the `.tpinfo` file's folder. The Defold engine assumes **all pages have identical dimensions** (they become one 2D-array texture). Multiple sprites may share/alias the same page area, but geometry must not spill into neighboring sprites.
- Fill `version`/`description` honestly — the maintainers explicitly ask third-party generators to do so for debuggability.

### `.tpatlas` format (schema `tpatlas.proto`, text format, authored in the Defold editor)

```proto
message AtlasAnimation {
    required string id = 1;
    repeated string images = 2;          // frame names = Sprite.name values
    optional Playback playback = 3 [default = PLAYBACK_ONCE_FORWARD];
    optional uint32 fps = 4 [default = 30];
    optional uint32 flip_horizontal = 5 [default = 0];
    optional uint32 flip_vertical = 6 [default = 0];
}
message AtlasDesc {
    required string file = 1;            // project path of the .tpinfo
    optional string rename_patterns = 2; // "search=replace,..."
    repeated AtlasAnimation animations = 3;
    optional bool is_paged_atlas = 4 [default = false]; // only meaningful for 1-page tpinfo
}
```

Real file (`examples/basic/basic.tpatlas`, complete):

```
file: "/examples/basic/basic.tpinfo"
rename_patterns: ""
animations {
  id: "BoxFlip"
  images: "box_fill_128"
  images: "box_fill_64"
  playback: PLAYBACK_LOOP_FORWARD
  fps: 4
  flip_horizontal: 0
  flip_vertical: 0
}
is_paged_atlas: false
```

Every sprite name automatically becomes a single-frame animation; `.tpatlas` adds flipbook animations on top. A `.tpinfo` with >1 page always builds as a paged (2D-array) texture and sprites must then use `/builtins/materials/sprite_paged_atlas.material`.

### How it registers in editor and bob

- **Bob side**: `texturepacker/plugins/share/pluginTexturePackerExt.jar` (checked into the extension) contains `com.dynamo.bob.pipeline.tp.AtlasBuilder`, a `ProtoBuilder` subclass with `@BuilderParams(inExts=".tpatlas", outExt=".a.texturesetc")`; bob discovers builder classes from dependency plugin jars automatically. The build task declares the `.tpinfo`, every page PNG, and the texture profile as inputs.
- **Editor side**: `texturepacker/editor/src/texturepacker.clj` is loaded by the editor's plugin system; it calls `resource-node/register-ddf-resource-type` for `tpinfo` (view-only) and `tpatlas` (`:build-ext "a.texturesetc"`, template `template.tpatlas`) and — crucially — `workspace/register-resource-kind-extension workspace :atlas "tpatlas"`, which makes `.tpatlas` selectable anywhere an atlas is accepted (sprite, GUI, particlefx, tilemap components). The Clojure plugin calls into the same Java jar for parsing/preview.
- `ext.manifest` makes the repo a native extension so it can be pulled in as a normal `game.project` dependency zip.

### Capabilities and limitations

- **Supported**: 90° rotation, trimming, polygon packing, multipage, pivots (v2.0+), aliases (duplicate sprites pointing at the same rect), flipbook animations, rename patterns, texture profiles/compression — output is a standard `TextureSet`, so sprites/GUI/particles all work.
- **Limitations**: `.tpinfo` is trusted as-is (no repacking, no extrude/padding added — bake those into the PNGs yourself); all pages must share dimensions; in the editor you cannot add images to a `.tpatlas` or delete auto single-frame animations (re-export instead); **version lock-step**: each extension release is pinned to a Defold version (2.7.0↔Defold 1.12.4, 2.5.0↔1.12.0, 2.3.0↔1.10.1, …) because the editor-plugin API churns — users must match `game.project` dependency version to their Defold version.

---

## Integration options for our packer

### (a) Emit `.atlas` + individual PNGs — "let Defold pack"

Our tool exports the loose sprite PNGs into the project plus a generated `.atlas` text file (trivial: `images {}` + `animations {}` blocks per the schema above, plus margin/extrude/pivot settings). Defold repacks at build time.

- Pros: zero dependencies, works in every Defold version, full editor UX (users can keep editing the atlas), pivots/trim supported natively.
- Cons: **our packing is thrown away** — no rotation, Defold's own packer/POT sizing decides layout; page PNGs from our tool are unused; nothing distinguishes our product from a file-copy script.
- Cost: ~1 day. Worth shipping as a secondary "Defold (unpacked)" preset.

### (b) Emit `.tpinfo` + page PNGs (mimic the official extension's input) — recommended

Our packer writes its packed pages as PNGs and one `.tpinfo` protobuf-text file per the field spec above (plus optionally a starter `.tpatlas` since `tpatlas.proto` is public — "if you wish to define a lot of animations up front", per README_FORMAT). Users add the official extension to `game.project` and point a `.tpatlas` at our `.tpinfo`.

- Pros: **preserves our packing** (rotation, trim, polygons, multipage, aliases, pivots); the format is explicitly designed for third-party generators; engine/editor integration is maintained by the Defold Foundation, not us; text format is diffable and easy to test.
- Cons: runtime dependency on the extension; page-size uniformity requirement; feature ceiling = what `tpinfo.proto` encodes.
- Implementation checklist for the exporter: y-down pixel coords; `frame_rect` w/h swapped when rotated; rotated blit = source top-left → placed top-right; `corner_offset == source_rect.xy`; always write all `required` fields (`trimmed`, `is_solid`, `source_rect` included); quad fallback `indices: [1,2,3,0,1,3]` with TR/TL/BL/BR vertices; pivot in px from untrimmed top-left; page `name` relative to the `.tpinfo`; identical page sizes (pad the last page); set `version`/`description` to our tool name+version. Validate by diffing against `examples/rotate/rotate.tpinfo` and `examples/anim_trim/anim_trim.tpinfo` and by building the extension's own example project with our file substituted. The extension even ships a CLI test harness (`utils/test_plugin.sh <file>.tpinfo`) that parses a `.tpinfo` and dumps computed triangles.

### (c) Write our own Defold extension (custom resource type for our format)

Clone of extension-texturepacker's architecture: bob plugin jar (Java `ProtoBuilder`) + Clojure editor plugin + protos.

- Pros: total format freedom (per-page sizes, nine-slice metadata, whatever).
- Cons: heavy: Java + Clojure against unstable internal editor APIs, **a new release required for nearly every Defold version** (see the extension's own release cadence), prebuilt plugin jars checked into the repo, CI against Defold betas. This is an ongoing maintenance tax with almost no user-visible benefit over (b).

**Recommendation:** ship **(b) as the primary Defold export target** ("Defold / TexturePacker-extension `.tpinfo`"), with **(a) as a zero-dependency fallback preset**. Do not do (c) — option (b) gives us everything (c) would, with the Defold Foundation maintaining the engine half.

## Minimal demo project plan

Goal: prove a `.tpinfo` produced by **our** packer renders correctly (incl. a rotated and a trimmed sprite + one flipbook animation) in a stock Defold build.

```
demo/
├── game.project
├── assets/
│   ├── packed.tpinfo        # generated by our packer
│   ├── packed-0.png         # page 0 (our packer's output)
│   ├── packed.tpatlas       # 5 lines, see below
└── main/
    └── main.collection
```

`game.project` (complete):

```
[bootstrap]
main_collection = /main/main.collectionc

[project]
title = tpinfo-demo
dependencies#0 = https://github.com/defold/extension-texturepacker/archive/refs/tags/2.7.0.zip

[display]
width = 960
height = 640
```

(The dependency tag must match the Defold/bob version — 2.7.0 ↔ Defold 1.12.4.)

`assets/packed.tpatlas`:

```
file: "/assets/packed.tpinfo"
animations {
  id: "walk"
  images: "walk_0"
  images: "walk_1"
  playback: PLAYBACK_LOOP_FORWARD
  fps: 8
}
```

`main/main.collection` — one embedded game object with an embedded sprite referencing the tpatlas (this is the modern `textures{}` sprite form; for a single-page non-paged atlas `sprite.material` works, use `sprite_paged_atlas.material` if `is_paged_atlas`/multipage):

```
name: "main"
embedded_instances {
  id: "hero"
  data: "embedded_components {\n"
  "  id: \"sprite\"\n"
  "  type: \"sprite\"\n"
  "  data: \"default_animation: \\\"walk\\\"\\n"
  "material: \\\"/builtins/materials/sprite.material\\\"\\n"
  "textures {\\n"
  "  sampler: \\\"texture_sampler\\\"\\n"
  "  texture: \\\"/assets/packed.tpatlas\\\"\\n"
  "}\\n"
  "\"\n"
  "}\n"
  position { x: 480.0 y: 320.0 z: 0.0 }
}
```

(For a native-atlas variant, the sprite instead uses `tile_set: "/assets/sprites.atlas"` — see `defold/examples` flipbook collection.)

**Headless build with bob:**

1. Download `bob.jar` from the Defold GitHub releases page (asset `bob.jar` per release; also mirrored per-SHA at `d.defold.com`). Requires OpenJDK 25 for current releases (older Defold: JDK 17/21). Keep bob's version == extension tag's Defold version.
2. `cd demo && java -jar bob.jar resolve` — downloads the extension zip dependency.
3. `java -jar bob.jar --archive build` — compiles everything. A malformed `.tpinfo` fails here (the extension's builder validates page images and animation frame references), so a green build already proves parse+compile. Add `--texture-compression true` to also exercise texture profiles.
4. Visual proof: `java -jar bob.jar --archive --platform x86_64-win32 build bundle` and run the bundled exe; on CI use `--variant headless` and assert the build/bundle succeeds. CI reference: the extension itself builds with the shared `defold/github-actions-common` bob workflow.

Acceptance checks in the demo: rotated sprite renders upright, trimmed sprite has correct size (`go.get("#sprite", "size")` equals untrimmed size), pivot offset visible, `walk` animation plays.

## Sources

- https://github.com/defold/extension-texturepacker — repo (MIT), incl. `README.md`, `README_FORMAT.md`
- https://github.com/defold/extension-texturepacker/blob/main/texturepacker/pluginsrc/tpinfo.proto and `tpatlas.proto` — format schemas
- https://github.com/defold/extension-texturepacker/blob/main/texturepacker/pluginsrc/com/defold/bob/pipeline/tp/AtlasBuilder.java, `Atlas.java`, `Loader.java` — reference loader (rotation/pivot/vertex conventions)
- https://github.com/defold/extension-texturepacker/blob/main/exporter/defold/grantlee/0.2/defoldscript.qs — reference exporter (quad/index conventions)
- Examples: `examples/basic/*.tpinfo|.tpatlas|.collection`, `examples/rotate/*`, `examples/anim_trim/*` in the same repo
- https://github.com/defold/extension-texturepacker/blob/main/texturepacker/editor/src/texturepacker.clj — editor registration
- https://github.com/defold/extension-texturepacker/releases — version↔Defold pinning
- https://defold.com/extension-texturepacker/ (== `docs/index.md`) — user manual
- https://defold.com/manuals/atlas/ — native atlas manual
- https://defold.com/manuals/texture-profiles/ — texture profiles manual
- https://defold.com/manuals/bob/ — bob usage & download
- https://github.com/defold/defold — `engine/gamesys/proto/gamesys/atlas_ddf.proto`, `texture_set_ddf.proto`, `tile_ddf.proto`, `sprite_ddf.proto`; `com.dynamo.cr.bob/src/com/dynamo/bob/pipeline/AtlasBuilder.java`
- https://github.com/defold/examples — real `.atlas`, `.texture_profiles`, `game.project`, `.collection` files (`animation/flipbook`, `animation/animation_states`)
- https://defold.com/2024/11/19/Defold-1-9-5/ — atlas pivot-point introduction (1.9.5)
