# CodeAndWeb TexturePacker — Research Reference

> **Status: non-normative point-in-time research.** Use as format evidence and
> fixture input; current product decisions come from
> [`../ntpacker-master-spec.md`](../ntpacker-master-spec.md).

Research date: 2026-07-10. Primary reference for our standalone texture/atlas packer (C engine-based, CLI + GUI).

## Summary

TexturePacker is the industry-standard commercial sprite-sheet/atlas packer (Windows/macOS/Linux, Qt-based GUI + CLI in one binary). Its core design decisions, all worth copying:

1. **One binary, two faces.** The same `TexturePacker` executable is the GUI and the CLI. The GUI is a settings editor for a project file (`.tps`); the CLI consumes that same `.tps` for automation. Every GUI setting has a CLI flag, and CLI flags can override or even rewrite (`--save`) the project file.
2. **The project file is the source of truth.** `.tps` is a plain XML "GenericDictionary" storing all settings, the source file/folder list, and per-sprite overrides. Artists tweak in the GUI, CI runs `TexturePacker project.tps`.
3. **Live-linked "smart folders".** You drop folders, not files. TexturePacker watches them; adding/removing/renaming sprites in the file system updates the sheet automatically. Sub-folder names become part of sprite names.
4. **Smart publish / incremental builds.** Re-publish only happens when sprites, settings, or the TexturePacker version changed (tracked via a `smartUpdateKey` hash embedded in outputs). Makes it cheap to call from build scripts on every build.
5. **Exporters are data, not code.** A new engine format = a folder with `exporter.xml` (descriptor + capability flags) + Grantlee/Django-style text templates + optional JS filters. Users add formats without touching the app; 45+ formats ship built in, and e.g. Defold's official support is literally a custom exporter folder maintained by the Defold team.
6. **Deep feature set around the packer core**: MaxRects/Polygon/Grid/Basic algorithms with heuristics, trim/crop with polygon tracing, 90° rotation, extrude, alias (duplicate) detection, multipack, scaling variants (@2x/@1x cooked from one source), pivot-point and 9-patch editors, normal-map co-packing with identical layout, animation detection, hardware texture compression (PVRTC/ETC/ASTC/Basis/DXT), content protection.

---

## Features

### Packing algorithms

| Algorithm | Description | Options |
|---|---|---|
| **MaxRects** | "Currently the best packing algorithm for rectangles." Default. | Heuristics: `Best` (tries all, picks most efficient), `ShortSideFit`, `LongSideFit`, `AreaFit`, `BottomLeft`, `ContactPoint` |
| **Polygon** | Tight packing of polygonal sprite outlines (requires engine mesh support: vertices + triangle indices in the data file). Used with `Polygon` trim mode. | Tracer tolerance controls outline precision (smaller = tighter fit, more vertices, slower) |
| **Grid** | Regular cells; largest sprite determines cell size. For classic fixed-grid sheets / `spritesheet`-style consumption. | — |
| **Basic** | Fills left-to-right until max width. Simple, predictable, for fixed-size sprites. | Sort by: `Best`, `Name`, `Width`, `Height`, `Area`, `Circumference`; order Ascending/Descending |

**Pack mode** (`--pack-mode`): `Fast` (POT-minimum search), `Good` (balanced with timeout, default), `Best` (exhaustive, slow). Controls how hard the packer searches for the minimal sheet size, independent of the placement heuristic.

### Trim / crop

| Mode | Effect |
|---|---|
| `None` | Keep original dimensions |
| `Trim` | Remove surrounding transparency; sprite *reports* its original size + offset (engine restores position). Anchor unchanged. |
| `CropKeepPos` | Remove transparency; smaller visible size but position reference kept |
| `Crop` | Remove transparency; sprite becomes genuinely smaller, position resets to 0/0 |
| `Polygon` | Trace sprite outline as a polygon path (for polygon packing / tight meshes) |

