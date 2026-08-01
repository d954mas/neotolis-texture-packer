# Sources and raster ingress

**Status:** Current architecture.

Project v5 supports tagged `folder` and `file` sources. Linked foreign-atlas
sources are a future schema/capability and are not accepted by current
operations or serialization.

## Source identity

A source has a stable structural ID, stored kind, and project-relative or
absolute path. A sprite is identified by that source ID plus its normalized
source-local key.

Keys:

- preserve the filename extension;
- normalize separators and path components;
- use strict UTF-8 and Unicode NFC through vendored utf8proc;
- preserve case, so case variants remain distinct runtime identities;
- reject absolute keys and `..` traversal outside the source root;
- are independent of display/export rename.

Case folding is used only to report a cross-platform portability collision; it
does not merge or rewrite two differently cased keys.

Removing a source that still owns sprite overrides or animation frames is a
graph-integrity error. A known source or key that is temporarily unavailable
remains a validation/runtime finding rather than being silently deleted.

## Scanning

Folder sources are recursively scanned; file sources contribute one image.
Current accepted extensions are:

```text
.png .jpg .jpeg .bmp .tga
```

Scanner ordering and source-key derivation are deterministic. Duplicate keys,
invalid UTF-8, path overflows, inaccessible files, and decode errors become
structured findings. The immutable runtime projection preserves the complete
source-local `tp_error` beside its coarse status, so a live headless client does
not need to reconstruct diagnostics from presentation text.

The stored `kind` is authoritative when disk cannot be consulted. Runtime
stat/scanning can still observe that a path's current filesystem type differs.

## External refresh

Refresh is the third session task kind beside Pack and Export. Admission
captures an immutable session snapshot, and the worker performs all filesystem
stat/scanning off the session owner thread. Its cancel token is polled between
sources and recursive directory entries. A successful terminal result first
prepares the complete replacement view, then atomically replaces one
session-owned immutable source-runtime projection keyed by stable atlas/source
IDs and canonical source-local keys.

Filesystem calls are not assumed to be preemptible. The Refresh worker holds a
private owner lease until terminal, so retiring the live session requests
cancellation but does not block indefinitely in `join`. Normal completion is
joined and compacted by the owner; a worker that outlives its retired session
releases and detaches its own completed thread state. It owns only the admitted
snapshot, previous projection, and terminal result—never the mutable session.

The adopted replacement advances source-runtime state and adds a non-undoable
visible refresh marker. It does not:

- change project revision;
- change semantic dirty state;
- add an Undo record;
- rewrite stored project paths;
- start Pack.

The GUI exposes explicit Refresh and renders only the borrowed projection from
`tp_session_view`. It has no filesystem scan cache, fingerprint baseline, or
parallel source truth. Pack and Export read their own immutable job inputs and
do not implicitly start Refresh. Automatic Refresh requests caused by source
membership edits, Undo/Redo, New, or Open are one coalesced host-owned pending
bit; an occupied task slot cannot lose the request and no second task queue is
created. The current product does not implement persistent filesystem watchers.
Watcher-driven refresh is a target capability and must preserve the same task
admission and semantic-purity boundary.

## Raster decode boundary

The shared `tp_image` ingress uses stb_image to decode into tightly packed,
y-down RGBA8 under explicit dimension/allocation limits. The matching decoder
allocator releases the returned pixels. Invalid or oversized input returns a
structured error rather than an assertion.

Current behavior does not implement:

- EXIF orientation transforms;
- ICC color transforms or ICC-specific notices;
- WebP input;
- a published color-management policy beyond the decoder's current byte output.

Those behaviors must not be claimed as current documentation. If introduced,
they require executable decoder fixtures and must feed the same canonical pixel
stream used by Pack and semantic image hashing.

## Engine raw-pixel lifetime

The pinned engine revision's public `nt_atlas_add_raw` contract borrows the RGBA
buffer for the duration of the call and deep-copies valid input. Packer releases
decoded source pixels after the atlas commit, and `tp_raw_ownership` protects
that public lifetime contract across engine updates. This guarantee is specific
to `nt_atlas_add_raw`; ownership for other engine entry points remains explicit.
