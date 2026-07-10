# Research Synthesis — Standalone Texture Packer

Synthesis of the six research docs in this folder. Read those for detail; this
file is the decision layer. Recommendation-first.

- `our-engine-baseline.md` — what `nt_builder` already gives us
- `texturepacker.md` — CodeAndWeb TexturePacker (the commercial reference)
- `spine-libgdx.md` — libGDX `.atlas` + Spine + gdx-texture-packer-gui
- `unity-raylib.md` — Unity SpriteAtlas + rTexPacker + stb_rect_pack
- `oss-and-architecture.md` — free-tex-packer + OSS survey + tool architecture
- `defold.md` — Defold `.atlas` / `.tpinfo` / extension-texturepacker

---

## 1. Feature comparison matrix

Columns: **NT** = our `nt_builder` baseline · **TP** = CodeAndWeb TexturePacker ·
**GDX** = libGDX/Spine · **U** = Unity SpriteAtlas · **rTP** = rTexPacker ·
**FTP** = free-tex-packer · **DF** = Defold native `.atlas`.
✅ full · ⚠️ partial/indirect · ❌ none.

| Feature | NT | TP | GDX | U | rTP | FTP | DF | Note |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|---|
| Pack algorithm | ✅ | ✅ | ✅ | ⚠️ | ⚠️ | ✅ | ⚠️ | NT = NFP/Minkowski **concave, sub-pixel** vector packer; others MaxRects/skyline/rect |
| 90° rotation | ✅ | ✅ | ✅ | ✅ | ❌ | ✅ | ❌ | DF native no; DF via `.tpinfo` extension yes |
| Full D4 (rot+flip) | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | **NT unique**; but no foreign format can encode a flip (see §4) |
| Trim / whitespace strip | ✅ | ✅ | ✅ | ⚠️ | ✅ | ✅ | ✅ | NT reports offset+source size (Trim model) |
| Trim **modes** (Trim/Crop) | ⚠️ | ✅ | ⚠️ | ❌ | ⚠️ | ✅ | ⚠️ | NT trim only; Crop is an export-time rewrite tp_core adds |
| Polygon / mesh packing | ✅ | ✅ | ⚠️ | ⚠️ | ❌ | ❌ | ✅ | NT concave-contour, vertex budget; GDX polygon = Spine only |
| Multipack (N pages) | ✅ | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ | NT `ATLAS_MAX_PAGES=64`; needs `{n}` output naming (tp_core) |
| Extrude / padding / margin | ✅ | ✅ | ✅ | ✅ | ⚠️ | ✅ | ✅ | rTP padding only (no extrude) |
| Pivots | ✅ | ✅ | ❌ | ✅ | ✅ | ⚠️ | ✅ | NT per-sprite normalized, off-frame allowed; GDX atlas can't store; U on sprite not atlas |
| 9-slice | ✅ | ✅ | ✅ | ✅ | ❌ | ⚠️ | ❌ | NT per-sprite slice9 borders |
| Alias / dedup | ✅ | ✅ | ✅ | ⚠️ | ❌ | ✅ | ⚠️ | NT dedup stage (hash+pixel) |
| Animation / name grouping | ❌ | ✅ | ✅ | ❌ | ⚠️ | ⚠️ | ✅ | NT is an ID/region model; grouping is tp_core/exporter work |
| Scaling variants (@2x cook) | ⚠️ | ✅ | ✅ | ✅ | ❌ | ⚠️ | ❌ | NT `pixels_per_unit` (one scale/pack); no in-tool multi-scale cook |
| Secondary textures (normal etc.) | ❌ | ✅ | ❌ | ✅ | ❌ | ❌ | ❌ | U supports N named; TP one |
| GPU compression output | ✅ | ✅ | ⚠️ | ✅ | ❌ | ⚠️ | ✅ | NT Basis ETC1S/UASTC vendored; TP the widest |
| Watch / live repack | ❌ | ⚠️ | ❌ | ✅ | ❌ | ✅ | ✅ | NT has content-hash cache, not a watcher; **no CLI OSS tool ships watch** |
| CLI | ⚠️ | ✅ | ✅ | ❌ | ✅ | ✅ | ✅ | NT builder is a lib; `main.c` is a stub |
| Project file | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | .tps / pack.json / .spriteatlas / .rtp / .ftpp / .atlas |
| Data-driven custom exporters | ❌ | ✅ | ❌ | ❌ | ⚠️ | ✅ | ❌ | TP Grantlee, FTP mustache |

