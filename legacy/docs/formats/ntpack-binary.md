# `.ntpack` binary parse-back contract

**Status:** retained as a format/schema contract (AGENTS.md exception — the
master spec keeps a historical plan only when it names an executable format
contract). This is the binary the engine builder writes and that `tp_pack_read`
parses back into the canonical `tp_model`; the **B0 import materializer** and
every extract/inspect path depend on this recovery math. It documents the
on-disk layout and the exact UV/geometry recovery, not the future package
manifest. Source: the Phase 1 implementation plan §2 (`docs/plans/phase-1.md`,
deleted 2026-07-20 — see git history for the surrounding task/test plan).

The `.ntpack` is produced by `nt_builder` (`nt_builder_finish_pack`) and read
back by `packer/src/tp_pack_read.c` into the canonical model
(`tp_result`/`tp_sprite`/`tp_page`). We are neither the builder (which validates)
nor the runtime (which trusts): the reader **bounds-checks every offset** and
returns a `tp_status` error, and **never asserts on file contents**. All
multi-byte fields are little-endian (`nt_pack_format.h`). Structs are
`#pragma pack(push,1)` with `_Static_assert`ed sizes — cast directly from a read
buffer only after validating magic + version + bounds.

Line/offset citations below are into the **engine** headers/source
(`external/neotolis-engine/`, read-only) as of the canonical baseline; they are
provenance, not a live coupling. Struct-version bumps fail loud; a same-version
math change fails **silent** and is guarded only by fixtures (see the engine
coupling note at the end).

---

## 1. Pack container (`nt_pack_format.h`)

```
[NtPackHeader : 32 bytes @ 0]              nt_pack_format.h:57-68
[NtAssetEntry : 24 bytes] × asset_count    @ 32   (immediately after header)
... padding to NT_PACK_DATA_ALIGN(8) ...
[asset data region]                        starts at header.header_size
[meta section]                             at header.meta_offset (0 = none)
```

- `NtPackHeader.magic == NT_PACK_MAGIC` (`0x4B41504E` "NPAK");
  `version == NT_PACK_VERSION` (2). Reject otherwise.
- Entry table: `asset_count` entries of `sizeof(NtAssetEntry) == 24` starting at
  byte 32 (the header is padded to 32 precisely so the entry array is 8-aligned).
- Each `NtAssetEntry`: `resource_id` (u64), `offset` (u32, from file start),
  `size` (u32), `format_version` (u16), `asset_type` (u8 = `nt_asset_type_t`),
  `meta_offset` (u32). Asset payload = `blob[entry.offset .. entry.offset+size)`;
  validate `offset + size <= total_size`.
- We need `NT_ASSET_ATLAS = 6` and `NT_ASSET_TEXTURE = 2`. Ignore other types.
- **Checksum** (CRC32 of data after header) is **not verified** on read: it is
  not needed for a file we just wrote and would add a dependency. (Optional
  later.)
- `pixels_per_unit` is stored as a **meta** blob, not in the atlas header. Its
  meta `kind == nt_hash64_str("pixels_per_unit")` and payload is a 4-byte float
  (`nt_builder_atlas.c:1864-1883`). To read it, walk `NtMetaEntryHeader` records
  (`nt_pack_format.h:93-100`, 20-byte header + payload) starting from the **atlas
  asset entry's own `meta_offset`** field — prefer this over the pack header's
  top-level `meta_offset` (`nt_builder.c:884-890`) — matching
  `resource_id == atlas entry.resource_id` and `kind`. The walk stride is **not**
  a fixed 20 bytes: the writer pads each record's payload to `NT_PACK_ASSET_ALIGN`
  (4) between records (`nt_builder.c:898-902`), so advance by
  `20 + align4(payload_size)` per record. If absent, default `1.0f`.

---

## 2. Atlas blob (`nt_atlas_format.h`, v6)

```
@0                        NtAtlasHeader (28 bytes)                     :36-46
@28                       uint64_t texture_resource_ids[page_count]    :23
@28 + 8*page_count        NtAtlasRegion regions[region_count] (48 B ea) :51-79
@hdr.vertex_offset        NtAtlasVertex vertices[total_vertex_count] (8 B) :84-100
@hdr.index_offset         uint16_t indices[total_index_count]          :24
```

- `NtAtlasHeader`: `magic == NT_ATLAS_MAGIC` (`0x534C5441`); `version == 6`;
  `region_count`, `page_count`, `vertex_offset`, `total_vertex_count`,
  `index_offset`, `total_index_count`. `vertex_offset`/`index_offset` are byte
  offsets **from the atlas blob start** (not the file start) — add
  `atlas_entry.offset`.
- `texture_resource_ids[page]` at blob offset 28: the `resource_id` of the
  `NT_ASSET_TEXTURE` entry holding page `p`'s pixels. **The reader matches this
  id against the entry table — never recompute a hash** (see Risk R2).

