# libGDX TexturePacker & Spine Atlas Packing — Research

Research date: 2026-07-10. Sources: libGDX wiki, libGDX `master` source (verified line-by-line against
`TexturePacker.java`, `TextureAtlas.java`, `ImageProcessor.java`, `MaxRectsPacker.java`,
`TexturePackerFileProcessor.java`), Esoteric Software (Spine) docs, and the
crashinvaders/gdx-texture-packer-gui source.

## Summary

- The **libGDX `.atlas` format** is a simple line-based text format: page entries (image file + key:value
  properties) followed by region entries (name + key:value properties), pages separated by blank lines.
  There are two dialects produced by the same writer, toggled by the `legacyOutput` setting:
  - **Legacy** (default in gdx-tools, required by Spine runtimes < 4.0): fields `rotate/xy/size/orig/offset/index`,
    every field always written, file starts with a blank line, no `pma` field.
  - **New** (libGDX 1.9.13+, Spine 4.0+): fields `index/bounds/offsets/rotate/split/pad`, default-valued
    fields omitted, much smaller files, supports `pma` and arbitrary rotation degrees, plus arbitrary
    custom key:value data per region.
  One parser (`TextureAtlas.TextureAtlasData`) reads both — the field names are what differ, not the syntax.
- **`pack.json`** is a direct JSON serialization of `TexturePacker.Settings` (~40 fields). A `pack.json` can be
  placed in any input subdirectory; settings inherit from the nearest ancestor `pack.json` and fields present
  locally override. Each directory is packed as a separate packer invocation whose pages are **appended** to
  one `.atlas` file; `combineSubdirectories` merges a subtree into one invocation.
- **Packing algorithm**: a Java port of Jukka Jylänki's *maximal rectangles* algorithm with 5 placement
  heuristics tried exhaustively, wrapped in a binary search over page width/height (with POT / multiple-of-4 /
  square constraints) to find the smallest page that fits. `fast` mode skips the brute-force insert order
  search; `grid` mode replaces MaxRects with a uniform-cell packer.
- **Spine's editor packer** is the same codebase/design (both written by Nathan Sweet); it exposes the same
  settings (plus Spine-only ones: polygon packing, mesh-aware whitespace stripping, "atlas per skeleton",
  auto scale) and reads the same `pack.json`. Attachment names in skeleton data are relative image paths, which
  is exactly what region names are — that's how runtimes bind attachments to atlas regions.
- **gdx-texture-packer-gui** is the reference OSS GUI: multi-atlas project files (`.tpproj`, a hybrid
  key=value + embedded-JSON text format), per-atlas settings, typed input lists (include dir/file, ignore,
  programmatic ninepatch, custom region name), scale-factor UI with per-scale resampling, a ninepatch editor,
  a texture unpacker, PNG compressors, KTX2/Basis Universal export, and a headless `--batch` CLI over the
  same project file.

---

## libGDX `.atlas` format spec

### Common syntax (both variants)

- Plain text, UTF-8, line-based. Lines are trimmed before interpretation; indentation is cosmetic only.
- Two kinds of lines:
  - **Name lines** — no `:` — either a page image filename or a region name.
  - **Field lines** — `key: v1, v2, v3, v4` — key before the first `:`, then up to **4** comma-separated
    values (the reference parser reads at most 4 and each value is trimmed).
