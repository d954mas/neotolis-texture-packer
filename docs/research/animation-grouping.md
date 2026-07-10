# Animation Grouping from Image Sequences — Competitive Study

Research date: 2026-07-10. Focused companion to `texturepacker.md`, `spine-libgdx.md`,
`unity-raylib.md`, and `defold.md`. Scope: how texture packers and game tools turn image
sequences (`walk_01.png`, `walk_02.png`, …) into animations — *where* that grouping happens
(pack-time vs editor vs runtime), whether it is **explicit** or **automatic**, and how tools
that automate it cope with **false positives** (the `icon_1`/`icon_2` problem).

Written to validate/refine our ruling in `docs/design/ux.md §3.7b`:
**numeric-suffix auto-grouping is a GUI SUGGESTION only; export emits only explicit
`animations[]`.** Bottom line up front: **the ruling is well-supported by precedent.** The
single tool that auto-groups by default (TexturePacker) ships a dedicated "how to turn it off"
knowledge-base article precisely because it produces unwanted groupings; every other surveyed
tool either keeps grouping **explicit in the editor** or defers it to a **runtime name
convention**. Refinements below concern the *suggestion heuristic quality* and one missing
*manual gesture*, not the ruling itself.

---

## Comparison table

| Tool | Detection mechanism | Where it lives | Explicit vs auto | False-positive handling |
|---|---|---|---|---|
| **CodeAndWeb TexturePacker** | "Auto-detect animations": sprites whose names differ only in a numeric suffix → one animation named by the base | **Pack-time** (writes into the *data file only*, never the sheet) — but only for a few exporters (libGDX, SpriteKit, Cocos2D-x, CSS) | **Auto, on by default** | Single global on/off toggle in Advanced settings; a KB article exists *because* it surprises users |
| **TexturePacker → Defold** (`extension-texturepacker`) | none in the generated `.tpinfo`; animations are `AtlasAnimation` blocks **hand-authored in the `.tpatlas`** on the Defold side | Editor (Defold), not the packer | **Explicit** | N/A — nothing is auto-grouped |
| **TexturePacker Animation Preview** | user hand-picks sprites in the Sprites panel into a playback loop | Editor (preview window) | **Explicit**, and **preview-only** (does not drive export) | N/A |
| **libGDX TexturePacker / TextureAtlas** | trailing `_N` (regex `(.+)_(\d+)$`, `useIndexes` default on) stored as `index`, stripped from region name | **Split**: pack-time bookkeeps `index`; **runtime** `atlas.findRegions(name)` assembles the ordered frame list | **Auto index at pack-time, explicit assembly at runtime** — pack never names an "animation" | `useIndexes=false` when names legitimately contain `_N`; Spine docs warn about this |
| **Defold native atlas editor** | none | Editor: right-click Atlas → **Add Animation Group**, then add images | **Explicit only** | N/A — zero auto-detection by design |
| **Aseprite** | frame **Tags** the user draws on the timeline; exported to `meta.frameTags[{name,from,to,direction}]` | Editor (tags) → JSON export | **Explicit only** | N/A — tags are hand-authored ranges, never inferred from filenames |
| **Godot SpriteFrames editor** | "Add frames from a Sprite Sheet" = **grid** slice (H×V) + manual cell multi-select → "Add N frames"; "Add Animation" per clip | Editor | **Explicit** (grid/selection driven, not name driven) | N/A — grid coords, not names |
| **Phaser (runtime)** | `generateFrameNames({prefix,start,end,zeroPad,suffix})` / `generateFrameNumbers({start,end})`; `createFromAseprite()` | **Runtime code** | **Explicit** (developer states the range) | N/A — developer specifies the exact prefix/range |
| **Unity** | drag a multi-selection of sprites into the scene → auto-creates Animator + AnimationClip in **selection order** | Editor | **Semi-auto from selection** (not filename); atlas packing never touches animation | Order/selection driven; slicing (grid/alpha) is a separate step |
| **Cocos Creator** | manual: drag SpriteFrames onto an AnimationClip track; Auto Atlas only *packs* frames | Editor | **Explicit** | N/A |
| **free-tex-packer** | none | — (packs frames only; GodotAtlas/Phaser3/Cocos2d/Spine/Unity3D exporters emit frame data, no animation lists) | **Neither** — punts entirely to runtime/editor | N/A |
| **ShoeBox / Kenney tools** | template/asset-editing focused; no name-based grouping into animation objects | Editor/templates | **Explicit / none** | N/A |

