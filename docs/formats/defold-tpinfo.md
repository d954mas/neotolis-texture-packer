# `defold` export format (`.tpinfo` + `.tpatlas`)

The Defold target emits the input files of the official
[extension-texturepacker](https://github.com/defold/extension-texturepacker)
(pinned research: `docs/research/defold.md`, extension **2.7.0** ↔ Defold
**1.12.4**): a **`.tpinfo`** describing our packed layout and a starter
**`.tpatlas`** wrapping it with animations, plus the page PNGs. Users add the
extension as a `game.project` dependency and reference the `.tpatlas` anywhere an
atlas is accepted. The extension never repacks pixels — it trusts our layout —
so this is a lossy *projection* of the full canonical model onto what the
`.tpinfo` protobuf can hold.

- **Exporter id:** `defold`
- **Output:** three artifacts at the target's `out_path` base (no extension):
  - `<base>.tpinfo` — the packed layout (protobuf **text** format).
  - `<base>.tpatlas` — the animation wrapper (protobuf text format).
  - `<base>-<N>.png` — one straight-alpha RGBA8 PNG per page (`<N>` 0-based).
- **`.tpinfo` format version:** `"2.0"` (the `version` field; enables `pivot`).
- **Encoding:** UTF-8, LF line endings, 2-space indent.

Coordinates are **pixels, y-down, origin top-left** — identical to our canonical
`tp_model` space *and* TexturePacker's convention, so **no y-flip is applied**.

---

## Files & resolution

The `.tpatlas` `file` field and each page's `name` are resolved differently:

- The `.tpatlas` `file` field is a Defold **resource** (`tpatlas.proto` marks it
  `[(resource)=true]`), resolved from the **project root** — so it must be a
  project-absolute `"/dir/<base>.tpinfo"`. A bare basename resolves to
  `/<base>.tpinfo` (project root) and the build fails. The exporter locates the
  project root by walking **up** from the `.tpinfo`'s own directory (bounded to 10
  levels) for a `game.project` file, then emits `"/" + relpath`. If none is found
  it falls back to the bare basename and raises a metadata notice. Zero config for
  the standard layout (`examples/defold-demo/game.project`).
- Each page `name` is resolved **next to the `.tpinfo`** (verified against
  `AtlasBuilder.java` 2.7.0: `infoResource.getResource(page.getName())`), so it
  stays the **relative basename** `"<base>-<N>.png"` — the same uniform,
  always-suffixed naming as `json-neotolis` (single page still gets `-0`).

Pages are **straight-alpha** (RGB not premultiplied); Defold's texture profiles
premultiply at build time (`premultiply_alpha: true`).

---

## Determinism

Byte-for-byte reproducible for identical input (pinned by a golden-bytes test and
a re-export `memcmp` test over real fixtures):

- fixed header/`version`/`description`; pages in page order; sprites in **final
  export name** order within each page; animations in `id` order; frames in
  explicit order.
- `is_solid` is computed from the page pixels (deterministic); floats use `%.9g`;
  no timestamps, no pointers, no unordered iteration.

---

## Capabilities & degradation (SUMMARY.md §5h)

`caps` describe the FORMAT's real abilities. Anything the format cannot hold is
either excluded by the per-target pack clamp (so the repack never produces it) or
dropped by the writer with a **metadata-loss notice** — never a hard error.

| Feature | `caps` | How it is handled for Defold |
|---|:--:|---|
| 90° rotation | ✅ `rotate90` | The `rotated` bool encodes exactly one D4 orientation (see below). **v1 packs identity-only** — see the clamp note. |
| Flips / full D4 | ❌ `flips` | No region-level flip exists (`.tpinfo` has none; flips are per-*animation*). Because a baked flip/180°/reflection is unrepresentable, the clamp forces identity packing. |
| Trim | ✅ | `trimmed` + `source_rect`/`corner_offset`. |
| Polygons | ✅ `polygons` | `vertices` + `indices` (else the canonical quad). |
| Pivot | ✅ `pivot` | Per-sprite `pivot` in px (v2.0). |
| 9-slice | ❌ `slice9` | No field in `.tpinfo`; a non-zero border set is dropped with a notice. |
| Multipage | ✅ `multipage` | One `pages {}` block per page. |
| Aliases | ✅ `aliases` | Each aliased name is emitted as its own full sprite entry sharing the page rect (how `.tpinfo` represents aliases). |

