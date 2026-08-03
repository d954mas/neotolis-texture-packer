# Pinned dependency: Lua

Sandbox runtime for target API v1 in
[`docs/formats/lua-package-v1.md`](../../../docs/formats/lua-package-v1.md).
Packet 0 pins the reproducible input and post-patch inventory. Packet 2 vendors
those exact files and adds the private static-library target; configure and
build never download them.

## Release source

| Field | Value |
|---|---|
| Library | PUC Lua |
| Release | **5.5.0** |
| Upstream | https://www.lua.org/ |
| Source artifact | `lua-5.5.0.tar.gz` |
| Download URL | https://www.lua.org/ftp/lua-5.5.0.tar.gz |
| Tarball SHA-256 | `57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d` |
| Licence | MIT; see `LICENSE.txt` |
| Bug-list snapshot | 2026-08-03; https://www.lua.org/bugs.html#5.5.0 |

Lua 5.5.1 was still a release candidate in the official
[`work/`](https://www.lua.org/work/) area at this pin and is deliberately not a
dependency input. A released replacement requires a normal dependency update,
not an automatic download.

## Official fixes applied

Apply source hunks in this exact order after verifying each GitHub `.patch`
response. Full commit IDs identify upstream history; SHA-256 identifies the
downloaded patch artifact used for this pin.

| # | Upstream commit / patch URL | SHA-256 | Subject |
|---:|---|---|---|
| 1 | [`632a71b24d8661228a726deb5e1698e9638f96d8`](https://github.com/lua/lua/commit/632a71b24d8661228a726deb5e1698e9638f96d8.patch) | `f51fb87843bb9f5591913ca97053bba60387f1a0fad9a81183a77dabbe796e1c` | arithmetic overflow in `collectgarbage("step")` |
| 2 | [`45c7ae5b1b05069543fe1710454c651350bc1c42`](https://github.com/lua/lua/commit/45c7ae5b1b05069543fe1710454c651350bc1c42.patch) | `fb6714c7ce620e99e77abf76d944a7bd08482f6f633ab7958883c9301d0af2bf` | overflow in `string.packsize` |
| 3 | [`10eb89d1141dc528806b32401e408e36fb2f3bf5`](https://github.com/lua/lua/commit/10eb89d1141dc528806b32401e408e36fb2f3bf5.patch) | `8779ea54800d2dd4d485e4a18a422f1ac0181ca0b2985f6fe7dcea1d6f0cc83b` | shift overflow in UTF-8 decode |
| 4 | [`f1bb2773bba8b16f0f01c00e59a7be541ef88cb7`](https://github.com/lua/lua/commit/f1bb2773bba8b16f0f01c00e59a7be541ef88cb7.patch) | `b8163ae5f4267041939a0748af9609b886a6db82e5d8a16bb06e3b1be7eb6e17` | binary-chunk load did not run GC |
| 5 | [`efddc2309c5ff8a1842bea8a9c0d7d4a5d6e1e60`](https://github.com/lua/lua/commit/efddc2309c5ff8a1842bea8a9c0d7d4a5d6e1e60.patch) | `0b3eb134ddf49e1dad0315d95ee2c2f790a4a349b9f72e257aa536f73a48257a` | inconsistent `gmatch` result after error |
| 6 | [`3228a97c6a953dcf397944161bb64b12f1ff5384`](https://github.com/lua/lua/commit/3228a97c6a953dcf397944161bb64b12f1ff5384.patch) | `ed618075618a3f4504ef59c21c4be3ee543f2aa3a311ec5cc96fceac054c5dca` | `lua_load` stack preservation |
| 7 | [`bc4bbcef651ba2870d6c68db16dc7d6ce6f68636`](https://github.com/lua/lua/commit/bc4bbcef651ba2870d6c68db16dc7d6ce6f68636.patch) | `d03a659fe257cd429ff6b6cdd420e5ffdab320ad475d4cdc8e7382dfbd2666fe` | incomplete `luaL_newmetatable` registry entry |
| 8 | [`b996f8fd1be7fb711cc6f754a31a1c87d2c2fd9b`](https://github.com/lua/lua/commit/b996f8fd1be7fb711cc6f754a31a1c87d2c2fd9b.patch) | `89fd8982d19bd02167b6eb71097303818ee1c0c1390f5dc1420359525a66fa1c` | write barrier through `__newindex` |

Upstream patches also contain test or makefile hunks where applicable. Those
files are outside the selected source inventory. Apply the matching `.c`/`.h`
hunks and verify every result against `SOURCE.sha256`; that manifest is the
authority for the vendored bytes.

## Selected source inventory

`SOURCE.sha256` lists every upstream file that Packet 2 vendors after the eight
fixes. It includes Lua core, `lauxlib`, and only the base/math/string/UTF-8
library translation units needed to construct the explicit safe environment.

The private library compiles:

- every core `.c` in the manifest;
- `lauxlib.c`, `lbaselib.c`, `lmathlib.c`, `lstrlib.c`, and `lutf8lib.c`;
- `LUA_NOCVTN2S` and `LUA_NOCVTS2N`;
- no compatibility-version macro.

`lua.c`, `luac.c`, `linit.c`, `lcorolib.c`, `ldblib.c`, `liolib.c`,
`loadlib.c`, `loslib.c`, and `ltablib.c` are excluded. There is no interpreter,
compiler executable, `luaL_openlibs` call, dynamic loader, filesystem/OS/debug/
coroutine/table library, or install target. Selected standard-library TUs are
used only to copy the exact allowlisted functions into the fresh sandbox
environment; their full tables are never script-reachable.

The target is private static third-party code, compiled with third-party warning
suppression rather than first-party `nt_set_warning_flags`. The host performs
compile-time checks for 64-bit `lua_Integer` and binary64 `lua_Number`.

## Reproduction and update

1. Download the pinned tarball and verify its SHA-256.
2. Download the eight `.patch` URLs and verify their SHA-256 values.
3. Extract the tarball into a temporary directory.
4. From the extracted tarball root, apply each patch in table order with
   `git apply --directory=src --exclude=src/makefile --exclude='src/testes/*'`.
   This applies only the selected source hunks; distribution makefile and
   upstream test-repository hunks are outside the vendored inventory.
5. Copy exactly `SOURCE.sha256` paths and `LICENSE.txt` into `packer/deps/lua/`.
6. Verify every copied file against `SOURCE.sha256`.
7. Build offline and run `tp_lua_vendor_contract` plus the Lua sandbox gates.

Never edit vendored upstream `.c`/`.h` files by hand. A new upstream release or
fix set updates this metadata and manifest as one reviewed dependency change.