---

## 2. Where we already win

Strengths that are ours today and beat the field — lean on these as the product's story:

1. **NFP/Minkowski concave vector packer** (`nt_builder_atlas_vpack.c`): sub-pixel exact,
   true concave silhouettes via Clipper2, NFP cache + optional multithreading. Everyone
   else packs rectangles (MaxRects) or convex-ish hulls (Spine). Densest packing tier.
2. **Full D4 orientations** (8 = 4 rot × 2 flip) per sprite. Unique — but only exploitable
   by our own `.ntpack` runtime (see §4 caveat).
3. **Basis Universal compression baked in** (ETC1S/UASTC, 5 presets). Matches TP/Unity/Defold;
   beats every OSS CLI tool.
4. **Per-sprite off-frame pivots** (normalized over pre-trim source, may exceed [0,1]) and
   **per-sprite 9-slice**, stable across animation frames.
5. **Content-hash build cache** (skip unchanged packs) — half of "incremental" already exists.
6. **`pixels_per_unit`** HD/SD story: HD pack renders at same on-screen size as SD sharing one Transform.
7. **Concave contour with vertex budget** (`max_vertices`, cap 16) — overdraw control that
   only Spine/Defold/TP-polygon otherwise offer.

## 3. Table-stakes we lack (must build in `tp_core` + frontends)

None of these need engine changes beyond the single export API in §5g — they are tool work:

- **CLI arg parsing** and **project file** (nothing exists).
- **PNG page export** + **JSON-hash / JSON-array** exporters (the ecosystem lingua franca).
- **Foreign-format exporters**: Defold `.tpinfo`, libGDX `.atlas`, then Godot/Phaser.
- **Trim `crop` mode** (export-time rewrite of offsets/source size).
- **Animation / numeric-suffix name grouping** (`walk_01…` → animation), needed by Defold/GDX/Phaser.
- **Scaling-variant cook** (`@2x`/`@1x` from one placement) and **multipack `{n}` output naming**.
- **Folder inputs with live rescan** + ignore globs (the workflow users actually want).
- **GUI** (toolbar/canvas/panels over `nt_ui`).

## 4. Differentiators worth building

- **Watch mode** — missing from *every* surveyed OSS CLI packer; TP only sells it via IDE plugins.
  Cheap given our content-hash cache. Highest-leverage differentiator.
- **N named secondary channels** (normal/emission/AO) from a single placement pass — beats TP's
  one-secondary limit; Unity is the only tool that does N.
- **Concave NFP + D4 packing quality** as a headline ("tighter sheets than MaxRects").
- **Identical-layout variants + secondaries from one placement result** (Unity's guarantee, done
  right for both axes at once).
- **Round-trip unpacker** (Spine ships one) — doubles as our exporter test oracle.
- **Free, no license/EULA friction** — TP's floating-license CI pain is a talking point.

> **Caveat on D4 flips.** Our packer can flip sprites, and the flip is *baked into the page pixels*.
> No foreign atlas format (TP/GDX/`.tpinfo`) can encode anything but a 90° `rotated` bool, so a
> baked flip or 180° rotation would render wrong through those exporters. Conclusion: **foreign
> exporters must constrain the pack to representable orientations** (identity-only for v1, rotation-
> only later). D4 stays a `.ntpack`-runtime optimization. This is TexturePacker's own "capability
> flags flow back into the packer" insight — bake it into the exporter descriptor (§5b).

---

## 5. Key decisions (recommendation-first)

### (a) Project file design
**Decision: JSON, schema-versioned integer, relative paths, `sources`/`sprites` split, `atlases[]` array.**
Rationale: JSON parses trivially in C (cJSON is vendored at `deps/cjson`), diffs well, and is
scriptable; every good tool uses text. Concretely, adopting the `oss-and-architecture.md` design:

