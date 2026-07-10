# Phase 1 Implementation Plan — `tp_core` skeleton + `.ntpack` parse-back reader

Executable plan for **Phase 1a** (`tp_pack_read`) and **Phase 1b** (`tp_core` skeleton +
canonical model + `tp_pack`). All §5 decisions in `docs/research/SUMMARY.md` are owner-resolved;
this plan does not re-open them. Every binary claim below is cited against the engine headers/source
(engine tree is **read-only**).

Naming (SUMMARY §6 Q7): lib `tp_core`, binaries `ntpacker` / `ntpacker-gui`, project ext
`.ntpacker_project`. Phase 1 produces **no binaries** — only `tp_core` + its ctest.

---

## 0. What Phase 1 delivers

- A static lib `packer/` (`tp_core`) that:
  1. Parses a `.ntpack` the builder just wrote back into the canonical `tp_result` model
     (placements, geometry, page pixels) — **Phase 1a**.
  2. Runs a pack through `nt_builder` and returns an owned `tp_result` — **Phase 1b**.
- No I/O opinions, no CLI/GUI, no exporters (those are Phase 2+). `tp_core` returns data.

Critical-path decision (SUMMARY §5g): we obtain placements by **parsing our own `.ntpack`**, not
via an engine callback. No engine change gates this.

---

## 1. Module layout

```
packer/
  CMakeLists.txt
  include/tp_core/
    tp_model.h        # canonical model: tp_result / tp_sprite / tp_page / tp_point / tp_transform
    tp_arena.h        # tiny bump allocator that owns every tp_result allocation
    tp_name_map.h     # xxh64(name) -> name reverse map
    tp_pack_read.h    # parse .ntpack (file or memory) -> tp_result
    tp_pack.h         # tp_pack(settings, arena) -> tp_result   (stub in 1a, real in 1b)
    tp_error.h        # tp_status enum + tp_error string buffer (no asserts on bad input)
  src/
    tp_arena.c
    tp_name_map.c
    tp_pack_read.c
    tp_pack.c
  tests/
    CMakeLists.txt
    tp_fixtures.h / tp_fixtures.c   # procedural sprite generators (shared by 1a/1b tests)
    test_pack_read.c               # 1a golden round-trip + UV property test
    test_pack.c                    # 1b tp_pack invariants
```

### 1.1 Root CMake wiring

Add to root `CMakeLists.txt` after the existing `add_subdirectory(apps/smoke)` line:
`add_subdirectory(packer)`.

### 1.2 `packer/CMakeLists.txt`

```cmake
add_library(tp_core STATIC
    src/tp_arena.c src/tp_name_map.c src/tp_pack_read.c src/tp_pack.c)
target_include_directories(tp_core PUBLIC include)
# nt_builder is PUBLIC-linked so tp_core drives the builder (1b) AND transitively gets
# shared/include (nt_pack_format.h, nt_atlas_format.h, nt_texture_format.h) + hash/nt_hash.h.
target_link_libraries(tp_core PUBLIC nt_builder nt_log)
target_compile_definitions(tp_core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(tp_core PRIVATE -U_DLL)   # match builder: static CRT (see apps/smoke)
nt_set_warning_flags(tp_core)      # AGENTS.md invariant
nt_set_sanitizer_flags(tp_core)
enable_testing()
add_subdirectory(tests)
```

**Link-dep facts (verified):**
- `nt_builder` PUBLIC-links `nt_shared nt_core nt_log_interface nt_time cgltf stb_image
  stb_image_resize2 stb_truetype stb_image_write nt_hash mikktspace nt_basisu_encoder glfw glad
  miniz tinycthread nt_clipper2_bridge` — `tools/builder/CMakeLists.txt:121`. Linking `nt_builder`
  therefore pulls **stb, basisu, clipper2, glfw** automatically; we add nothing.
- `nt_shared` exposes `shared/include` as an INTERFACE include dir (`shared/CMakeLists.txt:2`), so
  `#include "nt_atlas_format.h"` etc. resolve through the transitive link.
- `nt_hash64_str()` (the reverse-map + name-hash primitive) is declared in `hash/nt_hash.h` and
  provided by the `nt_hash` target, transitively available via `nt_builder`.
