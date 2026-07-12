# Upstream issue draft (DO NOT auto-file) — document the RGBA buffer lifetime of `nt_builder_atlas_add_raw`

Status: DRAFT for lead review. This is a proposed issue against the
`neotolis-engine` repository. It is NOT filed and the submodule is NOT patched.
The packer only needs a documented public contract; it does not need a behaviour
change.

## Summary

`nt_builder_atlas_add_raw` takes `const uint8_t *rgba_pixels`, but its public
header (`tools/builder/nt_builder.h`) does not state whether the builder **copies**,
**adopts**, **trims**, or **retains** that buffer, nor how long the caller must
keep it alive. Please document the lifetime contract in the public header.

## Public declaration (no lifetime promise)

`tools/builder/nt_builder.h` (Atlas API):

```c
void nt_builder_atlas_add_raw(NtBuilderContext *ctx, const uint8_t *rgba_pixels,
                              uint32_t width, uint32_t height,
                              const nt_atlas_sprite_opts_t *opts);
```

The surrounding block comment covers `opts` (name/pivot, zero-init footgun) but
says nothing about `rgba_pixels` ownership.

## Observed implementation (current pin)

`tools/builder/nt_builder_atlas.c` currently **deep-copies** the pixels at add
time, so the caller may free/reuse its buffer immediately after the call:

```c
/* Deep-copy RGBA pixels */
uint32_t pixel_bytes = width * height * 4;
uint8_t *pixels = (uint8_t *)malloc(pixel_bytes);
NT_BUILD_ASSERT(pixels && "atlas_add_raw: alloc failed");
memcpy(pixels, rgba_pixels, pixel_bytes);
...
sprite->rgba = pixels; /* the builder owns the copy */
```

(`nt_builder_atlas` around the `atlas_add_raw` body; the sibling `atlas_add`
path copies the same way.)

This is an implementation detail today, not a documented guarantee — a future
optimization (e.g. adopting the caller pointer to avoid the copy) could silently
break every downstream caller that relies on the current behaviour.

## Why it matters to the packer

The texture packer feeds decoded/synthesized RGBA into `add_raw` and would like
to free that scratch immediately after the call rather than pin it until
`finish_pack`. Whether it may do so depends entirely on this undocumented
lifetime. The master-spec pack pipeline (§10.5, §52.3) explicitly defers "whether
the engine copies, adopts, trims, or retains raw RGBA during packing" to "an
engine-builder decision to be fixed by API contract and profiling".

## Requested change

Document the lifetime in the public header comment for `nt_builder_atlas_add_raw`
(and `nt_builder_atlas_add`), e.g. one of:

- **Copy (matches current behaviour):** "The builder copies `rgba_pixels`
  immediately; the caller may free or reuse the buffer as soon as the call
  returns."
- **Adopt / borrow:** state the required lifetime (e.g. "the buffer must remain
  valid and unmodified until `nt_builder_finish_pack`") and, if adopted, who
  frees it.

Documenting the current copy semantics is the minimal, non-breaking fix and is
what the packer would build against.

## Evidence

Packer-side executable regression proving the current copy behaviour:
`packer/tests/test_raw_ownership.c` (`tp_raw_ownership` ctest). It fills an RGBA
buffer, calls `nt_builder_atlas_add_raw`, **mutates and frees that buffer before
`finish_pack`**, packs, reads the result back, and asserts the packed page still
holds the original pixels. It passes on the current submodule pin — i.e. the copy
is real today. If the engine adopts the contract in the header, this test becomes
the packer's guard that the promised behaviour holds.
