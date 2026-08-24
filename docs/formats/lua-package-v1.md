# Sandboxed Lua export package v1

**Status:** Current wire/runtime contract.

This document freezes package API version 1. It owns the exact `format.json`
wire shape, Lua-visible Export IR projection, sandbox surface, diagnostic
vocabulary, fingerprints, and resource limits. The product lifecycle and
process ownership are defined by
[`../spec/format-ecosystem.md`](../spec/format-ecosystem.md).

`api_version: 1` versions all of these together. A change that cannot preserve
this document requires a new integer API version. Packages have no independent
semantic version.

## Package and discovery

The only discovery root is `<real executable directory>/formats/`. Every direct
child real directory is one candidate. A package directory contains exactly two
real regular files:

```text
<package-directory>/
  format.json
  export.lua
```

Extra entries, symlinks, junctions, reparse points, devices, pipes, sockets,
hard-linked directory escapes, or nested modules make the row unavailable.
Non-directory entries directly under `formats/` are ignored but still count
toward the bounded root-enumeration work limit.

The root and package directory are opened as verified no-follow handles. POSIX
uses directory handles with `openat`, `O_NOFOLLOW`, and `fstat`. Windows opens
handles with reparse-point inspection and verifies final containment beneath
the opened root. Each package file is opened once, type/size checked through
that handle, and read to the observed bounded size. Those exact bytes are
validated, fingerprinted, snapshotted, compiled, and executed. A
stat-then-reopen path is non-conforming.

The directory spelling is diagnostic context, not package identity. Candidate
names must be Unicode-scalar UTF-8, 1..255 bytes, contain no control character,
slash, backslash, or colon, and be neither `.` nor `..`. An invalid native name
produces one unavailable `package_name_invalid` row keyed by a bounded lowercase
hex hash of its platform-native bytes/code units; its raw spelling is never put
in a report. The key is `invalid-name-posix-<id128>` over the POSIX directory
name bytes or `invalid-name-win16-<id128>` over each Windows UTF-16 code unit
encoded little-endian, using the same FNV-1a/128 and lowercase rendering as the
content fingerprint. This diagnostic-only key need not match across operating
systems because such a name is not a portable package. All other ordering
comparisons use unsigned UTF-8 bytes: available runtime rows sort by ID;
unavailable rows sort by package-directory spelling. Native rows precede
runtime rows.

Missing `formats/` produces a complete native-only catalog. A complete scan
with malformed children succeeds with unavailable rows. Exceeding a root,
candidate, or catalog-byte limit succeeds fail-closed as native-only with one
`catalog_limit` diagnostic; it never publishes an enumeration-order prefix.
Root identity/enumeration/I/O failure, host OOM, or incomplete compile protocol
makes a candidate catalog ineligible. Startup retains the static native-only
baseline; Reload retains its previous active generation.

## Descriptor JSON

`format.json` is Unicode-scalar UTF-8 without BOM or NUL. It is one JSON object
with no duplicate or unknown members, no trailing data, and the exact shape
below:

```json
{
  "api_version": 1,
  "id": "defold-tpinfo-2",
  "display_name": "Defold (.tpinfo + .tpatlas)",
  "capabilities": {
    "transforms": ["identity", "rotate_90_cw"],
    "polygons": true,
    "pivot": true,
    "slice9": false,
    "multipage": true,
    "aliases": true,
    "animations": true
  },
  "outputs": [
    {"id": "tpinfo", "suffix": ".tpinfo"},
    {"id": "tpatlas", "suffix": ".tpatlas"}
  ],
  "host_facts": [
    {
      "id": "tpinfo_resource",
      "kind": "project_resource",
      "output": "tpinfo",
      "root_marker": "game.project",
      "missing": "basename_notice"
    }
  ]
}
```

JSON numbers are accepted only for `api_version`, which must be the integer
token `1`; exponent, fraction, negative zero, and coercion from strings are
invalid. Every capability boolean is required. `host_facts` is optional and
defaults to an empty array. All other fields are required.

