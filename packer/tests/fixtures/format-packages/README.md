# Lua format package fixtures

This corpus freezes small API-v1 descriptor and source shapes before the
production parser and Lua host land. It is data only: no placeholder CTest is
registered for Packet 0.

| Directory | Expected admission |
|---|---|
| `valid-minimal` | available |
| `valid-full` | available |
| `invalid-duplicate-key` | unavailable: `descriptor_invalid_json` |
| `invalid-unknown-member` | unavailable: `descriptor_schema` |
| `invalid-reserved-id` | unavailable: `format_id_reserved` |
| `invalid-extra-entry` | unavailable: `package_extra_entry` |
| `duplicate-id-a`, `duplicate-id-b` | both unavailable: `duplicate_format_id` |

Each package normally contains exactly `format.json` and `export.lua`.
`invalid-extra-entry` intentionally violates that rule. Reparse-point, file
type, invalid-UTF-8, bytecode, maximum-size, and allocator/fault cases are
created under the build tree by their owning tests; large or platform-specific
artifacts are not checked in.

Packets 1 and 2 make these rows executable through
`tp_format_fixture_contract`, `tp_format_catalog_contract`, and the Lua gates
listed in `docs/formats/lua-package-v1.md`.