---

## Per-tool notes

### CodeAndWeb TexturePacker — the only default-on auto-grouper (and it's opt-out for a reason)

- **What it does:** "Auto-detect animations" groups sprites "which have the same base name and
  only differ in a numerical suffix." Example from the docs: `walk_001.png`, `walk_002.png`,
  `walk_003.png` → an animation `walk` in the data file. Enabled by default in Advanced settings.
- **Scope is narrow and preview-agnostic:** it "affects **only the exported data file**, not the
  packed texture sheet," and — critically — it is **"only available for some frameworks like
  LibGDX, SpriteKit, Cocos2D-x."** The most-used JSON hash/array (Phaser, Pixi, generic) exporters
  do **not** emit an animations list; they keep the full frame name (`walk_001`) so a **runtime**
  helper reassembles it. So even TexturePacker, for its flagship formats, leaves animation
  assembly to runtime. (Note: "Trim sprite names" is a *separate* setting that only strips file
  extensions; the numeric-suffix stripping feeds the `trimmedName` template variable.)
- **Defold path carries no animations:** the Defold custom exporter emits a `.tpinfo` with sprite
  layout only. Flipbook animations are `AtlasAnimation` blocks the user **hand-authors in the
  `.tpatlas`** in the Defold editor (confirmed in our `defold.md` — `examples/basic/basic.tpatlas`
  is hand-written). **TexturePacker does not generate `.tpatlas` animations.**
- **Animation Preview** is a floating window: you *select* sprites in the Sprites panel to add
  them to a loop, with playback-speed/loop controls; it runs live while you edit pivots. It is a
  **manual, preview-only** feature — it does not decide exports.