Related knobs: **Trim margin** (keep N transparent pixels after trimming), **Transparency threshold** (`--trim-threshold`, alpha 1–255 below which a pixel counts as transparent), **Tracer tolerance** (polygon fit precision).

### Layout / geometry

| Feature | Flags | Notes |
|---|---|---|
| Rotation | `--enable-rotation` / `--disable-rotation` | 90° CW/CCW when it fits better; requires format support (exporter capability flag) |
| Extrude | `--extrude <n>` | Repeats border pixels outward; sprite size unchanged in data. Fixes tile-edge flickering/bleeding |
| Border padding | `--border-padding <n>` | Transparent margin around whole sheet |
| Shape padding | `--shape-padding <n>` | Space between sprites; ≥2 px recommended against GPU bleeding |
| Common divisor | `--common-divisor-x/y <n>` | Extends sprite sizes to be divisible by N (uniform sizing / consistent scaling) |
| Align to grid | `--align-to-grid <n>` | Sprite corners land on coordinates divisible by N |
| Max size | `--max-width/--max-height/--max-size` | Default 2048×2048 |
| Fixed size | `--width/--height` | Explicit output dimensions |
| Size constraints | `--size-constraints` | `POT`, `MultipleOf4`, `WordAligned` (16-bit row alignment), `AnySize` |
| Force squared | `--force-squared` | width == height |

### Multipack

- **Automatic** (`--multipack`): auto-split into multiple textures + data files when sprites don't fit one sheet. Output names must contain `{n}` / `{n0}` / `{n1}` placeholder (sheet index, 0- or 1-based), e.g. `sheet-{n1}.png`.
- **Manual**: create pages in the GUI and drag sprites between them — art director controls page assignment.
- Combinable with scaling variants: `sheet{n1}-{v}.png`.

### Scaling variants (multi-resolution cooking)

One source set → several output resolutions in one publish. Per variant: **name** (replaces `{v}` in output filenames, e.g. `@2x`/`@1x`/`hd`/`sd`), **scale factor**, **sprite filter** (apply only to matching sprites), **per-variant max texture size**. `--force-identical-layout` places sprites identically on all variants (only overall scale differs) — required when data files must match across resolutions. "Accept fractional values" allows sub-pixel source sizes when no common divisor exists. Scale algorithms for upscaling: Smooth, Fast, Scale2x/3x/4x, Eagle, Hq2x. CLI: `--variant <scale:name[:filter]>` (repeatable), `--scale <f>`.

### De-duplication / naming

- **Alias detection** (on by default, `--disable-auto-alias` to turn off): byte-identical sprites are packed once; all names become data entries pointing at the shared rect. Big win for animations with duplicate frames.
- **Animation detection / trim sprite names** (`trimSpriteNames`): sprites whose names differ only in a numeric suffix (`walk-01`, `walk-02`, …) are grouped as an animation; some exporters strip the suffix and/or emit animation lists (Phaser, Godot AnimatedSprite frames). Disable in Advanced settings.
- **Name mangling** (hidden features via .tps editing): `ignoreFileList` (Unix wildcards like `*.psd`, `*/test/*` to exclude files) and `replaceList` (Perl regex `REGEXP=STRING` pairs to rewrite sprite names, e.g. `.*/=` strips directory prefixes).
- **Heuristic mask** (hidden, `.tps` key `heuristicMask`): converts a solid background color to transparency for legacy assets.

### Per-sprite metadata

- **Pivot/anchor points**: global default (Center, corners, custom) + per-sprite override via GUI editor; exact pixel coordinates in absolute or relative (0..1) terms. Stored in `.tps` under `globalSpriteSettings` / `individualSpriteSettings`; exported when the format supports it (`supportsPivotPoint` exporter flag).
- **9-patch (scale9)**: per-sprite 3×3 zone editor — center zone stretches, borders fixed. Stored as `scale9Enabled` + `scale9Borders`; exported when `supportsScale9`.

### Secondary textures (normal maps etc.)

