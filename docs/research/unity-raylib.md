# Unity Sprite Atlas & raylib rTexPacker — Research Reference

Research date: 2026-07-10. Companion to `texturepacker.md`. Two very different reference points for our standalone texture/atlas packer (C engine-based, CLI + GUI): Unity's engine-integrated, asset-driven atlas pipeline, and rTexPacker's tiny immediate-mode (raygui) standalone tool.

## Summary

- **Unity SpriteAtlas** is not a standalone packer but an *asset* (`.spriteatlas` / `.spriteatlasv2`, plain Unity YAML) that declares *what* to pack (sprites, textures, whole folders) and *how* (rotation, tight packing, padding, per-platform compression). Packing happens automatically inside the editor/build pipeline — the user never presses "pack" except to preview. Key ideas worth stealing: folder-driven packables, master/variant atlases (scaled alternates from one packing result), secondary textures packed in parallel pages with guaranteed-identical layout, per-platform texture overrides stored with the atlas, and a late-binding runtime hook (`SpriteAtlasManager.atlasRequested`) for atlases excluded from the build.
- **rTexPacker** (raylib technologies, closed source, $19.95, ~1 MB single-file C executable built on raylib + raygui) proves that a serious packer fits in an immediate-mode single-window UI: top toolbar, central zoom/pan canvas, two toggleable panels (sprite list F5, atlas settings F6), overlay toggles for rects/names/origins/colliders, and an export dialog. It packs sprites *and* font glyphs (TTF/OTF, SDF), uses **stb_rect_pack** (Skyline BL/BF heuristics) plus a "Basic" row packer, and exports to text (`.rtpa`), binary (`.rtpb`), JSON, XML, C header, and PNG with the binary descriptor embedded as a custom `rTPb` PNG chunk. Full CLI included. The `.rtpa` text format is self-documenting (header comment describes every field) — full spec below from real files.
- **stb_rect_pack** is the ~600-line public-domain skyline packer underneath rTexPacker, raylib's font atlas, and Dear ImGui. Fine baseline; does not rotate, trim, sort by strategy, grow the bin, or multipack — all of that is the tool's job.

---

## Unity SpriteAtlas

### Feature set