**Clamp note (v1 reality).** `nt_builder` can only pack *all-8-D4* or *identity*;
there is no rotation-only mode yet. `tp_export_effective_settings` therefore keeps
transforms on only when the target can hold the full D4 (`rotate90 && flips`).
Defold is the canonical **"rotate90-only" target** (`rotate90 = true`,
`flips = false`), so it **repacks identity-only** in v1 — correct output, with
rotation density deferred to the future engine transform-policy PR (SUMMARY.md
§5g). The writer's rotation path is fully implemented and tested directly, ready
for that day.

**Rotation encoding.** `rotated: true` means the content was rotated 90°
clockwise (source top-left lands at the frame's top-right). Verified against
`examples/rotate/rotate.tpinfo` and the `tp_transform_decode` corner mapping, this
is **exactly** our D4 mask `DIAGONAL | FLIP_H` (mask `5`). Every other non-identity
mask (pure flips, 180°, the transpose/anti-transpose reflections, the opposite
rotation) is **not** representable; the writer never bakes one for Defold (the
clamp prevents it) and raises a notice as a guard if handed one.

---

## `.tpinfo` field reference

`Atlas` → `pages[]` → each `Page { name, size, sprites[] }`. Sprite field order
mirrors the reference 2.0 exporter (`examples/basic/basic.tpinfo`):

| Field | Source in `tp_sprite` | Notes |
|---|---|---|
| `name` | final export name | rename overrides already applied by `tp_normalize`. |
| `trimmed` | `trimmed` | informational (bob ignores it, but `required`). |
| `rotated` | `transform == 5` | see rotation encoding above; else `false`. |
| `is_solid` | scanned from page pixels | `true` iff no transparent texel in the placed footprint. Informational. |
| `corner_offset {x,y}` | hull **vertex-bbox** origin (else `spriteSourceSize.{x,y}`) | untrimmed top-left → trimmed rect; drives where the extension anchors the hull. |
| `source_rect {x,y,w,h}` | hull **vertex bbox** (else `spriteSourceSize`) | trimmed rect inside the original image, **unrotated** dims; equals the `vertices` bounding box (TP invariant). |
| `pivot {x,y}` | `pivot × sourceSize` | px from the untrimmed top-left, y-down. Centered default → `dim/2`. |
| `frame_rect {x,y,w,h}` | `frame.{x,y}` + hull footprint | placement on the page; `w,h` are the **as-drawn** footprint of the hull bbox (swapped when `rotated`). |
| `untrimmed_size {w,h}` | `sourceSize` | original image size. |
| `indices` | canonical `[1,2,3,0,1,3]` or hull | flat triangle list. |
| `vertices {x,y}` | source-space quad or hull | **untrimmed source space**, y-down, **never rotated**; hull verts are `trim-local + spriteSourceSize.{x,y}`. |

- **Quad fallback** (plain rects, or no hull): `vertices` are the `source_rect`
  corners in order **TR, TL, BL, BR** with `indices: [1, 2, 3, 0, 1, 3]` — the
  reference exporter's convention.
- **Polygon** (concave/convex hull): our `vertices` (trim-local → source space)
  and `indices` verbatim. The builder's clipper2 pass inflates a non-RECT hull
  OUTWARD, and `tp_pack_read` recovers `spriteSourceSize` assuming the hull's
  minimum local coordinate is `(0,0)` — true only for RECT sprites. For a real
  hull the minimum X is negative (Y is already normalized by the y-down flip), so
  the exporter derives `corner_offset` / `source_rect` / `frame_rect` from the
  **actual `vertices` bounding box**, not from `spriteSourceSize`. This preserves
  TexturePacker's own invariant that **`source_rect` == the vertices bbox** (see
  `examples/basic/basic.tpinfo`) and keeps the residual inflation **symmetric**.
  Emitting `corner_offset = spriteSourceSize.xy` instead left the hull's true left
  edge unaccounted for: the extension positions each vertex at
  `frame_rect.x + (vertex.x − corner_offset.x)` (`Atlas.getTriangles`,
  extension-texturepacker 2.7.0), so the hull was drawn `|min_x|` px too far left
  — an asymmetric offset on near-symmetric shapes (circle) and a stretch on
  strongly asymmetric ones (a trimmed triangle). Fixed in `tp_export_defold.c`;
  regression: `test_hull_untrimmed_space` + the `basic.tpinfo` bbox parity check.

