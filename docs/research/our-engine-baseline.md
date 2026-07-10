# Baseline: what neotolis-engine already gives us

Inventory of existing engine capability relevant to the texture packer, from
`external/neotolis-engine`. This is the "what we have" column for the
competitor comparison.

## Packing pipeline (`tools/builder`, target `nt_builder`)

Source: `tools/builder/nt_builder.h`, `nt_builder_atlas.c`,
`nt_builder_atlas_geometry.c`, `nt_builder_atlas_vpack.c`;
spec: `docs/spec/builder/builder.md`.

**Algorithm** — NFP/Minkowski vector packer (not MaxRects/skyline):
sub-pixel exact, concave-aware via Clipper2, tries all 8 D4 orientations
(4 rotations × 2 flips) per sprite, NFP cache with seqlock, optional
multithreading (`nt_builder_set_threads[_auto]`). This is *stronger* than
what TexturePacker/libGDX/stb_rect_pack ship (they do rect or convex-ish
polygon packing; ours packs true concave silhouettes).

**Pipeline stages** (10): alpha_trim → cache_check → dedup → geometry →
tile_pack → compose → debug_png → cache_write → serialize → register.

**Per-atlas options** (`nt_atlas_opts_t`, defaults via
`nt_atlas_opts_defaults()`):

| Feature | Field / note |
|---|---|
| Max page size | `max_size` (default 2048, hard cap `NT_BUILD_MAX_TEXTURE_SIZE` 4096) |
| Padding between sprites | `padding` (default 2) |
| Page edge margin | `margin` |
| Extrude (edge duplication) | `extrude` (RECT shape only) |
| Alpha threshold for trim | `alpha_threshold` |
| Silhouette mode | `shape`: RECT / CONVEX_HULL / CONCAVE_CONTOUR |
| Polygon vertex budget | `max_vertices` (default 8, cap 16) |
| Rotation/flips | `allow_transform` (8 D4 orientations) |
| POT pages | `power_of_two` |
| Premultiplied alpha | `premultiplied` (default true) |
| Pixels-per-unit scale | `pixels_per_unit` (SD/HD variant support) |
| Sampler defaults | `filter_min/mag`, `wrap_u/v`, `gen_mipmaps` |
| GPU compression | `compress` → Basis Universal (encoder vendored) |
| Debug page PNGs | `debug_png` (via `stbi_write_png`) |

**Per-sprite options** (`nt_atlas_sprite_opts_t`): name, pivot
(`origin_x/y`, normalized over pre-trim source, may lie outside [0,1]),
slice9 borders (auto-forces RECT + no-rotate), per-sprite shape / rotate /
margin / extrude / max_vertices overrides.

**Inputs**: file (`nt_builder_atlas_add`, stb_image → PNG/JPG/BMP/TGA/GIF/
PSD/HDR/PNM), raw RGBA (`_add_raw`), glob (`_add_glob`,
`nt_builder_glob_iterate` reusable).

**Also in the pipeline**: alpha trimming, duplicate/alias detection (dedup
stage), content-hash build cache (skip unchanged), codegen of asset-ID
headers, fonts/meshes/shaders/blobs in the same pack (out of scope for the
tool but shows the pack model).

## Output formats (today)

- `.ntpack` (custom flat binary, `shared/include/nt_pack_format.h`) +
  generated `.h` ID header. The ONLY real output.
- Debug page PNGs (unpadded naming, debug purpose).
- **Gap**: composed page pixels + placement table (`AtlasPlacement`,
  `page_pixels[]`) are internal statics in `nt_builder_atlas.c` with no
  public getter → an export API must be added (engine issue/PR) or the
  exporter feeds off a placement callback. This is THE key engine change the
  tool needs.

## Runtime atlas (`engine/atlas`)

Loader only (`nt_atlas.h`): parses `NT_ASSET_ATLAS` from `.ntpack`, exposes
`nt_texture_region_t` (trim offsets, source size, pivot, slice9, transform,
u16-packed UVs). Useful for the GUI preview of `.ntpack` output and for
verification, not for packing.

## App/UI stack for the GUI frontend

- Native window: GLFW + OpenGL 3.3 (`engine/window/native`), app loop
  `nt_app_run(frame_fn)` (`engine/app`).
- Immediate-mode UI over Clay v0.14 (`engine/ui/nt_ui.h` + per-widget
  headers): buttons, checkbox/radio/switch, slider, progress, text input
  (caret/selection/clipboard), scroll areas, virtualized list, dropdown/
  combo, menus, tabbar, modal/popup/tooltip, slice9 panels, labels, images,
  rich text, inspector/debug overlay. Reference app:
  `examples/ui_showcase/main.c`.
- Text rendering with real fonts (`engine/font` + text renderer).

## Gaps the tool must fill

| Need | Status | Plan |
|---|---|---|
| Placement/page-pixels export API | ❌ internal statics | engine issue/PR |
| Exporters (Defold, JSON, PNG page export) | ❌ | `tp_core` exporter layer |
| Project file (save/load settings) | ❌ | `tp_core` (cJSON is vendored) |
| CLI arg parsing | ❌ (builder `main.c` is a stub) | `apps/cli` |
| File open/save dialogs, folder scan UI | ❌ no engine widget | OS-native dialogs (Win32/tinyfd) + `nt_builder_glob_iterate` |
| Directory enumeration / file write in `engine/fs` | ❌ read-only API | plain stdio + glob util (builder already does this) |
| Image preview of arbitrary source files in GUI | ⚠️ runtime loads no source images | stb_image (already linked via builder) → `nt_gfx` texture |
| Watch mode / auto-repack | ❌ | `tp_core` (content hashes already exist in cache stage) |