**Objects for packing ("Packables").** The atlas asset holds a list of packables: individual Sprites, Textures, or **folders** (drag-and-drop onto the inspector or `+` button). A folder packable means "everything spriteable inside, recursively" — the artist workflow is "drop the folder once, forget it". (Sprite Atlas V2's experimental versions temporarily dropped folder support; full folder support returned in 2021.2 — user backlash over this confirms folders-as-input is a workflow people depend on.)

**Master vs Variant.** `Type: Master` is a normal atlas. `Type: Variant` references a Master atlas and has **no packables of its own** — it inherits the master's packed result and rescales the texture by a `Scale` factor (0.1–1.0, stored as `variantMultiplier`). Purpose: HD/SD alternates (e.g. 0.5 = half-res atlas for low-end devices) with *identical UV layout*, so sprites can be rebound to either at runtime. Caveat from Unity's own docs: if both master and variant are `Include in Build`, which one loads is effectively ambiguous — the supported pattern is to include only one, or include neither and late-bind.

**Include in Build vs late binding.** Checkbox per atlas. On (default): atlas auto-loads with the project and sprites resolve automatically. Off: sprites referencing the atlas render *blank* until code registers an atlas at runtime — the late-binding path (below).

**Packing settings** (master only):
- `Allow Rotation` — packer may rotate sprites for better fit. Docs warn to disable for UI/Canvas sprites (Canvas doesn't counter-rotate).
- `Tight Packing` — packs by sprite outline mesh instead of bounding rect; sprites can nest inside each other's transparent areas. Same warning: disable for UI; can cause neighboring-sprite bleed with tight meshes.
- `Alpha Dilation` — expands edge colors into adjacent transparent pixels (bleed guard for filtering/mipmaps).
- `Padding` — pixels between packed sprites, default 4 (UI offers powers of two: 2/4/8).
- (Serialized but not exposed: `blockOffset`, `allowAlphaSplitting` — the latter is ETC1 alpha-split for old Android.)

**Texture settings** (apply to the packed atlas texture, overriding the source textures' own import settings): Read/Write, Generate Mip Maps, sRGB, Filter Mode (Point/Bilinear/Trilinear), Aniso Level.

**Platform-specific overrides.** Per build target (Default, Standalone, iOS, Android, WebGL…): `Max Texture Size` (atlas is auto-downscaled to fit; default 2048, up to 8192), `Format`, `Compression` (None/Low/Normal/High), `Use Crunch Compression` + `Compressor Quality`. This is the whole "one atlas definition → N platform binaries" story and lives *in the atlas asset*, not in a global setting.

**Sprite pivots.** Not an atlas concern at all in Unity — pivot is a per-sprite property set in the Sprite Editor / TextureImporter. Packing preserves each sprite's geometry, pivot, and border (9-slice) data; the atlas only remaps where the pixels are sampled from (render data). Lesson: keep pivot metadata attached to the *sprite*, and make packing a pure layout transform that never touches it.

**Pack Preview.** Inspector button that packs on demand and shows the resulting page(s) in the preview pane, with a page selector and — when secondary textures exist — a dropdown to flip between the main page and each secondary page.

### Secondary textures (normal maps etc.) — layout parity

Unity's mechanism (2019.2+):

1. Each source sprite can declare **Secondary Textures** in its TextureImporter (Sprite Editor > Secondary Textures): a list of `(name, texture)` pairs, with conventional shader names like `_NormalMap`, `_MaskTex`.
2. When a SpriteAtlas packs sprites that carry secondary textures, it emits **one extra atlas page per distinct secondary-texture name**, blitting each sprite's secondary texture into *the same rectangle the packer chose for the main sprite*. Layout parity is guaranteed trivially: **there is only one packing pass**; the secondary pages reuse its placement result rather than being packed independently. (This is the same reasoning TexturePacker documents: shaders sample both textures with one set of UVs, so layouts must be byte-identical.)
3. Where a sprite has no texture for a given secondary name, that region in the secondary page is simply empty — hence Unity's documented advice: don't mix sprites with and without secondary textures in one atlas, and keep the *set* of secondary names uniform across the atlas, or you waste space.
4. Per-secondary settings exist (the `secondaryTextureSettings` map in the asset — e.g. sRGB off for normal maps, set via a "Secondary Settings" panel). The C# API for this is mostly internal (`SpriteAtlasExtensions.SetSecondaryColorSpace` is internal as of 2020.2), a long-standing community complaint — expose this properly in our tool.

Implementation takeaway for us: model secondary textures as *named channels per sprite* (`sprite.png` + `sprite_n.png`/explicit mapping), run the packer once on the main channel, then render every channel with the same placements; per-channel output settings (color space, compression) needed.

### How the asset stores settings — real YAML

Unity's `.spriteatlas` is standard Unity text-serialized YAML, class ID `687078895` (`SpriteAtlas`). Real example (from github.com/fabriziospadaro/SpriteSheetRenderer, `Assets/Atlas.spriteatlas`, trimmed):

```yaml
%YAML 1.1
%TAG !u! tag:unity3d.com,2011:
--- !u!687078895 &4343727234628468602
SpriteAtlas:
  m_ObjectHideFlags: 0
  m_Name: Atlas
  m_EditorData:
    serializedVersion: 2
    textureSettings:
      serializedVersion: 2
      anisoLevel: 1
      compressionQuality: 50
      maxTextureSize: 2048
      textureCompression: 0
      filterMode: 1
      generateMipMaps: 0
      readable: 0
      crunchedCompression: 0
      sRGB: 1
    platformSettings: []
    packingSettings:
      serializedVersion: 2
      padding: 4
      blockOffset: 1
      allowAlphaSplitting: 0
      enableRotation: 1
      enableTightPacking: 1
    secondaryTextureSettings: {}
    variantMultiplier: 1
    packables:
    - {fileID: 2800000, guid: 252e73463dc5649eca48720230253e81, type: 3}
    totalSpriteSurfaceArea: 921600
    bindAsDefault: 1
    isAtlasV2: 0
    cachedData: {fileID: 0}
  m_MasterAtlas: {fileID: 0}
  m_PackedSprites:
  - {fileID: -5192769841606136664, guid: 252e73463dc5649eca48720230253e81, type: 3}
  # ... one GUID/fileID reference per packed sprite ...
  m_PackedSpriteNamesToIndex: []   # (present in other files)
  m_Tag: Atlas                     # atlas name used by SpriteAtlasManager
  m_IsVariant: 0
```

Notes:
- **Inputs are references, not paths**: packables are `{fileID, guid}` asset references (a folder packable is just the folder asset's GUID). Sprite *names* don't appear in the source asset — resolution happens at pack time. `m_PackedSprites`/`m_RenderDataMap` (rects, offsets, UV transforms) are populated in the *built* atlas, cached in `Library/AtlasCache` for V1, never in the source file.
- Newer Unity adds `enableAlphaDilation` to `packingSettings`.
- `m_MasterAtlas: {fileID: 0}` + `m_IsVariant: 0` = master; a variant fills `m_MasterAtlas` with the master's reference and uses `variantMultiplier` as its Scale.
- Later editor versions also serialize `secondaryTextureSettings` as a name → settings map when secondary channels are configured.

### Sprite Atlas V2

Real `.spriteatlasv2` (class ID `612988286`, `SpriteAtlasAsset`) from github.com/darklinden/unity-atlas-release-test, trimmed — note `m_EditorData` became `m_ImporterData`, and a populated per-platform override block:

```yaml
--- !u!612988286 &1
SpriteAtlasAsset:
  m_Name:
  m_MasterAtlas: {fileID: 0}
  m_ImporterData:
    serializedVersion: 2
    textureSettings: { ... same fields as V1 ... }
    platformSettings:
    - serializedVersion: 3
      m_BuildTarget: DefaultTexturePlatform
      m_MaxTextureSize: 2048
      m_ResizeAlgorithm: 0
      m_TextureFormat: 4
      m_TextureCompression: 1
      m_CompressionQuality: 50
      m_CrunchedCompression: 0
      m_AllowsAlphaSplitting: 0
      m_Overridden: 1
      m_AndroidETC2FallbackOverride: 0
      m_ForceMaximumCompressionQuality_BC6H_BC7: 0
    packingSettings:
      serializedVersion: 2
      padding: 2
      blockOffset: 1
      allowAlphaSplitting: 0
      enableRotation: 1
      enableTightPacking: 0
      enableAlphaDilation: 0
    secondaryTextureSettings: {}
    variantMultiplier: 1
    packables:
    - {fileID: 21300000, guid: 611935c42bb8845b3bd7bcb90eee903d, type: 3}
    bindAsDefault: 1
    isAtlasV2: 1
  m_IsVariant: 0
```

What actually changed in V2 (default since Unity 2022.2):

| | V1 | V2 |
|---|---|---|
| Packing mechanism | Custom, outside the asset pipeline; results cached in `Library/AtlasCache` | A real **importer** (`SpriteAtlasImporter`) on AssetDatabase V2 |
| Cache server / Accelerator | Not supported | Supported (packed results shareable across a team — a big team-scale win) |
| Asset | `.spriteatlas`, settings inside the asset object | `.spriteatlasv2` (`SpriteAtlasAsset`), settings in `m_ImporterData`; edited via `SpriteAtlasAsset.Load/Save` + `SpriteAtlasImporter` API |
| Pack timing modes | Always packs | "Enabled" (packed in editor + play mode) or **"Enabled for Builds"** (editor uses loose source textures; packing only happens for Players/AssetBundles/Addressables — fast iteration, pack cost paid only at build) |
| Folder packables | Yes | Removed in experimental versions, fully restored in 2021.2 |
| Migration | — | One-way auto-conversion of V1 assets; cannot revert |

### Runtime API / late binding

- `SpriteAtlas` (runtime type): `GetSprite(name)` (clones a Sprite bound to the atlas texture), `GetSprites()`, `spriteCount`, `tag`.
- **Late binding**: with Include in Build off, any scene sprite that was packed into that atlas renders blank; at first need Unity fires `SpriteAtlasManager.atlasRequested(string tag, System.Action<SpriteAtlas> callback)`. User code loads the atlas from anywhere (AssetBundle, Addressables, variant choice by device class) and calls `callback(atlas)`; all pending sprite references bind. `SpriteAtlasManager.atlasRegistered` fires after. Canonical use: ship master *and* variant in bundles, register the half-res variant on low-end devices — same UVs, so nothing else changes.
- `SpriteAtlasManager.CreateSpriteAtlas` exists for fully runtime-built atlases.

### UX lessons from Unity

1. **Folder-driven packing is the workflow.** Sources are declared once (folders preferred); membership updates automatically as files come and go. Removing folder support in V2-experimental caused enough pain that Unity added it back.
2. **Pack-on-build, not pack-on-click.** The user edits settings; packing is an automatic consequence (with "Pack Preview" for inspection). V2's "Enabled for Builds" mode goes further: don't even pack during development. For our tool: cheap incremental repack + a build-system-friendly CLI is the equivalent.
3. **The atlas is the unit of platform configuration.** Max size/format/compression per platform live on the atlas asset, so one definition produces every platform's binary.
4. **Scaled variants derive from one packing result** — never re-pack per resolution, just rescale the page. Identical UVs are what make runtime swapping trivial.
5. **Secondary channels reuse the main placement** — single packing pass, N output pages.
6. **Settings the GUI can't script become complaints** (internal secondary-texture API). Everything in our project file must be settable via CLI/API.
7. **Text-serialized, VCS-diffable asset format** with stable field names and `serializedVersion` for migrations.

---

## rTexPacker

### What it is

Sprite **and font** atlas packer by raylib technologies (Ramon Santamaria / raysan5). Closed source; $19.95+ on itch.io and Steam (Windows x64, macOS x64+arm64, Linux x64) plus a free limited HTML5 web version. Single-file portable executable (~1 MB), no dependencies, "handmade" C. Built on **raylib + raygui (immediate-mode UI) + rpng + rini + tinyfiledialogs**. v1.0-alpha 2019 → v5.5 (Nov 2025). Directly relevant to us: it is the existence proof of a commercial-quality packer with an immediate-mode single-window GUI.

### Features

- **Packing**: two algorithm families — *Basic* (row/left-to-right) and *Skyline*, with **Bottom-Left** and **Best-Fit** heuristics. Uses **stb_rect_pack** for placement (the Skyline BL/BF pair is exactly stb_rect_pack's two heuristics; raylib's own `GenImageFontAtlas` uses the same library). Configurable **padding** and **spacing**; **alpha-trim** (global and per-sprite since v5.0); **manual positioning** of individual sprites on the canvas (Space to enter placement mode); atlas up to **16384×16384**; up to **4096 sprites** (v4.0+). No rotation support is advertised — consistent with stb_rect_pack's limitation.
- **Per-sprite metadata editing in the GUI**: origin/pivot (visualize + edit, preset-position selector), **collider** rect per sprite (edited in-tool, exported), **tag** string per sprite (v5.0), multi-selection (Ctrl+click, v4.0).
- **Slicing**: "auto-slice" / slice-by-alpha — cut an existing spritesheet into sprites inside the tool (also on CLI `--auto-slice`).
- **Font packing**: load TTF/OTF; font size; **SDF** generation; monospace; forced glyph height/width; custom Unicode charset from UTF-8 text files (TXT/MD/LIST) with automatic duplicate-codepoint removal; export of missing codepoints. Glyph metrics (offset, advance) go into the descriptor (see `.rtpa` spec).
- **Inputs**: PNG, QOI, TGA, JPG(, BMP via CLI); TTF/OTF; `.rtp` project files.
- **Project file**: `.rtp` — portable project containing full atlas data (image data compressed by default since v4.0), load/save of complete sessions. (Earlier versions used `.rtpp`; current docs say `.rtp`.) Tool config auto-persisted to `config.ini` (rini).
- **CLI**: shipped in the paid desktop version, "available by default" since v2.0; batch packing, slicing, tags, monospace, fonts. Usage:

```
rtexpacker [--help] --input <dir>,[file01.ext],[file02.ext]
           [--output <filename.ext>]
           [--atlas-desc <value>] [--atlas-image <value>] [--atlas-format <value>]
           [--atlas-size <value>]
           [--pack-algorithm <value>] [--pack-heuristic <value>] [--padding <value>]
           [--trim-alpha] [--auto-slice] [--debug]
           [--font-size <value>] [--font-charset <file.txt>]
           [--font-force-height] [--font-force-width] [--font-sdf]
```

  Note the shape: `--input` accepts comma-separated directories *or* individual files; algorithm/heuristic/size/padding are plain value flags mirroring the GUI settings panel 1:1.

### UI / UX (raygui immediate-mode, single window) — the part to study

Layout, reconstructed from the itch page, devlogs, and shortcut list:

- **Top main toolbar**: file operations (New Ctrl+N, Load Ctrl+O, Save Ctrl+S, Export Ctrl+E, Close Ctrl+X), visual-style selector (themes: default, Amber, Genesis, others), and view toggles.
- **Central canvas** = the atlas itself, always visible: mouse-wheel zoom, middle-button pan, left-click select, Ctrl+click multi-select, `F` fit-atlas-to-window, `D` fit-to-selected-sprite. Background color + checkerboard/fill options.
- **Overlay toggles** on the canvas (single keys): `U` sprite rectangles, `I` sprite name info, `O` origins, `P` colliders, `1/2` scale filters. Debug-style instant visualization instead of separate inspector views.
- **Two side panels, toggled by function keys** (floating raygui windows, not docks): **F5 sprites window** (scrollable list view with names — added v5.0; selection syncs with canvas) and **F6 atlas settings panel** (atlas size, packing algorithm + heuristic, padding/spacing, trim, background, plus the font section: size, SDF, monospace, charset).
- **Modal edit modes on the selection**: `G` edit origin (drag or pick from a 9-point preset selector), `M` edit collider, `Space` manual sprite positioning. Mode-based editing keeps the immediate-mode UI simple — one active tool at a time.
- **Export window** (Ctrl+E): choose descriptor format (.rtpa/.rtpb/.json/.xml/.h) + image format (.png/.qoi/.dds/.raw) + options (v5.5 added "prepend folder to nameId").
- **Welcome/about window** on first launch (v4.0).
- Native OS file dialogs via tinyfiledialogs (don't hand-roll file browsing in the immediate-mode UI).

UX verdict: everything is at most one keypress away; no menus-in-menus; the atlas preview *is* the workspace; settings panel maps 1:1 to CLI flags. Weaknesses to improve on: no rotation, floating panels can occlude the canvas, no incremental/watch mode, per-sprite overrides are limited (trim/origin/collider/tag only).

### Export formats & the `.rtpa` spec

Descriptor formats: **`.rtpa` (text)**, **`.rtpb` (binary)**, **JSON**, **XML**, **`.h` C header**. Image formats: **PNG, QOI, DDS, RAW**. Special: PNG export can **embed the binary descriptor as a custom `rTPb` PNG chunk** — a self-contained single-file atlas (rpng makes this easy). Sample loader code for `.rtpa`, `.rtpb`, PNG+rTPb chunk, and `.h` ships with the tool; community loaders exist (e.g. a Godot `.rtpb` importer).

**`.rtpa` v2.0 format** — real file (jasonliang-dev/spry, `examples/jump/atlas.rtpa`); the header comment *is* the spec:

```
#
# rTexPacker atlas descriptor file (v2.0)
#
# Number of packed sprites:     6
#
# Atlas info:    a <imagePath> <width> <height> <spriteCount> <isFont> <fontSize>
# Sprite info:   s <nameId> <originX> <originY> <positionX> <positionY> <sourceWidth> <sourceHeight> <padding> <trimmed> <trimRecX> <trimRecY> <trimRecWidth> <trimRecHeight>
#
a atlas.png 128 128 6 0 32
s bg 0 0 102 2 16 16 0 0 0 0 16 16
s platform 0 0 2 2 18 18 0 0 0 0 18 10
s spikes 0 0 42 2 18 18 0 0 0 9 18 9
s spring_down 0 0 62 2 18 18 0 0 0 8 18 10
```

Line grammar: comment lines start `#`; `a` = atlas record (`imagePath` relative, page `width height`, `spriteCount`, `isFont` 0/1, `fontSize`); `s` = one sprite record. `originX/originY` = pivot in sprite-local pixels; `positionX/positionY` = placement in the atlas; `sourceWidth/sourceHeight` = original (untrimmed) size; `padding`; `trimmed` 0/1 + `trimRec*` = the visible sub-rect within the source rect (so consumers can restore trimmed sprites at their original size — same model as TexturePacker's `Trim`).

**`.rtpa` v5.0 format** — real font-atlas file (manuelcabrerizo/ant, `data/fonts/atlas.rtpa`); v5.0 widened the sprite record with tag, collider, and font-glyph fields:

```
#
# rTexPacker atlas descriptor file (v5.0)
#
# Number of packed sprites:     95
#
# Atlas info:    a <imagePath> <width> <height> <spriteCount> <isFont> <fontSize>
# Sprite info:   s <nameId> <tag> <originX> <originY> <positionX> <positionY> <sourceWidth> <sourceHeight> <padding> <trimmed> <trimRecX> <trimRecY> <trimRecWidth> <trimRecHeight> <colliderType> <colliderX> <colliderY> <colliderSizeX> <colliderSizeY> <charValue> <charOffsetX> <charOffsetY> <charAdvanceX>
#
# NOTE: This atlas contains a sprite font description
#
a atlas.png 1024 1024 95 1 32
s ALGER-U0020 "" 0 0 0 0 7 32 0 0 0 0 7 32 0 0 0 0 0 32 0 0 7
s ALGER-U0021 "" 0 0 245 32 9 20 0 0 0 0 9 20 0 0 0 0 0 33 0 6 9
```

Added fields: `tag` (quoted string), `colliderType colliderX colliderY colliderSizeX colliderSizeY`, and for fonts `charValue` (Unicode codepoint), `charOffsetX/charOffsetY` (glyph draw offset), `charAdvanceX`. Font glyphs are just sprites named `<FontName>-U<codepoint hex>` with the char fields filled — **fonts and sprites share one record type and one pipeline.** `.rtpb` is the binary mirror of the same records (also what the `rTPb` PNG chunk contains). v5.5 additionally supports folder-relative `nameId`s ("prepend folder to nameId" export option) for namespacing.

### Lessons for an immediate-mode packer UI

1. **Single window: toolbar + canvas + two toggleable panels is enough.** Don't build docking. Canvas is the hero; panels (sprite list, settings) toggle with F-keys and float over it.
2. **Single-key overlay toggles** (rects, names, origins, colliders) give inspection without any inspector UI.
3. **Explicit modal edit states** (`G` origin, `M` collider, `Space` move) fit immediate-mode perfectly: one global `currentMode` enum, canvas input interpreted per mode.
4. **Settings panel = CLI flags = project file fields**, 1:1. rTexPacker's GUI settings map directly onto `--pack-algorithm`, `--padding`, `--font-size`… Ours should too.
5. **Self-documenting text descriptor**: put the format spec in a header comment of every exported file; single-char record types (`a`/`s`) make parsers ~20 lines of C.
6. **Binary descriptor embedded in a PNG chunk** = zero-extra-file distribution; worth copying (we control the engine loader).
7. **Fonts are sprites.** One record schema with optional glyph fields covers both; SDF + charset-from-text-file are cheap, high-value features.
8. **Ship loader snippets** for every export format; that, not the exporter, is what makes a format "supported".
9. **Persist tool state in a plain ini** (`config.ini`), auto-saved; native file dialogs via tinyfiledialogs.
10. Gaps we can beat: no rotation, no MaxRects, no multipack/multiple pages, no folder watching/incremental repack, no secondary-texture channels, no scaling variants.

---

## stb_rect_pack note

`stb_rect_pack.h` (Sean Barrett, single header, MIT/public domain, ~600 LOC) implements **Skyline Bottom-Left** packing: it tracks only the top silhouette of placed rects (a 1-D height map of "nodes") and places each rect at the candidate position with the lowest y (BL, default) or least wasted area (`STBRP_HEURISTIC_Skyline_BF_sortHeight`, ~2× slower, usually tighter). API is two calls: `stbrp_init_target(ctx, w, h, nodes, num_nodes)` (caller supplies a node array, ideally ≥ width — no allocations) and `stbrp_pack_rects(ctx, rects, n)`, which writes `x,y,was_packed` per rect and returns 1 only if everything fit. It sorts input by height internally (via `qsort`) and restores order. What it deliberately does **not** do — and what any tool built on it must add on top: **no rotation, no bin growth or minimal-size search (fixed target; caller must retry with other sizes), no multiple pages, no trimming/extrusion/padding semantics (caller inflates rects and offsets results), no polygon/tight packing, and "only had a few tests run" per its own header.** Its ubiquity (raylib fonts, Dear ImGui font atlas, rTexPacker, countless engines) makes it the de-facto baseline: matching stb_rect_pack quality is table stakes; MaxRects + rotation + auto-size search is how tools differentiate (cf. TexturePacker).

---

## Lessons for our tool

1. **Inputs = folders + files with live membership** (Unity packables, TexturePacker smart folders, rTexPacker CLI `--input dir`). Store references/paths in the project file; resolve at pack time.
2. **Pack automatically, preview on demand.** Unity users never "run the packer"; rTexPacker's CLI makes it a build step. We need: fast repack in the GUI + deterministic CLI consuming the same project file.
3. **One packing pass, N outputs**: secondary channels (normal/emission) and scaled variants (@1x/@0.5x) must both derive from a single placement result — this is how Unity guarantees layout parity and cheap HD/SD swaps. Design the packer core to return a placement list that renderers then apply per channel/scale.
4. **Per-platform/per-target output profiles live in the project file** (Unity `platformSettings`): max size, pixel format, compression per profile.
5. **Pivot/collider/tag are per-sprite metadata, orthogonal to packing** (Unity pivots survive packing untouched; rTexPacker edits + exports them). Keep a per-sprite override table in the project; packing never mutates it.
6. **Descriptor format strategy** (superset of rTexPacker's): text with self-documenting header (diff-able), binary mirror, C header for zero-parse embedding, JSON for tooling, and optionally the descriptor embedded in a PNG chunk. Record fields to cover, per real `.rtpa` v5.0: name, tag, origin, atlas position, source size, trim rect + trimmed flag, padding, collider, optional glyph data (codepoint, offset, advance) — fonts as sprites.
7. **Immediate-mode GUI blueprint validated by rTexPacker**: toolbar / canvas / F-key panels / single-key overlays / modal edit states / native file dialogs / ini-persisted config. No docking, no retained widgets needed.
8. **Runtime side**: late binding (Unity `atlasRequested`) is an engine feature, not the packer's — but the packer enables it by emitting stable atlas names/tags and identical UVs across variants.
9. **stb_rect_pack is an acceptable v1 core** (deterministic, tiny, zero-alloc) if we wrap it with: size search (retry POT/stepped sizes), padding/extrude inflation, optional rotation via a different core (MaxRects) later, and multipack by re-running on leftovers (`was_packed == 0`).
10. **Anti-lessons**: Unity V2's one-way migration and internal-only APIs; rTexPacker's floating panels occluding the canvas and missing rotation/multipack.

---

## Sources

- Unity Manual — Sprite Atlas inspector reference: https://docs.unity3d.com/6000.3/Documentation/Manual/sprite/atlas/sprite-atlas-reference.html
- Unity Manual — Create a sprite atlas (packables, secondary textures preview): https://docs.unity3d.com/6000.3/Documentation/Manual/sprite/atlas/create-sprite-atlas.html
- Unity Manual — Sprite Atlas V2: https://docs.unity3d.com/2023.2/Documentation/Manual/SpriteAtlasV2.html and https://docs.unity3d.com/6000.2/Documentation/Manual/sprite/atlas/v2/enable-sprite-atlas-v2.html
- Unity Manual — Variant sprite atlases / scale: https://docs.unity3d.com/6000.3/Documentation/Manual/sprite/atlas/master-variant/master-variant-landing.html, https://docs.unity3d.com/6000.1/Documentation/Manual/sprite/atlas/master-variant/scale-textures-variant-sprite-atlas.html
- Unity Manual — Late binding: https://docs.unity3d.com/6000.1/Documentation/Manual/sprite/atlas/distribution/late-binding.html
- Unity Scripting — SpriteAtlasManager: https://docs.unity3d.com/ScriptReference/U2D.SpriteAtlasManager.html
- Unity Manual (URP) — Sprite secondary textures (`_NormalMap`, `_MaskTex`): https://docs.unity3d.com/6000.3/Documentation/Manual/urp/SecondaryTextures.html
- Unity Discussions — Sprite Atlas V2 folder support: https://discussions.unity.com/t/spriteatlas-v2-removes-support-for-folders/801432; secondary-texture C# API: https://discussions.unity.com/t/how-to-add-secondary-textures-settings-to-sprite-atlas-through-c/854657
- Real V1 `.spriteatlas`: https://github.com/fabriziospadaro/SpriteSheetRenderer (Assets/Atlas.spriteatlas); real V2 `.spriteatlasv2`: https://github.com/darklinden/unity-atlas-release-test (Assets/Sprites/20.spriteatlasv2)
- TexturePacker docs on normal-map layout parity (cross-reference): https://www.codeandweb.com/texturepacker/tutorials/packing-normal-map-sprite-sheets
- rTexPacker — itch.io product page (features, shortcuts, formats, versions): https://raylibtech.itch.io/rtexpacker; Steam page: https://store.steampowered.com/app/2377030/rTexPacker/; site: https://www.raylibtech.com/rtexpacker/
- rTexPacker devlogs — v2.0: https://raylibtech.itch.io/rtexpacker/devlog/341527/rtexpacker-v20-published, v4.0: https://raylibtech.itch.io/rtexpacker/devlog/769790/rtexpacker-v40-published, v5.0: https://raylibtech.itch.io/rtexpacker/devlog/933703/rtexpacker-v50-published, v5.5: https://raylibtech.itch.io/rtexpacker/devlog/1119201/rtexpacker-v55-published
- Real `.rtpa` files — v2.0: https://github.com/jasonliang-dev/spry (examples/jump/atlas.rtpa); v5.0 font: https://github.com/manuelcabrerizo/ant (data/fonts/atlas.rtpa)
- Godot `.rtpb` importer (community): https://github.com/alfredbaudisch/godot_rtex_binary_spritesheet_importer
- stb_rect_pack.h source: https://github.com/nothings/stb/blob/master/stb_rect_pack.h
- Rect-packing algorithm comparison (skyline vs MaxRects context): https://www.david-colson.com/2020/03/10/exploring-rect-packing.html