After bounded JSON syntax and duplicate-member validation, version dispatch
requires one root `api_version`. A missing or non-integer-token member is
`descriptor_schema`. Any valid integer token other than `1` is
`api_unsupported`, and the host performs no API-v1 field validation or source
compilation for that row. This lets an older packer reject a newer package by
version even when that package contains fields unknown to API v1.

### Token grammars

Lengths below are UTF-8 byte lengths, excluding the terminating C NUL:

| Value | Grammar | Bytes |
|---|---|---:|
| format ID | `[a-z][a-z0-9]*(?:-[a-z0-9]+)*` | 1..63 |
| output/fact ID | `[a-z][a-z0-9]*(?:_[a-z0-9]+)*` | 1..63 |
| display name | non-empty Unicode-scalar UTF-8, no C0/C1 control | 1..255 |
| suffix | `\.[a-z0-9][a-z0-9._-]*`, no `..`, no trailing dot | 2..32 |
| package directory | Unicode-scalar UTF-8, restrictions above | 1..255 |
| diagnostic package path | Unicode-scalar UTF-8, no NUL | 0..4095 |

Format IDs `json-neotolis` and every other compiled-in native ID are reserved.
Output IDs are unique among outputs, fact IDs among facts, and suffixes among
outputs. Suffixes are compared with ASCII case folding as well as byte-exactly
and must not contain a slash, backslash, colon, drive/device spelling, space,
or page-name collision. Array order is contract order. Transform tokens must
be unique and in ascending value order; `identity` is mandatory.

The D4 vocabulary maps exactly to the existing stored transform values:

| Value | Token | Bits |
|---:|---|---|
| 0 | `identity` | none |
| 1 | `flip_h` | horizontal |
| 2 | `flip_v` | vertical |
| 3 | `rotate_180` | horizontal + vertical |
| 4 | `transpose` | diagonal |
| 5 | `rotate_90_cw` | diagonal + horizontal |
| 6 | `rotate_90_ccw` | diagonal + vertical |
| 7 | `anti_transpose` | diagonal + horizontal + vertical |

Capability loss uses the existing append-only `tp_notice_field` vocabulary.
Transforms, polygons, pivot, slice9, and aliases map to their current
`TP_NOTICE_FIELD_*` values. API v1 appends
`TP_NOTICE_FIELD_ANIMATION = 7`, whose CLI/JSON field token is `animation`.
When animations are unsupported and the IR has one or more, common projection
emits one atlas-wide notice with that field and
`TP_NOTICE_REASON_CAPS_UNSUPPORTED`, then exposes zero animations. A descriptor
with `multipage: false` receives a multi-page IR as a hard target error rather
than a lossy notice. Lua never decides or duplicates these losses.

### Outputs

`outputs` contains 1..32 objects with exactly `id` and `suffix`. The common
artifact planner maps each item to `<target-base><suffix>` in declared order.
The core appends its existing page paths after the documents. Lua cannot change
the count, order, suffix, base, directory, or PNG names.

### Host facts

API v1 accepts 0..8 facts and only the exact `project_resource` shape shown
above:

- `output` references a declared output ID;
- `root_marker` is exactly `game.project`;
- `missing` is exactly `basename_notice`.

The host probes at most ten ancestors from the resolved planned output
directory. If found, the fact is the normalized leading-slash project resource
path to the referenced document. Otherwise it is that document's basename and
the core emits one typed fallback notice. Lua receives only the final string;
it receives no probed path, directory, or filesystem operation.

## Content fingerprint

The non-authenticating package fingerprint is the existing FNV-1a/128
`tp_hasher` result over this byte frame:

```text
ASCII "ntpacker-format-package-v1\0"
u32le api_version
u64le format_json_byte_count
exact format.json bytes
u64le export_lua_byte_count
exact export.lua bytes
```

It is rendered as the canonical 32-character lowercase `tp_id128` hex string.
Whitespace and line-ending edits intentionally change it. The fingerprint is a
job identity and diagnostic aid, not a signature or trust decision.

## Compile admission

