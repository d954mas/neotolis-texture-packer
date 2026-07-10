# Defold demo project

In-repo Defold project proving atlases exported by **ntpacker** work in a stock
Defold build (ROADMAP Phase 5), on **real assets** with a three-way comparison
of atlas pipelines over the same source images:

| pipeline | input | example |
|---|---|---|
| Defold native atlas | source PNGs, packed by Defold at build time | `examples/rotate/rotate_d.atlas` |
| CodeAndWeb TexturePacker | pre-packed page + `.tpinfo`/`.tpatlas` (extension-texturepacker) | `examples/*/ *.tpinfo` (the `.tps` files show the TexturePacker settings used) |
| **neotolis packer** | same source PNGs through our `defold` exporter | added in Phase 5 (`packed.tpinfo` + page PNGs + `.tpatlas`) |

Source images live under `examples/basic/original/`, `examples/rotate/*.png`
(a/b/c), and `examples/anim_trim/anims/` — these are the inputs every pipeline
packs, so outputs are directly diffable.

## Provenance & license

`examples/`, `input/` and the comparison assets are copied from
[defold/extension-texturepacker](https://github.com/defold/extension-texturepacker)
(MIT, see `UPSTREAM-LICENSE`), examples as of tag **2.7.0**. The upstream menu
screen was dropped (dead code unrelated to the examples); `game.project` was
rewritten to consume the extension as a **pinned dependency** instead of
in-tree source.

## Version pinning (lock-step rule)

The extension is version-locked to Defold releases. This demo pins:

- extension-texturepacker **2.7.0** ↔ Defold/bob **1.12.4**
- dirtylarry at commit `6f2070e909a9` (GUI font/button helpers used by
  `basic.gui`; upstream floats on `master.zip`, we do not)

Bumping either pin is a deliberate small PR that changes the dependency URL and
the bob.jar version in CI together (see `docs/research/defold.md`).

## Building headless (CI does this in Phase 5)

```sh
cd examples/defold-demo
java -jar bob.jar resolve   # fetches pinned dependencies
java -jar bob.jar --archive build --variant headless
```

Or open the folder in the Defold editor and run. Switch which example boots via
`[bootstrap] main_collection` (`basic`, `rotate`, `anim_trim`, `skins`).