**`NtAtlasRegion` (48 bytes, `:51-79`)** — one per sprite, aliases included
(duplicates share `vertex_start`/`atlas_u/v` with their original):

| field | off | meaning |
|---|---|---|
| `name_hash` u64 | 0 | `nt_hash64_str(sprite_name)` (raw name, `:1806`) — key into the reverse map |
| `source_w/h` u16 | 8/10 | untrimmed source size |
| `trim_offset_x` i16 | 12 | pixels stripped from **left** |
| `trim_offset_y` i16 | 14 | pixels stripped from **bottom**, **y-up** (v5+, `:57`) |
| `origin_x` f32 | 16 | pivot X / source_w, left = 0 |
| `origin_y` f32 | 20 | pivot Y / source_h, **y-up** (0 = bottom, 1 = top, `:65`) |
| `vertex_start` u32 | 24 | index into `vertices[]` |
| `index_start` u32 | 28 | index into `indices[]` |
| `vertex_count` u8 | 32 | ≤ 16 |
| `page_index` u8 | 33 | which page |
| `transform` u8 | 34 | D4 mask (see §4) |
| `index_count` u8 | 35 | triangle indices |
| `flags` u8 | 36 | render hints (`NT_ATLAS_REGION_FLAG_QUAD_*`) — ignore in parse-back |
| `slice9_lrtb[4]` u16 | 38 | [left, right, top, bottom]; all-zero = none (`:77`) |

**`NtAtlasVertex` (8 bytes, `:84-100`)**: `local_x` i16, `local_y` i16,
`atlas_u` u16, `atlas_v` u16.

- `local_x` ∈ [0, trim_w], PNG x-direction (left→right).
- `local_y` ∈ [0, trim_h], **y-up** (0 = bottom of trim, trim_h = top). The
  builder flips to y-up at write time (`nt_builder_atlas.c:1746-1757`).
- `atlas_u/atlas_v`: page UV × 65535. **`atlas_v` is y-DOWN** — it indexes raw
  pixel rows top-first (`:1749`). So UV→pixel gives y-down page coords that match
  the page pixel buffer directly.
- Indices are **local** (0..vertex_count-1), triangle list, offset by
  `index_start` (`:29-32`).

---

## 3. Texture asset → page pixels (`nt_texture_format.h`)

For the entry matched by `texture_resource_ids[page]`:

```
@entry.offset            NtTextureAssetHeader (28 bytes)   :101-116
@entry.offset + 28       pixel data (data_size bytes)
```

- Validate `magic == NT_TEXTURE_MAGIC` (`0x58455454`), `version == 3`.
- **Require** `compression == NT_TEXTURE_COMPRESSION_RAW` (0) and
  `format == NT_TEXTURE_FORMAT_RGBA8` (1). The export/preview pack pass sets
  `compress = NULL` and `format = RGBA8`, so mip0 is `width*height*4` straight
  bytes right after the header (`nt_builder_texture.c:197-228`, `mip_count = 1`).
  If the reader sees BASIS, return `TP_STATUS_UNSUPPORTED_TEXTURE` telling the
  caller to use the export-friendly settings profile — do not transcode.
- Straight vs premultiplied: check `flags & NT_TEXTURE_FLAG_PREMULTIPLIED`
  (bit0). The **reader only surfaces this bit** on `tp_page.premultiplied` — it
  never asserts/rejects on its value, because parse-back must be able to read a
  premultiplied pack too. Only the **export-profile test** asserts the flag is
  clear, and only for packs produced via the export profile
  (`premultiplied = false`).
- `tp_page.rgba` points into (or is copied from) `blob[entry.offset + 28 ..]`,
  `w = header.width`, `h = header.height`. Page pixels are y-down (top row
  first), matching `atlas_v`.

---

## 4. D4 transform decode (`transform` byte)

`nt_builder_atlas_geometry.c:970-1001`, `nt_atlas_format.h:70-71`:

```
bit0 (1) = flip H      bit1 (2) = flip V      bit2 (4) = diagonal (swap x,y)
apply order: diagonal -> flipH -> flipV        0 = identity
output dims: if (bits & 4) -> (trim_h, trim_w) else (trim_w, trim_h)
```

Reference decode (matches `transform_point`, `:983-1001`) — maps a trim-local
corner `(x,y)` in [0..tw]×[0..th] to the on-page-relative corner:

```c
if (flags & 4) { swap(x, y); }
int w = (flags & 4) ? th : tw;
int h = (flags & 4) ? tw : th;
if (flags & 1) x = w - x;
if (flags & 2) y = h - y;
```

The **canonical model keeps `transform` as the raw D4 mask** (`uint8_t
transform`) and stores `frame.w/h` **unrotated** (pre-D4). Parse-back does not
un-bake the transform — it records the mask and the unrotated trim size, and
asserts only the consistency invariant in §6.