`export.lua` is Unicode-scalar UTF-8 without BOM or NUL. A leading bytecode
escape and every binary chunk are rejected before Lua. The isolated validator
uses `luaL_loadbufferx` with mode `"t"`, a sanitized logical chunk name
`@formats/<id>/export.lua`, the production allocator, and the same compile
limits as execution.

The host streams one already-admitted candidate at a time to a persistent
self-reexec validation worker. Before compiling, the worker returns the
candidate index; after compiling it returns that row's result. A panic, crash,
or timeout therefore marks only the announced row `compile_worker_failed`. The
host restarts at the next index. A reply without an attributable current index,
malformed frame, host OOM, or exhausted total restart/stream budget makes the
whole candidate catalog ineligible.

Successful compilation does not execute the chunk and cannot make a row
available until the complete batch finishes. The runtime recompiles the exact
job-owned bytes in its isolated job worker.

## Handler lifecycle

The source chunk executes in a fresh Lua state and must return exactly one Lua
function and no additional value:

```lua
return function(atlas, host)
    local output = host:document("metadata")
    output:write("atlas=")
    output:write_json_string(atlas:name())
    output:write("\n")
    output:finish()
end
```

The host calls that function with two opaque immutable userdata values. A
successful handler returns no values and has opened and finished each declared
document exactly once. Returning a value, missing/duplicating a document,
writing after `finish`, retaining an invalid view, or calling a method with a
wrong userdata/type/index is `handler_contract`.

Every serialization creates and destroys a state. Bytecode, globals, compiled
functions, user tables, writers, and IR views are never cached between targets
or jobs.

## Lua-visible Export IR

All collection indices are 1-based and preserve common-core deterministic
order. A bad index raises `handler_contract`; it never returns a borrowed C
pointer. Integer coordinates are exact signed Lua integers. `lua_Integer` is
required to be 64-bit and `lua_Number` IEEE-754 binary64; the build fails if
either premise is false.

### Atlas

| Call | Result |
|---|---|
| `atlas:name()` | final atlas name string |
| `atlas:pixels_per_unit()` | positive finite number originating from the canonical f32 |
| `atlas:page_count()` | integer, 1..8 |
| `atlas:page(i)` | page userdata |
| `atlas:sprite_count()` | integer, 0..65535 |
| `atlas:sprite(i)` | final-name-sorted sprite userdata |
| `atlas:animation_count()` | integer, 0..65535; zero when animations are unsupported |
| `atlas:animation(i)` | ID-sorted animation userdata |

### Page

| Call | Result |
|---|---|
| `page:index()` | 1-based page index |
| `page:width()`, `page:height()` | integer dimensions, 1..16384 (the build-wide `TP_PACK_MAX_PAGE_DIM`) |
| `page:image()` | planned PNG basename only |

### Sprite

| Call | Result |
|---|---|
| `sprite:name()` | final export name |
| `sprite:page()` | 1-based page index |
| `sprite:frame()` | placed page `x, y, width, height`; dimensions are canonical unrotated/pre-D4 values |
| `sprite:footprint()` | stored on-page `width, height`; swaps frame dimensions for every diagonal D4 value |
| `sprite:trim_rect()` | unrotated source trim `x, y, width, height` |
| `sprite:source_size()` | untrimmed `width, height` |
| `sprite:transform()` | one D4 token admitted by descriptor capabilities |
| `sprite:trimmed()` | boolean |
| `sprite:is_solid()` | boolean |
| `sprite:pivot()` | finite normalized `x, y` (values may lie outside `[0,1]`), or `nil` when unsupported |
| `sprite:slice9()` | four integers `left, right, top, bottom` in `0..65535`, or `nil` when all-zero/unsupported |
| `sprite:alias_of()` | 1-based original sprite index, or `nil`; hidden when unsupported |
| `sprite:vertex_count()` | 0 when polygons are unsupported, otherwise 0..16 |
| `sprite:vertex(i)` | trim-local unrotated integer `x, y` |
| `sprite:index_count()` | 0 when polygons are unsupported, otherwise 0..42 and divisible by 3 |
| `sprite:index(i)` | canonical zero-based vertex index value |

