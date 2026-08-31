# Runtime format example

`runtime-formats.ntpacker_project` packs one source set once and exports it
through native `json-neotolis`, bundled `defold-tpinfo-2`, and bundled
`phaser-3-multiatlas`. The committed `golden/` tree is the canonical byte-stable
producer output. `tests/expected.png` is a separate consumer oracle generated
directly from literal RGBA fixture maps and scene geometry; it never reads an
exported atlas or metadata file.

Regenerate the fixtures and exports from the repository root:

```sh
npm ci --prefix examples/runtime-formats
node examples/runtime-formats/tests/generate-visual-fixtures.mjs
build/apps/cli/native-release/ntpacker pack examples/runtime-formats/runtime-formats.ntpacker_project --json
```

The ordinary `tp_runtime_formats_example_contract` CTest copies the example to
a clean staging directory, exports there, compares all seven files with
`golden/`, checks the three core-owned PNGs are byte-identical, requires one
shared pack run, and covers Defold's project-absolute `.tpinfo` resource through
the exact committed `.tpatlas` bytes.

## Real engine proof

The pinned consumer gate uses Node 24.15.0, Phaser 3.90.0, Vite 7.3.6,
Playwright 1.62.1, Chromium installed by that Playwright version, Temurin
25.0.2, Defold Bob 1.12.4, and TexturePacker extension commit
`f15c7343efca064665ad608c76b80fa58841f96c` (tag 2.7.0). Bob's expected SHA-256
is `54ac57d9712fcd1e9de63f405803d159d88a2fbe940b8ef058ba503aa418fa18`.

After building the release CLI and obtaining `bob.jar`:

```sh
cd examples/runtime-formats
npm ci
npx playwright install chromium
npm run test:runtime -- \
  --ntpacker ../../build/apps/cli/native-release/ntpacker \
  --java /path/to/java \
  --bob /path/to/bob.jar
```

The test creates a clean temporary copy without `golden/`, reruns `ntpacker
pack`, builds the Phaser production bundle and Defold `wasm-web` bundle, and
serves each locally. Both must expose an engine-specific readiness marker after
rendering, keep one 256x144 canvas at DPR 1, produce two identical captures,
match `tests/expected.png` pixel-for-pixel, and report no page, console, request,
or HTTP errors. Failure images, pixel diffs, and logs go to
`build/runtime-consumer-artifacts/`.

The visual fixture is deliberately small and deterministic: opaque 0/255 RGBA
colors, asymmetric sprites, nearest filtering, no CSS scaling, no text,
animation, transforms, trim, polygons, multipage, or alpha blending. Broader
format behavior remains covered by the wire and capability tests.

Defold warning: Bob resolves the TexturePacker native extension through
Defold's external `build.defold.com` service. Running the Defold half transmits
this public example project's sources and resources to that service. Run only
the Phaser half without external Defold build traffic:

```sh
npm run test:runtime -- --engines phaser --ntpacker /path/to/ntpacker
```