---

## 5. UV → pixel rect recovery (exact)

Builder encode (`nt_builder_atlas.c:1762-1780`): for a vertex whose on-page pixel
coord is `px` (integer, `= inner_x + tx`, `inner_x = pl->x + extrude`):

```
u = (uint16_t)( px * 65535.0f / page_w + 0.5f )   // clamp to [0,65535], round-half-up
```

**Inverse (exact for page dims ≤ 4096):**

```
px = lround( (double)u * page_w / 65535.0 )
```

Proof it round-trips: `|u/65535 − px/W| ≤ 0.5/65535`, so
`|u·W/65535 − px| ≤ 0.5·W/65535 ≤ 0.5·4096/65535 ≈ 0.031 < 0.5` → the outer
`lround` recovers `px` exactly. Holds for any `W ≤ NT_BUILD_MAX_TEXTURE_SIZE`
(= 4096), power-of-two or not, at both edges (px = 0 → u = 0; px = W → u = 65535
→ W). Pinned by the UV property test.

**Frame rect recovery** (per region):

1. `trim_w = max_v(local_x)`, `trim_h = max_v(local_y)` over the region's
   vertices (min is 0). These are **exact int16** — no UV quantization. (Depends
   on the hull touching all four trim edges; see Risk R1.)
2. On-page AABB from UVs: `x0 = min_v round(atlas_u·W/65535)`,
   `y0 = min_v round(atlas_v·H/65535)` (both y-down). `frame.x = x0`,
   `frame.y = y0`.
3. `frame.w = trim_w`, `frame.h = trim_h` (**unrotated**).
4. **Extrude is excluded**: UVs are built from `inner_x/inner_y` (past the
   extrude band), so the recovered frame is the inner trimmed content rect —
   never the extrude band.

---

## 6. Atlas (y-up) → canonical (y-down PNG) conversions

The canonical model is **y-down PNG space**. Precise field mapping:

| canonical field | formula from region/vertices | note |
|---|---|---|
| `name` | `name_map[name_hash]` | error if unknown/collision |
| `page` | `region.page_index` | |
| `frame.{x,y}` | §5 step 2 (y-down page px) | AABB top-left |
| `frame.{w,h}` | `trim_w, trim_h` (§5 step 1) | unrotated |
| `transform` | `region.transform` | raw D4 mask |
| `sourceSize.{w,h}` | `source_w, source_h` | |
| `spriteSourceSize.x` | `trim_offset_x` | left strip = same in y-down |
| `spriteSourceSize.y` | `source_h − trim_offset_y − trim_h` | y-up bottom strip → y-down top strip (inverse of `:1816`) |
| `spriteSourceSize.{w,h}` | `trim_w, trim_h` | |
| `pivot.x` | `origin_x` | |
| `pivot.y` | `1.0f − origin_y` | inverse of the y-up flip (`:1824`) |
| `slice9_lrtb[4]` | `region.slice9_lrtb` verbatim | order [L,R,T,B]; top/bottom already PNG-oriented |
| `trimmed` | `!(ssrc.x==0 && ssrc.y==0 && trim_w==source_w && trim_h==source_h)` | |
| `verts[i]` | `x = local_x`, `y = trim_h − local_y` | **trim-local y-down** |
| `indices` | copied verbatim (local 0..vc-1) | winding caveat below |
| `alias_of` | index of the region whose `(vertex_start, atlas_u/v, page)` it shares; else −1 | see §7 |
| result `pixels_per_unit` | meta float (§1) or 1.0 | |
| result `pages[p]` | §3 | y-down, straight-alpha |

Invariants to assert on the recovered `tp_sprite` (where packer bugs hide):

- `!trimmed ⇒ spriteSourceSize == {0, 0, source_w, source_h}`.
- on-page AABB dims equal transform-swapped `(trim_w, trim_h)`:
  `(max_px−min_px, max_py−min_py) == (transform&4 ? (trim_h, trim_w) : (trim_w, trim_h))`.
- `frame` fully inside its page:
  `x0 ≥ 0 && y0 ≥ 0 && x0 + aabb_w ≤ page_w && y0 + aabb_h ≤ page_h`.

**Winding caveat.** The builder swaps triangle winding to world-CCW for the y-up
runtime (`:1730`). Our verts are re-flipped to y-down, which inverts winding
again → triangles are CW in y-down space. Parse-back copies indices verbatim and
documents this; polygon **exporters** decide the winding they emit, and
round-trip tests compare vertex **sets/positions**, not winding.

---

## 7. Aliases