Alias sprite rows remain present so every final name can be emitted; disabling
alias support hides only the relationship and produces the common loss notice.
Single-page descriptors reject a multipage IR before Lua. Unsupported pivots,
slice9, polygons, animations, and transform values cannot be rediscovered
through another method.

### Animation

| Call | Result |
|---|---|
| `animation:id()` | final animation ID |
| `animation:frame_count()` | ordered frame count, 0..65535 |
| `animation:frame(i)` | final sprite name string |
| `animation:fps()` | positive finite canonical f32 number |
| `animation:playback()` | integer project playback ID 0..6 |
| `animation:flip_h()`, `animation:flip_v()` | booleans |

## Host and writer API

| Call | Contract |
|---|---|
| `host:document(id)` | opens the declared document once and returns its writer |
| `host:fact(id)` | returns a declared ready host-fact string |
| `host:notice(message)` | appends one deterministic prose-only `tp_export_notice` |
| `host:fail(message)` | raises the uncatchable `handler_failed` sentinel |

Writer methods append directly to one host-owned bounded buffer:

| Call | Bytes |
|---|---|
| `writer:write(text)` | exact valid UTF-8 text; NUL rejected |
| `writer:write_json_string(text)` | quoted JSON string using the common core escaper |
| `writer:write_i64(integer)` | canonical base-10 signed integer |
| `writer:write_u64(integer)` | canonical base-10 integer in `0..INT64_MAX` |
| `writer:write_f32(number)` | finite, range-checked binary32 rounded value, `C` locale `%.9g` |
| `writer:write_bool(boolean)` | `true` or `false` |
| `writer:write_null()` | `null` |
| `writer:finish()` | seals the document; exactly once |

The final document must be Unicode-scalar UTF-8 with no NUL. It is text, not an
arbitrary binary blob. Output order is descriptor order regardless of the order
in which the handler opens writers.

## Sandbox environment

The implementation never calls `luaL_openlibs`. It builds a fresh environment
table from an explicit allowlist and installs that table as the chunk's `_ENV`.
No library table or function not named below is reachable. Strings have no
library metatable beyond this same safe `string` table.

Allowed globals:

```text
assert  error  select  type
```

Allowed library members:

```text
math.abs  math.ceil  math.floor  math.max  math.min
math.tointeger  math.type  math.ult  math.maxinteger  math.mininteger

string.byte  string.char  string.len  string.sub

utf8.char  utf8.codepoint  utf8.codes  utf8.len  utf8.offset
```

Lua syntax for local variables, functions, tables, numeric/generic loops,
arithmetic, comparisons, indexing, length, and concatenation remains available.
The private Lua build defines `LUA_NOCVTN2S` and `LUA_NOCVTS2N`, so implicit
number/string conversion is rejected; typed writer functions own numeric text.

Everything else is absent, including `_G`, `_VERSION`, `collectgarbage`,
`dofile`, `load`, `loadfile`, `getmetatable`, `setmetatable`, `next`, `pairs`,
`ipairs`, `pcall`, `xpcall`, `print`, `warn`, `rawequal`, `rawget`, `rawlen`,
`rawset`, `tonumber`, `tostring`, `coroutine`, `debug`, `io`, `os`, `package`,
`require`, native modules, clocks, random, locale functions, `string.dump`,
`string.find`, `string.format`, `string.gmatch`, `string.gsub`, `string.lower`,
`string.match`, `string.pack`, `string.packsize`, `string.rep`, `string.reverse`,
`string.unpack`, `string.upper`, and the entire `table` library.

No userdata exposes `__gc`, `__close`, mutable `__newindex`, or a script-visible
metatable. Scripts cannot catch cancellation or limit sentinels because
protected-call functions are absent.

## Fixed limits

These are API-v1 constants, not defaults. Descriptors and user settings cannot
change them. All byte arithmetic is checked before allocation.

### Discovery, descriptor, and binding