- `"version": 1` **schema** integer, separate from informational `app.version`; loader refuses
  newer, runs chained migrations for older (FTP's fatal omission was no migrations).
- **All paths relative to the project file**, normalized to `/` on save (TP's documented
  portability behavior; fixes FTP's absolute-path defect). Accept absolute on load, relativize on save.
- **`sources`** = live folders (rescanned on open + watched) + pinned files + `ignore` globs;
  **`sprites`** = sparse per-sprite *overrides only*, keyed by atlas-relative name: `pivot`,
  `slice9`, `trim` on/off, `shape` override, `tag`/animation. Never store defaults.
- **`atlases[]` array** (multi-atlas project, gdx-tpgui model) — required for per-atlas settings,
  secondary channels and variants without N project files. *(Confirm with owner — §7.)*
- **Stable identifiers, not display strings** (`"json-hash"`, never `"JSON (hash)"` — FTP's CLI
  mapping-table smell). Deterministic output: sorted keys, 2-space, LF, trailing newline.

### (b) Exporter architecture for a C tool
**Decision: confirm the `oss-and-architecture.md` hybrid — hardcoded C core exporters + mustache-template exporters with JSON descriptors.**
Rationale: binary/fiddly formats need C; the long tail is cheaply data-driven; matches TP/FTP proof.

- **Hardcode in C**: our `.ntpack` (already `nt_builder`'s job), `json-hash`, `json-array`, and
  **Defold `.tpinfo`** (its rotation-corner and vertex/index rules are error-prone — hardcode, don't template).
- **Mustache engine** (<1k LOC, vendor or implement) + FTP's **formatter extension**
  (`add/subtract/multiply/divide/offsetLeft/offsetRight/mirror/escapeName`) for coordinate conventions.
  Long-tail formats (libGDX `.atlas`, Godot, Phaser3, CSS) ship as templates.
- **Pre-normalize once, in core** (FTP's `prepareData`): trim-`crop` rewrite, scale multiply, name
  munging happen before templating; templates are logic-less.
- **Each exporter carries a JSON descriptor** with capability flags (`supportsRotation`,
  `supportsTrim`, `supportsPolygon`, `supportsPivot`, `supportsScale9`, `supportsMultipack`,
  `yAxis`). Flags **gate GUI/CLI controls and constrain the pack** (drive the §4 D4 restriction).
- **Discovery**: built-ins compiled in (embed `.mst` as C strings); user exporters from a config
  dir + a project-relative `exporters/` dir; project references user templates by relative path
  (fixes FTP's "CLI can't use a custom exporter" gap). **Defer** Lua/script plugins — they slot in
  later as another descriptor `kind` over the same model.

### (c) CLI + GUI structure
**Decision: two thin binaries over one `tp_core` static lib (not a single dual-mode binary).**
Rationale: the repo layout (`apps/cli`, `apps/gui`) and AGENTS.md "two equal frontends" already imply
two artifacts; two binaries avoid the Windows console/GUI-subsystem `AttachConsole` hack, give clean
release artifacts, and keep each frontend trivial. The invariant that matters (AGENTS.md tool-parity)
holds regardless: **all state and capability live in `tp_core`; frontends do argv/UI + disk I/O only**,
`tp_core` returns data (`files[{name,buffer}]`-style), never touches opinions about I/O. Single-binary
is the documented fallback if we later want one shippable exe. *(Owner call — §7; low cost to reverse.)*

### (d) Canonical sprite data model
**Decision: one `tp_result` model, produced from the engine export snapshot (§5g), consumed by every exporter.**
Adapts `oss-and-architecture.md`'s `ntp_sprite` to what `nt_builder` actually emits (D4 transform,
slice9, `pixels_per_unit`, hull polygon already computed):

```c
typedef struct { int32_t x, y; } tp_point;

typedef struct {
    const char *name;                 // atlas key (rel path, ext/folder policy applied)
    int   page;                       // multipack page index
    struct { int x, y, w, h; } frame; // placed TRIMMED rect on page; w/h UNROTATED (pre-D4)
    uint8_t transform;                // D4 mask: bit0 flipH, bit1 flipV, bit2 diag; 0=identity
    bool  trimmed;
    struct { int x, y, w, h; } spriteSourceSize; // trimmed rect inside original image (y-down)
    struct { int w, h; } sourceSize;             // original untrimmed size
    struct { float x, y; } pivot;     // normalized over sourceSize; default 0.5,0.5
    uint16_t slice9_lrtb[4];          // 0,0,0,0 = none
    tp_point *verts; int vert_count;  // hull, source/trim space, y-down (polygon)
    uint16_t *indices; int index_count;
    int   alias_of;                   // -1 unique, else index of the sprite it duplicates
} tp_sprite;

typedef struct { const char *image_name; int w, h; const uint8_t *rgba; } tp_page; // straight-alpha, y-down
typedef struct {
    const char *atlas_name; float pixels_per_unit;
    tp_page *pages; int page_count;
    tp_sprite *sprites; int sprite_count;   // sorted by name for deterministic output
} tp_result;
```

Invariants to assert (where packer bugs hide): `!trimmed ⇒ spriteSourceSize=={0,0,src.w,src.h}` and
`frame.w/h==src.w/h`; `trimmed ⇒ frame.w/h==spriteSourceSize.w/h`; `crop` is export-time only, never
stored; rotation/flip direction pinned in docs, exporters flip via `mirror`-style formatters.

### (e) Defold integration
**Decision: confirm `defold.md` — `.tpinfo` (+ page PNGs, optional `.tpatlas`) as primary, `.atlas`+loose-PNG as fallback.**
Rationale: `.tpinfo` **preserves our packing** (rotation/trim/polygon/multipage/aliases/pivots), the
Defold Foundation maintains the engine half, and the text protobuf format is diffable/testable. The
`.atlas`+loose-PNG preset is the zero-dependency escape hatch (Defold repacks, throwing our layout
away). **Do not** build our own Defold extension (option c) — heavy, version-locked, no user benefit.
Exporter checklist lives in `defold.md §(b)` (y-down; `frame_rect` w/h swapped when rotated; rotated
blit = source-TL → page-TR; `corner_offset==source_rect.xy`; quad fallback `indices:[1,2,3,0,1,3]`;
all `required` fields written; pivot px from untrimmed TL). Validate by diffing the extension's own
`examples/rotate` + `examples/anim_trim` fixtures and by building its demo with our file substituted.

### (f) Incremental / watch strategy
**Decision: content-hash sidecar + embedded smart-update, one FS-watcher feeding the same async pack job.**
- CLI: hash inputs + effective options + exporter template text + tool version → sidecar `*.hash`;
  match ⇒ "up to date", exit 0; `--force` overrides (crunch model). This makes unconditional
  build-script use safe. `nt_builder`'s pack-level content cache already covers half of this.
- Also embed the key in JSON formats' `meta.smartupdate` (TP model) so it survives clean output checkouts.
- Watch: one watcher thread (`ReadDirectoryChangesW` / inotify / kqueue) on live folders, 150–300 ms
  debounce, coalesce, then trigger the same pack job the GUI uses. `--watch` reuses it in the CLI.
- Cache **decoded+trimmed source images keyed by file hash** (the actual slow part); don't attempt
  partial-layout incrementality — MaxRects/NFP on 1–2k sprites is ms-scale and layout is global.

### (g) Required engine change — the placement/page-pixels export API
**Decision: one purely-additive read-only export callback on `NtBuilderContext`, fired inside `nt_builder_end_atlas` after `pipeline_serialize` and before `pipeline_register`.**

All data the exporter needs already lives in the `AtlasPipeline` struct at that point (verified in
`external/neotolis-engine/tools/builder/nt_builder_atlas.c`): `page_w/page_h/page_count`, `page_pixels[]`
(raw **straight-alpha** RGBA — premultiply happens later in the texture encoder, so PNG export gets
straight alpha for free), `AtlasPlacement[]` (`nt_builder_atlas_vpack.h`: sprite_index, page, x, y,
trimmed_w/h, trim_x/y, D4 transform), plus per-sprite `hull_vertices`/`vertex_counts`, `dedup_map`
(→ `alias_of`), and sprite input (name, source w/h, `origin_x/y`, slice9). Geometry, trim and dedup
stages run *unconditionally* before the cache-hit branch, so the hook fires correctly on **both cache
hit and miss**. Proposed public shape (add to `nt_builder.h`):

```c
typedef struct { int32_t x, y; } nt_atlas_export_point_t;
typedef struct {
    const char *name; uint32_t page;
    uint32_t frame_x, frame_y;        // inner top-left on page (past extrude)
    uint32_t frame_w, frame_h;        // trimmed size, UNROTATED
    uint8_t  transform;               // D4 mask (0 = identity)
    uint32_t source_w, source_h;      // untrimmed
    uint32_t trim_x, trim_y;          // trim offset in source, y-down
    float    origin_x, origin_y;      // normalized pivot over source, y-down (PNG)
    uint16_t slice9_lrtb[4];
    int32_t  alias_of;                // -1 or index
    const nt_atlas_export_point_t *vertices; uint32_t vertex_count; // source/trim space, y-down
} nt_atlas_export_sprite_t;
typedef struct {
    const char *name; float pixels_per_unit; uint32_t page_count;
    const uint32_t *page_w, *page_h;  // [page_count]
    uint8_t *const *page_pixels;      // [page_count] straight-alpha RGBA8, y-down; transform+extrude baked
    uint32_t sprite_count;
    const nt_atlas_export_sprite_t *sprites;
} nt_atlas_export_t;

// Fires once per end_atlas; snapshot valid ONLY for the call — copy what you keep. Read-only.
typedef void (*nt_builder_atlas_export_fn)(const nt_atlas_export_t *atlas, void *user);
void nt_builder_set_atlas_export_cb(NtBuilderContext *ctx, nt_builder_atlas_export_fn cb, void *user);
```

**How minimal:** ~1 struct + 1 setter + ~15 lines populating a stack snapshot from `AtlasPipeline`
and firing the callback. **Zero change** to `.ntpack` output, the cache format, or any existing
signature — a pure addition. Ships as an engine issue + PR (never a submodule edit). *Optional
follow-up PR (§7, Phase 7-adjacent):* a transform-**policy** enum (`NONE`/`ROT90`/`D4`) replacing the
`allow_transform` bool, so foreign exporters can request rotation-only packing (representable) instead
of falling back to identity-only. Not needed for v1.

---

### (h) Export-time validation & warnings (owner requirement)
**Decision: every export runs a validation pass of the placement result against the target's
capability flags; anything unrepresentable is reported per sprite — never silently-wrong output.**
Rationale: our packer bakes 8 D4 orientations (rotations + flips) into page pixels; Defold native
supports NO placement rotation at all, and `.tpinfo` encodes only a single 90° `rotated` bool — a
baked flip/180°/270° would silently render wrong in the target engine.

- Two severities. **Error** (output would render incorrectly): baked flip/180°/270° for a target
  without D4 support; page-size mismatch for `.tpinfo`; page count > 1 for single-page-only
  formats. Export fails (non-zero exit) by default; `--force` downgrades to warnings.
  **Warning** (metadata degrades): pivot dropped for pivot-less formats, polygon flattened to
  rect, slice9 not representable.
- Prevention over cure: when a project has export targets configured, pack constraints are the
  **intersection of all enabled targets' capability flags** (v1: any foreign target →
  `allow_transform=false`; after the engine transform-policy PR (§5g follow-up) → rotation-only),
  so validation normally reports nothing.
- One report format in `tp_core`, surfaced by both frontends: CLI → stderr + exit code,
  GUI → warnings panel listing affected sprites.

## 6. Open questions for the product owner

1. **CLI+GUI packaging** (§5c): two binaries (recommended) vs one dual-mode exe?
2. **Project scope** (§5a): `atlases[]` array (recommended) vs one-atlas-per-file?
3. **v1 foreign-format set**: Defold `.tpinfo` + `json-hash`/`json-array` are in. Which next —
   libGDX `.atlas`, Godot, Phaser3, Unity? (Drives how early Phase 7's template engine lands.)
4. **Transform policy** (§4/§5g): OK to ship foreign atlases identity-only for v1 and add the
   optional rotation-only engine PR later? Or invest in that second PR up front?
5. **Own-engine story**: is `.ntpack` (via `nt_builder`) the canonical own-engine output, with the
   tool's added value being foreign exporters + project file + GUI? Or do we also want a generic
   text/JSON descriptor for our runtime?
6. **Font-atlas packing** (rTP does it; `nt_builder` has fonts): in scope for this tool or out?
7. **Naming/branding**: tool name/CLI verb (`ntp`?), project extension (`.ntpp`?), license.
8. **Engine PR sequencing**: require the export-API PR merged upstream before `tp_core` consumes it,
   or is a temporary submodule branch acceptable to unblock parallel work? (Critical-path item.)
