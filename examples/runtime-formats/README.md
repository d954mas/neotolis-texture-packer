# Runtime format example

`runtime-formats.ntpacker_project` exports the same two sprites through the
native `json-neotolis`, bundled `defold-tpinfo-2`, and bundled
`phaser-3-multiatlas` formats. The committed `golden/` tree is the canonical
byte-stable output, including each target's core-owned PNG.

Regenerate from the repository root with the release CLI:

```sh
build/apps/cli/native-release/ntpacker pack examples/runtime-formats/runtime-formats.ntpacker_project --json
```

The three `atlas-0.png` files must remain byte-identical. To run the pinned
Phaser 3.90 consumer:

```sh
cd examples/runtime-formats
npm ci
npm run dev
```