| Limit | Value |
|---|---:|
| root entries examined | 4096 |
| package candidates | 256 |
| package-directory UTF-8 bytes | 255 |
| `format.json` bytes | 65,536 |
| `export.lua` bytes | 1,048,576 |
| admitted descriptor + source bytes per catalog | 67,108,864 |
| distinct bindings per job | 256 |
| descriptor + source bytes per job | 67,108,864 |
| target binding references per job | 262,144 |
| output declarations | 32 |
| host facts | 8 |
| JSON depth | 16 |
| JSON nodes | 2,048 |
| entries in one JSON container | 256 |

A JSON node is each object, array, member value, array value, string, number,
boolean, or null. A container entry is one object member or array element.

### Compile-validation transport

| Limit | Value |
|---|---:|
| worker restarts after the initial process | 64 |
| protocol frames over the complete attempt | 1,024 |
| cumulative request-stream bytes | 83,886,080 (80 MiB) |
| cumulative response-stream bytes | 16,777,216 (16 MiB) |
| candidates compiled | 256 |
| cumulative source bytes compiled | 67,108,864 (64 MiB) |

The existing outer-worker five-minute timeout applies to each process. There is
no package-controlled or Lua-specific wall-clock setting. Exhausting any table
entry makes the candidate catalog ineligible; it does not silently omit later
rows.

### Lua execution and output

| Limit | Value |
|---|---:|
| Lua allocator live bytes, including bootstrap/compile/runtime | 134,217,728 (128 MiB) |
| VM instructions per serialization | 250,000,000 |
| instruction hook interval | 10,000 |
| all host API calls | 8,388,608 |
| writer calls | 4,194,304 |
| bytes in one writer string argument | 1,048,576 |
| bytes in one host-fact value | 4,095 |
| bytes in one document | 67,108,864 (64 MiB) |
| bytes across all documents | 67,108,864 (64 MiB) |
| notices | 4,096 |
| one notice message | 1,024 bytes |
| all notice messages | 1,048,576 bytes |
| Lua C-call nesting | upstream `LUAI_MAXCCALLS`, pinned to 200 |

The custom allocator covers state creation through close and distinguishes its
budget sentinel from host OOM. A saturating instruction counter polls the
latched cancellation token. Every host call is constant or explicitly bounded,
increments the host-call counter, and polls cancellation; writer calls also
increment their narrower counter. Compilation polls before and after the
bounded text-only load because Lua hooks do not run in the parser.

### Diagnostic ownership

| Limit | Value |
|---|---:|
| diagnostics in one report | 256, including truncation marker |
| dynamic bytes in one report | 1,048,576 |
| message bytes | 1,024 |
| Lua frames per diagnostic | 16 |
| bytes in one sanitized frame | 256 |
| diagnostics across a catalog or job | 4,096 |
| diagnostic bytes across a catalog or job | 4,194,304 |

Reports reserve their final slot for one allocation-free
`diagnostics_truncated` marker. The primary `tp_status` and `tp_error`, including
file-I/O phase/path/native code, are validated first and are never replaced by
this report.

## Diagnostic vocabulary

Codes are append-only stable ASCII tokens. Phase and normal severity are shown
below; a primary error may make the surrounding operation fail even when an
individual row diagnostic is informational.