- The engine's `deps/unity` target is added **unconditionally** (`external/neotolis-engine/
  CMakeLists.txt:124`, `EXCLUDE_FROM_ALL`), so it is available to us as a submodule consumer. The
  engine's own tests/tools/examples are top-level-guarded (`CMakeLists.txt:161-194`), which is why
  our root CMake already pulls `tools/builder` in explicitly.

### 1.3 `tp_arena` (small design call, flagged)

The engine exposes **no** public arena allocator (grep of `shared/`+`engine/` public headers found
none). ROADMAP 1b signs `tp_pack(project*, arena*) -> tp_result*`, so `tp_core` ships a minimal
bump arena: `tp_arena_create/alloc/free`. Every string/array in a `tp_result` is arena-allocated;
freeing the arena frees the whole result. This gives the "owned tp_result" ownership model cleanly
and keeps output deterministic. `tp_pack_read` takes a `tp_arena*` too (its output is a `tp_result`).

---

## 2. Binary parsing spec

All multi-byte fields little-endian (`nt_pack_format.h:8`). Structs are `#pragma pack(push,1)` and
have `_Static_assert` sizes — cast directly from a read buffer, but only after validating magic +
version + bounds (builder validates, runtime is a safety net — we are neither, so we **bounds-check
every offset** and return `tp_status` errors, never assert on file contents).

### 2.1 Pack container (`nt_pack_format.h`)

```
[NtPackHeader : 32 bytes @ 0]          nt_pack_format.h:57-68
[NtAssetEntry : 24 bytes] × asset_count  @ 32   (immediately after header)
... padding to NT_PACK_DATA_ALIGN(8) ...
[asset data region]                    starts at header.header_size
[meta section]                         at header.meta_offset (0 = none)
```

- `NtPackHeader.magic == NT_PACK_MAGIC` (`0x4B41504E` "NPAK", line 13); `version == NT_PACK_VERSION`
  (2, line 14). Reject otherwise.
- Entry table: `asset_count` (`:62`) entries of `sizeof(NtAssetEntry)==24` starting at byte 32
  (header is padded to 32 precisely so the entry array is 8-aligned, `:67`).
- Each `NtAssetEntry` (`:77-85`): `resource_id`(u64), `offset`(u32, from file start), `size`(u32),
  `format_version`(u16), `asset_type`(u8 = `nt_asset_type_t`), `meta_offset`(u32).
- Asset payload for an entry = `blob[entry.offset .. entry.offset+entry.size)`. Validate
  `offset+size <= total_size` (`:64`).
- We need `NT_ASSET_ATLAS = 6` (`nt_pack_format.h:45`) and `NT_ASSET_TEXTURE = 2` (`:41`). Ignore
  other types.
- Checksum (`:65`, CRC32 of data after header) — **do not verify** on read; it is not needed for a
  file we just wrote and adds a dependency. (Optional later.)
