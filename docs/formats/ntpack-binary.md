# Engine `.ntpack` parse-back contract

**Status:** Current engine-integration contract.

`nt_builder` writes an `.ntpack` and `tp_pack_read` parses its atlas and texture
assets back into the canonical pack result used by preview and exporters.
Input bytes are untrusted at this boundary. The reader bounds-checks and
version-checks the fields it consumes and returns a structured status for
invalid input; it is not a general validator for every reserved or currently
unused field in the engine formats.

The authoritative layout definitions are the read-only engine headers:

- `external/neotolis-engine/shared/include/nt_pack_format.h`
- `external/neotolis-engine/shared/include/nt_atlas_format.h`
- `external/neotolis-engine/shared/include/nt_texture_format.h`

This document records the versions consumed by the current project. Engine
format version changes require a coordinated reader/test update.

## Pack container

All multi-byte values are little-endian.

```text
NtPackHeader (32 bytes)
NtAssetEntry[asset_count] (24 bytes each)
padding to 8-byte data alignment
asset payloads
metadata section
```

Required constants:

- magic `NPAK` / `0x4B41504E`;
- pack version 2;
- atlas asset type 6;
- texture asset type 2.

Asset offsets are relative to the file. For entries it consumes, `offset +
size` must remain within the declared total size. The reader checks container
magic, pack version, declared total size, entry-table bounds, required asset
types, and payload magic/version. It does not currently enforce the container
`header_size`, CRC32, global `meta_count`/`meta_offset`, or each asset entry's
`format_version`; payload headers provide the versions used for admission.

`pixels_per_unit` is a four-byte float metadata payload keyed by
`nt_hash64_str("pixels_per_unit")`. Records have a 20-byte header and payloads
are padded to four-byte alignment. Missing metadata defaults to 1.

## Atlas asset version 6

```text
NtAtlasHeader                         28 bytes
uint64_t texture_resource_ids[]       8 bytes each
NtAtlasRegion[]                       48 bytes each
NtAtlasVertex[]                       8 bytes each
uint16_t indices[]
```

Required atlas magic is `ATLS` / `0x534C5441`. Header vertex and index offsets
are relative to the atlas payload.

An atlas region contains:

- name hash;
- source dimensions;
- y-up trim offsets and normalized pivot;
- vertex/index starts and counts;
- page index and raw D4 transform mask;
- 9-slice borders `[left,right,top,bottom]`.

Vertices store trim-local y-up `int16` positions and quantized page UVs.
Indices are local to the region and form a triangle list.

The reader converts the engine's y-up geometry back to canonical y-down PNG
space. It preserves the raw transform mask, normalizes recovered hull vertices
to a `(0,0)` bounding-box origin, and makes `spriteSourceSize` equal the
recovered vertex bbox. Polygon hull inflation means this bbox may differ
slightly from the decoder's original alpha trim.

## Texture asset version 3

`NtTextureAssetHeader` is 28 bytes with magic `TTEX` / `0x58455454`.
The builder profile currently produces:

- compression `RAW`;
- format `RGBA8`;
- mip count 1;
- pixel bytes immediately after the header.

Reader admission requires `RAW`, `RGBA8`, nonzero dimensions, and enough entry
payload bytes for one RGBA8 image after the header. It does not currently
enforce the texture header's `mip_count` or `data_size` fields. Compressed/BASIS
assets return `unsupported_texture`; the reader does not transcode. The
premultiplied flag is surfaced on the page result rather than rejected.

Atlas page references are matched against stored texture resource IDs. The
reader never reconstructs the texture hash from a guessed name.

## D4 and frame recovery

The transform byte uses:

- bit 0: horizontal flip;
- bit 1: vertical flip;
- bit 2: diagonal coordinate swap.

Apply diagonal, then horizontal flip, then vertical flip. A diagonal transform
swaps output dimensions.

Page coordinates are recovered from the quantized UV exactly for supported page
sizes:

```text
pixel = round(uv16 * page_dimension / 65535)
```

The builder encoded integer page coordinates with the inverse rounded mapping,
and the supported maximum dimension keeps quantization error below half a
pixel. `tp_uv_property` pins this behavior.

For each sprite the reader verifies:

- page index and all geometry spans are in bounds;
- frame footprint lies inside its page;
- transformed page AABB matches the recovered unrotated hull dimensions;
- names resolve uniquely through the caller-provided hash map;
- aliases refer to shared placement rather than duplicate pixels.

## Coupling risk

Struct-version changes fail loudly. Same-version builder-math changes could
otherwise fail silently, so `tp_pack_read`, UV property, polygon, alias,
transform, multipage, and exporter tests are the executable guards.