| Code | Phase | Severity | Meaning |
|---|---|---|---|
| `catalog_limit` | discovery | error | root/candidate/catalog bound exceeded; native-only fail-closed result |
| `root_not_directory` | discovery | error | runtime root has the wrong type |
| `root_reparse` | discovery | error | root is a symlink/reparse point |
| `root_io` | discovery | error | root identity or enumeration failed |
| `package_name_invalid` | discovery | error | candidate name is not a portable bounded UTF-8 name |
| `package_reparse` | discovery | error | candidate directory is indirect |
| `package_extra_entry` | discovery | error | package contains an entry other than the two fixed files |
| `descriptor_missing` | discovery | error | `format.json` is absent |
| `source_missing` | discovery | error | `export.lua` is absent |
| `package_file_type` | discovery | error | a fixed entry is not a regular file |
| `package_file_reparse` | discovery | error | a fixed entry is indirect or escapes containment |
| `package_file_too_large` | discovery | error | descriptor/source byte cap exceeded |
| `package_read_failed` | discovery | error | opened file could not be read completely |
| `descriptor_invalid_utf8` | descriptor | error | descriptor is not admitted UTF-8 text |
| `descriptor_invalid_json` | descriptor | error | JSON syntax, depth, node, duplicate, or trailing-data failure |
| `descriptor_schema` | descriptor | error | required/unknown field or value-type failure |
| `api_unsupported` | descriptor | error | integer API is not 1 |
| `format_id_invalid` | descriptor | error | format ID grammar failure |
| `format_id_reserved` | descriptor | error | runtime ID conflicts with a native ID |
| `output_invalid` | descriptor | error | output ID/suffix/count failure |
| `output_conflict` | descriptor | error | duplicate or host-case/path collision |
| `host_fact_invalid` | descriptor | error | fact shape/reference/count failure |
| `duplicate_format_id` | descriptor | error | two runtime rows claim one ID; both unavailable |
| `source_invalid_utf8` | compile | error | source is not admitted UTF-8 text |
| `source_binary` | compile | error | bytecode/binary source was supplied |
| `compile_error` | compile | error | protected text compilation failed; line is reported when known |
| `compile_worker_failed` | compile | error | announced candidate crashed, panicked, or timed out |
| `compile_protocol` | compile | error | response cannot be attributed or decoded; catalog ineligible |
| `compile_budget` | compile | error | restart/frame/byte/work budget exhausted; catalog ineligible |
| `handler_contract` | runtime | error | handler signature, view, writer, or return contract was violated |
| `handler_failed` | runtime | error | uncaught script error or explicit `host:fail` |
| `handler_panic` | runtime | error | worker terminated from `lua_atpanic` |
| `memory_limit` | limit | error | Lua allocator ceiling reached |
| `instruction_limit` | limit | error | saturating instruction ceiling reached |
| `host_call_limit` | limit | error | host/writer call ceiling reached |
| `output_limit` | limit | error | per-document or total text ceiling reached |
| `notice_limit` | limit | error | notice count/byte ceiling reached |
| `document_unknown` | output | error | undeclared output ID requested |
| `document_duplicate` | output | error | declared output opened more than once |
| `document_unfinished` | output | error | opened writer was not finished |
| `document_missing` | output | error | declared output was never opened |
| `document_write_after_finish` | output | error | sealed writer was reused |
| `document_invalid_utf8` | output | error | final document is not Unicode-scalar UTF-8 |
| `document_contains_nul` | output | error | final document contains NUL |
| `diagnostics_truncated` | any | warning | one or more diagnostics/frames/messages were omitted by a hard cap |

Discovery reports are owned by catalog rows. Runtime/output reports are owned by
the job result and targets reference validated slices. Diagnostic paths are
logical (`formats/<directory>/format.json` or `export.lua`), never arbitrary
host paths. Lua frames contain only the sanitized chunk name, bounded function
label, and positive line; column is absent when Lua cannot provide it.

Compile/source failures keep a row unavailable until Reload. Runtime, data,
limit, or output failure affects only that target invocation and does not mutate
the active catalog.

`host:notice` is deliberately not a diagnostic code. It appends to the existing
export-notice slice with `sprite = NULL`, `target = <format ID>`,
`TP_NOTICE_FIELD_NONE`, and `TP_NOTICE_REASON_NONE`; its sanitized message is
the only package-controlled prose. Hitting the notice bound fails with the
`notice_limit` diagnostic, so a successful export never silently loses notices.

## Required gates

Packet implementations register these exact CTest names as their owning behavior
lands; no placeholder test is considered a gate:

```text
tp_format_fixture_contract
tp_format_catalog_contract
tp_format_catalog_paths
tp_format_catalog_faults
tp_format_catalog_context
tp_lua_vendor_contract
tp_lua_sandbox_contract
tp_lua_limits_faults
tp_lua_determinism
tp_format_compile_worker
tp_format_binding_protocol
```

The checked-in corpus under `packer/tests/fixtures/format-packages/` exercises
the production descriptor parser, compiler, sandbox, and runtime host.
