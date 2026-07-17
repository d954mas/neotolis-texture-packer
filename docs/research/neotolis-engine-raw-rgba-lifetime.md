# Neotolis Engine raw RGBA lifetime contract

Status: research note and upstream issue draft. Do not file automatically.
The engine submodule remains read-only.

## Gap

`nt_builder_atlas_add_raw` accepts `const uint8_t *rgba_pixels`, but the public
declaration in `tools/builder/nt_builder.h` does not say whether the builder
copies, adopts, trims, or retains the buffer, or how long the caller must keep
it alive:

```c
void nt_builder_atlas_add_raw(NtBuilderContext *ctx,
                              const uint8_t *rgba_pixels,
                              uint32_t width,
                              uint32_t height,
                              const nt_atlas_sprite_opts_t *opts);
```

The current pinned implementation deep-copies `width * height * 4` bytes during
the call and stores the copy on the builder sprite. Therefore current behavior
allows the caller to reuse or free its input immediately. This is observed
implementation behavior, not yet a public API promise; an optimization that
retains the pointer could silently break callers.

## Why the packer cares

The packer feeds decoded or synthesized RGBA into `nt_builder_atlas_add_raw` and
wants to release scratch storage immediately rather than retain it through
`nt_builder_finish_pack`. Master spec §§10.5 and 52.3 intentionally defer the
copy/adopt/borrow choice to an engine-builder API contract backed by profiling.

## Requested upstream clarification

Document the lifetime for both `nt_builder_atlas_add_raw` and
`nt_builder_atlas_add` in their public header. The minimal non-breaking wording
for current behavior is:

> The builder copies the pixel buffer before the call returns; the caller may
> free or reuse the buffer immediately afterward.

If the engine instead chooses borrow or adopt semantics, the header must state
the required validity interval, mutation rules, and ownership of deallocation.

## Executable evidence

`packer/tests/test_raw_ownership.c` (`tp_raw_ownership`) exercises the current
copy behavior. It passes a raw RGBA buffer, mutates and frees the caller buffer
before `nt_builder_finish_pack`, then verifies that the packed page still
contains the original pixels. This guards the observed engine pin today and can
guard the public promise if the engine adopts it later.
