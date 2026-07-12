# OSS Texture Packers & Tool-Architecture Patterns

> **Status: non-normative point-in-time research.** Recommendations below are
> evidence, not current architecture; see
> [`../ntpacker-master-spec.md`](../ntpacker-master-spec.md).

Research for a standalone C texture/atlas packer (immediate-mode UI engine, CLI + GUI in one tool, project file format, pluggable exporters). Researched 2026-07-10 from primary sources (repos, raw source files, official docs).

---

## Summary

- **free-tex-packer** is the best free reference for the *whole product shape*: a packing core (options → pages of rects → rendered files), a GUI, a CLI that consumes the GUI's project file (`.ftpp`, plain JSON), and a mustache-template exporter system where *every* built-in format is just a template + 4 lines of metadata. Its main architectural flaw is instructive too: the GUI does **not** actually depend on the core npm package — the packing/exporter code is duplicated between `free-tex-packer` (GUI) and `free-tex-packer-core`, a drift hazard your C tool should avoid by construction.
- The **canonical sprite data model** is TexturePacker's JSON-hash: `frame` (packed rect), `rotated`, `trimmed`, `spriteSourceSize` (trim offset+size in original image), `sourceSize` (original size), `pivot`, plus per-page `meta`. Every engine importer (Phaser, Pixi, Godot, Spine, Starling, cocos2d) is a projection of this model. Build this struct once in core; exporters only read it.
- **CLI+GUI**: the dominant successful pattern for small native tools is **one binary** — GUI when launched bare, headless when given args (TexturePacker, rTexPacker, Cheetah). The clean variant is *core static lib + two thin frontends* compiled into one executable. All persistent state (project, options, sprite list, pack results) lives in core; the GUI owns only view state (zoom, selection, panels).
- **Project file**: JSON, versioned (`"version": N`), **relative paths** (free-tex-packer's absolute paths are its second big mistake; TexturePacker explicitly converts to relative for portability), sorted keys + stable sprite ordering for VCS-friendly diffs, folder references (rescanned) rather than frozen file lists, and a separate `sprites` override section for pivot/9-slice.
- **Exporters**: for a C tool, hardcode 2–3 first-class exporters (your engine's binary + JSON-hash) **and** add a mustache-template exporter (`.mst` file + tiny JSON descriptor). Mustache is trivially embeddable in C, covers ~90% of real formats (proven by free-tex-packer shipping 15+ engine formats as templates), and lets users add formats without recompiling. Skip scripting plugins initially.
- **Incremental workflow**: crunch's approach is the gold standard for CLI simplicity — write a hash file next to the output; if input hashes match, exit without repacking (`--force` to override). TexturePacker embeds the hash *inside the exported data file* (`smartupdate` key) so it survives clean checkouts of outputs. GUI watch mode = chokidar-style FS watcher on live-linked folders (free-tex-packer does exactly this).

---

## free-tex-packer deep dive

**Repos:** [odrick/free-tex-packer](https://github.com/odrick/free-tex-packer) (GUI: web + Electron), [odrick/free-tex-packer-core](https://github.com/odrick/free-tex-packer-core) (npm packing library), [odrick/free-tex-packer-cli](https://github.com/odrick/free-tex-packer-cli) (CLI), plus webpack/gulp/grunt plugin packages. Maintenance mode ("only critical bugs will be fixed").

### 1. Architecture split — how one "core" serves three frontends (and where it doesn't)

```
free-tex-packer-core (npm)          <- packers/, exporters/*.mst, index.js
        ^                ^
        |                |
free-tex-packer-cli   webpack/gulp/grunt plugins     (true consumers of core)

free-tex-packer (GUI)                                (does NOT depend on core!)
  src/client/PackProcessor.js       <- its own copy of the engine
  src/client/packers/{MaxRectsBin,MaxRectsPacker,OptimalPacker,Packer,index}.js
  src/client/exporters/{index.js,list.json} + resources/static/exporters/*.mst
  src/client/platform/web/{FileSystem,Project,Controller,Downloader}.js
  src/client/platform/electron/{FileSystem,Project,Controller,Downloader}.js
```

Verified from the repo tree and `package.json`: the GUI's dependencies contain `mustache`, `maxrects-packer`, `chokidar`, `electron`, `react` — but **not** `free-tex-packer-core`. Core was extracted from the GUI code and the two copies are maintained in parallel (same file names, same `list.json`). The pieces that genuinely share the core are the CLI and the build-tool plugins.

Two transferable ideas despite the flaw:

1. **Platform abstraction layer.** The GUI is one React codebase; everything OS-specific is behind `platform/web` vs `platform/electron` modules with identical interfaces (`FileSystem`, `Project`, `Controller`, `Downloader`). Webpack aliases `platform/` to one or the other at build time. Web builds get drag-drop + zip import + download; Electron builds get real paths, native dialogs, `chokidar` file watching, recent-projects. Same idea maps to a C tool: `fs_backend`, `dialog_backend` vtables for CLI vs GUI.
2. **The core's contract is pure and I/O-free at the edges:** `pack(images[{path, contents}], options) -> files[{name, buffer}]`. Images come in as buffers, results come out as named buffers; the *caller* (CLI/plugin/GUI) does all disk I/O. This is exactly the right boundary for a C library too.

### 2. Core API (free-tex-packer-core)

`texturePacker(images, options, cb)` / `packAsync(images, options)`. Full options with defaults:

| Option | Default | Notes |
|---|---|---|
| `textureName` | `"pack-result"` | base name for outputs |
| `suffix` / `suffixInitialValue` | `"-"` / `0` | multipack page naming: `name-0`, `name-1`… |
| `width` / `height` | `2048` | max atlas size |
| `fixedSize` | `false` | always emit exactly width×height |
| `powerOfTwo` | `false` | |
| `padding` | `0` | px between sprites |
| `extrude` | `0` | edge-pixel replication (bleed guard) |
| `allowRotation` | `true` | 90° rotation |
| `allowTrim` | `false`* | (*GUI default true) transparent-border trim |
| `trimMode` | `"trim"` | `"trim"` keeps sourceSize/offsets; `"crop"` zeroes them (exports as if never trimmed) |
| `alphaThreshold` | `0` | alpha ≤ threshold counts as empty for trimming |
| `detectIdentical` | `true` | dedupe identical images (aliases) |
| `removeFileExtension` | `false` | name munging at export time |
| `prependFolderName` | `true` | `folder/sprite.png` names |
| `textureFormat` | `"png"` | png/jpg/webp |
| `base64Export` | `false` | embed image as data-URI in the data file |
| `scale` / `scaleMethod` | `1` / `"BILINEAR"` | post-scale of atlas + all coords |
| `tinify` / `tinifyKey` | `false` | TinyPNG API compression |
| `packer` | `"MaxRectsBin"` | see below |
| `packerMethod` | `"BestShortSideFit"` | heuristic, per packer |
| `exporter` | `"JsonHash"` | string name or custom-exporter object |
| `filter` | `"none"` | `grayscale` / `mask` pixel filters |
| `appInfo` | null | `{url, version}` stamped into `meta` |

### 3. Packers

- **MaxRectsBin** — Jukka Jylänki's classic MaxRects ([juj/RectangleBinPack](https://github.com/juj/RectangleBinPack), the same lineage crunch/texpack/Cheetah use). Methods: `BestShortSideFit`, `BestLongSideFit`, `BestAreaFit`, `BottomLeftRule`, `ContactPointRule`.
- **MaxRectsPacker** — the npm [`maxrects-packer`](https://github.com/soimy/maxrects-packer) library. Methods: `Smart`, `Square`, `SmartSquare`, `SmartArea` (grows the bin instead of packing into fixed size).
- **OptimalPacker** — not an algorithm: a meta-packer that **runs every packer × every method combination and keeps the best result** (fewest pages, then smallest area). Recommended default in the UI. Cheap to implement, big UX win — copy this.

Multipack is implicit: whatever doesn't fit starts a new page; exporters that support it (Phaser3 multiatlas) receive all pages, others get one file per page with `suffix` numbering.

### 4. Exporter system — mustache templates

An exporter is only `{ type, description, allowTrim, allowRotation, template, fileExt, predefined }` in `exporters/list.json`. All 18 built-ins are mustache templates; several formats share a template (Pixi and PhaserHash both use `JsonHash.mst`). A **custom exporter** is passed as an object instead of a name:

```js
exporter: {
  fileExt: "lua",
  template: "./MyFormat.mst"   // or content: "inline template string"
}
```

Built-in exporter list (`type` → template, ext): JsonHash→`JsonHash.mst` json, JsonArray→json, XML→xml, Css→css, OldCss→css (trim/rotation disallowed), Pixi→`JsonHash.mst` json, GodotAtlas→tpsheet, GodotTileset→tpset, PhaserHash/PhaserArray→json, Phaser3→json, Cocos2d→plist, Unreal→paper2dsprites, Starling→xml, Spine→atlas, UIKit→plist (no rotation), Unity3D→tpsheet (no rotation), Egret2D→json (no trim/rotation). Note the per-exporter **capability flags**: `allowTrim`/`allowRotation` let the app disable options the target format can't represent — do the same in your UI.

**Template data model** — `startExporter` renders the template with exactly three roots:

```
rects: [           // one per packed sprite, in pack order
  name             // munged per removeFileExtension/prependFolderName
  frame     { x y w h hw hh }     // packed rect on the page; hw/hh = half sizes (for CSS transform-origin)
  spriteSourceSize { x y w h }    // trimmed rect inside the original image
  sourceSize       { w h }        // original image size
  rotated, trimmed                // booleans
  index, first, last              // loop helpers (last drives JSON comma logic: {{^last}},{{/last}})
]
config: {          // per-page
  imageName, imageWidth, imageHeight, format ("RGBA8888"), scale,
  base64Export, base64Prefix, imageData (base64), textureFormat, ...all options
}
appInfo: { displayName, version, url }
```

`prepareData` (exporters/index.js) is where trim-mode `crop` rewrites `spriteSourceSize`/`sourceSize`, and where `scale` multiplies every coordinate — i.e. **normalization happens once, before templating**. Templates are dumb.

Mustache is extended with **formatters** via `mustache-wax` (`{{ value | add : 2 }}`): `add`, `subtract`, `multiply`, `divide`, `offsetLeft`, `offsetRight`, `mirror` (for engines with flipped Y or center-based origins), `escapeName`. This tiny arithmetic layer is what lets logic-less mustache serve engines with different coordinate conventions.

**`JsonHash.mst` verbatim** (this template *is* the TexturePacker-compatible exporter):

```mustache
{
  "frames": {
    {{#rects}}
    "{{{name}}}": {
      "frame": {"x": {{frame.x}}, "y": {{frame.y}}, "w": {{frame.w}}, "h": {{frame.h}}},
      "rotated": {{rotated}},
      "trimmed": {{trimmed}},
      "spriteSourceSize": {"x": {{spriteSourceSize.x}}, "y": {{spriteSourceSize.y}}, "w": {{spriteSourceSize.w}}, "h": {{spriteSourceSize.h}}},
      "sourceSize": {"w": {{sourceSize.w}}, "h": {{sourceSize.h}}},
      "pivot": {"x": 0.5, "y": 0.5}
    }{{^last}},{{/last}}
    {{/rects}}
  },
  "meta": {
    "app": "{{{appInfo.url}}}",
    "version": "{{appInfo.version}}",
    "image": "{{^config.base64Export}}{{config.imageName}}{{/config.base64Export}}{{#config.base64Export}}{{{config.base64Prefix}}}{{{config.imageData}}}{{/config.base64Export}}",
    "format": "{{config.format}}",
    "size": {"w": {{config.imageWidth}}, "h": {{config.imageHeight}}},
    "scale": {{config.scale}}
  }
}
```

(Whitespace compacted; original at `exporters/JsonHash.mst`.)

**`Css.mst`** shows how far logic-less templates stretch — rotation handled with section blocks:

```mustache
{{#rects}}
.{{{name}}} { background:url({{config.imageName}}) no-repeat -{{frame.x}}px -{{frame.y}}px;
  {{^rotated}}width:{{frame.w}}px;height:{{frame.h}}px;{{/rotated}}
  {{#rotated}}width:{{frame.h}}px;height:{{frame.w}}px;
    transform-origin:{{frame.hw}}px {{frame.hh}}px;transform:rotate(-90deg);{{/rotated}}
  {{#trimmed}}margin-left:{{spriteSourceSize.x}}px;margin-top:{{spriteSourceSize.y}}px{{/trimmed}} }
{{/rects}}
```

**`Spine.mst`** shows a non-JSON, line-oriented format is equally trivial; **`Phaser3.mst`** shows the multipack shape — a `"textures": [ {image, size, frames:[…]} ]` array with `meta` at top level.

### 5. Project file format — `.ftpp`

Plain JSON (the CLI just `JSON.parse`s it). Written by `Project.getData()` (src/client/platform/electron/Project.js):

```json
{
  "meta":     { "version": "0.6.7" },
  "savePath": "C:/game/assets/atlases",
  "images":   [ { "name": "logo.png", "path": "C:/game/art/logo.png", "folder": "" } ],
  "folders":  [ "C:/game/art/enemies" ],
  "packOptions": {
    "fileName": "pack-result", "savePath": "...",
    "width": 2048, "height": 2048, "fixedSize": false, "powerOfTwo": false,
    "padding": 0, "extrude": 0,
    "allowRotation": true, "allowTrim": true, "trimMode": "trim", "alphaThreshold": 0,
    "detectIdentical": true, "removeFileExtension": false, "prependFolderName": true,
    "base64Export": false, "scale": 1, "filter": "none",
    "tinify": false, "tinifyKey": "",
    "packer": "MaxRectsBin", "packerMethod": "BestShortSideFit",
    "exporter": "JSON (hash)"
  }
}
```

Key observations:

- **`images` vs `folders` split**: individually added files are frozen entries; added folders are stored as a single path and **rescanned recursively on every load** (and watched via chokidar while open). This "live folder" concept is the feature users actually want — new art dropped in a folder is picked up automatically.
- **Paths are absolute** (Windows backslashes normalized to `/`). This is the format's worst defect: projects don't survive a different checkout path or another machine. The CLI even resolves `project.folders[i]` directly against the filesystem. TexturePacker, by contrast, relativizes paths in `.tps` files specifically for team/VCS portability.
- **`packOptions.exporter` stores the UI display string** (`"JSON (hash)"`), so the CLI carries a 16-line mapping table from display names to core names — a versioning smell. Store stable identifiers, render display names in UI.
- `meta.version` is the app version, not a schema version; there is no migration logic.
- The GUI marks the project dirty via an event bus (`IMAGES_LIST_CHANGED`, `PACK_OPTIONS_CHANGED`, `PACK_EXPORTER_CHANGED` → `CURRENT_PROJECT_MODIFIED`), keeps a 10-entry recent-projects list in localStorage.

### 6. CLI (`free-tex-packer-cli`)

~180 lines total: `free-tex-packer-cli --project file.ftpp [--output dir]`. Reads the project JSON, loads `images` entries + rescans `folders` recursively, maps display exporter names to core names, refuses `exporter: "custom"` (custom templates aren't stored in the project — a design gap: store the template path in the project instead), calls the core, writes returned buffers to `--output` (fallback: project `savePath`, then the project file's directory). Everything else — packing, exporting — is core. That is the right thickness for a CLI.

---

## OSS tools survey

**crunch** ([ChevyRay/crunch](https://github.com/ChevyRay/crunch), C++/MIT, built for Celeste). Pure CLI, one small codebase, no config file: `crunch bin/atlases/atlas assets/characters,assets/tiles -p -t -v -u -r -j`. Options: xml/binary/json data output, premultiply, trim, rotate (90° CW), unique (dedupe), size (64–4096), pad (0–16), `-f/--force`, `-d` default bundle. Two distinctive ideas: (1) a **documented fixed-layout binary format** — `int16 num_textures { string name; int16 num_images { string img_name; int16 x,y,w,h [,frame_x,frame_y,frame_w,frame_h if --trim] [,byte rotated if --rotate] } }` — note frame fields are *conditionally present* based on flags, which keeps files tiny but means the loader must know the flags (your binary format should instead carry feature bits in a header); (2) **hash-cache incrementality** — alongside the atlas it writes a hash file of all inputs+settings; rerunning with unchanged inputs is a no-op unless `--force`. Ideal model for build-pipeline integration.

**Cheetah-Texture-Packer** ([scriptum/Cheetah-Texture-Packer](https://github.com/scriptum/Cheetah-Texture-Packer), C++/Qt). GUI and CLI **in the same Qt executable** (run with args → batch mode). MaxRects "with aggressive heuristics", claims 5–20% better density than contemporaries; features: auto-crop with alpha threshold, merge duplicates, rotation, extrude+border, recursive folders, square/POT constraints, "autosize" (grow until a fill-rate threshold is met), selectable sort order. Output is its own simple `.atlas` text format (positions, offsets, original sizes, rotation flags). Distinctive lesson: a heuristic *bake-off* (try many, keep best) as a user-facing "best quality" toggle, and honesty that GUI is just a veneer over the same pack function.

**ShoeBox** ([renderhjs.net/shoebox](https://renderhjs.net/shoebox/), Adobe AIR, free but closed). A grab-bag of drag-drop tools (pack sprites, extract sprites, bitmap fonts, tile extraction). Its exporter system is **template strings with `@placeholders`**, configured in the settings dialog of each tool: per-sprite line template (`txtFormatLoop`) with `@id`, `@x @y @w @h`, trim fields `@fx @fy @fw @fh`, and a document template with `@loop`, `@TexName`, `@W @H`, `@fileName`. Ships presets for Starling/Sparrow, cocos2d, NGui, HTML5 etc. It's mustache-minus-conditionals: simpler to implement, but the lack of sections/conditionals means formats needing rotation blocks or last-comma logic get ugly — evidence that mustache's `{{#section}}` power is worth the small extra implementation cost.

**Leshy SpriteSheet Tool** ([leshylabs.com/apps/sstool](https://www.leshylabs.com/apps/sstool/)). 100% in-browser HTML5, no install/upload. Distinctive: it's as much an atlas **editor** as a packer — import an *existing* sheet plus its metadata, rename sprites inline, tweak rects, mark sprites ignored, then re-export (XML, JSON-TP-compatible, CSS, ImageMagick shell scripts). Lesson: round-tripping (import atlas+data → edit → export) and inline sprite-name editing are cheap features with outsized utility; also proof a serviceable packer needs zero platform integration.

**TexPack** ([urraka/texpack](https://github.com/urraka/texpack), C++ CLI). Thin, faithful wrapper around Jylänki's MaxRects (all five heuristics plus `auto` = try all and keep best). Distinctive: **sprite metadata pass-through** — you can feed it a JSON file of per-sprite custom properties that get merged into the output JSON (solves pivots/9-slice without the packer knowing about them); multi-page output; `--alpha-bleeding` post-process (dilate RGB into transparent areas — prevents dark halos with bilinear filtering; distinct from extrude); input filename ranges `fire[0001-0012].png`; emits legacy, JSON-hash, and JSON-array schemas.

**atlasc** ([septag/atlasc](https://github.com/septag/atlasc), C99). Closest sibling to your project: pure C, CMake, builds as **CLI executable or static library** (`STATC_LIB=1`) with three API forms (file→file, file→memory, memory→memory). Bundles stb (image I/O), sjson, delaunay triangulation. Distinctive: `--mesh` produces **polygon (mesh) sprites** — non-rectangular outlines with a vertex budget (default 25) to cut overdraw; alpha trimming; scale; POT. Limitation it documents: no multiple pixel-islands per sprite. Good reference for C-level API shape and dependency policy (vendored single-file libs, zero external deps).

**rTexPacker** ([raylibtech.itch.io/rtexpacker](https://raylibtech.itch.io/rtexpacker), raylib ecosystem, raysan5). The closest reference for *your UI approach*: **immediate-mode GUI (raygui)** in a ~1 MB single-file, zero-dependency portable executable, and the **same executable is the CLI** (batch flags for input/output/format/algorithm). Packing: Basic and Skyline (Bottom-Left / Best-Fit). Atlas descriptors: its own text (.rtpa), binary (.rtpb), XML, JSON, and **C header (.h)** export; images as PNG/QOI/DDS/RAW. Distinctive tricks: embeds the binary descriptor as a custom `rTPb` chunk *inside the output PNG* (one file ships both image and metadata); self-contained `.rtp` project files (store everything needed to reopen/edit); per-sprite manual placement, origins, and simple collision shapes; also packs font glyphs (incl. SDF). Proof that IMGUI + single binary + CLI flags is a viable, shipped product shape.

**Phaser's packing story.** Phaser deliberately ships **no packer**: it consumes atlases via loaders (`atlas` = TexturePacker JSON-hash/array with trim+rotation support; `multiatlas` = the multi-page `textures:[]` JSON that both TexturePacker "Phaser 3" and free-tex-packer's `Phaser3.mst` emit). The ecosystem filled the gap: TexturePacker has first-class Phaser presets and is the community default; free-tex-packer was promoted on phaser.io as the free alternative; Phaser Editor 2D embeds its own packer based on **libGDX's TexturePacker** (MaxRects) with an asset-pack editor. Lesson: exporting *someone else's de-facto format well* matters more than inventing formats — the JSON-hash/multiatlas family is the lingua franca.

---

## CLI+GUI architecture patterns

Observed approaches:

| Tool | Pattern |
|---|---|
| TexturePacker | One product; the GUI executable also runs headless: `TexturePacker myproject.tps --sheet out.png --data out.json`. Docs maintain strict 1:1 GUI-option ↔ CLI-flag parity (`--trim-mode`, `--algorithm`, …). |
| rTexPacker | One ~1 MB binary; bare launch → raygui IMGUI app, launch with flags → batch CLI. |
| Cheetah | One Qt binary, same duality. |
| crunch / texpack / atlasc | CLI only (atlasc also builds as a C static lib). |
| free-tex-packer | Separate packages: core lib + Electron/web GUI + node CLI. Works, but GUI drifted from core because sharing wasn't enforced. |

**Recommendation for your C tool: single binary, three layers.**

```
core/      pack_project(project*, arena*) -> pack_result*      (no I/O opinions, no UI)
           project_load/save, sprite scan+trim, packers, exporters, hashing
frontend/cli.c   argv -> load project (or synthesize one from flags) -> pack -> write files
frontend/gui.c   IMGUI loop; owns ONLY: selection, zoom/pan, dirty flag, panel state,
                 async pack job handle, preview textures
main.c           argc>1 (or --headless / a project path arg) ? cli_main() : gui_main()
```

Rules that keep it honest:

- **Everything the CLI needs must live in core**, which forces the right split: project model, image loading/trimming, packing, exporting, path resolution, hashing. If the GUI ever computes something the CLI can't, that's a layering bug.
- **Core returns data, frontends do I/O** (free-tex-packer-core's `files[{name, buffer}]` contract). This makes core trivially unit-testable and lets the GUI show a "what will be written" preview from the same buffers.
- **One source of truth for options**: define options in an X-macro table (`OPTION(width, int, 2048, "Max atlas width")`) and generate: struct fields, JSON (de)serialization, CLI flags, and GUI widgets iteration. This is how you get TexturePacker-grade GUI/CLI parity for free and never let them diverge.
- On Windows, a single binary that is both console and GUI app needs the usual trick: build as GUI subsystem and `AttachConsole(ATTACH_PARENT_PROCESS)` when CLI args are present (or ship a tiny `tool-cli.exe` shim, like `TexturePackerCLI` does on some platforms).
- Run packing on a worker thread in the GUI (pack jobs are pure functions of project state → easy to cancel/restart on change); the CLI calls the same function synchronously.

---

## Project file design

**Format: JSON.** Every surveyed tool with a project file uses a text format (.ftpp = JSON; .tps = XML; rTexPacker .rtp = text config). JSON wins for you: trivially parsed in C (vendored single-file parser), human-diffable, users can generate it from scripts. Design points, learned from the good and bad above:

```jsonc
{
  "version": 1,                          // SCHEMA version, integer, migrations keyed on it
  "app": { "name": "ntp", "version": "0.3.0" },   // informational only
  "sources": [
    { "type": "folder", "path": "art/enemies", "recursive": true },   // live: rescanned on open
    { "type": "file",   "path": "art/logo.png" }
  ],
  "ignore": [ "*.psd", "art/enemies/wip/**" ],
  "atlas": {                             // per-atlas settings (one project = one atlas; use N files or an "atlases" array for more)
    "name": "gameplay",
    "maxSize": [2048, 2048], "pot": false, "fixedSize": false,
    "padding": 2, "extrude": 1,
    "allowRotation": true, "trim": "trim", "alphaThreshold": 0,
    "detectIdentical": true,
    "packer": "maxrects", "heuristic": "auto",
    "outputs": [
      { "exporter": "json-hash", "data": "out/gameplay.json", "texture": "out/gameplay.png" },
      { "exporter": "template", "template": "exporters/mygame.mst", "data": "out/gameplay.lua" }
    ]
  },
  "sprites": {                           // sparse per-sprite OVERRIDES only; absent = defaults
    "enemies/slime.png": { "pivot": [0.5, 1.0] },
    "ui/panel.png":      { "nineSlice": [8, 8, 8, 8], "trim": false }
  }
}
```

- **Relative paths, always**, resolved against the project file's directory; normalize to forward slashes on save. This is TexturePacker's documented behavior ("converts absolute to relative when saving, enabling portability of complete project sets") and the fix for free-tex-packer's absolute-path defect. Accept absolute paths on load, warn, relativize on next save.
- **Schema version as an integer**, separate from app version. Loader: `if version > CURRENT → refuse with message; if < CURRENT → run chained migrations v1→v2→v3 and mark dirty`. free-tex-packer stores only app version and has no migrations — every renamed option silently resets.
- **Stable identifiers, not display strings** in the file (`"json-hash"`, not `"JSON (hash)"` — see the CLI mapping-table smell above).
- **Sources vs sprites separation**: `sources` declares *where sprites come from* (live folders — rescan on open and watch while open — plus pinned files); `sprites` is a sparse map of per-sprite overrides keyed by the sprite's *atlas name* (relative path), never by absolute path. Prune overrides whose sprite disappeared only on explicit user action (art may be temporarily missing on a coworker's machine).
- **Per-sprite override set** worth supporting from day one: `pivot` (normalized), `nineSlice` (l,t,r,b margins), `trim` on/off, `extrude` override, `tag/animation` grouping. This mirrors TexturePacker (pivot + scale9 are per-sprite in .tps) and texpack's metadata pass-through.
- **Deterministic output for VCS**: write JSON with sorted keys, fixed 2-space indent, trailing newline, LF; sort `sources`, `ignore`, and `sprites` keys lexicographically; never write defaults (sparse files diff better and survive default changes); no timestamps in the project file. Same discipline for *exported* data files: sort sprites by name (or a documented stable order), so re-packing without changes produces a byte-identical data file — this plus content hashing makes exports safe to commit.

---

## Exporter plugin design (with recommendation for a C tool)

The spectrum, with real examples:

| Level | Example | Pros | Cons |
|---|---|---|---|
| (a) Hardcoded in core | crunch (xml/json/binary writers in C++), atlasc (sjson) | fastest, exact control (binary formats!), no runtime deps | users must recompile to add formats |
| (b) Logic-less text templates | free-tex-packer (mustache + formatters), ShoeBox (`@placeholder` strings) | tiny engine; users add formats by dropping a text file; 15+ engine formats proven expressible; templates are data → safe | text output only; awkward beyond arithmetic (needs formatter escape hatch) |
| (c) Descriptor + template | TexturePacker (`exporter.xml` capabilities + Grantlee/Django template + optional JS filters) | descriptor lets the app adapt UI (supportsRotation/scale9/multipack, custom per-exporter properties); template does the rendering | more machinery; Django-style template engine is much bigger than mustache |
| (d) Scripting plugins | (Aseprite Lua scripts; TexturePacker's `.qs` JS filters are a hybrid) | arbitrary logic, binary output possible | embed interpreter, sandboxing, API stability burden |
| (e) Separate process piping | (generic Unix pattern; no surveyed packer ships it as primary) | any language, total isolation | slow per-invoke, packaging/discovery pain on Windows, hard to surface errors in GUI |

**Recommendation: (a) + (b/c hybrid).**

1. **Hardcode in C**: your engine's native format (especially if binary — templates can't do binary), plus `json-hash` and `json-array` for ecosystem compatibility. Binary formats stay hardcoded forever (crunch's lesson: they're ~50 lines of writes).
2. **Ship a mustache engine** (implement it or vendor a small C one; core mustache is <1k LOC: variables, `{{#}}`/`{{^}}` sections, `{{{raw}}}`, partials optional). Add free-tex-packer's **formatter extension** (`{{frame.x | add : 2}}`) — its 8 formatters (`add/subtract/multiply/divide/offsetLeft/offsetRight/mirror/escapeName`) are demonstrably sufficient for Godot/Unreal/Spine/cocos2d coordinate conventions. Feed templates the canonical model (next section) pre-normalized — scaling, trim-mode rewriting, and name munging happen in core *before* rendering, exactly like `prepareData`.
3. **Pair each template with a small JSON descriptor** (TexturePacker's idea at 5% of the cost — free-tex-packer's `list.json` entry is the minimal version):

```json
{
  "id": "godot-atlas", "displayName": "Godot (atlas)",
  "fileExt": "tpsheet",
  "template": "GodotAtlas.mst",
  "supportsTrim": true, "supportsRotation": true, "supportsMultipack": false
}
```

   The capability flags drive the GUI (grey out rotation when the target can't express it — both free-tex-packer and TexturePacker do this) and CLI validation. `supportsMultipack:false` → render the template once per page with `-N` suffixes; `true` → render once with a `pages` array in scope.
4. **Discovery**: built-ins compiled in (embed the `.mst` text as C strings); user exporters loaded from `<config-dir>/exporters/*/` and a project-relative `exporters/` dir; project files reference user templates by relative path (fixing free-tex-packer's "CLI does not support a custom exporter" gap — the template travels with the project).
5. **Defer (d) and (e).** If demand appears later, a Lua embed slots in cleanly *as another exporter kind in the same descriptor* (`"script": "exporter.lua"`), consuming the same canonical model. Don't build it speculatively.

---

## Incremental workflow patterns

- **Content-hash skip (CLI, crunch model)**: hash all inputs that affect output — every source image's bytes (or `size+mtime` fast path with hash fallback), the effective option set, exporter template text, tool version. Write the hash next to outputs (`gameplay.hash`); on run, if the stored hash matches, print "up to date" and exit 0. `--force` bypasses. This makes it safe to wire the packer unconditionally into build scripts.
- **Embedded hash (TexturePacker model)**: TexturePacker instead writes the key *into the exported data file's* `meta.smartupdate` field: `"$TexturePacker:SmartUpdate:<hash>:<hash>:<hash>$"` (multiple hashes = images / settings / etc., letting it distinguish "images changed" from "only data-file-relevant settings changed" and skip re-encoding the PNG when possible). Advantage over a sidecar: the key survives with the artifact through VCS. A JSON-hash-compatible exporter can carry the same field; for binary formats use the sidecar. Recommended: support both — sidecar always, plus embed in formats that allow it.
- **Watch mode (GUI)**: free-tex-packer's Electron build uses chokidar on every *live folder* (and individually added files); any FS event fires `FS_CHANGES` → debounce → rescan → repack → refresh preview. For C: one watcher thread (Win32 `ReadDirectoryChangesW` on the folder handles; inotify/kqueue elsewhere), 150–300 ms debounce, coalesce events, then trigger the same async pack job the UI uses. Combined with content hashing, a save that doesn't change pixels (e.g. re-export with identical bytes) triggers no repack.
- **Watch mode (CLI)**: a `--watch` flag reusing the same watcher + hash logic turns the CLI into a dev-server-like companion — the pattern users now expect from asset tooling; none of the surveyed OSS packers ship it (TexturePacker sells it via IDE integrations), so it's cheap differentiation.
- **Live-linked folders vs explicit lists**: free-tex-packer supports both (folders rescan + watch; individually added files are pinned). Keep that dual model: folders for the 95% case, pinned files for odd one-offs, plus `ignore` globs. Rescan folders on project open *and* on pack in CLI (the CLI must never trust a stale file list — note free-tex-packer's CLI rescans folders but uses frozen `images` entries, which is correct).
- **Incremental packing** (only repacking changed sprites in place) is not worth it: layout is globally sensitive, packing 1–2k sprites with MaxRects is milliseconds-to-tens-of-ms in C. Optimize instead by caching *decoded+trimmed source images* keyed by file hash (trim rects and pixel data are the actual slow part), so a repack after touching one file re-decodes one file.

---

## Canonical sprite data model

TexturePacker's JSON-hash is the de-facto reference; free-tex-packer reproduces it field-for-field, and Phaser/Pixi/etc. parse it natively. Canonical example (from TexturePacker output, via fzipp/texturepacker's test data):

```json
{
  "frames": {
    "test_sprite2": {
      "frame": {"x": 18, "y": 5, "w": 25, "h": 32},
      "rotated": false,
      "trimmed": true,
      "spriteSourceSize": {"x": 30, "y": 34, "w": 25, "h": 32},
      "sourceSize": {"w": 60, "h": 100},
      "pivot": {"x": 0.5, "y": 0.5}
    }
  },
  "meta": {
    "app": "https://www.codeandweb.com/texturepacker", "version": "1.0",
    "image": "TestSheet.png", "format": "RGBA8888",
    "size": {"w": 400, "h": 200}, "scale": "1",
    "smartupdate": "$TexturePacker:SmartUpdate:02448c...:f8d8f2...:0d78c8...$"
  }
}
```

**The normalized in-core model every exporter consumes** (superset; fields map to TexturePacker's template variables `frameRect`/`cornerOffset`/`untrimmedSize`/`pivotPointNorm`/`vertices`/`triangleIndices`/`scale9Borders` and to free-tex-packer's `rects` entries):

```c
typedef struct ntp_sprite {
    const char *name;        // atlas key: relative path, opts applied (ext strip, folder prefix)
    int   page;              // atlas page index (multipack); page owns image name + size
    // placement on the page
    struct { int x, y, w, h; } frame;   // packed rect ON the page. Convention (TexturePacker):
                                        // w/h are the UNROTATED trimmed size; if rotated, the
                                        // sprite occupies h×w pixels on the page, rotated 90° CW
    bool  rotated;                      // 90° clockwise (TexturePacker/free-tex-packer; Spine differs: CCW — exporter's problem)
    // trim reconstruction
    bool  trimmed;
    struct { int x, y, w, h; } spriteSourceSize; // where the trimmed pixels sit inside the original image
    struct { int w, h; } sourceSize;             // original (untrimmed) image size
    // gameplay metadata (per-sprite overrides from project file)
    struct { float x, y; } pivot;       // normalized 0..1 in sourceSize space; default 0.5,0.5
    struct { int l, t, r, b; } nine_slice; bool has_nine_slice;
    // polygon packing (optional; atlasc --mesh / TexturePacker polygon mode)
    struct { float x, y; } *verts;  int vert_count;   // in sprite space; UVs derived from frame
    uint16_t *indices;              int index_count;  // triangles
    // dedupe
    int   alias_of;                     // -1, or index of identical sprite (detectIdentical)
} ntp_sprite;

typedef struct ntp_page  { const char *image_name; int w, h; /* + pixels */ } ntp_page;
typedef struct ntp_result{ ntp_page *pages; int page_count;
                           ntp_sprite *sprites; int sprite_count;   // sorted by name: deterministic
                           const char *format;  float scale; } ntp_result;
```

Invariants worth enforcing with asserts (they're where every packer bug hides):

- `!trimmed ⇒ spriteSourceSize == {0,0,sourceSize.w,sourceSize.h}` and `frame.w/h == sourceSize.w/h`.
- `trimmed ⇒ frame.w == spriteSourceSize.w && frame.h == spriteSourceSize.h` (frame is always the *trimmed* size, pre-rotation).
- Reconstruction rule consumers rely on: draw texel `frame` (un-rotating if needed) at offset `(spriteSourceSize.x, spriteSourceSize.y)` inside a `sourceSize` canvas; pivot applies to that canvas.
- `trimMode == crop` is an *export-time* rewrite (zero the offsets, shrink sourceSize), never stored in the model — free-tex-packer does this in `prepareData`.
- Rotation direction must be pinned in docs (TexturePacker exports CW for most formats and its exporter descriptor even declares `rotationDirection` per target; Spine's atlas format means CCW by `rotate: true`). Keep CW in the model; let exporters/templates flip via the `mirror`-style formatters.

Derived conveniences to precompute for templates (free-tex-packer does): `hw/hh` (half sizes), `index/first/last` flags per sprite, and per-page `imageName/imageWidth/imageHeight`.

---

## Sources

- free-tex-packer GUI: https://github.com/odrick/free-tex-packer — incl. raw files `src/client/platform/electron/Project.js`, `.../FileSystem.js`, `src/client/ui/PackProperties.jsx`, `package.json`, repo tree
- free-tex-packer-core: https://github.com/odrick/free-tex-packer-core — incl. raw `exporters/index.js`, `exporters/list.json`, `exporters/JsonHash.mst`, `exporters/Css.mst`, `exporters/Phaser3.mst`, `exporters/Spine.mst`
- free-tex-packer-cli: https://github.com/odrick/free-tex-packer-cli — incl. raw `index.js`
- crunch: https://github.com/ChevyRay/crunch (binary format spec, options, hash cache)
- Cheetah-Texture-Packer: https://github.com/scriptum/Cheetah-Texture-Packer
- ShoeBox: https://renderhjs.net/shoebox/ and https://renderhjs.net/shoebox/packSprites.htm (template placeholders)
- Leshy SpriteSheet Tool: https://www.leshylabs.com/apps/sstool/
- texpack: https://github.com/urraka/texpack
- atlasc: https://github.com/septag/atlasc
- rTexPacker: https://raylibtech.itch.io/rtexpacker
- TexturePacker custom exporters (exporter.xml, Grantlee templates, sprite properties): https://www.codeandweb.com/texturepacker/documentation/custom-exporter
- TexturePacker settings / CLI / .tps relative paths: https://www.codeandweb.com/texturepacker/documentation/texture-settings
- Canonical JSON-hash example incl. smartupdate key: https://github.com/fzipp/texturepacker (`spritesheet_test.go`)
- MaxRects reference implementation (Jukka Jylänki): https://github.com/juj/RectangleBinPack
- Phaser ecosystem: https://phaser.io/news/2020/02/free-texture-packer , https://phaser.io/news/2019/01/atlas-packer-phaser , https://help.phasereditor2d.com/v2/texture-packer-editor.html