- **False-positive handling = one blunt global switch.** The existence of a dedicated KB article
  ("Why does TexturePacker remove suffixes like -01, -02…?") and the documented fix ("uncheck
  Auto-detect animations") are direct evidence that default-on auto-grouping regularly does the
  wrong thing — exactly our `icon_1`/`icon_2` concern.
- Sources: <https://www.codeandweb.com/texturepacker/knowledgebase/animation-detection>,
  <https://www.codeandweb.com/texturepacker/documentation/texture-settings>,
  <https://www.codeandweb.com/texturepacker/documentation/user-interface-overview>.

### libGDX TexturePacker / TextureAtlas — the clean pack-time/runtime split

- **Pack-time** only *bookkeeps* the sequence: `ImageProcessor` parses a trailing `_N`
  (`useIndexes` default true, regex `(.+)_(\d+)$`), stores it as the `index` region field, and
  strips it from the region name. It never emits a named "animation." Multiple frames become
  multiple regions that **share a name** and differ by `index`.
- **Runtime** does the assembly: `atlas.findRegions("walk")` returns "all regions with the
  specified name, ordered by smallest to largest index," which is passed straight into
  `new Animation<>(frameDuration, atlas.findRegions("walk"))`; `Animation.getKeyFrame(elapsed)`
  returns the current frame. So the trailing-index convention is a **runtime responsibility**,
  authored by the developer at the call site — the pack step is agnostic about what is or isn't an
  animation.
- **False positives:** if names legitimately contain `_N`, set `useIndexes=false` (Spine's docs
  explicitly warn that `foo_2` won't resolve if index-stripping is on). Opt-out, same as TP.
- Sources: <https://libgdx.com/wiki/graphics/2d/2d-animation>,
  <https://github.com/libgdx/libgdx/blob/master/gdx/src/com/badlogic/gdx/graphics/g2d/TextureAtlas.java>,
  plus `spine-libgdx.md` (`index` field, `useIndexes`, `ImageProcessor` name rules).

### Defold native atlas editor — explicit only, zero auto-detection (our actual target)

- Flipbook animations are created by **right-clicking the Atlas root in the Outline → "Add
  Animation Group"**, which adds an empty group ("New Animation"); images are added by drag-drop
  from the Assets pane or "Add Images." **Space** previews the group; **Ctrl/Cmd+T** closes it.
  Properties: id, fps, playback, flip horizontal/vertical.
- There is **no filename-based auto-grouping anywhere** in the Defold pipeline. Every single image
  is *implicitly* a 1-frame animation (a Defold sprite always references an animation id) — which
  is exactly the model our `§3.7b` adopts. Our generated `.tpatlas` animations are likewise
  explicit `AtlasAnimation` records.
- Sources: <https://defold.com/manuals/atlas/>, <https://defold.com/manuals/flipbook-animation/>,
  and `defold.md` (`atlas_ddf.proto`, `tpatlas.proto`).

### Aseprite — explicit tags become explicit JSON ranges

- Animations are **frame Tags** the user draws over frame ranges on the timeline (with a name,
  direction, and color). They are **never inferred from filenames.**
- Export to a sprite sheet writes them into `meta.frameTags`, e.g.
  `{"name":"Walk","from":0,"to":3,"direction":"forward","color":"#000000ff"}`. `direction` is
  `forward` / `reverse` / `pingpong`. CLI: `--list-tags`, `--split-tags`,
  `--filename-format "{tag} {tagframe}"`.
- Downstream engines consume the explicit ranges (e.g. Phaser `this.anims.createFromAseprite()`).
  The whole model is **explicit author-time ranges → explicit export**, which is the strongest
  "designer states the animation, the tool records it" precedent.
- Sources: <https://www.aseprite.org/docs/cli/>,
  <https://github.com/aseprite/aseprite/issues/1514> (frame-tag metadata),
  <https://community.aseprite.org/t/json-export-frame-tags-as-frame-names/1109>.

### Godot SpriteFrames editor — grid-slice + manual selection, plus the multi-select gesture

- Workflow: select `AnimatedSprite2D` → new `SpriteFrames` → **"Add frames from a Sprite Sheet"**,
  set the grid's horizontal/vertical cell counts, **multi-select the cells**, then **"Add N
  frames."** Additional clips via **"Add Animation."** Per-clip FPS and Loop toggle.
- Grouping is **grid-coordinate + selection driven, not name driven** — Godot never guesses an
  animation from filenames. The **"select cells → Add N frames"** gesture is a strong precedent
  for a *manual multi-select → create-animation* affordance (see recommendation R3).
- Sources: <https://docs.godotengine.org/en/stable/tutorials/2d/2d_sprite_animation.html>,
  <https://docs.godotengine.org/en/stable/classes/class_spriteframes.html>.

### Phaser — animation-from-names lives in **runtime code**, as an explicit range

- No editor step: at runtime the developer calls
  `this.anims.generateFrameNames('gems', {prefix:'ruby_', start:1, end:6, zeroPad:4})` for atlases
  (or `generateFrameNumbers('explosion', {start:0, end:11})` for grid sheets) and feeds the result
  to `this.anims.create({...})`. The **prefix + zeroPad + start/end range** is stated explicitly
  by the developer — a precise, false-positive-free contract.
- This is the canonical example of the "name convention is a runtime concern" model: the packer
  just has to *preserve the suffix in frame names* so the runtime range works.
- Sources: <https://docs.phaser.io/phaser/concepts/animations>,
  <https://newdocs.phaser.io/docs/3.55.2/focus/Phaser.Animations.AnimationManager-generateFrameNames>.

### Unity — semi-automatic, but from **selection order**, not filenames

- Dragging a multi-selection of sprites into the scene auto-creates a GameObject + SpriteRenderer
  + Animator + a looping AnimationClip whose keyframes are the sprites **in selection order**.
  Convenient, but it keys off the *selection*, not a name pattern; sprite *slicing* (grid/alpha)
  and *atlas packing* are separate steps that never touch animation. Pivots/borders are per-sprite
  and survive packing untouched (see `unity-raylib.md`).
- Sources: <https://learn.unity.com/tutorial/introduction-to-sprite-animations>,
  <https://discussions.unity.com/t/how-to-prevent-creating-animation-when-dragging-multiple-sprites-into-the-hierarchy/557748>.

### Cocos Creator — packing and animation are fully decoupled

- **Auto Atlas** packs every SpriteFrame in a folder at build time; **AnimationClips are built by
  hand** — drag SpriteFrames onto a track in the Animation panel, set the sample rate. No
  name-based grouping.
- Sources: <https://docs.cocos.com/creator/3.8/manual/en/asset/auto-atlas.html>,
  <https://docs.cocos.com/creator/3.8/manual/en/asset/sprite-frame.html>.

### free-tex-packer, ShoeBox, Kenney — the "free" tools punt animation entirely

- **free-tex-packer / free-tex-packer-core**: no animation detection at all. Its exporters
  (`JsonHash`, `JsonArray`, `Pixi`, `GodotAtlas`, `Phaser3`, `Cocos2d`, `Spine`, `Unity3D`, custom
  Mustache templates) emit **frame layout data only** — assembly is left to runtime/editor. So the
  most popular free alternative validates "export frames, not animations" as a viable default.
- **ShoeBox** is template-driven (NGui/Cocos2d/Starling/Sparrow/HTML5) with animation-*sheet*
  cleanup tools, not a name-based animation-object grouper. **Kenney** tooling is asset-editing
  focused; no evidence of name-pattern animation export.
- Sources: <https://github.com/odrick/free-tex-packer-core>,
  <https://www.free-tex-packer.com/app/>, <https://renderhjs.net/shoebox/>,
  <https://kenney.nl/knowledge-base/game-assets-2d/editing-2d-game-assets>.

---

## Implications for ntpacker (validate/refine `§3.7b`)

### Verdict: the ruling holds — export explicit `animations[]` only

The competitive landscape splits cleanly into three camps, and **none** supports "auto-emit
animations from filenames by default":

1. **Explicit editor assembly** (Defold, Aseprite, Godot, Cocos, Unity-from-selection) — the
   animation is a first-class thing the designer *creates*. This is our primary UX ("как в Defold").
2. **Runtime name convention** (libGDX `findRegions`, Phaser `generateFrameNames`) — the packer
   only *preserves the suffix in names*; the developer states the range at the call site.
3. **Auto-emit at pack-time** — **only TexturePacker**, only for 3 framework exporters, **only as
   an opt-out default that ships with a "how to turn it off" article.** This is the anti-pattern
   our ruling deliberately avoids (`icon_1`/`icon_2`).

**Strongest precedents FOR our suggestion-only export:**

- **Defold (our real target): explicit-only, "Add Animation Group," zero auto-detection.** Our
  export path must match this or we would be *more* aggressive than the engine we serve.
- **libGDX: the numeric suffix is a runtime concern.** Pack-time never names an animation — it
  only records an index. Perfect support for "grouping is a hint, not an export decision."
- **TexturePacker's own opt-out + KB article**: the one tool that auto-groups by default documents
  that it regularly groups things users didn't mean — empirical proof of the false-positive risk.
- **free-tex-packer**: the popular free tool exports zero animations and nobody considers that a
  defect — assembly downstream is the norm.

**Strongest precedent AGAINST** (i.e. arguing for auto-export): only TexturePacker's default-on
`Auto-detect animations`, and it is exactly the behavior our ruling rejects. So the "against" case
is self-defeating.

Our `§3.7b` design also *beats* every surveyed false-positive mitigation: TexturePacker and libGDX
offer only a **single global on/off**. Our **per-group accept/ignore suggestion row + per-atlas
disable** is strictly finer-grained. Keep it.

### Recommendations (owner decides)

- **R1 — Keep `§3.7b` as written.** Explicit `animations[]` only in export; grouping runs in
  compute-suggestions mode for the GUI, OFF in the export path. Fully validated. No change needed
  to the ruling.

- **R2 — Sharpen the suggestion heuristic beyond "trailing digits."** A bare "same base + trailing
  number" rule is what produces `icon_1`/`icon_2` noise. Borrow the explicit-range model that
  Phaser (`prefix`/`zeroPad`/`start`/`end`) and Aseprite (`from`/`to`) already normalize on, and
  only raise a suggestion when the group is *animation-shaped*:
  - shared prefix **and** a consistent **separator** (`_`, `-`, or none) — mixed separators split
    groups;
  - **consistent zero-padding width** across the run (`walk_01..walk_12`, not `walk_1` + `walk_02`);
  - a **contiguous** numeric range with **≥ N frames** (default N=3; configurable down to 2) — two
    non-contiguous items (`icon_1`, `icon_4`) are the classic false positive and should not
    suggest;
  - present the suggestion as a **range**: `Create "walk" (walk_01..walk_12, 12 frames)?` — clearer
    than a bare count and mirrors how every runtime consumer thinks about it.
  This keeps recall high for real cycles while cutting the icon-style noise the owner flagged.

- **R3 — Add a manual "Create animation from selection" gesture (currently missing from `§3.7b`).**
  Every explicit-assembly tool has a fast multi-select path: Godot "select cells → Add N frames,"
  TexturePacker's Animation Preview "add selected sprites," rTexPacker Ctrl-click multi-select,
  Cocos drag-frames-to-track. Our `§3.7b` describes "+ Animation" then an "add frames via
  multi-select picker" — good, but the reverse gesture is faster and expected: **select frames in
  the sprite tree/canvas → right-click → "Create animation from selection"** (id prefilled from the
  common prefix; order = numeric-then-selection). This should be a *primary* manual path next to
  the picker, not an afterthought.

- **R4 — Never strip the numeric suffix from exported frame/region names.** Our model already keeps
  sprite names, so this is satisfied — but state it as an invariant: preserving the suffix is what
  lets the *runtime* path (libGDX `findRegions`, Phaser `generateFrameNames`) work even when the
  user created no explicit animation. Result: we support **both** the Defold explicit `.tpatlas`
  path **and** the runtime-convention path for free, with a single behavior. (Contrast
  TexturePacker, whose suffix-stripping `trimmedName` can *break* the runtime path if an exporter
  uses it.)

- **R5 — Keep the animation preview player; it's universal.** TexturePacker (anim-preview window),
  Defold (Space-to-preview), Godot, Aseprite all ship one. Our canvas player (fps + playback mode +
  frame step + flip, pre-pack from decode cache, post-pack on packed regions) matches or exceeds
  them and doubles as trim/pivot validation in motion. No change.

- **R6 — Batch the suggestions but accept/reject individually.** Prefer "3 suggested animations
  [Create all] / per-row [Create] [Ignore]" over TexturePacker's single global toggle. Individual
  dismissal + per-atlas disable (already in `§3.7b`) is the finer-grained, less-surprising control
  the survey shows is missing elsewhere.

No finding contradicts the ruling. The only gaps versus best-in-class are the *heuristic quality*
(R2) and the *multi-select → create* gesture (R3); both are additive to `§3.7b`, not reversals.

---

## Sources

- TexturePacker animation detection: <https://www.codeandweb.com/texturepacker/knowledgebase/animation-detection>
- TexturePacker texture settings (Auto-detect animations, Trim sprite names): <https://www.codeandweb.com/texturepacker/documentation/texture-settings>
- TexturePacker UI (Animation Preview window): <https://www.codeandweb.com/texturepacker/documentation/user-interface-overview>
- TexturePacker → Defold extension (`.tpinfo`/`.tpatlas`, hand-authored animations): <https://github.com/defold/extension-texturepacker>, and `docs/research/defold.md`
- libGDX 2D animation (findRegions → Animation): <https://libgdx.com/wiki/graphics/2d/2d-animation>
- libGDX `TextureAtlas` source (findRegions/index): <https://github.com/libgdx/libgdx/blob/master/gdx/src/com/badlogic/gdx/graphics/g2d/TextureAtlas.java>; index/`useIndexes` details in `docs/research/spine-libgdx.md`
- Defold atlas manual (Add Animation Group): <https://defold.com/manuals/atlas/>; flipbook manual: <https://defold.com/manuals/flipbook-animation/>
- Aseprite CLI / tags / JSON: <https://www.aseprite.org/docs/cli/>, <https://github.com/aseprite/aseprite/issues/1514>, <https://community.aseprite.org/t/json-export-frame-tags-as-frame-names/1109>
- Godot SpriteFrames / 2D sprite animation: <https://docs.godotengine.org/en/stable/tutorials/2d/2d_sprite_animation.html>, <https://docs.godotengine.org/en/stable/classes/class_spriteframes.html>
- Phaser animations (generateFrameNames/Numbers, createFromAseprite): <https://docs.phaser.io/phaser/concepts/animations>, <https://newdocs.phaser.io/docs/3.55.2/focus/Phaser.Animations.AnimationManager-generateFrameNames>
- Unity sprite animation (drag-selection → clip): <https://learn.unity.com/tutorial/introduction-to-sprite-animations>, <https://discussions.unity.com/t/how-to-prevent-creating-animation-when-dragging-multiple-sprites-into-the-hierarchy/557748>
- Cocos Creator (Auto Atlas vs manual clips): <https://docs.cocos.com/creator/3.8/manual/en/asset/auto-atlas.html>, <https://docs.cocos.com/creator/3.8/manual/en/asset/sprite-frame.html>
- free-tex-packer-core (no animation detection): <https://github.com/odrick/free-tex-packer-core>, <https://www.free-tex-packer.com/app/>
- ShoeBox: <https://renderhjs.net/shoebox/>; Kenney asset editing: <https://kenney.nl/knowledge-base/game-assets-2d/editing-2d-game-assets>
</content>
</invoke>
