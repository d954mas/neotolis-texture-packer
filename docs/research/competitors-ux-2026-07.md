# Competitive UX Landscape — Texture/Atlas Packers

Research date: 2026-07-13. Scope: UX, usability, and user sentiment — the layer *above*
the feature/format matrix already covered in `docs/research/texturepacker.md` and
`docs/research/SUMMARY.md` (read those for algorithms, formats, `.tps` internals).
Compiled from five parallel web-research sweeps (~150 searches/fetches) across Reddit,
HN, engine forums (Defold/Unity/Godot/cocos/Phaser/Solar2D), GitHub issue trackers,
itch.io comments, AlternativeTo, and vendor docs. Every claim carries a source URL.

---

## 1. TexturePacker (CodeAndWeb) — UX teardown + sentiment

### 1.1 Onboarding / trial / free-tier mechanics

- Fresh install starts a **7-day full-Pro trial**, no purchase or signup required:
  "You can test all features of TexturePacker for 7 days."
  (https://www.codeandweb.com/texturepacker/documentation/installation-and-licensing)
- After the trial the app silently degrades into **"Essential" (free) mode** — it keeps
  working, no watermark, but Pro features corrupt output via the **red-sprites
  mechanic**: "Some sprites are red because TexturePacker is in Essential mode, and
  you're using a feature from the Pro version." Publish shows a list of triggering
  features. (https://www.codeandweb.com/texturepacker/knowledgebase/why-are-sprites-red)
- The mechanic misfires in the wild: a Solar2D user got red sprites with **no warning
  shown** — "When I publish my pack of textures, some of them are painted red, though
  when publishing it shows no messages saying I used a Pro feature."
  (https://forums.solar2d.com/t/red-sprites-problem/324319)
- Essential locks: MaxRects/Polygon/Grid algorithms, multipack, all engine-specific
  exporters (only generic JSON/XML), advanced texture formats, PngQuant/OptiPng,
  pivot editor, 9-slice editor, batch `.tps` processing, and **commercial use**.
  (https://www.codeandweb.com/texturepacker/licenses-comparison)
- Practical consequence users cite: free version "doesn't export to Godot" (r/godot,
  u/pasimako); "Fairly certain the CLI for Texture Packer is not a free feature. Most
  of the useful stuff is not free." (u/KhalilRavanna,
  https://www.reddit.com/r/gamedev/comments/1027zx0/)

### 1.2 Pricing / licensing (2025–2026)

- **Pro single: $49.99 perpetual**, "no subscription," updates for 1 year, then paid
  upgrade to use newer releases; 2 installs / 1 concurrent seat / online activation.
  (https://www.codeandweb.com/store/texturepacker-single)
- Single license **"can not be activated on a server or CI machine"** — CI requires a
  **separate yearly Docker/CI floating-license subscription** (Linux-only by default,
  price revealed only at checkout). Historical activation bugs: seats not released,
  "Received GOAWAY" errors, builds failing when no seat was free.
  (https://www.codeandweb.com/texturepacker/support,
  https://www.codeandweb.com/store/texturepacker-ci)
- Repeated 50% indie discounts (Andreas Loew on the Defold forum, 2024: "less than $23
  or €25"); historically free licenses for gamedev bloggers.
  (https://forum.defold.com/t/defold-now-has-texturepacker-support/76557)

### 1.3 GUI / publish-flow friction (observed, vendor-acknowledged)

- **Auto-multipack redistributed sprites whenever one was added**, which "breaks the
  sprite references in Unity" — vendor-admitted; TexturePacker 7 (2023) added *manual*
  multipack to compensate. Same release notes admit pre-7 rough edges: "Improvements to
  dark mode UI," "Fixes for minor UI issues on macOS 13," "Fixed crash in polygon
  outline tracer."
  (https://www.codeandweb.com/texturepacker/tutorials/whats-new-in-texturepacker-7)
- Slow publishes are config-induced, not packer-induced: "Setting high PNG optimization
  values might take a long time during Publish!"; polygon packing "more efficient but
  slower." (https://www.codeandweb.com/texturepacker/documentation/texture-settings)
- Ecosystem gaps land on TP's doorstep: "sorry but PIXI cant load multiPack from tp" —
  users with 37-page multipacks scripted loading by hand.
  (https://www.html5gamedevs.com/topic/46225-texturepacker-multipacked-textures/)
- Defold integration gripes: polygon misalignment in MaxRects mode; trim modes not
  working except "polygon outline" (Dragosha); iOS bundling errors with the extension.
  (https://forum.defold.com/t/defold-now-has-texturepacker-support/76557,
  https://forum.defold.com/t/bundle-on-ios-gives-error-with-texturepacker-plugin/78054)
- `$TexturePacker:SmartUpdate:<hash>$` embedded in every exported data file changes on
  each repack → VCS diff noise; docs also warn about CRLF/LF churn in mixed-OS teams.
  (https://www.codeandweb.com/texturepacker/documentation/commandline)
- Batch/many-atlas management is DIY: docs recommend shell loops over `.tps` files
  (`for %%X in (*.tps) do TexturePacker.exe %%X`) — no project-of-projects UI.
  (https://www.codeandweb.com/texturepacker/tutorials/batch-conversion-using-windows)

### 1.4 What users LOVE (quotes)

- "It is one of the few tools in my development toolbox that **just works**." (R101);
  "Worth every penny. And Andreas quickly implemented one small feature I requested."
  (qwpeoriu) — responsive solo dev is a recurring theme.
  (https://forum.cocosengine.org/t/texturepacker-50-indie-discount/44162)
- Smart Folders: "It's pretty much as convenient as using the sprite files directly...
  the workflow adds essentially zero overhead." (u/CalinLeafshade,
  https://www.reddit.com/r/godot/comments/1c4lyf7/are_atlas_textures_worth_it/kzrmh8c/)
- "licensed version costs about $40 and worth every penny" (u/tylerjhutchison,
  https://www.reddit.com/r/gamedev/comments/5cphj4/d9z5xdi/); "it was like $30 back
  then, so it was an easy call to just buy it rather than code my own"
  (u/SaturnineGames,
  https://www.reddit.com/r/gameenginedevs/comments/1938kvf/proper_format_for_a_image_atlas_map/kh88auo/)
- "Pretty ubiquitous among tiny shops" (u/HaskellHystericMonad); "Incredible tool"
  (u/darkgnostic). (https://www.reddit.com/r/gamedev/comments/1027zx0/,
  https://www.reddit.com/r/roguelikedev/comments/5vufok/de5sdes/)
- Auto pivot points are cited as *the* paid differentiator vs free tools: "you can't
  automatically set the pivot point" with free packers (u/CryzyStudio,
  https://www.reddit.com/r/Unity2D/comments/oycuc6/)
- HN credits its success to "multiple algorithms, parameters, format support, and GUI
  quality" (eigenbom, https://news.ycombinator.com/item?id=6703858); "loading
  spritesheets works perfect in Pixi especially if you use a tool like TexturePacker"
  (kanobo, https://news.ycombinator.com/item?id=24182918)

### 1.5 What users HATE (quotes)

- **Crippled free tier as bait**: "the full version... isn't free and the free version
  just isn't good enough" — the literal motivation for the thread "I just released a
  free alternative to Texture Packer" (u/Umexios); "Most of the useful stuff is not
  free" (u/KhalilRavanna). (https://www.reddit.com/r/gamedev/comments/1027zx0/)
- **Red-sprite output corruption** confusing/burning free users (Solar2D thread above).
- **Price vs hobbyists**: "Quite expensive for 'indie developers' coding for fun.
  Especially with free software out there." (captainflyaway,
  https://forum.cocosengine.org/t/texturepacker-50-indie-discount/44162)
- **CI friction**: desktop license explicitly banned on build servers; separate yearly
  subscription; activation-server dependency and historical seat-release bugs.
  (https://www.codeandweb.com/store/texturepacker-ci)
- **Layout churn**: years of auto-multipack breaking Unity references (vendor-fixed
  only in TP7); SmartUpdate hash diff noise.
- OSS crowd sees no reason to pay: "TexturePacker2 by libGDX has been available for
  10+ years. It's free and open source" (u/richmondavid,
  https://www.reddit.com/r/gamedev/comments/1027zx0/j2sewid/)
- Notably **absent** complaints: no watermarks (there are none), no desktop
  subscription resentment post-2020, and almost no raw packing-speed complaints —
  slowness reports trace to PNG optimizer settings / polygon mode. AlternativeTo lists
  9 alternatives but has zero written reviews — sentiment lives in forums.
  (https://alternativeto.net/software/texturepacker/about/)

---

## 2. Per-competitor UX notes

### 2.1 Free Texture Packer (odrick / free-tex-packer.com) — DEAD

- Was the standard "free TexturePacker" answer, Phaser-endorsed
  (https://phaser.io/news/2020/02/free-texture-packer); picked over TP's "crippleware
  shenanigans" (https://www.html5gamedevs.com/topic/43760-free-texture-packer/).
- README: "I don't have time to improve this app anymore." Last release v0.6.7, April
  2021 (https://github.com/odrick/free-tex-packer/releases). Website went down
  repeatedly in 2026 (issues #98/#99); the domain now redirects to a parked page while
  Phaser docs/forums still link to it.
- Issue #101: "free-tex-packer is completely dead, so I built an alternative" — outage
  "completely blocked our own project's packing pipeline"
  (https://github.com/odrick/free-tex-packer/issues/101). Life continues via forks
  (Funkin-Packer, https://github.com/NeeEoo/Funkin-Packer).
- Exporter fidelity pain even when alive: a melonJS user had to "run a sed script to
  update the json manually" (https://www.html5gamedevs.com/topic/43760-free-texture-packer/).
- **Lesson**: a popular free tool dying strands users mid-pipeline; "actively
  maintained" is itself a feature. Exporter output must be engine-exact.

### 2.2 gdx-texture-packer-gui (crashinvaders) — slowing

- Why chosen: THE GUI for libGDX's canonical packer, wiki-recommended
  (https://libgdx.com/wiki/tools/texture-packer). Free, multi-atlas `.tpproj` project
  model, headless batch mode (`--batch --project ... --atlases`), 9-patch editor,
  KTX2/Basis. (https://github.com/crashinvaders/gdx-texture-packer-gui)
- Pains: issue tracker dominated by **crash reports** (#163 Jun 2026, #158, #156,
  #155, #152); "Sprite packing collection order **not deterministic**" (#162, open,
  Apr 2026 — reporter needs "the packed data to be same on same inputs"); "Old pack
  file were not replaced correctly" (#155); UTF-8 Windows path bugs; needs JRE 8+ and
  OpenGL 2 — heavyweight runtime for a utility.
  (https://github.com/crashinvaders/gdx-texture-packer-gui/issues,
  https://github.com/crashinvaders/gdx-texture-packer-gui/issues/162)
- Maintenance: last release 4.13.0 Nov 2024; 2025–2026 issues sit unanswered.

### 2.3 rTexPacker (raylibtech, itch.io, ~$19.95)

- Why chosen: raylib loyalty, single small binary, also does font atlases, responsive
  developer (Ray). "A very nice interface and tool for what it does" (mrToad).
  (https://raylibtech.itch.io/rtexpacker/comments)
- Detailed user teardown (tiger.blue): **tiny UI on high-DPI screens, low-contrast
  buttons, poor legibility, missing tooltips, unscalable interface** — "I'll not be
  switching to rTexPacker over TexturePacker"; Ray conceded "most of the issues you
  listed are related to UI/UX." Also: no batch pivot presets, no arrow-key nudge/snap,
  no filmstrip auto-slicing, no multi-atlas overflow, no macOS build, an acknowledged
  alpha-corruption bug. (https://raylibtech.itch.io/rtexpacker/comments)
- Actively maintained (v5.5, 2025). **Lesson**: a custom-engine UI lives or dies on
  DPI scaling, contrast, and tooltips — exactly our stack's risk area.

### 2.4 ShoeBox (renderhjs) — abandoned but unreplaced

- Free Adobe AIR "Swiss-army 2D toolbox": packing plus **sprite extraction/unpacking,
  tile extraction, bitmap-font generation** from glyph images, drag-and-drop-everything
  + clipboard workflow. Still rated 8/10 on AlternativeTo — higher than TexturePacker's
  7. (https://renderhjs.net/shoebox/, https://alternativeto.net/software/shoebox/)
- Runs on Adobe AIR (discontinued 2020); modern installs need expired-certificate
  hacks or the HARMAN runtime (https://cocoalopez.com/blog/?p=3189). No updates since
  ~2016. Kept alive purely because nothing replaced its extraction/bitmap-font jobs.
- **Lesson**: sheet *unpacking* and bitmap fonts are orphaned use cases a new packer
  can absorb (SUMMARY.md already flags a round-trip unpacker as our test oracle).

### 2.5 Others (desktop)

- **Atlased** (itch, PWYW): atlas editor+packer+slicer, 4.9/5 from 9 ratings; gripes:
  can't rearrange sprites, thick region borders at zoom, no macOS; last release 2023.
  (https://witnessmonolith.itch.io/atlased)
- **Cheetah-Texture-Packer**: author admits "a research implementation... never been
  officially released"; dormant. (https://github.com/scriptum/Cheetah-Texture-Packer)
- **SpriteSheetPacker (nickgravelyn)**, **sprite-sheet-packer (amakaseev)**,
  **spritesheet.js**: all mid-2010s abandonware kept compiling by forks.
- **PixiJS AssetPack**: the modern web-stack answer — folder-tag (`{tps}`) build-time
  packing, watch mode, multi-res, WebP/AVIF; actively maintained; **CLI-only, no GUI**.
  (https://pixijs.io/assetpack/docs/guide/pipes/texture-packer/)
- **spright** (https://github.com/houmain/spright): power-user config-file CLI packer.
- Determinism-first libraries exist and market it: rectangle-pack ("deterministic...
  useful... when generating a texture atlas that is meant to be cached based on the
  hash of the contents", https://github.com/chinedufn/rectangle-pack), NXP
  gtec-texture-packer ("All output files should be deterministic"), tex-packer (Rust,
  "modern, deterministic", https://github.com/Latias94/tex-packer).

### 2.6 Web-based packers

- **Leshy SpriteSheet Tool** (2013, still recommended in Phaser threads): fully
  client-side ("Assets are never sent to any server"); also *edits* existing sheets and
  converts formats. Complaint: "super tiny view into the sprite sheet, making it really
  hard to select tiny sprites"; author says fixes are low priority — the app
  "generates minimal revenue or traffic."
  (https://www.leshylabs.com/apps/sstool/,
  https://www.html5gamedevs.com/topic/2425-leshy-spritesheet-tool-a-tool-for-packing-and-editing-sprite-sheets/)
- **CodeAndWeb runs two free web funnels** — "Free Sprite Sheet Packer" and
  "TexturePacker Online" — steering to the paid desktop app; the incumbent itself
  validates web-demo-as-acquisition. (https://www.codeandweb.com/free-sprite-sheet-packer,
  https://www.codeandweb.com/tp-online)
- 2024–26 SEO wave (ilovesprites.com, spritesheetmaker.com, etc.): all lead with
  "free / no signup / engine-ready export / files never leave your browser" — privacy
  via client-side WASM is now a *pro-web* argument.
- Why web wins: zero install, instant one-off, zero license friction, locked-down
  machines (Chromebook classrooms — Piskel's entire channel).
- Where web falls down (universal): **no CLI/build integration, no project
  persistence (refresh = start over), browser canvas ceilings (>4096² fails), no GPU
  compression, cramped viewports, link-rot/abandonment** (the top-recommended free web
  packer is now a parked domain). (https://codeshack.io/images-sprite-sheet-generator/)

---

## 3. Table stakes — UX every serious packer must have

Derived from what every loved tool ships and what users assume exists:

1. **Smart folders + live repack.** Drop a folder; FS watching auto-updates the sheet;
   sub-folder path → sprite name prefix. TP's single most-copied feature; "adds
   essentially zero overhead" is the bar. (No OSS *CLI* tool ships watch — SUMMARY.md.)
2. **Live always-on preview** — every settings change repacks instantly; loud, visible
   packing with explicit success/fail. (Unity Sprite Atlas V2 is the cautionary tale:
   "the button just gets grayed out... it looks like the whole thing is just broken",
   https://discussions.unity.com/t/sprite-atlas-v2-doesnt-work-at-all/1551723)
3. **GUI = CLI = one project file.** Every dialog option has a CLI flag; CI runs the
   same project the artist edited (TP `.tps`, Aseprite `-b`, Tiled `--export-map`).
   Smart no-op rebuild (hash inputs+settings+version) so build scripts can call it
   unconditionally.
4. **Per-sprite editors**: pivot point editor + 9-slice editor with drag handles —
   cited as *the* reason to pay for TP over free tools.
5. **Engine-exact exporters** users never have to post-process ("sed the JSON" is a
   documented failure mode), with capability flags gating GUI controls; plus animation
   preview (TP has it; no free tool does — parity, not headline).
6. **Trim/rotation/extrude/padding/multipack/alias** defaults that just work (feature
   research already covers these) and **remembered per-file export settings** with a
   one-click "export again" (Aseprite's most-loved QoL).
7. **Crash-safety + project/session hygiene**: crash-proof backups (LDtk: "can even
   restore unsaved changes if the app crashes"), project vs session split so VCS stays
   clean (Tiled), relative paths in project files (TP does this; free-tex-packer's
   absolute paths were a defect).
8. **DPI-scalable, legible UI with tooltips** — rTexPacker's #1 complaint; table
   stakes for a custom-engine (non-Qt) UI.

---

## 4. Beloved features — what earns love (stealable mechanics)

- **Aseprite's love formula** (99% positive, ~11.8k Steam reviews): does ONE thing
  completely; fast; **pay-once cheap + source-available + visible monthly changelog**
  ("best $15 I've spent on software... every time I open it up there is some update
  that I'm excited about", https://news.ycombinator.com/item?id=17343864; "such a joy
  to use that I paid for it just to support the developers",
  https://news.ycombinator.com/item?id=47272799). People pay despite a free fork.
- **GUI/CLI as the same product** with filename-format templating
  (`--filename-format '{path}/{title}_{tag}_{tagframe}.{extension}'`) — a tiny
  template language beats a wall of checkboxes. (https://www.aseprite.org/docs/cli)
- **Semantic metadata rides along**: tags/slices/9-patch land in exported JSON `meta`
  so engines get animation data for free. (https://github.com/aseprite/docs/blob/main/cli.md)
- **Folder-as-atlas declaration** (Unity: drag a folder into Objects-for-Packing and
  new sprites auto-include; Godot users *beg* for a `.atlasImport` marker file —
  https://github.com/godotengine/godot-proposals/issues/3530).
- **Variant atlases** (Unity): one authoring source → N resolutions.
  (https://docs.unity3d.com/Manual/VariantSpriteAtlas.html)
- **Auto-reload changed images from disk** (Tiled preference, exactly the
  edit-in-Photoshop-see-it-repack loop; use robust watching, not naive mtime —
  https://doc.mapeditor.org/en/stable/manual/preferences/,
  https://github.com/mapeditor/tiled/issues/2024) and **native .aseprite/.psd
  ingestion with live reload** (LDtk: "you paint your tiles, save and LDtk updates
  everything accordingly", https://ldtk.io/).
- **Ctrl+P fuzzy file-open across the project** (Tiled) — no OS file dialogs.
- **"Super simple export" defaults** (LDtk: "a few PNGs per level, a tiny JSON... and
  that's it") + **published, documented output schema** (Tiled/LDtk's open formats are
  their moat; parsers write themselves).
- **Engine-side import plugins** so packer output becomes native engine objects
  (CodeAndWeb's Godot/Unity plugins; Defold `.tpinfo` extension) — meet every engine
  where it is.
- **Responsive solo developer** with visible fast fixes (TP's Andreas, raylib's Ray,
  Tiled's bjorn funded at ~$3600/mo on Patreon) — community bond is a product feature.

---

## 5. Pain points — what earns hate (our opportunities)

1. **Free-tier crippling / output corruption** (TP red sprites, non-commercial clause,
   locked CLI) — the single biggest driver of the free-alternative ecosystem. We are
   MIT-free: entire resentment category deleted.
2. **CI licensing friction** (TP: desktop key banned on build servers; separate yearly
   floating-license subscription; activation-server outages breaking builds). Free +
   headless + no EULA prompt is an advertised feature, not an absence.
3. **Non-determinism / layout churn** — the root cause behind three complaint streams:
   VCS pollution (Unity: "SpriteAtlases keep changing their hash without sprites
   changing, polluting source control",
   https://discussions.unity.com/t/spriteatlases-keep-changing-their-hash-without-sprites-changing-polluting-source-control/760434),
   CI cache/diff breakage (gdx-gui #162), and live-ops patch bloat (Unity: users
   "download the sprite atlas every time when we make an asset update",
   https://discussions.unity.com/t/assetbundle-generation-from-spriteatlas-is-not-deterministic/929544).
   TP's own KB concedes "adding, removing, or resizing sprites can significantly
   change the packing result" and offers only grid/manual placement as mitigation
   (https://www.codeandweb.com/texturepacker/knowledgebase/manual-sprite-order).
4. **Opaque packing** — users disable Unity tight packing over bleed/overlap because
   they can't see what the packer did; "atlases contain lots of empty space" threads
   end in guesswork; no tool explains placement or quantifies waste.
5. **Crashes and jank in the free GUI space** (gdx-gui's crash-report-dominated
   tracker; rTexPacker's DPI/legibility teardown; free-tex-packer's broken splitter).
   "Just works" reliability is TP's #1 compliment — the free tier of the market never
   achieved it.
6. **Abandonment risk** — every free GUI packer is dead or slowing (free-tex-packer
   dead 2021/domain parked 2026, ShoeBox dead on AIR, Cheetah/nickgravelyn ancient,
   Atlased 2023, gdx-gui unanswered issues). Users have been burned; visible active
   maintenance is a differentiator.
7. **Exporter infidelity** — hand-patching output JSON, Pixi multipack unloadable,
   Defold trim-mode bugs. Engine-exactness with golden-fixture tests is a UX feature.
8. **Batch management sprawl** — TP scales to many atlases via shell loops; Unity
   repacks the world (asset-bundle builds "last for hours",
   https://discussions.unity.com/t/asset-bundle-compilation-time-takes-too-long-with-sprite-atlas/944709);
   libGDX fast-vs-slow is "a few minutes" vs "over 10 hours"
   (https://github.com/tommyettinger/libgdx-texturepacker). Nobody has a first-class
   multi-atlas project manager with incremental builds + settings inheritance.

---

## 6. Unserved gaps (verdicts)

| Gap | Verdict | Evidence anchor |
|---|---|---|
| Hundreds of atlases, one project: settings inheritance, incremental repack, batch UI | **Underserved.** TP = shell loops; engines repack everything | Unity hours-long bundle builds; TP batch docs |
| Team collaboration: mergeable project format, clean diffs, no spurious rebuilds | **Underserved.** Documented multi-year VCS pain; nobody targets git as a surface | Unity atlas-hash threads; TP SmartUpdate/CRLF noise |
| CI/headless determinism: byte-identical output, GUI=CLI, any machine | **Strongly underserved** in GUI tools; only low-level libs market determinism | gdx-gui #162 (open); Unity IN-55415; rectangle-pack |
| Packing observability: waste heatmaps, "why is this sprite here," run-to-run layout diff | **Nobody serves this.** Only prior art is WebRender's internal debug SVG | https://nical.github.io/posts/etagere.html; Unity tight-packing distrust |
| Stable/incremental packing minimizing layout delta vs last release (live-ops patch size) | **Nobody.** Today stability = dumb grid or manual placement | TP manual-sprite-order KB; Unity Addressables threads |
| Animation preview in-packer | Served by TP only (paid). Parity feature vs TP, differentiator vs all free tools | TP UI docs |
| Sheet unpacking / bitmap fonts (orphaned ShoeBox jobs) | **Orphaned** since AIR died; still 8/10-rated demand | AlternativeTo ShoeBox page |
| AI agents as users (MCP/CLI-first atlas pipeline) | **Wide open.** Aseprite has 6+ MCP servers, Unreal MCP 305 tools; atlas packing has ~2 toy utilities, no project-aware incumbent | https://github.com/youichi-uda/aseprite-mcp-pro; https://lobehub.com/mcp/trebeljahr-sprite-tools |

### Synthesis for ntpacker positioning

1. **One root cause, four wins**: a deterministic, incremental, diffable pack core
   simultaneously answers VCS pollution, CI parity, patch-size pain, and batch build
   times. No incumbent is built around this.
2. **Trust the tight packing**: concave-hull packing already exists in engines but is
   *disabled by users* because it's opaque and bleeds. Our real concave preview +
   explain-the-layout observability attacks the trust problem, not just the density
   ratio — and observability (waste %, placement rationale, layout diff) is virgin
   territory.
3. **The AI/MCP window is open now**: the generation side (AI makes sprites) is
   crowded; the pipeline side (agent manages atlas projects, packs, inspects, enforces
   budgets) has no incumbent; free-tex-packer's death orphaned the OSS CLI audience.
4. **Beloved-tool playbook** (Aseprite/Tiled/LDtk): focused scope, GUI=CLI parity,
   remembered settings, crash-proof, published schema, visible maintenance cadence,
   free/MIT — plus TP's smart folders and per-sprite editors as table stakes.
5. **Custom-UI risk to manage**: rTexPacker proves a custom-engine UI gets judged on
   DPI scaling, contrast, tooltips, keyboard nudge — budget for these explicitly.