- `pixels_per_unit` is stored as a **meta** blob, not in the atlas header. Its meta `kind ==
  nt_hash64_str("pixels_per_unit")` and payload is a 4-byte float
  (`nt_builder_atlas.c:1864-1883`). To read it: walk `NtMetaEntryHeader` records
  (`nt_pack_format.h:93-100`, 20-byte header + payload), starting from the **atlas asset entry's
  own `meta_offset`** field — prefer this over the pack header's top-level `meta_offset`
  (`nt_builder.c:884-890` shows the writer sets each entry's `meta_offset` to where that specific
  asset's meta records begin) — matching `resource_id == atlas entry.resource_id` and `kind`. The
  walk stride is **not** a fixed 20 bytes: the writer pads each record's payload to
  `NT_PACK_ASSET_ALIGN` (4) between records (`nt_builder.c:898-902`), so advance by
  `20 + align4(payload_size)` per record. If absent, default `1.0f`.

### 2.2 Atlas blob (`nt_atlas_format.h`, v6)

Layout (header comment `:18-33`, sizes `_Static_assert`ed):

```
@0                       NtAtlasHeader (28 bytes)                    :36-46
@28                      uint64_t texture_resource_ids[page_count]   :23
@28 + 8*page_count       NtAtlasRegion regions[region_count]  (48 B each)  :51-79
@hdr.vertex_offset       NtAtlasVertex vertices[total_vertex_count] (8 B)  :84-100
@hdr.index_offset        uint16_t indices[total_index_count]         :24
```

- `NtAtlasHeader`: `magic==NT_ATLAS_MAGIC` (`0x534C5441`, line 7); `version==6` (line 9);
  `region_count`, `page_count`, `vertex_offset`, `total_vertex_count`, `index_offset`,
  `total_index_count`. `vertex_offset`/`index_offset` are byte offsets **from the atlas blob start**
  (not the file start) — add `atlas_entry.offset`.
- `texture_resource_ids[page]` at blob offset 28: the `resource_id` of the `NT_ASSET_TEXTURE` entry
  holding page `p`'s pixels. **Reader matches this id against the entry table** — never recompute a
  hash. (The builder writes `nt_hash64_str("<atlas>/tex<p>")` here, `nt_builder_atlas.c:1676-1681`;
  the texture entry id is `nt_hash64_str(normalize_path("<atlas>/tex<p>"))`,
  `nt_builder.c:74-77` + `nt_builder_atlas.c:1895,1920`. For our tool-chosen atlas names these are
  identical, and matching-by-id is robust regardless.)

**`NtAtlasRegion` (48 bytes, `:51-79`)** — one per sprite, aliases included (duplicates share
`vertex_start`/`atlas_u/v` with their original):
| field | off | meaning |
|---|---|---|
| `name_hash` u64 | 0 | `nt_hash64_str(sprite_name)` (raw name, `:1806`) — key into reverse map |
| `source_w/h` u16 | 8/10 | untrimmed source size |
| `trim_offset_x` i16 | 12 | pixels stripped from **left** |
| `trim_offset_y` i16 | 14 | pixels stripped from **bottom**, **y-up** (v5+, `:57`) |
| `origin_x` f32 | 16 | pivot X / source_w, left=0 |
| `origin_y` f32 | 20 | pivot Y / source_h, **y-up** (0=bottom,1=top, `:65`) |
| `vertex_start` u32 | 24 | index into `vertices[]` |
| `index_start` u32 | 28 | index into `indices[]` |
| `vertex_count` u8 | 32 | ≤16 |
| `page_index` u8 | 33 | which page |
| `transform` u8 | 34 | D4 mask (see 2.4) |
| `index_count` u8 | 35 | triangle indices |
| `flags` u8 | 36 | render hints (`NT_ATLAS_REGION_FLAG_QUAD_*`) — ignore in Phase 1 |
| `slice9_lrtb[4]` u16 | 38 | [left,right,top,bottom]; all-zero = none (`:77`) |

**`NtAtlasVertex` (8 bytes, `:84-100`)**: `local_x` i16, `local_y` i16, `atlas_u` u16, `atlas_v` u16.
- `local_x` ∈ [0, trim_w], PNG x-direction (left→right).
- `local_y` ∈ [0, trim_h], **y-up** (0 = bottom of trim, trim_h = top). Builder flips to y-up at
  write time (`nt_builder_atlas.c:1746-1757`).
- `atlas_u/atlas_v`: page UV × 65535. **`atlas_v` is y-DOWN** — it indexes raw pixel rows
  top-first (`nt_builder_atlas.c:1749`). So UV→pixel gives y-down page coords that match the page
  pixel buffer directly.
- Indices are **local** (0..vertex_count-1), triangle list, offset by `index_start`
  (`nt_atlas_format.h:29-32`).

### 2.3 Texture asset → page pixels (`nt_texture_format.h`)

For the entry matched by `texture_resource_ids[page]`:
```
@entry.offset            NtTextureAssetHeader (28 bytes)   :101-116
@entry.offset + 28       pixel data (data_size bytes)
```
- Validate `magic==NT_TEXTURE_MAGIC` (`0x58455454`, line 7), `version==3` (line 8).
- **Require** `compression==NT_TEXTURE_COMPRESSION_RAW` (0) and `format==NT_TEXTURE_FORMAT_RGBA8`
  (1). The export/preview pack pass sets `compress=NULL` and `format=RGBA8`, so mip0 is
  `width*height*4` straight bytes right after the header (`nt_builder_texture.c:197-228`,
  `mip_count=1` at `:206`). If the reader sees BASIS (1), return an error telling the caller to use
  the export-friendly settings profile — do not attempt transcode in Phase 1.
- Straight vs premultiplied: check `flags & NT_TEXTURE_FLAG_PREMULTIPLIED` (bit0, line 41). The
  **reader only surfaces this bit** on `tp_page.premultiplied` — it never asserts or rejects on its
  value, because `tp_pack_read` must be able to parse a premultiplied pack too (a premultiplied
  smoke pack is part of task 4's Done criteria, §4). Only the **export-profile test** (§3.4/task 10)
  asserts the flag is clear, and only for packs produced via the §5 profile (`premultiplied=false`).
- `tp_page.rgba` points into (or is copied from) `blob[entry.offset+28 ..]`, `w=header.width`,
  `h=header.height`. Page pixels are y-down (top row first), matching `atlas_v`.

### 2.4 D4 transform decode (`transform` byte)

`nt_builder_atlas_geometry.c:970-1001` and `nt_atlas_format.h:70-71`:
```
bit0 (1) = flip H      bit1 (2) = flip V      bit2 (4) = diagonal (swap x,y)
apply order: diagonal -> flipH -> flipV        0 = identity
output dims: if (bits & 4) -> (trim_h, trim_w) else (trim_w, trim_h)
```
Reference decode (matches `transform_point`, `:983-1001`) — maps a trim-local corner
`(x,y)` in [0..tw]×[0..th] to the on-page-relative corner:
```c
if (flags & 4) { swap(x,y); }
int w = (flags & 4) ? th : tw;
int h = (flags & 4) ? tw : th;
if (flags & 1) x = w - x;
if (flags & 2) y = h - y;
```
The **canonical model keeps `transform` as the raw D4 mask** (SUMMARY §5d `uint8_t transform`) and
stores `frame.w/h` **unrotated** (pre-D4). Phase-1 does not need to un-bake the transform — it
records the mask and the unrotated trim size. Assert only the consistency invariant in 2.6.

### 2.5 UV → pixel rect recovery (exact)

Builder encode (`nt_builder_atlas.c:1762-1780`): for a vertex whose on-page pixel coord is
`px` (integer, = `inner_x + tx`, `inner_x = pl->x + extrude`, `:1737-1738`):
```
u = (uint16_t)( px * 65535.0f / page_w + 0.5f )   // clamp to [0,65535], truncate = round-half-up
```
**Inverse (exact for page dims ≤ 4096):**
```
px = lround( (double)u * page_w / 65535.0 )
```
Proof it round-trips: `|u/65535 − px/W| ≤ 0.5/65535`, so `|u·W/65535 − px| ≤ 0.5·W/65535 ≤
0.5·4096/65535 ≈ 0.031 < 0.5` → the outer `lround` recovers `px` exactly. Holds for any W ≤
`NT_BUILD_MAX_TEXTURE_SIZE` (=4096, `nt_builder.h:43`), power-of-two or not, and at both edges
(px=0 → u=0; px=W → u=65535 → W). Pinned by the property test (§3.3).

**Frame rect recovery** (per region):
1. `trim_w = max_v(local_x)`, `trim_h = max_v(local_y)` over the region's vertices (min is 0). These
   are **exact int16** — no UV quantization. (Depends on the hull touching all four trim edges;
   see Risk R1.)
2. On-page AABB from UVs: `x0 = min_v round(atlas_u·W/65535)`, `y0 = min_v round(atlas_v·H/65535)`
   (both y-down). `frame.x = x0`, `frame.y = y0`.
3. `frame.w = trim_w`, `frame.h = trim_h` (**unrotated**).
4. **Extrude is excluded**: UVs are built from `inner_x/inner_y` (past the extrude band,
   `:1737-1738`), so the recovered frame is the inner trimmed content rect — never the extrude band.

### 2.6 Atlas (y-up) → canonical (y-down PNG) conversions

Canonical model is **y-down PNG space** (SUMMARY §5d). Precise field mapping:

| canonical field | formula from region/vertices | note |
|---|---|---|
| `name` | `name_map[name_hash]` | error if unknown/collision |
| `page` | `region.page_index` | |
| `frame.{x,y}` | §2.5 step 2 (y-down page px) | AABB top-left |
| `frame.{w,h}` | `trim_w, trim_h` (§2.5 step 1) | unrotated |
| `transform` | `region.transform` | raw D4 mask |
| `sourceSize.{w,h}` | `source_w, source_h` | |
| `spriteSourceSize.x` | `trim_offset_x` | left strip = same in y-down |
| `spriteSourceSize.y` | `source_h − trim_offset_y − trim_h` | y-up bottom strip → y-down top strip (inverse of `:1816`) |
| `spriteSourceSize.{w,h}` | `trim_w, trim_h` | |
| `pivot.x` | `origin_x` | |
| `pivot.y` | `1.0f − origin_y` | inverse of y-up flip (`:1824`) |
| `slice9_lrtb[4]` | `region.slice9_lrtb` verbatim | order [L,R,T,B]; top/bottom are already PNG-oriented borders |
| `trimmed` | `!(ssrc.x==0 && ssrc.y==0 && trim_w==source_w && trim_h==source_h)` | |
| `verts[i]` | `x = local_x`, `y = trim_h − local_y` | **trim-local y-down** |
| `indices` | copied verbatim (local 0..vc-1) | winding caveat below |
| `alias_of` | index of the region whose `(vertex_start,atlas_u/v,page)` it shares; else −1 | see 2.7 |
| result `pixels_per_unit` | meta float (§2.1) or 1.0 | |
| result `pages[p]` | §2.3 | y-down, straight-alpha |

Invariants to assert on the recovered `tp_sprite` (SUMMARY §5d — where packer bugs hide):
- `!trimmed ⇒ spriteSourceSize == {0,0,source_w,source_h}`.
- on-page AABB dims equal transform-swapped `(trim_w,trim_h)`:
  `(max_px−min_px, max_py−min_py) == (transform&4 ? (trim_h,trim_w) : (trim_w,trim_h))`.
- `frame` fully inside its page: `x0≥0 && y0≥0 && x0+aabb_w ≤ page_w && y0+aabb_h ≤ page_h`.

**Winding caveat (defer to Phase 2):** the builder swaps triangle winding to world-CCW for the
y-up runtime (`:1730`). Our verts are re-flipped to y-down, which inverts winding again → triangles
are CW in y-down space. Phase 1 copies indices verbatim and documents this; polygon **exporters**
(Phase 2 `json-neotolis`) decide the winding they emit. Phase-1 round-trip tests compare vertex
**sets/positions**, not winding.

### 2.7 Aliases

Duplicate sprites dedup to the same region geometry: the builder propagates the original's placement
(`nt_builder_atlas.c:1646-1650,1787-1796`), so duplicate regions carry identical `page_index`,
`vertex_start`, and `atlas_u/v` as their original but a distinct `name_hash`. Recover `alias_of` by
grouping regions with identical `(page_index, vertex_start, atlas_u/v of first vertex)`; the first in
sorted-by-name order is the original (`alias_of = −1`), the rest point to its index. (1b sorts
sprites by name for determinism — SUMMARY §5d/§5d-invariants.)

### 2.8 Name reverse map (`tp_name_map`)

`region.name_hash = nt_hash64_str(sprite_name)` over the **raw** name string passed to the builder
(`nt_builder_atlas.c:1806`) — **not** the normalized path used for `resource_id`. So: the tool knows
every input name; build `map[nt_hash64_str(name)] = name`. On insert, if two distinct names hash
equal → `TP_STATUS_HASH_COLLISION` (would already be a build error). On lookup miss →
`TP_STATUS_UNKNOWN_REGION`. Use the engine `nt_hash64_str` (link `nt_hash`); do not reimplement
xxh64. (Verify once whether `nt_hash_init(NULL)` is required before hashing; the builder hashes
without an explicit init in the atlas path, so pure hashing is expected to be init-free — confirm in
the first test run.)

---

## 3. Test plan

Vendored **Unity** (`external/neotolis-engine/deps/unity`, target `unity`) wired into ctest, mirroring
`apps/smoke/CMakeLists.txt:11-13`.

> **Unity float constraint (flagged):** the engine builds `unity` with `UNITY_EXCLUDE_FLOAT
> UNITY_EXCLUDE_DOUBLE` **PUBLIC** (`deps/unity/CMakeLists.txt`), so `TEST_ASSERT_EQUAL_FLOAT` is
> unavailable to consumers. Compare floats (pivot, `pixels_per_unit`) via
> `TEST_ASSERT_TRUE(fabsf(a-b) < 1e-5f)`. Do **not** add a private float-enabled unity copy — the
> exclude exists to dodge a Windows/Clang `_fdclass` link error.

> **Unity link is unverified (flagged):** nothing in this repo currently links the vendored `unity`
> target — `apps/smoke` does **not** link it either. The target is `EXCLUDE_FROM_ALL` in the engine's
> CMake (§1.2 link-dep facts) but is defined and reachable in our submodule-consumer mode. We are the
> **first** consumer to link it, so verify it actually links (no missing symbols/config surprises) on
> the first build — this falls under task 1 (§4), now that the CMake skeleton lives there.

`packer/tests/CMakeLists.txt` per test:
```cmake
add_executable(tp_test_pack_read test_pack_read.c tp_fixtures.c)
target_link_libraries(tp_test_pack_read PRIVATE tp_core unity)
target_compile_options(tp_test_pack_read PRIVATE -U_DLL)
add_test(NAME tp_pack_read COMMAND tp_test_pack_read "${CMAKE_BINARY_DIR}/tp_test")
```

### 3.1 Fixture sprite set (`tp_fixtures.c`, procedural like `apps/smoke`)

Generate RGBA8 in memory and add via `nt_builder_atlas_add_raw` (`nt_builder.h:424`; opts via
`nt_atlas_sprite_opts_defaults`, `:369`). Cover every branch the reader must handle:
- **plain** opaque rect (untrimmed: `spriteSourceSize == full`).
- **trimmed**: opaque disc/blob padded with transparent margins (asymmetric so trim_x≠trim_y and a
  non-zero top strip — catches the `source_h − trim_offset_y − trim_h` y-flip).
- **rotated/flipped**: force packing to pick non-identity D4 by shaping sizes so the packer rotates;
  to make it deterministic also add a case with `allow_transform=true` + tall+wide mix. (We assert
  whatever transform comes back is *consistent*, per §2.6 invariants, not a specific mask.)
- **polygon**: `shape=NT_ATLAS_SHAPE_CONCAVE_CONTOUR` with `max_vertices` on an L-shape / diamond
  (non-trivial hull; exercises trim_w/h-from-local-coords, Risk R1).
- **slice9**: non-zero `slice9_left/right/top/bottom` (forces RECT + no-rotate,
  `nt_builder.h:333-334`).
- **off-frame pivot**: `origin_x/y` outside [0,1] (e.g. 1.5) — checks pivot y-flip for out-of-range.
- **alias**: two names, identical pixels → one dedups to the other (`alias_of`).
- **multipage**: enough/large sprites with a small `max_size` (e.g. 128) to force ≥2 pages
  (`page_index` varies; per-page texture lookup).
- **1×1 sprite**: smallest possible source size — exercises degenerate trim/AABB math at the floor.
- **sprite exactly == page size**: on-page UV reaches the `u=65535` edge end-to-end (both min and
  max edges of the page), pinning the §2.5 encode/decode round-trip at its extreme.
- **non-POT page, end-to-end**: a fixture packed with `power_of_two=false` so the page dimensions
  themselves are non-power-of-two, exercising the §2.5 UV math against a non-POT `W/H` (not just
  non-POT `px`).

### 3.2 Golden round-trip assertions (`test_pack_read.c`)

Per fixture: the generator records the **expected** canonical values it fed in (name, source size,
trim offsets, trim size, pivot, slice9, expected vertex count, alias). Build →
`nt_builder_finish_pack` to a temp `.ntpack` → `tp_pack_read` → assert exact match on: **frame
`w,h`** (trim size — the generator knows this), `sourceSize`, `spriteSourceSize` trim offsets,
`transform`-consistency invariants (§2.6), `pivot`, `slice9_lrtb`, `trimmed`, polygon vertex
positions (as a set, winding-agnostic), `alias_of`, page count, and each page's `w/h` +
straight-alpha flag. **`frame.x,y` (and page/transform) are packer-chosen** — the generator has no
oracle for them, and re-deriving them from the UVs to check against the UV-derived value would be
circular — so they are **not** asserted for exact value; instead they are validated indirectly by
the pixel-sampling assertion below. **Required per fixture (not optional):** sample page pixels at
the recovered frame rect and match known sprite content (e.g. corner/centre pixels of the source
image, accounting for `transform`). This is the only independent check of the frame origin
(`frame.x,y`) and of `atlas_v`'s y-orientation — do not treat it as a soft/spot-check; every fixture
must include it.

### 3.3 UV recovery property test

Pure function test (no builder): for `W,H ∈ {2,3,7,16,100,127,128,255,256,1000,2048,4095,4096}` and
every (or random dense) `px ∈ [0,W]`, assert `uv_to_px(px_to_uv(px,W),W) == px` using the exact
encode (`floor(px*65535/W+0.5)`) and decode (`lround(u*W/65535)`) from §2.5. This pins the
"exact for all page sizes ≤ 4096" acceptance criterion independent of a full pack.

**Caveat:** this property test validates the **idealized** encode `floor(px*65535/W+0.5)` using
exact (double) arithmetic; the margin analysis in §2.5 covers why float32 rounding doesn't break the
round-trip in principle, but the **real builder encodes via `float32`**
(`nt_builder_atlas.c:1762-1780`), not the idealized formula. Only the golden tests (§3.2), which
build through the actual `nt_builder` and its float32 path, exercise the real encode. Do not treat
§3.3 alone as sufficient coverage of the UV round-trip — it is a necessary but not sufficient check.

### 3.4 Phase 1b test (`test_pack.c`)

Pack the **smoke** sprite set (reuse `apps/smoke` disc generator) through `tp_pack` → assert every
`tp_result` invariant (§2.6 + SUMMARY §5d), **stable sprite ordering** (sorted by name), and correct
`alias_of` on an injected duplicate. Assert byte-stable output across two runs (determinism).

### 3.5 CI

Runs in existing `ctest --preset native-release|native-debug` (AGENTS.md) on all 3 OSes via
`ci.yml`. No new CI wiring — new `add_test`s are picked up automatically.

### 3.6 Transform decode coverage

Synthetic unit test (no builder, no `.ntpack`): construct an `NtAtlasRegion` **in memory** for each
known non-identity `transform` mask (§2.4) — flip-H, flip-V, flip-H+flip-V, and especially the
**diagonal** (dim-swap) mask and its flip-H/flip-V combinations — and assert the reference decode
(§2.4) produces the expected on-page corner mapping and `w/h` dim-swap for each. This is independent
of whatever the packer actually chooses to emit, so it is the only place the diagonal/dim-swap
decode path is guaranteed to be exercised deterministically.

Additionally, add a **guard assertion** over the packed fixture set (§3.1/§3.2): after building all
fixtures, assert that **at least one** recovered region has a diagonal transform bit set (`transform
& 4`). If none do, fail loudly with a message telling the implementer to adjust the fixtures (e.g.
tweak sizes in the rotated/flipped fixture, §3.1) until the packer actually picks a diagonal
transform. Without this guard, the dim-swap path can go green **vacuously** in the golden test —
because the packer, not the test, chooses transforms.

---

## 4. Task breakdown (ordered, sized for delegation)

Legend: **[fast]** mechanical, **[deep]** design/tricky-correctness.

**Phase 1a**
1. **[fast]** `tp_error.h` (`tp_status` enum, error buffer) + `tp_arena.{h,c}` (bump alloc/free) +
   the **CMake skeleton** — `packer/CMakeLists.txt` (§1.2), a stub `packer/tests/CMakeLists.txt`,
   and the root `add_subdirectory(packer)` (§1.1). Moved here from task 8 so tasks 2-7 compile
   incrementally instead of only at the very end. Done: `tp_core` compiles and links standalone via
   `cmake --build`; a unit smoke of arena alloc/reset passes.
2. **[fast]** `tp_model.h` — adapt SUMMARY §5d verbatim (add `tp_page.premultiplied` bool + a
   `tp_transform` bit enum + `tp_result.pixels_per_unit`). Done: header compiles; documents y-down.
3. **[fast]** `tp_name_map.{h,c}` — hash→name via `nt_hash64_str`, collision detection. Done: unit
   test inserts N names, resolves all, flags a forced collision.
4. **[deep]** `tp_pack_read.c` container+atlas+texture parse (§2.1-2.3) with full bounds-checking;
   emits `tp_result`. Done: parses the smoke `.ntpack`, page count/dims correct.
5. **[deep]** Recovery math in `tp_pack_read.c` — frame rect, D4-consistency, y-conversions,
   aliases (§2.4-2.7). Done: §2.6 invariants hold when run against the **smoke pack** (task 4) plus
   the **synthetic transform-decode test** (§3.6) — the `tp_fixtures` set (§3.1) doesn't land until
   task 6, so this task's Done criterion cannot yet depend on it.
6. **[deep]** `tp_fixtures.{h,c}` generators (§3.1). Re-tagged from [fast]: reliably triggering
   dedup/multipage/rotation (and the diagonal-transform guard, §3.6) in procedural fixtures is
   correctness-sensitive, not mechanical. Done: each case builds a valid `.ntpack`.
7. **[deep]** `test_pack_read.c` golden round-trip (§3.2) + `tp_fixtures` glue. Done: all cases green.
8. **[fast]** Remaining test wiring + UV property test (§3.3) — the CMake skeleton itself now lands
   in task 1, so this is just adding the `tp_uv_property` test target/`add_test` and any leftover
   `packer/tests/CMakeLists.txt` entries. Done: `ctest` shows `tp_pack_read` + `tp_uv_property`
   passing.

**Phase 1b** (depends on 1a)
9. **[deep]** `tp_pack.c` — drive `nt_builder` begin/add/end from a minimal settings input, write
   the session `.ntpack` to a temp/cache path, parse-back via `tp_pack_read`, sort sprites by name,
   return owned `tp_result`. Use the **export-friendly profile** (§5). Done: returns a populated
   `tp_result`.
10. **[fast]** `test_pack.c` (§3.4) invariants + determinism. Done: `ctest` green on all targets.

`tp_pack.{h,c}` is created as a **stub** in task 4 (header + `TP_STATUS_UNIMPLEMENTED` body) so 1a
links; task 9 fills it. The 1a golden test drives `nt_builder` **inline** via `tp_fixtures` and does
not depend on `tp_pack`.

---

## 5. Export-friendly pack settings profile (used by `tp_pack` and every fixture)

Start from `nt_atlas_opts_defaults()` (`nt_builder.h:293`) and override so the parse-back reads
straight-alpha uncompressed mip0:
- `premultiplied = false` (straight alpha; the export-profile test, §3.4/task 10, asserts the flag
  is clear for packs produced via this profile — the reader itself only surfaces the flag, §2.3).
- `compress = NULL` (RAW RGBA8 — reader requires it, §2.3).
- `gen_mipmaps = false` and `format = NT_TEXTURE_FORMAT_RGBA8`.
- `debug_png = false`.
- **Atlas names passed to `tp_pack` must be normalization-invariant**: no `\\`, no `./`, no `..`, no
  `//`, no trailing `/`. `texture_resource_ids[]` in the atlas blob hashes the **raw**
  `"<atlas>/tex<N>"` string (`nt_builder_atlas.c:1679`), while the entry table hashes the
  **normalized** path (`nt_builder.c:74-77`). If the atlas name needs normalization, the two hashes
  diverge and **every page lookup misses** (R2, §6). This is a caller obligation, not something the
  reader can fix.
Everything else (shape, `allow_transform`, padding, `max_size`, `power_of_two`) is caller-driven per
target — Phase 2 per-target packing (SUMMARY §5h) sets these; Phase 1 just plumbs them through.

---

## 6. Risks / unknowns — verify early

- **R1 — hull AABB vs trim rect (medium).** `trim_w/trim_h` are recovered as `max(local_x/y)`
  assuming the (possibly RDP-simplified) concave hull touches all four trim edges. For RECT/CONVEX
  this is exact; for `CONCAVE_CONTOUR` a near-flat edge could theoretically drop the extreme vertex.
  The on-page-AABB-dims invariant (§2.6) does **not** cross-check this — both sides derive from the
  same hull vertices, so it is circular and cannot catch a hull that fails to reach the true trim
  edges. The real oracle is the fixture generator's **known trim rect** (golden assertion, §3.2) plus
  the **required** pixel sampling at the recovered frame rect (§3.2/P1-2), since both are derived
  independently from the source pixels, not from the hull. Add a fixture whose alpha bbox is provably
  hull-touching (e.g. a plus/cross shape whose extreme pixels sit at all four trim edges and are
  guaranteed to survive RDP simplification). **Circles/discs are the worst case for this recovery
  path**: a simplified 8-vertex hull of a disc may not reach the trim bbox edges at all four extremes,
  under-reporting `trim_w/h` — do not rely on a disc fixture alone to validate hull-extent trim
  recovery. If it ever fails, fall back to the UV-derived AABB (same info) or restrict polygon frame
  recovery to the AABB and flag upstream.
- **R2 — texture entry id vs atlas blob id.** The atlas blob stores `nt_hash64_str("<atlas>/texN")`
  (un-normalized, `:1679`) while the texture entry id is normalized (`:74-77,1920`). Identical for
  our slash-free tool names, but **verify by matching succeeds** in the first multipage test; the
  reader must key off the blob's stored ids and fail loudly (`TP_STATUS_PAGE_NOT_FOUND`) if no entry
  matches — never silently produce empty pages.
- **R3 — `premultiplied=false` warning (low, expected).** `begin_atlas`/`end_atlas` emits
  `NT_LOG_WARN` when `premultiplied=false` (`nt_builder_atlas.c:2007-2011`). It is **non-fatal** and
  correct for our straight-alpha export/preview pass; keep it (or route logs) — do not try to
  suppress via engine changes.
- **R4 — BASIS/compressed input.** If a caller ever hands a compressed atlas, mip0 is not raw RGBA8.
  Reader must detect `compression != RAW` and return `TP_STATUS_UNSUPPORTED_TEXTURE` with a message
  pointing at the export profile (§5) — no transcoder dependency in Phase 1.
- **R5 — `nt_hash` init.** Confirm `nt_hash64_str` needs no `nt_hash_init` (expected: pure function).
  If the first test asserts, call `nt_hash_init(NULL)` once in test/`tp_pack` setup.
- **R6 — Unity float exclusion.** Already mitigated (§3): manual epsilon compares.

---

## 7. ROADMAP deltas (recommend)

- **1a acceptance wording:** the reverse-map hashes the **raw sprite name** (`nt_hash64_str(name)`,
  `:1806`), not the normalized resource path. Suggest ROADMAP §1a note this so implementers don't
  reuse `normalize_and_hash`.
- **1a → add the export-friendly profile constants** verbatim (`premultiplied=false`, `compress=NULL`,
  `gen_mipmaps=false`, `format=RGBA8`) — already listed in ROADMAP 1a deliverables; this plan pins
  the exact source defaults to start from (`nt_atlas_opts_defaults`).
- **1b signature `tp_pack(project*, arena*)`:** no engine arena exists; this plan introduces a
  `tp_core`-local `tp_arena`. Flag that `project*` in Phase 1b is a **minimal settings struct**, not
  the full Phase-3 `.ntpacker_project` (project file is Phase 3) — recommend ROADMAP 1b say "settings
  input" to avoid implying the project loader is a 1b dependency.
- **No engine change** is required or proposed by Phase 1 (confirms SUMMARY §5g/§6 Q8).