- **File structure / state machine** (exactly as `TextureAtlasData.load` implements it):
  1. Skip blank lines at the start of the file.
  2. Any field lines before the first name line are a *header* and are **silently ignored** (reserved).
  3. A name line when no page is active starts a **page**: the line is the page image filename
     (relative to the atlas file's directory). Subsequent field lines are page fields.
  4. A name line while a page is active starts a **region** belonging to that page; subsequent field
     lines are region fields.
  5. A **blank line ends the current page**; the next name line starts a new page.
  6. Unknown **page** fields are silently ignored. Unknown **region** fields are preserved as custom
     `name -> int[]` data (Spine uses this for user data; values that aren't integers parse as 0).
- Consequence of the syntax: **region and page names must not contain `:`** (a colon turns the line into a
  field line) and cannot be empty or consist of whitespace. Leading/trailing spaces in names are lost (trim).
- Coordinates are integer pixels. **Origin is the top-left corner of the page image, y grows downward.**
- One `.atlas` file may describe multiple pages, and the gdx writer opens the file in **append mode** — a
  second packer invocation with the same output appends more pages to the same file (it first re-parses the
  file and errors on duplicate region names). A parser must accept pages after regions after pages, repeatedly.

### Page fields

| Field    | Values                                                                | Default when absent | Notes |
|----------|-----------------------------------------------------------------------|---------------------|-------|
| `size`   | `width, height` (ints)                                                | 0,0                 | Page image dimensions. Always written by gdx. |
| `format` | `Alpha`, `Intensity`, `LuminanceAlpha`, `RGB565`, `RGBA4444`, `RGB888`, `RGBA8888` | `RGBA8888` | In-memory pixel format hint for the runtime (not the file encoding). |
| `filter` | `minFilter, magFilter` — each one of `Nearest`, `Linear`, `MipMap`, `MipMapNearestNearest`, `MipMapLinearNearest`, `MipMapNearestLinear`, `MipMapLinearLinear` | `Nearest, Nearest` | Any `MipMap*` min filter implies mipmap generation at load. |
| `repeat` | `x`, `y`, `xy`, `none`                                                | `none` (ClampToEdge)| Parser just checks for the letters `x`/`y` in the value. |
| `pma`    | `true` / `false`                                                      | `false`             | Premultiplied alpha. **New format only** — the legacy writer never emits it. |

### Region fields

| Field     | Values | Default when absent | Meaning |
|-----------|--------|---------------------|---------|
| `bounds`  | `x, y, w, h` | 0,0,0,0 | **New.** Position of the region's top-left corner in the page (y-down) and its packed size. Replaces `xy` + `size`. |
| `xy`      | `x, y` | 0,0 | **Legacy.** Top-left corner in the page. |
| `size`    | `w, h` | 0,0 | **Legacy.** Packed (post-whitespace-strip) size. If rotated, this is still the *unrotated* w×h; the pixels occupy h×w on the page. |
| `offsets` | `offsetX, offsetY, origW, origH` | 0,0,packed size | **New.** Whitespace-strip restore data: pixels stripped from the **left** and **bottom** edges, plus the original image size. Replaces `offset` + `orig`. |
| `offset`  | `offsetX, offsetY` | 0,0 | **Legacy.** Pixels stripped from left/**bottom** (bottom-left convention, matching y-up sprite space at runtime — note this is the opposite vertical convention from `bounds`/`xy`). |
| `orig`    | `origW, origH` | packed size | **Legacy.** Original size before whitespace stripping (for `.9` ninepatches: size *after* the 1px border strip — ninepatches are never whitespace-stripped). |
| `rotate`  | `true`, `false`, or integer degrees 0–359 | `false` (0) | `true` == 90. The packed pixels are stored rotated **90° counter-clockwise** relative to the source image (per libGDX `AtlasRegion` docs; degrees are CCW). gdx's writer only ever emits `rotate: true`; Spine 4 can emit degree values. Omitted when not rotated (new format); always written in legacy. |
| `index`   | integer, `-1` = none | -1 | Animation-frame index parsed from a trailing `_N` in the source filename (see `useIndexes`). Regions sharing a name are retrieved as an index-ordered list; the loader sorts regions by index when any region has one (−1 sorts last). |
| `split`   | `left, right, top, bottom` | null | Ninepatch stretch insets (from the `.9.png` border markers). |
| `pad`     | `left, right, top, bottom` | null | Ninepatch content padding. If pads exist but splits don't, the writer emits `split: 0, 0, 0, 0` first — so `pad` never appears without `split`. |
| *(other)* | up to 4 ints | — | Any other key is application/custom data (new-format loaders keep it; Spine uses e.g. custom region attributes). |

The parser applies `if (originalWidth == 0 && originalHeight == 0) { originalWidth = width; originalHeight = height; }` after reading a region, so `offsets`/`orig` may be omitted for unstripped images.

### Legacy variant (gdx-tools default, `legacyOutput: true`)

Writer behavior (verbatim semantics from `writePageLegacy` / `writeRectLegacy`):

- Every page is preceded by a blank line — **including the first**, so the file starts with `\n`.
- Page always writes all of: name, `size`, `format`, `filter`, `repeat` (with literal `none` when unset).
  There is **no `pma`** in legacy output — premultiplication is not recorded.
- Every region writes **all** fields, in this exact order, with two-space indent and `", "` separators:
  `rotate`, `xy`, `size`, [`split`], [`pad`], `orig`, `offset`, `index`.
- `prettyPrint` has no effect on legacy output.

Real example (two pages; one ninepatch; one indexed animation frame; one rotated region):

```

sprites.png
size: 256, 128
format: RGBA8888
filter: Nearest, Nearest
repeat: none
ui/button.9 → written as ui/button
ui/button
  rotate: false
  xy: 2, 2
  size: 64, 32
  split: 8, 8, 8, 8
  pad: 4, 4, 4, 4
  orig: 64, 32
  offset: 0, 0
  index: -1
walk
  rotate: false
  xy: 68, 2
  size: 46, 52
  orig: 48, 56
  offset: 1, 2
  index: 3
tree
  rotate: true
  xy: 116, 2
  size: 60, 40
  orig: 60, 40
  offset: 0, 0
  index: -1

sprites2.png
size: 128, 64
format: RGBA8888
filter: Linear, Linear
repeat: none
cloud
  rotate: false
  xy: 2, 2
  size: 100, 40
  orig: 100, 40
  offset: 0, 0
  index: -1
```

(The `ui/button.9 → ...` line above is annotation, not file content. `tree` occupies 40×60 pixels on the
page because it is rotated; `size` still says 60×40.)

### New variant (libGDX 1.9.13+, Spine 4.0+, `legacyOutput: false`)

Writer behavior (verbatim semantics from `writePackFile` / `writePage` / `writeRect`):

- No leading blank line. A single blank line is written **between** pages (before every page except the
  first, or before the first too when appending to an existing file).
- `prettyPrint: true` (default) → fields indented with one **tab**, separators `": "` and `", "`.
  `prettyPrint: false` → no indent, separators `":"` and `","` (no spaces).
- Page: name line, then `size` (always); `format` only if ≠ `RGBA8888`; `filter` only if either ≠ `Nearest`;
  `repeat` only if some wrap is `Repeat` (`x`/`y`/`xy`); `pma: true` only if `premultiplyAlpha`.
- Region field order: `index` (only if ≠ −1), `bounds` (always), `offsets` (only if
  `offsetX != 0 || offsetBottom != 0 || origW != w || origH != h`), `rotate` (only if rotated, as `true`),
  `split` (if any), `pad` (if any, preceded by zero `split` when splits are null).

The same content as above in the new format (pretty-printed; tabs shown as spaces here):

```
sprites.png
	size: 256, 128
ui/button
	bounds: 2, 2, 64, 32
	split: 8, 8, 8, 8
	pad: 4, 4, 4, 4
walk
	index: 3
	bounds: 68, 2, 46, 52
	offsets: 1, 2, 48, 56
tree
	bounds: 116, 2, 60, 40
	rotate: true

sprites2.png
	size: 128, 64
	filter: Linear, Linear
cloud
	bounds: 2, 2, 100, 40
```

Spine 4.x export of the same idea, with `pma` and custom degree rotation (from the official
format doc — Spine's writer, unlike gdx's, may emit degree values and custom fields):

```
page1.png
   size: 640, 480
   format: RGBA8888
   filter: Linear, Linear
   repeat: none
   pma: true
dagger
   bounds: 372, 100, 26, 108
head
   index: 0
   bounds: 2, 21, 103, 81
   rotate: 90

page2.png
   size: 640, 480
   format: RGB565
   filter: Nearest, Nearest
   repeat: x
bg-dialog
   index: -1
   rotate: false
   bounds: 519, 223, 17, 38
   offsets: 2, 2, 21, 42
   split: 10, 10, 29, 10
   pad: -1, -1, 28, 10
```

### Exporter checklist (what a correct writer must get right)

1. `bounds`/`xy` y-coordinate is measured from the **top** of the page; `offset(s)` vertical component is
   pixels stripped from the **bottom** of the source image.
2. Rotated regions: write source-orientation `w, h` in `bounds`/`size`; place `h × w` pixels on the page,
   rotated 90° CCW; write `rotate: true`.
3. Region names: input path relative to the pack root, `\` → `/`, file extension stripped, `.9` stripped,
   `_N` suffix stripped into `index` (when indexes are enabled). Reject/escape `:` in names.
4. Ninepatch `split` is mandatory whenever `pad` is written.
5. Legacy: write every field every time, blank line before every page, no `pma`.
   New: omit defaults exactly as listed, blank line only between pages.
6. Filenames on page lines are relative to the atlas file location (loader does `imagesDir.child(line)`).

---

## `pack.json` settings reference

`pack.json` is `TexturePacker.Settings` serialized as JSON (field-for-field; partial files are fine — absent
fields keep inherited/default values). Defaults below are verified against the current `Settings` class.

### Output geometry

| Setting | Default | Meaning |
|---|---|---|
| `pot` | `true` | Force power-of-two page dimensions. |
| `multipleOfFour` | `false` | Force page dimensions divisible by 4 (needed by block compressors like ETC/BCn). (Note: absent from the older wiki table but present in code.) |
| `minWidth` / `minHeight` | 16 / 16 | Minimum page size. |
| `maxWidth` / `maxHeight` | 1024 / 1024 | Maximum page size; a region larger than this (after padding) is a hard error. |
| `square` | `false` | Force width == height (PVRTC etc.). |
| `grid` | `false` | Uniform-grid packing (cell = largest input) in order, instead of MaxRects. |

### Padding & bleeding

| Setting | Default | Meaning |
|---|---|---|
| `paddingX` / `paddingY` | 2 / 2 | Blank pixels between packed images. |
| `edgePadding` | `true` | Also apply half the padding at the page borders. |
| `duplicatePadding` | `false` | Copy region edge pixels into the padding gutter (hides linear-filter/mipmap seams; wants padding ≥ 2). |
| `bleed` | `true` | Flood RGB of fully transparent pixels from nearest opaque pixels (fixes dark halos with non-PMA linear filtering). PNG output only. |
| `bleedIterations` | 2 | Number of bleed dilation passes. |

### Input processing

| Setting | Default | Meaning |
|---|---|---|
| `rotation` | `false` | Allow 90° rotation for tighter packing (runtime must support `rotate`). |
| `stripWhitespaceX` / `stripWhitespaceY` | `false` | Trim blank columns/rows at the region edges; trimmed amounts recorded in `offset(s)`. |
| `alphaThreshold` | 0 | Alpha ≤ this counts as blank for whitespace stripping (0–255). |
| `alias` | `true` | CRC-deduplicate pixel-identical images; duplicates become extra region entries over the same bounds. |
| `ignoreBlankImages` | `true` | Fully transparent images produce no region at all. |
| `useIndexes` | `true` | Parse trailing `_N` on names (regex `(.+)_(\d+)$`) into the `index` field and strip it from the name. Set `false` to keep literal names. |
| `premultiplyAlpha` | `false` | Multiply RGB by A in the output pages (and write `pma: true` in the new format). |

### Texture/runtime hints (written into the atlas, don't affect packing)

| Setting | Default | Meaning |
|---|---|---|
| `filterMin` / `filterMag` | `Nearest` / `Nearest` | Page `filter` field. `MipMap*` values imply mipmaps. |
| `wrapX` / `wrapY` | `ClampToEdge` | Page `repeat` field (`Repeat` → `x`/`y`/`xy`). |
| `format` | `RGBA8888` | Page `format` field (in-memory format hint). |

### Output files

| Setting | Default | Meaning |
|---|---|---|
| `outputFormat` | `"png"` | Page image encoding: `png` or `jpg` (JPEG output composites onto opaque). |
| `jpegQuality` | 0.9 | JPEG quality 0–1. |
| `atlasExtension` | `".atlas"` | Extension appended to the pack file name. |
| `prettyPrint` | `true` | New format only: tab indent + spaces after `:`/`,`. (The wiki's description of this flag is inverted/garbled; behavior per code is as stated here.) |
| `legacyOutput` | `true` | Emit the legacy dialect (see spec above). gdx-tools still defaults to legacy for backwards compatibility; new projects should set `false`. |

### Scale variants

| Setting | Default | Meaning |
|---|---|---|
| `scale` | `[1]` | For each entry, all inputs are scaled by the factor and a complete atlas (pages + pack file) is emitted. |
| `scaleSuffix` | `[""]` | Per-scale filename suffix. Naming rule (`getScaledPackFileName`): if the suffix is non-empty it is appended to the pack file name (`pack@2x.atlas`); otherwise, if there are multiple scales, output goes into a subdirectory named after the scale value (`2/pack.atlas`, `0.5/pack.atlas`); a single scale of 1 with empty suffix changes nothing. |
| `scaleResampling` | `[bicubic]` | Per-scale interpolation: `nearest`, `bilinear`, `bicubic` (downscale < 1 uses area-averaging regardless). |

Ninepatches (`*.9.png`) are scaled too; their splits/pads are read from the 1px border *before* scaling and
the border is stripped before scaling.

### Directory / invocation control

| Setting | Default | Meaning |
|---|---|---|
| `combineSubdirectories` | `false` | Pack this directory **and** its whole subtree in one invocation (one page set) — except subtrees that contain their own `pack.json`, which always pack separately (they have different settings). |
| `flattenPaths` | `false` | Strip subdirectory prefixes from region names at write time (`a/b/hero` → `hero`). |
| `ignore` | `false` | Skip this directory entirely. (Code-only; not in the old wiki table.) |
| `fast` | `false` | Faster, less optimal packing (see algorithm section). |
| `limitMemory` | `true` | Keep only one source image in RAM (each image decoded twice). |
| `debug` | `false` | Draw magenta rectangles around regions/pages in the output images. |
| `silent` | `false` | Suppress console progress output. (Code-only.) |

### Settings cascade & directory semantics (`TexturePackerFileProcessor`)

- All `pack.json` files under the input root are collected first and sorted shallowest-first. Each one's
  effective settings = a **copy of the nearest ancestor `pack.json`'s settings** (or the invocation defaults)
  with the local file's JSON fields merged over it (`json.readFields` — only present fields override).
- When packing, each directory resolves its settings by walking up to the nearest directory that had a
  `pack.json`.
- **Every directory containing images is packed as its own invocation** (its images never share pages with
  other directories), but all invocations **append** their pages to the same output `.atlas` file, and region
  names keep the path prefix relative to the input root. Cross-invocation duplicate region names are an error.
  `combineSubdirectories: true` merges a subtree into a single invocation (better page utilization).
- Files inside a directory are sorted by name with numeric-suffix awareness (`walk_2` before `walk_10`).
- Input extensions: `.png`, `.jpg`, `.jpeg`.
- CLI: `java -cp gdx.jar;gdx-tools.jar com.badlogic.gdx.tools.texturepacker.TexturePacker inputDir [outputDir] [packFileName] [settingsFile]`
  (1–4 args; `packFileName` defaults to `pack.atlas`; a trailing `.atlas` on it is stripped before the
  extension from settings is re-appended). Old output (`.atlas` + page images matching the scaled name
  patterns) is deleted before packing.

---

## Packing algorithm notes

`TexturePacker` picks the packer by settings: `grid: true` → `GridPacker`, else `MaxRectsPacker`
(a port of Jukka Jylänki's maximal-rectangles bin packing, "Even more rectangle bin packing").

Pipeline per invocation (per scale factor):

1. **ImageProcessor** loads each image → converts to RGBA8888 → for `.9.png`: read splits/pads from the 1px
   border, strip the border, mark non-rotatable and non-strippable → scale by the current scale factor →
   parse `_N` index → whitespace-strip (unless ninepatch) → CRC hash; identical CRCs become **aliases** of one
   rect instead of separate rects.
2. Rects get `paddingX/paddingY` added to their dimensions for packing purposes; page free area is reduced by
   `edgePadding` when enabled.
3. **Page size search (`BinarySearch`)**: for each page, binary-search the smallest working size between
   `minWidth/Height` and `maxWidth/Height`, width and height searched independently (a single combined search
   when `square`). In POT mode the search runs over exponents of two; `multipleOfFour` rounds candidates up to
   %4==0; otherwise a "fuzziness" threshold (15 normally, 25 in `fast` mode) stops the search early for speed.
4. **Placement**: at each candidate size, try **all five** MaxRects free-rectangle heuristics —
   BestShortSideFit, BestLongSideFit, BestAreaFit, BottomLeftRule, ContactPointRule.
   - Normal mode is brute force: at every step, score *every* remaining rect against every free rect and
     insert the globally best (both orientations scored when `rotation` is enabled).
   - `fast` mode pre-sorts rects (by longest side desc when rotation is on, by width desc otherwise) and
     inserts them in that order with the heuristic, no global re-scoring.
   - The best heuristic result is kept by comparing **occupancy** (packed area / page area).
5. Rects that don't fit remain in the input set and the loop emits another page (`while (inputRects.size > 0)`).
6. **Output**: pages are drawn (rotated blits for rotated rects, `duplicatePadding` edge copies,
   `bleed` dilation on PNG, optional PMA, optional debug rectangles), written as `png`/`jpg`, and the pack
   file is appended (see format spec). Region entries are written sorted, aliases right after their source rect.

GridPacker simply lays rects into uniform cells (cell = max rect w/h) left-to-right, top-to-bottom, in input
order — the atlas equivalent of a classic spritesheet.

---

## Spine packing specifics

Spine's editor embeds what is essentially the same texture packer (same author — Nathan Sweet — same settings
vocabulary, same `pack.json` per-folder cascade, same `.atlas` output; Spine ≥ 4.0 writes the new dialect and
has a **Legacy output** checkbox for pre-4.0 runtimes). Differences and Spine-specific behavior:

### Settings dialog deltas vs. libGDX defaults

- `alphaThreshold` default **3** (vs 0), `filterMin/Mag` default **Linear** (vs Nearest),
  `maxWidth/maxHeight` default **2048** (vs 1024), `premultiplyAlpha` default **true** (vs false;
  PMA + bleed are mutually relevant — bleed is the non-PMA seam fix).
- **Packing method** selector: `Grid` / `Rectangles` (default) / **`Polygons`** — polygon packing traces the
  useful (non-transparent / mesh-hull) area and packs polygons, allowing regions to overlap where both are
  transparent. Slower, needs the project for context; the `.atlas` file still stores rectangular `bounds`,
  so the format is unchanged.
- **Divisible by 4** (= `multipleOfFour`), **Square**, **Power of two** as in libGDX.
- **Auto scale**: reduces the scale until everything fits a single page (useful for LOD/preview builds).
- **Current project**: use the open project's **mesh UVs** to decide what counts as whitespace for mesh
  attachment images — pixels outside the mesh are strippable even if opaque. When packing happens as part of
  a data export this is forced on ("meshes in the current project are always used to strip whitespace").
- **Duplicate padding** is applied selectively: only for images used by region attachments or by meshes that
  cover the whole image (a partially-covering mesh never samples the edge, so padding duplication is skipped).

### Whitespace stripping ↔ meshes / unpacking

- Stripped amounts go into `offsets` exactly as in libGDX; runtimes reconstruct the original quad from
  `offsets` + `bounds` (+ `rotate`). Mesh attachments map their UVs into the packed region rectangle.
- Spine ships a **Texture Unpacker** (also in the editor CLI) that inverts packing: un-rotates, restores
  stripped whitespace from `offsets`, and can un-premultiply — handy round-trip validation for any exporter
  (our tool should be able to survive pack → unpack → repack).

### Naming & folder conventions

- Skeleton **attachment names are relative paths** under the project's images folder (e.g. `red/head` →
  `./images/red/head.png`, extension omitted, forward slashes). Since region names are exactly
  "path relative to pack root, extension stripped", packing the images folder yields region names that match
  attachment names 1:1 — that's the entire runtime lookup contract. An attachment's `path` property can
  override the name for lookup.
- Folder structure is the page-grouping tool: "images in the same folder go on the same set of pages";
  subfolders segregate content to minimize texture binds and to give groups different `pack.json` settings.
- `Flatten paths` and `Indexes` (underscore-suffix stripping) mutate region names, so Spine users are warned:
  attachments named `foo_2` will not resolve if index stripping is on — disable `useIndexes` when names
  contain `_N` legitimately.

### Packing during data export

- Export dialog (JSON/binary) has a **Pack** checkbox plus:
  - **Attachments** mode: pack only images actually used by attachments; `pack.json` files are ignored.
  - **Image folder** mode: pack everything under each skeleton's images path; per-folder `pack.json` honored.
  - **Atlas per skeleton** (atlas named after the skeleton data file) vs. **single atlas** (named after the
    project file).
- Export **scale** variants produce one atlas per scale, using the suffix/subfolder naming scheme described
  under `scaleSuffix` above (Spine default: blank suffix → per-scale subfolders).

---

## gdx-texture-packer-gui study

Repo: `crashinvaders/gdx-texture-packer-gui` (Apache-2.0, Java/libGDX + LWJGL3, ~700 stars; successor to
Aurelien Ribon's TexturePacker-GUI). It wraps the real `TexturePacker.Settings`/packer classes, so results
are bit-identical to gdx-tools.

### Project file format (`.tpproj`)

Plain text, custom hybrid format (verified in `ProjectSerializer.java`):

- The file is a list of **pack sections** separated by a line `---`, followed by one **project section**
  introduced by the divider `-PROJ-`.
- Each pack section is `key=value` lines:
  - Identity: `name=`, `filename=` (output atlas filename override), `output=` (output dir, stored relative
    to the project file, resolved back to absolute on load).
  - A fixed subset of `TexturePacker.Settings`, one per line (`alias=`, `alphaThreshold=`, `debug=`,
    `duplicatePadding=`, `edgePadding=`, `fast=`, `filterMag=`, `filterMin=`, `ignoreBlankImages=`,
    `maxHeight=`, `maxWidth=`, `minHeight=`, `minWidth=`, `paddingX=`, `paddingY=`, `pot=`, `mof=`,
    `rotation=`, `stripWhitespaceX=`, `stripWhitespaceY=`, `wrapX=`, `wrapY=`, `premultiplyAlpha=`,
    `grid=`, `square=`, `bleed=`, `limitMemory=`, `useIndexes=`, `prettyPrint=`, `legacyOutput=`).
    Missing keys fall back to libGDX defaults — forward/backward compatible by construction.
    Notably `format`, `outputFormat`, `jpegQuality` are deliberately **not** stored per pack — image encoding
    is a project-level "file type" concern (see below).
  - `scaleFactors=` — embedded JSON array of `{suffix, factor, resampling}` objects (per-scale resampling!).
  - `inputFiles=` — embedded JSON array (see below).
  - `keepInputFileExtensions=` — append source extension to region names (option added in 4.10.1).
- Project section: `version=` (app version, used for load-time migrations), `fileTypeType=` (`png`, `jpg`,
  `basis`) + `fileTypeData=` (serialized per-type state: PNG compression backend & options, JPEG quality,
  Basis/KTX2 params), `previewBackgroundColor=`, `projectSettings=`.
- Versioned migrations on load (e.g. pre-4.4 `input=` directory entries are converted to the typed input-file
  list; obsolete `ktx` file type auto-migrates to `basis`).

**Input file entries** (`InputFileSerializer`): each is
`{path (relative to project), type: Input|Ignore, ...}` where a **directory Input** carries
`dirFilePrefix` (prefix prepended to region names from that dir), `recursive`, `flattenPaths`; a **file
Input** carries optional `regionName` override and an optional `ninepatch: {splits:[l,r,t,b], pads:[l,r,t,b]}`
block — a "programmatic ninepatch" defined in the project instead of baked into a `.9.png`. `Ignore` entries
exclude files matched by directory entries. This include/exclude list replaces libGDX's
directory-walk + `pack.json` model.

### UX / features worth stealing

- **Multiple atlases ("packs") per project**, each with its own settings, input list, scale factors and output
  dir; pack one / selected / all (`packMultipleAtlases`); clone pack; per-pack "new/changed" indicators.
- **Atlas preview canvas**: page paging, zoom, region outlines, hover shows region size, hovering an input
  file highlights its region, configurable background color, async page loading.
- **Ninepatch editor**: create 9-patch data on plain images on the fly (stored programmatically in the
  project) or author real `.9.png` files; preview pane and numeric fields.
- **Texture unpacker** tool (extract regions from an existing atlas).
- **Image encoding pipeline** decoupled from packing: PNG with pluggable compressors (pngtastic, Zopfli,
  indexed PNG8 via anim8-gdx, pngquant/imagequant, TinyPNG cloud service), JPEG, and **KTX2 / Basis
  Universal** (UASTC + zstd, ETC1S; mipmap chains generated when a `MipMap*` min filter is selected).
  Legacy ETC1/ETC2/KTX support was dropped in favor of KTX2.
- **Safety**: refuses to write output into any directory that overlaps the input files (prevents clobbering
  sources); save prompt on close; crash reporting.
- **CLI batch mode** over the same project file — CI-friendly:
  `gdx-texture-packer --batch --project "/path/to/project.tpproj" --atlases "atlas_a" "atlas_b"`,
  plus a standalone compressor `gdx-texture-packer --basis-pack --container ktx2 --format uastc file.png`;
  a Dockerfile ships in the repo.
- Misc: OS-native file dialogs, drag-and-drop of images/projects, customizable hotkeys, i18n via
  `bundle_XX.properties`, interface scaling, "show in file manager".
- **No file-watch / hot-reload**: repacking is manual (button/hotkey) or via the CLI; a watch mode would be a
  genuine differentiator for our tool.

---

## Lessons for our tool

**Format & compatibility**
1. Implement the `.atlas` writer against the exact rules above (both dialects behind one flag, like
   `legacyOutput`). Default to the new dialect; keep legacy for Spine <4.0 runtimes and old gdx.
   Parse both plus degree rotations and unknown key:value region data (pass-through, don't drop).
2. Get the two coordinate conventions right: `bounds` is top-left/y-down; `offsets` vertical component is
   bottom-based. This is the most common exporter bug.
3. Respect the append-multiple-page-sets property of the format, or at least parse files that contain it.
4. `pma` is only expressible in the new dialect — warn if user picks PMA + legacy output.
5. Validate names: no `:`; normalized `/` separators; document the `_N` index and `.9` conventions and make
   both opt-out (`useIndexes`, treating `.9` literally).

**Packing core**
6. MaxRects with the 5 heuristics + occupancy comparison + binary-searched page size (POT / mult-of-4 /
   square constraints) is the proven baseline; add `fast` (sorted single-pass) and `grid` modes. Polygon
   packing (Spine) is the advanced tier — format-compatible since bounds stay rectangular.
7. Ship the full processing set: whitespace strip w/ `alphaThreshold`, CRC aliasing, blank-image skip,
   rotation, `edgePadding`/`duplicatePadding`, bleed (dilation) for non-PMA, PMA, scale variants with
   per-scale resampling and the suffix-or-subdirectory naming rule.

**Configuration model**
8. Support both config styles: cascading per-directory `pack.json` (CLI/asset-pipeline friendly; nearest-
   ancestor inheritance, field-level override) and a GUI project file with per-atlas settings + typed
   include/exclude input lists (gdx-tpgui's model). Make settings keys a superset of libGDX's so existing
   `pack.json` files import cleanly.
9. Store paths relative to the project file; version the project format and migrate on load (tpgui does both
   and it visibly pays off).
10. Keep image *encoding* (PNG optimizers, JPEG, KTX2/Basis) orthogonal to *packing* settings, as tpgui does.

**Tooling & UX**
11. Provide: preview with region hover/highlight + input↔region cross-highlight, programmatic ninepatch
    editing (data in project, not only `.9.png`), a texture **unpacker** (also great for round-trip tests),
    headless CLI over the same project file, and the output-overlaps-input safety check.
12. A file-watch/hot-reload mode is missing from all three studied tools — cheap differentiation.
13. Spine interop story: read/write Spine-compatible atlases, keep region names == relative paths so Spine
    attachment lookup works, and consider "atlas per skeleton" style grouping (per-folder → per-page) with a
    `combineSubdirectories`-like merge toggle.

---

## Sources

- libGDX TexturePacker wiki: https://libgdx.com/wiki/tools/texture-packer
- libGDX source (master, verified 2026-07-10):
  - `extensions/gdx-tools/src/com/badlogic/gdx/tools/texturepacker/TexturePacker.java` (writer, `Settings`)
  - `extensions/gdx-tools/src/com/badlogic/gdx/tools/texturepacker/MaxRectsPacker.java` (algorithm)
  - `extensions/gdx-tools/src/com/badlogic/gdx/tools/texturepacker/ImageProcessor.java` (names, `.9`, `_N`, strip, alias)
  - `extensions/gdx-tools/src/com/badlogic/gdx/tools/texturepacker/TexturePackerFileProcessor.java` (`pack.json` cascade)
  - `gdx/src/com/badlogic/gdx/graphics/g2d/TextureAtlas.java` (`TextureAtlasData` parser, both dialects)
- Spine atlas format spec: https://esotericsoftware.com/spine-atlas-format
- Spine texture packer docs: https://esotericsoftware.com/spine-texture-packer (incl. `#Packing-during-data-export`)
- Spine images/attachment naming: https://esotericsoftware.com/spine-images
- Spine export docs: https://esotericsoftware.com/spine-export
- gdx-texture-packer-gui: https://github.com/crashinvaders/gdx-texture-packer-gui (README, `CHANGES.md`,
  `core/src/com/crashinvaders/texturepackergui/controllers/projectserializer/*.java`,
  `controllers/model/*.java`)
- MaxRects reference: Jukka Jylänki, "Even more rectangle bin packing" / "A Thousand Ways to Pack the Bin"