- Checkbox "Normal maps / Pack with same layout": normal maps are detected (blue names + "N" badge in the sprite list; auto-detect by folder/filename convention — `sprites/` vs `normals/` folders, or `_n` suffix — with manual override) and packed into a **second sheet with the identical layout**, same data format, filename suffix `_n`, published alongside the main sheet.
- The GUI supports only **one** secondary type. More types (AO, emission, …) are done via repeated CLI runs against the same `.tps`: `--normalmap-filter <substring>` (identifies the variant files by path), `--normalmap-sheet <file>` (output name), `--background-color RRGGBB` for empty areas. Layout is guaranteed identical across all sheets.

### Output / encoding

- **Texture formats**: PNG, PNG-8, JPG, BMP, TGA, TIFF, WebP, PVR3 (+gz/ccz), PKM, KTX, KTX2/zKTX, ASTC, Basis, DDS, ATF.
- **Pixel formats**: RGBA8888/4444/5551, RGB888/565, ALPHA, ALPHA_INTENSITY; compressed PVRTC 2/4bpp, ETC1/ETC2 (RGB/RGBA), DXT1/DXT5, ASTC 4×4…12×12, Basis Universal (ETC1S/UASTC).
- **Dithering** (`--dither-type`): NearestNeighbour, Linear, FloydSteinberg(+Alpha), Atkinson(+Alpha), PngQuantLow/Medium/High.
- **Alpha handling** (`--alpha-handling`): KeepTransparentPixels, ClearTransparentPixels (better compression), ReduceBorderArtifacts (alpha bleeding: transparent pixels get nearest solid color), PremultiplyAlpha.
- Quality knobs: `--png-opt-level 0–7` (PngQuant/Zopfli), `--jpg-quality 0–100`, `--webp-quality` (>100 = lossless), per-codec quality for PVRTC/ETC/ASTC/Basis.
- **Content protection** (`--content-protection <key>`): encrypts cocos2d PVR.ccz sheets with a stored/reusable key (deters asset ripping).
- `--flip-pvr` (vertical flip for Unity et al.), `--shape-debug` (draw sprite outlines for debugging), `--convert-texture in.png out.astc` (standalone image conversion mode).

### Auxiliary tools in the app

- **Sprite sheet cutter/splitter**: decompose an existing sheet back into sprites (grid-based) for re-packing.
- **Image conversion**: use the codec pipeline without packing.
- **Animation preview**: floating window playing selected sprites with speed/loop controls.

---

## UI/UX walkthrough

Classic three-panel layout:

```
+------------------------------------------------------------------+
| Toolbar: Add sprites | Add smart folder | Publish | Sprite settings ...|
+---------------+----------------------------------+---------------+
| Sprites panel | Atlas preview (center)           | Settings panel|
| (left, tree)  | - live repack on every change    | (right)       |
| - smart       | - page tabs when multipack       | - exporter/   |
|   folders     | - zoom controls at bottom (1:1,  |   data format |
| - blue "N"    |   steps, continuous)             | - texture     |
|   normal-map  | - selected sprite highlight      | - layout/algo |
|   badges      | - debug outlines toggle          | - Basic vs    |
+---------------+----------------------------------+   Advanced    |
| Status / warnings / errors                       |   sections    |
+------------------------------------------------------------------+
```

Key workflow facts:

- **Everything is live.** Drop files/folders on the left panel → sheet re-packs immediately in the center; any settings change re-packs too. Instant visual feedback is the core UX contract.
- **Smart folders**: dropping a folder creates a live link — "When a change is made to the folder, TexturePacker will automatically update the sheet… adding, removing or renaming sprites." Sub-folder names become sprite-name prefixes (`enemies/tank/walk-01`). This is the recommended workflow, individual file adds are the exception.
- **Input formats**: PNG, JPG, WebP, **PSD**, animated GIF (frames extracted), SWF etc.
- **Settings panel** is split into *Basic* and *Advanced* (progressive disclosure); the exporter/data-format dropdown is at the top and gates which settings are enabled (e.g. rotation greys out if the chosen exporter's `supportsRotation` is false).
- **Publish button** on the toolbar writes sheet + data files. Output filenames support placeholders `{n}`/`{n0}`/`{n1}` (multipack page) and `{v}` (variant name).
- **Sprite settings** (toolbar button) opens the per-sprite editors: **pivot point editor** (drag a marker on the sprite; enter exact pixel coordinates, absolute or relative) and **9-patch editor** (drag guide lines splitting the sprite into 3×3 zones).
- **Animation preview** is a floating window with playback-speed and loop controls.
- **Multipack navigation**: pages shown as tabs above the preview; in manual mode sprites can be dragged between pages.
- **Warnings/errors** appear non-modally (banner/status area): e.g. "sprites don't fit", rotation unsupported by exporter, missing `{n}` placeholder with multipack enabled. Publish is blocked on hard errors.
- Zoom: continuous plus predefined steps; `1:1` = 100%.

---

## CLI design

Same binary as the GUI (`--gui` even launches the GUI from a CLI invocation). Installed into PATH via a menu item ("Install Command Line Tool") on Win/macOS; automatic on Linux .deb.

### Invocation patterns

```bash
# 1. Everything on the command line (no project file)
TexturePacker --format phaser --sheet out.png --data out.json spritefolder
# input folders are scanned recursively

# 2. Project-file driven (the canonical CI workflow)
TexturePacker sheet.tps
TexturePacker sheet1.tps sheet2.tps sheet3.tps   # batch

# 3. Project file + overrides (CLI args win over .tps values)
TexturePacker configuration.tps --sheet mysheet.png --data mysheet.json sprites

# 4. Rewrite the project file without opening the GUI
TexturePacker project.tps --padding 10 --save project.tps

# 5. Standalone image conversion
TexturePacker --convert-texture input.png output.astc --texture-format astc
```

### Key options (grouped; every GUI setting has one)

- **Output**: `--sheet <file>`, `--data <file>`, `--format <exporter-name>`, `--texture-format <id>`; some exporters add extra outputs (`--class-file`, `--header-file`, `--source-file` for code-generating formats).
- **Algorithm**: `--algorithm MaxRects|Polygon|Grid|Basic`, `--maxrects-heuristics <h>`, `--basic-sort-by <k>`, `--pack-mode Fast|Good|Best`, `--multipack`.
- **Geometry**: `--width/--height`, `--max-width/--max-height/--max-size`, `--size-constraints POT|WordAligned|AnySize`, `--force-squared`, `--enable-rotation/--disable-rotation`, `--shape-padding`, `--border-padding`, `--extrude`, `--common-divisor-x/y`, `--align-to-grid`.
- **Trim**: `--trim-mode`, `--trim-threshold`, `--trim-margin`, `--tracer-tolerance`.
- **Variants**: `--variant <scale:name[:filter]>`, `--force-identical-layout`, `--scale`.
- **Pixels**: `--opt <pixelformat>`, `--dither-type`, `--alpha-handling`, `--background-color RRGGBB`, `--jpg-quality`, `--png-opt-level`, `--webp-quality`, `--content-protection`, `--flip-pvr`.
- **Secondary sheets**: `--normalmap-filter`, `--normalmap-sheet`.
- **Misc**: `--help`, `--version`, `--gui`, `--verbose`, `--quiet`, `--shape-debug`, `--force-publish`, `--license-info`, `--activate-license <key>`.

### Automation behavior

- **Smart rebuild**: republishes only if sprites changed (add/remove/modify), settings changed, or the TexturePacker version changed; `--force-publish` overrides. Implemented via a `smartUpdateKey` hash written into output data files — so it works even without extra state files. Fast no-op makes per-build invocation viable.
- **Exit codes**: 0 on success, non-zero on failure (missing sprites, sprites don't fit without multipack, license problems) — standard make/CI contract. Errors go to stderr; `--quiet` for logs.
- **Line endings**: platform-native (LF on macOS/Linux, CRLF on Windows) — a documented gotcha for VCS in mixed teams.
- **Docker/CI licensing**: no official image; a Dockerfile template (Ubuntu 24.04) is provided. EULA accepted headlessly via `RUN echo agree | TexturePacker --version`. License via env var `TP_FLOATING_LICENSE=TP-xxxx-...` or `--activate-license`; CI licenses are floating seats that auto-release after ~2 minutes if the process dies. (For our free tool: no licensing needed — one less CI pain point, worth advertising.)

---

## Project file format (`.tps`)

A `.tps` file is a plain **XML "GenericDictionary"** — a typed key/value serialization of the whole settings object (Qt-style). Real example (from CodeAndWeb's TexturePacker-Phaser repo, `assets/cityscene.tps`):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<data version="1.0">
    <struct type="Settings">
        <key>fileFormatVersion</key>
        <int>3</int>
        <key>texturePackerVersion</key>
        <string>4.0.0</string>
        <key>fileName</key>
        <string>/Users/sk/Programming/TexturePacker-Phaser/assets/cityscene.tps</string>
        <key>autoSDSettings</key>
        <array>
            <struct type="AutoSDSettings">
                <key>scale</key>
                <double>1</double>
                <!-- variant name, filter, maxTextureSize ... -->
            </struct>
        </array>
        <key>dataFormat</key>
        <string>phaser-json-hash</string>
        <key>textureFormat</key>
        <enum type="SettingsBase::TextureFormat">png</enum>
        <key>maxTextureSize</key>
        <QSize>
            <key>width</key><int>2048</int>
            <key>height</key><int>2048</int>
        </QSize>
        <key>algorithmSettings</key>
        <struct type="AlgorithmSettings">
            <!-- algorithm, maxRects heuristic, basic sort, ... -->
        </struct>
        <key>globalSpriteSettings</key>
        <struct type="SpriteSettings">
            <key>scale</key>
            <double>1</double>
            <key>pivotPoint</key>
            <enum type="SpriteSettings::PivotPoint">Center</enum>
            <key>trimMode</key>
            <enum type="SpriteSettings::TrimMode">Trim</enum>
        </struct>
        <key>fileList</key>
        <array>
            <filename>cityscene</filename>   <!-- a smart folder, relative path -->
        </array>
        ...
    </struct>
</data>
```

Value encoding: `<key>name</key>` followed by a typed value node — `<string>`, `<int>`, `<uint>`, `<double>`, `<true/>`/`<false/>`, `<enum type="...">`, `<QSize>`, `<point_f>`, `<array>`, `<struct type="...">`, `<map>`, and `<filename>` (a string that is **resolved relative to the .tps location** — this is what makes projects portable across machines/VCS).

What it stores (observed keys):

| Area | Keys |
|---|---|
| Meta | `fileFormatVersion`, `texturePackerVersion`, `fileName` |
| Exporter | `dataFormat` (exporter `name`), `dataFileNames` (map: logical file → output path with placeholders), exporter-specific structs (`andEngine`, `libGdx` filtering, …) |
| Texture | `textureFormat`, `pngOptimizationLevel`, `jpgQuality`, `webpQualityLevel`, `premultiplyAlpha`, `backgroundColor`, `ditherType`, pixel format |
| Layout | `algorithmSettings` (algorithm + heuristics + sorting), `maxTextureSize` (QSize), `sizeConstraints`, `forceSquared`, `allowRotation`, `borderPadding`, `shapePadding`, `extrude`, `commonDivisor`, `multiPack` |
| Variants | `autoSDSettings` array of `AutoSDSettings` structs (scale, extension/`{v}` name, filter, maxTextureSize, forceIdenticalLayout) |
| Sprites | `fileList` array of `<filename>` entries (files and smart folders, relative paths), `globalSpriteSettings` (default scale/pivot/trimMode/trim threshold), `individualSpriteSettings` (per-sprite pivot/9-patch/trim overrides keyed by sprite name) |
| Behavior | `autoAliasEnabled`, `trimSpriteNames` (animation detection), `cleanTransparentPixels`, `ignoreFileList`, `replaceList`, `heuristicMask` |

Notes:

- Some "hidden" settings (`ignoreFileList`, `replaceList`, `heuristicMask`) have **no GUI** — users hand-edit the XML and reopen the project. Evidence that a human-editable project format is a real feature, not an implementation detail.
- The GUI-written `fileName` absolute path is informational; portability comes from `fileList` etc. being relative.
- Format is versioned twice: `fileFormatVersion` (schema) and `texturePackerVersion` (writer) — the latter also feeds smart-rebuild invalidation.

---

## Custom exporter system

The killer extensibility feature. An exporter = **a folder** containing a descriptor + templates, discovered from a user-configurable exporter directory (Preferences; env var `TEXTUREPACKER_EXPORTER_DIR`; built-ins live in `<install>/resources/exporters`). No code, no recompilation, no plugin ABI.

### exporter.xml — descriptor

```xml
<exporter version="1.0">
    <name>testexporter</name>                 <!-- unique id: used by --format and .tps dataFormat -->
    <displayName>TestExporter</displayName>   <!-- shown in the GUI dropdown -->
    <description>Custom exporter description</description>
    <version>1.0</version>

    <files>                                   <!-- one or more output files -->
        <file>
            <name>mydata</name>               <!-- logical name, keys dataFileNames in .tps -->
            <displayName>MyDataFile</displayName>
            <fileExtension>txt</fileExtension>
            <template>testexporter.txt</template>
            <optional>false</optional>
        </file>
    </files>

    <!-- capability flags: gate GUI settings + packer behavior -->
    <supportsTrimming>true</supportsTrimming>
    <supportsRotation>true</supportsRotation>
    <supportsNPOT>true</supportsNPOT>
    <supportsPolygonPacking>false</supportsPolygonPacking>
    <supportsPivotPoint>true</supportsPivotPoint>
    <supportsScale9>false</supportsScale9>
    <!-- also: yAxisDirection, transferMaskToPath, multipack support, etc. -->

    <properties>                              <!-- optional custom GUI settings -->
        <property>
            <name>sprite-prefix</name>
            <type>string</type>               <!-- string | bool | enum -->
            <default>img_</default>
            <displayName>Sprite Prefix</displayName>
        </property>
    </properties>
</exporter>
```

Design insight: the **capability flags flow backwards into the app** — if `supportsRotation` is false the packer won't rotate and the GUI disables the checkbox; if `supportsTrimming` is false trim is disabled. The exporter declares the target engine's constraints and the whole pipeline respects them.

### Template language (Grantlee — Django-syntax)

Three token types: `{{ variable }}` / `{{ obj.prop }}` / `{{ list.0 }}`; filters `{{ name|upper }}`, `{{ v|floatformat:3 }}`, `{{ list|join:", " }}`; tags `{% if %}…{% endif %}`, `{% for x in list %}…{% endfor %}` (with `forloop.last` etc.), `{% load customfilter %}`.

### Data model exposed to templates

Root context:

| Variable | Type | Meaning |
|---|---|---|
| `texture` | TEXTURE | current sheet (one render per multipack page × variant) |
| `allSprites` | SPRITE[] | all sprites **including aliases** (duplicates as name-entries) |
| `sprites` | SPRITE[] | unique sprites only |
| `settings` | SETTINGS | project settings snapshot |
| `variantIndex` | INT | index of current scaling variant |
| `smartUpdateKey` | STRING | hash to embed in output for incremental rebuild detection |
| `exporterProperties.<NAME>` | STRING/BOOL | custom property values from the descriptor |

SPRITE:

```
trimmedName       STRING     name without numeric animation suffix
fullName          STRING     full sprite name (with folder prefix)
frameRect         RECT       x,y,w,h on the sheet (post-trim, post-rotation)
sourceRect        RECT       region of the original image that survived trim
cornerOffset      POINT      offset of trimmed rect inside original image
untrimmedSize     SIZE       original sprite dimensions
trimmed           BOOL
rotated           BOOL       90° rotated on the sheet
pivotPoint        POINT      (also normalized variant)
vertices          POINT[]    polygon outline (polygon mode)
verticesUV        POINT[]    UV coords of vertices
triangleIndices   INT[]      triangulation of the polygon
scale9Enabled     BOOL
scale9Borders     RECT
```

TEXTURE: `size (SIZE)`, `fullName`, `trimmedName`, `absoluteFileName`, `allSprites`, `area`, plus normal-map counterpart names.

### Example template

```
// Created with TexturePacker ({{ texture.fullName }})
sprites = [
{% for sprite in allSprites %}
    {{ sprite.trimmedName }} = [
        frame  = [{{ sprite.frameRect.x }}, {{ sprite.frameRect.y }},
                  {{ sprite.frameRect.width }}, {{ sprite.frameRect.height }}],
        rotated = {{ sprite.rotated }},
        source = [{{ sprite.cornerOffset.x }}, {{ sprite.cornerOffset.y }},
                  {{ sprite.untrimmedSize.width }}, {{ sprite.untrimmedSize.height }}]
    ]{% if not forloop.last %},{% endif %}
{% endfor %}
]
// {{ smartUpdateKey }}
```

### Escape hatch: JavaScript filters

Custom text transforms as `.qs` files in `exporter/grantlee/0.2/`:

```javascript
var MakeSelectorFilter = function(input) {
    return input.rawString().replace("-hover", ":hover");
};
MakeSelectorFilter.filterName = "makecssselector";
Library.addFilter("MakeSelectorFilter");
```

Then `{% load makecssselector %}` … `{{ value|makecssselector }}`. Covers the "template language isn't quite expressive enough" cases without opening up full plugin code execution.

### Real-world proof

Defold's official TexturePacker support (defold/extension-texturepacker) is exactly this: a `defoldexporter` folder users point TexturePacker at, which emits a `.tpinfo` file consumed by a Defold engine extension as `.tpatlas`. Third parties routinely ship exporters this way.

---

## Export formats list

~45–50 built-in exporters. Names as they appear on codeandweb.com:

**Generic / most used**: JSON (hash) — the de-facto interchange format, JSON (array), Generic XML, plain text, **CSS** (sprites for web), **cocos2d plist** (the historic default; consumed by many engines beyond cocos2d), **Phaser (JSON hash)** / Phaser 3, **PixiJS** (json-hash compatible), **Unity** (via TexturePacker Importer plugin on the Asset Store), **Unreal Engine** (Paper2D), **Godot** (sprite sheet + AtlasTexture/TileSet exporters, animation support), **Spine** (atlas), **libGDX** (.atlas), **SpriteKit**, **Starling/Sparrow XML**, **EaselJS/CreateJS**, **MelonJS**, **Solar2D (Corona SDK)**, **Cocos2d-x**, **MonoGame / MonoGame.Extended**, **GameMaker** (via docs), **Spriter**, **UIKit**.

**Long tail**: 2D Toolkit, Amethyst, AppGameKit, BatteryTech, BHive, Blacksmith 2D, CAAT, CEGUI/OGRE, Egret Engine, Gideros, Kivy, Kwik, LayaAir, Mapbox, Moai, Molecule, Noesis GUI, Orx, Panda 2, PopcornFX, Slick2D, Spark AR Studio, SpriteStudio, x2d.

**Not built in**: Defold (official support via external custom exporter, see above) — confirms the exporter system is the extension path, and relevant to us since Defold is a target of interest.

Most-used in practice: JSON hash/array (Phaser/Pixi/web), cocos2d plist, Unity, libGDX atlas, Spine atlas, Godot, CSS.

---

## Lessons for our tool

1. **One binary, GUI = project editor, CLI = project runner.** Adopt the exact `.tps` pattern: our project file holds *everything*; `pack myproject.xxx` is the whole CI story; every setting also gets a CLI flag with CLI-overrides-project semantics, plus `--save` to script project edits.
2. **Human-readable, hand-editable, relative-path project file.** Text format (we can pick JSON over XML), versioned with `fileFormatVersion` + writer version, all source paths relative to the project file. Allow settings that exist only in the file before they get GUI (TexturePacker does this deliberately).
3. **Smart folders + live preview are the UX core.** Folder-linking with filesystem watching and instant repack is what people actually pay for. Sub-folder path → sprite name prefix. Per-file adds are secondary.
4. **Smart no-op publish.** Hash inputs+settings+tool version (`smartUpdateKey`) into the output; skip work when unchanged; `--force-publish` escape hatch. This single feature makes "run the packer on every build" acceptable.
5. **Exporters as data with capability flags.** Descriptor (name, display name, N output files, template per file) + template language + capability flags (`supportsRotation`, `supportsTrimming`, `supportsPolygonPacking`, `supportsPivotPoint`, `supportsScale9`, `supportsNPOT`, y-axis direction, multipack) that gate both packer behavior and GUI controls. Expose a sprite model at least as rich as TexturePacker's (frameRect/cornerOffset/untrimmedSize/rotated/vertices/triangles/pivot/scale9). A small filter/escape hatch (Lua for us?) beats extending the template language forever.
6. **Feature priority order** (based on what's default/most documented): MaxRects+heuristics, trim with threshold+margin, rotation, extrude, shape/border padding, alias detection, multipack with `{n}` placeholders, POT/max-size/force-square constraints → then variants (`{v}`), pivot/9-patch editors, polygon mode, normal-map co-packing, animation detection.
7. **Placeholders in output names** (`{n}`, `{n0}`, `{n1}`, `{v}`) are the mechanism tying multipack + variants together; validate their presence and error early.
8. **Identical-layout co-packing** (normal/emission/AO sheets) is a differentiator we can do better: TexturePacker's GUI supports only one secondary type; supporting N named secondary channels natively would beat it.
9. **Warnings, not modals**: constraint violations (doesn't fit, unsupported-by-exporter feature) render as persistent non-modal warnings; publish blocked only on hard errors.
10. **Docker/CI friendliness**: headless EULA-free operation, env-var config, exit codes, quiet mode. As a free tool we skip TexturePacker's floating-license friction entirely — call that out.

---

## Sources

- Docs index / quickstart: https://www.codeandweb.com/texturepacker/documentation
- Texture settings reference (algorithms, trim, padding, multipack, variants, formats): https://www.codeandweb.com/texturepacker/documentation/texture-settings
- GUI overview: https://www.codeandweb.com/texturepacker/documentation/user-interface-overview
- Command line: https://www.codeandweb.com/texturepacker/documentation/commandline
- Custom exporters: https://www.codeandweb.com/texturepacker/documentation/custom-exporter
- Hidden features (ignoreFileList, replaceList, heuristicMask): https://www.codeandweb.com/texturepacker/documentation/hidden-features
- Docker & CI: https://www.codeandweb.com/texturepacker/documentation/docker-ci
- Product page (formats + headline features): https://www.codeandweb.com/texturepacker
- Real .tps example: https://github.com/CodeAndWeb/TexturePacker-Phaser/blob/master/assets/cityscene.tps
- Full `--help` dump (community gist): https://gist.github.com/baba-s/097d24099151378dea94161681e3767d
- Normal maps tutorial: https://www.codeandweb.com/texturepacker/tutorials/packing-normal-map-sprite-sheets
- Multiple texture types with same layout: https://www.codeandweb.com/texturepacker/knowledgebase/pack-multiple-texture-types-with-same-layout
- Normal map auto-detection: https://www.codeandweb.com/texturepacker/knowledgebase/normal-map-detection
- Animation detection (numeric suffix stripping): https://www.codeandweb.com/texturepacker/knowledgebase/animation-detection
- Defold official TexturePacker support (custom-exporter based): https://defold.com/extension-texturepacker/ , https://github.com/defold/extension-texturepacker