---

## `.tpatlas` field reference

A starter wrapper the user points a Defold sprite/GUI at. `AtlasDesc`:

| Field | Value |
|---|---|
| `file` | project-absolute `"/dir/<base>.tpinfo"` (a Defold resource, resolved from the project root found by walking up for `game.project`; bare basename + notice if none). |
| `rename_patterns` | `""` — renames are baked into names by `tp_normalize`. |
| `animations[]` | one block per explicit animation (see below). |
| `is_paged_atlas` | `false` — `>1` page always builds as a paged (2D-array) texture regardless, matching the upstream 2-page `basic.tpatlas`. |

**Implicit 1-frame animations.** Every `.tpinfo` sprite name is automatically
promoted to a single-frame animation *by bob* — we do **not** emit them into the
`.tpatlas`. The `.tpatlas` only adds explicit flipbooks. (A test asserts every
sprite name is present in the `.tpinfo` so each is usable as a 1-frame animation.)

### Animation block

| Field | Source | Notes |
|---|---|---|
| `id` | `tp_export_anim.id` | |
| `images` (repeated) | `frames[]` | final sprite names, explicit playback order. |
| `playback` | `playback` id → enum token | mapping below. |
| `fps` | `round(fps)` | Defold `fps` is `uint32`. |
| `flip_horizontal` | `flip_h` | `0`/`1`. |
| `flip_vertical` | `flip_v` | `0`/`1`. |

### Playback mapping

Our stable playback id is pinned to Defold's set (`docs/design/ux.md` §3.7b). An
out-of-range id falls back to `PLAYBACK_ONCE_FORWARD` **with a notice**.

| Stable id | `.tpatlas` token |
|:--:|---|
| `0` | `PLAYBACK_ONCE_FORWARD` (default) |
| `1` | `PLAYBACK_LOOP_FORWARD` |
| `2` | `PLAYBACK_ONCE_BACKWARD` |
| `3` | `PLAYBACK_LOOP_BACKWARD` |
| `4` | `PLAYBACK_ONCE_PINGPONG` |
| `5` | `PLAYBACK_LOOP_PINGPONG` |
| `6` | `PLAYBACK_NONE` |

---

## Notices (informational, never fatal)

- `pivot dropped for '<name>' …` — only if a pivot-less variant is ever used
  (Defold `caps.pivot` is `true`, so not raised here).
- `slice9 dropped for '<name>' (target has no 9-slice support)` — a non-zero
  border set was present.
- `polygon flattened to rect for '<name>' …` — only for a polygon-less variant
  (Defold `caps.polygons` is `true`, so not raised here).
- `transform N dropped for '<name>' …` — a non-representable transform reached the
  writer (guard; the clamp prevents this in the supported pipeline).
- `animation '<id>' has unknown playback id N …` — out-of-range playback fallback.
- `could not locate game.project above '<path>' …` — no `game.project` ancestor
  found; the `.tpatlas` `file` field fell back to a bare basename (the Defold
  build may not resolve it). Put the export under a Defold project tree.