Duplicate sprites dedup to the same region geometry: the builder propagates the
original's placement (`nt_builder_atlas.c:1646-1650,1787-1796`), so duplicate
regions carry identical `page_index`, `vertex_start` and `atlas_u/v` as their
original but a distinct `name_hash`. Recover `alias_of` by grouping regions with
identical `(page_index, vertex_start, atlas_u/v of first vertex)`; the first in
sorted-by-name order is the original (`alias_of = −1`), the rest point to its
index. (Pack output sorts sprites by name for determinism.)

---

## 8. Name reverse map (`tp_name_map`)

`region.name_hash = nt_hash64_str(sprite_name)` over the **raw** name string
passed to the builder (`nt_builder_atlas.c:1806`) — **not** the normalized path
used for `resource_id`. So: the tool knows every input name; build
`map[nt_hash64_str(name)] = name`. On insert, if two distinct names hash equal →
`TP_STATUS_HASH_COLLISION` (already a build error). On lookup miss →
`TP_STATUS_UNKNOWN_REGION`. Use the engine `nt_hash64_str` (link `nt_hash`); do
not reimplement xxh64. Pure hashing is init-free (the builder hashes without an
explicit `nt_hash_init` in the atlas path).

---

## Caller obligation — export-friendly pack settings

`tp_pack` and every fixture start from `nt_atlas_opts_defaults()` and override so
the parse-back reads straight-alpha uncompressed mip0: `premultiplied = false`,
`compress = NULL`, `gen_mipmaps = false`, `format = NT_TEXTURE_FORMAT_RGBA8`,
`debug_png = false`.

**Atlas names must be normalization-invariant**: no `\`, no `./`, no `..`, no
`//`, no trailing `/`. `texture_resource_ids[]` hashes the **raw**
`"<atlas>/tex<N>"` string (`nt_builder_atlas.c:1679`) while the entry table
hashes the **normalized** path (`nt_builder.c:74-77`). If the atlas name needs
normalization the two hashes diverge and **every page lookup misses** (Risk R2).
This is a caller obligation, not something the reader can fix.

---

## Risks / recovery caveats

- **R1 — hull AABB vs trim rect (VERIFIED 2026-07-10).** Worse than a
  theoretical edge: the builder **INFLATES** non-RECT hulls via clipper2
  (`nt_builder_atlas.c:1288-1315`, `inflate_amt = max_outside + 1.0`), so
  `max(local_x/y)` systematically **OVER-reports** trim by ~1px for every
  `CONVEX_HULL`/`CONCAVE_CONTOUR` sprite; only `NT_ATLAS_SHAPE_RECT` writes hull
  corners exactly at the trim rect. **Exact trim/frame golden assertions are
  therefore valid only for RECT fixtures; polygon fixtures must assert the §6
  consistency invariants + pixel sampling at the recovered frame rect, not exact
  trim equality.** The on-page-AABB-dims invariant does **not** cross-check trim
  extent — both sides derive from the same hull vertices, so it is circular. The
  real oracle is the generator's known trim rect plus independent pixel sampling.
  **Circles/discs are the worst case**: a simplified 8-vertex hull may not reach
  the trim bbox at all four extremes, under-reporting `trim_w/h` — do not rely on
  a disc alone. Fallback: use the UV-derived AABB (same info) or restrict polygon
  frame recovery to the AABB and flag upstream. This inflation also shapes the
  Defold exporter's `corner_offset`/`source_rect` derivation (see
  `defold-tpinfo.md`, polygon note).
- **R2 — texture entry id vs atlas blob id.** The atlas blob stores
  `nt_hash64_str("<atlas>/texN")` (un-normalized) while the texture entry id is
  normalized. Identical for slash-free tool names, but the reader must key off
  the blob's stored ids and fail loudly (`TP_STATUS_PAGE_NOT_FOUND`) if no entry
  matches — never silently produce empty pages.
- **R3 — `premultiplied=false` warning (expected).** `begin_atlas`/`end_atlas`
  emits `NT_LOG_WARN` when `premultiplied=false`. It is non-fatal and correct for
  the straight-alpha export/preview pass; do not suppress via engine changes.
- **R4 — BASIS/compressed input.** If a caller hands a compressed atlas, mip0 is
  not raw RGBA8. Detect `compression != RAW` and return
  `TP_STATUS_UNSUPPORTED_TEXTURE` pointing at the export profile — no transcoder
  dependency.

---

## Engine coupling note

`tp_pack_read` re-implements builder math: the UV round-half-up, `transform_point`
(a byte-for-byte mirror of `nt_builder_atlas_geometry.c:983-1001`), hull-inflation
recovery, and alias inference from layout. Struct-version bumps fail loud;
**same-version math changes fail silent**, guarded only by fixtures. The standing
mitigations are (a) an upstream engine issue to expose the decode contract as a
public engine header, and (b) an engine-format fingerprint (hash of a known
fixture's packed bytes) pinned in a test so any silent math change breaks CI.
