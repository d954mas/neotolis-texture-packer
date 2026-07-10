# Neotolis Texture Packer

Standalone texture/atlas packer built on [neotolis-engine](https://github.com/d954mas/neotolis-engine):
one packing core, two frontends (CLI + native GUI), pluggable export formats
(Defold, generic JSON + PNG, `.ntpack`, ...).

Status: research / bootstrap. See `docs/research/` and `AGENTS.md`.

## Build

Requires CMake 3.25+, Ninja, Clang. Clone with submodules:

```bash
git clone --recursive <repo-url>
cmake --preset native-debug
cmake --build --preset native-debug
```
