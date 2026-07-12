# UI/UX Redesign — ntpacker-gui (2026-07-11)

> **Status: historical visual implementation record.** Color, spacing, and icon
> evidence may still be useful, but its architecture, packets, and snapshot
> assumptions are not an active plan. See
> [`../ntpacker-master-spec.md`](../ntpacker-master-spec.md) and
> [`ux.md`](ux.md).

Visual-language spec for the native GUI (`ntpacker-gui`, engine `nt_ui`/Clay). Owner
verdict: the current UI/UX is "very awkward" — wants it clearer, more responsive,
image-icons allowed. This doc defines ONE coherent dark-tool visual language, an
iconography plan, responsive rules, and a bounded implementation split. It does NOT
re-spec function or overflow (a parallel packet fixes pure overflow — assume content
fits); it must not contradict `ux.md` (esp. §3.3b stale-state, §3.3e mouse-complete,
principle 5 non-modal feedback) or re-litigate `ux-audit-2026-07-10.md`.

Scope grounding: current screenshots at `…/scratchpad/shots/*.png` (4 sizes/scales);
constants in `apps/gui/main.c` (layout ~L80-112, palette ~L114-213); icon vehicle
`external/neotolis-engine/engine/ui/nt_ui_image.h` (atlas region + `color_packed` tint
+ `flip_bits`); bake path `apps/gui/build_packs.c` + `apps/gui/CMakeLists.txt`.

---

## 1. Competitor comparison

| Axis | TexturePacker (benchmark) | Free Texture Packer | gdx-tex-packer-gui | rTexPacker | **ntpacker today** |
|---|---|---|---|---|---|
| Layout | 3-panel + top toolbar + bottom zoom bar | 3-panel (src list / preview / settings) | 3-panel, pack list left | compact single-window | 3-panel + menubar + canvas strip + status bar ✅ |
| Toolbar | **icon buttons** (Add sprites, Add smart folder, Publish, Sprite settings, Anim…) | flat icon/text mix | icon buttons, dark skin | raygui icon strip | **text-only grey blobs, no icons** ❌ |
| Settings org | Basic + **Advanced disclosure**; exporter dropdown at top gates controls | flat list, format dropdown top | grouped panels | flat | collapsible sections exist ✅ but headers recede (bg darker than panel) ❌ |
| Icons | throughout: toolbar, folder-type colors (yellow=smart, blue=nested) | some | dark flat icons | yes | **none anywhere** (unicode glyphs only) ❌ |
| Primary action | **Publish** emphasized on toolbar | Export button | Pack button | Export | Pack/Export identical to secondary ❌ |
| List affordances | tree, colored folder icons, N-badges, Delete-key remove | flat source list | pack list + source list, "show in FM" | flat | flat vlist, no row icons, blue-on-blue selection ❌ |
| Feedback | non-modal warnings/status; anim preview floats | inline | progress + FM links | status | one 256-byte status line; no notices panel (audit C2) ❌ |
| Scaling | continuous zoom slider + −/1:1/+/Fit | fixed | fixed | compact | −/%/+/Fit + DPI UI-scale ✅ |

**Takeaways.** (1) Every serious packer uses an **icon toolbar**; ours is the only
text-only one — this is the loudest "looks unfinished" signal in the shots. (2)
TexturePacker's **Basic/Advanced disclosure + a strong primary (Publish)** is the pattern
to match; we have the disclosure but no primary emphasis and headers that recede. (3)
TexturePacker's **colored folder icons** (smart vs nested) prove per-row iconography is a
core inspection affordance — our flat list has none.

**What ntpacker already does BETTER — preserve, do not regress:** the canvas renders the
**real packed page** with honest D4/hull/trim/pivot overlays and two-way row↔region
selection sync (`ux-audit` "keep" #1) — no surveyed free tool shows the true concave hull;
snapshot undo/redo (`gui_history.c`); virtualized sprite vlist. The redesign is chrome
only: it must not touch canvas rendering or the pack model.

---

## 2. Design direction

One language: a **dense dark engineering tool** (not a web app) — quiet surfaces, one
saturated accent, hierarchy from *elevation + a single accent rule + type-color tiers*,
icons for affordance. The current defect is **monotone flatness**: bg/panel/header/status
all sit within ~12 values of each other and section headers are *darker* than the panel
they head, so nothing reads as structured (see all 4 shots). Fix = widen value gaps, put
headers ABOVE panel value with a left accent rule, and reserve saturation for actions.

### 2.1 Palette tokens (exact RGB)

Clay colors are `{r,g,b,255}`; button `bg_tint` uses the existing `RGBA8(r,g,b)` macro
(main.c L103) — author every tint through it, never a hand-packed `0xFF..` literal (the
hand-packed accent is the root of the amber/blue-swap bug, `ux-audit` Polish + L153).

| Token | Role | Current const → RGB | **Proposed RGB** | Why |
|---|---|---|---|---|
| `bg` | window base | C_BG 18,18,22 | **17,18,23** | keep darkest |
| `canvas` | canvas well | C_CANVAS 12,13,16 | **11,12,15** | keep; darkest surface |
| `panel` | side panels | C_PANEL 30,34,42 | **28,31,40** | mid surface |
| `header` | section-header fill | C_STATUS 24,26,34 (too dark) | **40,45,57** | **lighter than panel** so headers advance, not recede |
| `input` | field well | (ad-hoc 42,46,56) | **21,23,30** | recessed well, darker than panel + border |
| `hover` | row/btn hover | C_HOVER 42,48,60 | **46,52,66** | keep, +contrast |
| `sel` | selected-row fill | C_SEL 52,78,120 | **48,74,120** | desaturated blue FILL (reads as selection, not a button) |
| `border` | 1px separators | C_BORDER 58,64,78 | **52,58,72** | keep |
| `border-strong` | focus ring / active | — | **86,132,204** | keyboard focus + active input |
| `accent` | **primary action** | (was mis-packed) | **64,140,214** | one bright saturated blue; clearly ≠ `sel` (brighter+saturated) |
| `accent-press` | primary pressed | — | **48,112,182** | |
| `warn` | **stale / warning** | g_warn 224,158,96 | **228,158,92** | amber — distinct hue from every blue (fixes §3.3b blue-on-blue) |
| `success` | up-to-date / exported | — | **104,186,124** | green, notices |
| `danger` | remove / error | — | **214,96,96** | red, destructive + errors |
| `text-strong` | values, active row | body 214,220,230 | **230,234,242** | strongest tier |
| `text` | rows, body | row 206,212,222 | **196,204,216** | |
| `text-dim` | labels, captions | caption 150,156,168 | **140,148,164** | |
| `text-faint` | hints, stale stats, disabled | dim 110,114,124 | **98,104,120** | |
| `link` | hyperlink | link 110,170,245 | keep 110,170,245 | |

Note the **title style bug**: `g_title` (170,180,196) is currently *dimmer* than `g_body`
(214,220,230) — titles recede. Retire the separate dim title color; section titles get
their weight from **uppercase + tracking + accent rule** (2.4), and panel/atlas titles use
`text-strong`.

### 2.2 Spacing scale (via `S()`)

Formalize the 4px base already implied by the `Su(4/6/8)` sprinkle. Tokens (logical px):
`SP_XS S(4)`, `SP_SM S(6)`, `SP_MD S(8)`, `SP_LG S(12)`, `SP_XL S(16)`. Rules: icon↔label
gap `SP_SM`; row inner padding `SP_MD` horizontal; section gap `SP_LG`; panel padding
`SP_MD`. `BASE_ROW_H 27` and strip/header `34/26` heights stay. One touch-up: strip
`childGap` → `SP_SM` and give the strip a bottom `border` (`border`) so it separates from
the canvas (today the semi-transparent strip floats).

### 2.3 Type scale (single Slug vector font — weight comes from size+color+case)

The app has one font; "weight" is faked. Current sizes (15–19) are too bunched for
hierarchy. Widen and assign color tiers:

| Style | px (base) | Color tier | Use |
|---|---|---|---|
| `title` | 16 | text-strong | panel/atlas title ("Atlas settings · animals") |
| `section` | 13 | text-dim, **UPPERCASE, +8% tracking** | ATLASES / SPRITES / Region / Export targets |
| `body` | 15 | text | button labels, values, editable fields |
| `row` | 15 | text | list rows (14→15, +1 for legibility) |
| `caption` | 13 | text-dim | secondary/meta ("Source", stats) |
| `hint` | 20 | text-faint | empty-state canvas hint |
| `tag` | 13 | on-accent/on-warn | chips, badges |

Section headers are the hierarchy backbone: quiet (dim, small, uppercase) but structured
by the accent rule — the pro-tool convention (TexturePacker/gdx). Do NOT make them big/bright.

### 2.4 Section-header treatment (biggest single hierarchy win)

Current `panel_header` (main.c L3118) = dark bar + chevron glyph + title, bg *darker* than
panel → recedes. Replace with: **fill `header` (lighter than panel) + a 3px `accent`
left-rule + a chevron ICON + uppercase `section` title.** Same treatment for the left
panel's ATLASES/SPRITES/ANIMATIONS captions (main.c L2076/2176/2277) — add the left-rule so
the three zones read as distinct stacked sections. The rule is the "you are in a section"
signal; the fill is secondary.

### 2.5 Button hierarchy (four tiers)

`nt_ui_button` already draws a rounded fill from `bg_tint` (Clay `cornerRadius`, no 9-slice
needed — confirmed main.c L1830). Define four styles:

- **Primary** (`g_btn_primary`, NEW): fill `accent`, `text-strong` label, icon. The single
  hero affirmative in a context — the Export-dialog run button, and **Pack** in its
  up-to-date/ready state. Exactly one primary visible per region.
- **Stale/action** (`g_btn_accent` reuse, RE-TINT to `warn`): Pack when preview is stale
  (§3.3b) — amber fill + `alert-triangle` icon + "Pack" label. Unmistakable, and distinct
  from primary-blue AND selection-blue (fixes the audit's blue-on-blue).
- **Secondary** (`g_btn`): quiet grey fill (panel+8 value), `text` label. Export (when not
  the primary), +Files/+Folder/+Atlas/+Animation/+Target.
- **Ghost / icon-only** (`g_btn_ghost`): transparent idle, `hover` on hover. Zoom −/%/+/Fit,
  page ◄/►, Refresh, remove ×, section chevrons. Icon-only ghosts REQUIRE a tooltip
  (`nt_ui_tooltip`) — mouse-complete + discoverability (`ux-audit` Polish "tooltips only on…").

Disabled = existing 0.35 opacity. Hover/press keep the existing eased scale (1.02/0.97) —
subtle and good; keep it.

### 2.6 Focus / hover / states

- **Hover:** rows/headers → `hover` fill; buttons → per-style hover tint (existing).
- **Keyboard focus** (unblocks audit I2, buildable game-side, not an engine gap): the game
  owns a focus index; draw a 1px `border-strong` inset border (Clay `.border`) on the
  focused row/button/field. Text inputs already exist (`s_num_input`); add the focus border.
- **Selection:** full-row `sel` fill + `text-strong` label. Because `sel` is a desaturated
  fill and primary is a bright button, they never confuse.
- **Active input:** `input` well + `border-strong`.

### 2.7 Empty states (audit Polish; ux.md §3.1)

Empty project canvas currently says "press Pack" with zero sources — wrong. Replace with a
centered empty state: a large `image`/`folder-plus` icon (text-faint) + "Add a folder to
start" (`hint`) + a **primary "Add folder" button** wired to `do_add_folder`. Same pattern
for an atlas with no sources. The ANIMATIONS empty note ("None. Multi-select…") stays text
but loses its tofu bar.

### 2.8 Status bar + notices color language

The bottom status bar (`declare_statusbar` L2752) is interim until the notices panel (audit
C2 / ux.md region H) lands. Give it the shared **severity language now** so C2 inherits it:
a leading severity icon + tint — info `text-dim`+`info`, success `success`+`check-circle`,
warning `warn`+`alert-triangle`, error `danger`+`octagon-alert`. Errors must not be
overwritten by the next info write (that's C2's job; here just stop tinting errors as plain
text). Stale stats keep `text-faint` (they describe the LAST pack).

### 2.9 Stale-state visuals (ux.md §3.3b, verbatim)

Pack stale → amber Pack (2.5) + amber "outdated — press Pack" chip (exists, re-tint from
blue to `warn`, add `alert-triangle` icon) + **dim the canvas ~12% and show a corner
"outdated" tag** (audit Polish: canvas itself not yet dimmed). Up-to-date → Pack is primary,
tooltip "up to date (packed N ms ago)". This is the one place amber is mandatory.

---

## 3. Iconography plan

**Vehicle:** `nt_ui_image` — an atlas-region image with `color_packed` **tint**, composed
as a child inside `nt_ui_button_begin/_end` (button content = children, header L46) or
standalone in a row. Because tint is per-draw, icons are baked **monochrome white-on-alpha
once** and tinted at render to any text tier / accent / warn — one sprite serves dim label,
active row, and amber-stale states. No new engine widget required.

**Bake pipeline (into the existing UI atlas):** add icons to `ntpacker_ui_atlas` in
`build_packs.c` (alongside `_white`, L97-105) — that atlas is already `shape=RECT`,
`allow_transform=false` (icons must never rotate/flip-pack), `LINEAR` min/mag (clean
downscale), premultiplied (white·alpha premult tints correctly). Codegen then emits
`ASSET_ATLAS_REGION_NTPACKER_UI_ATLAS__PACK` etc.; resolve at runtime exactly like the white
region (main.c L1806-1809): `nt_atlas_find_region(handle, MACRO.value)` → `nt_atlas_ref_idx`
→ store an `nt_atlas_region_ref_t` per icon. Add via
`nt_builder_atlas_add(ctx, "<icons_dir>/pack.png", &opts)` per file (name derived from
basename) or one `nt_builder_atlas_add_glob(ctx, "<icons_dir>/*.png", NULL)`. Because
`build_packs` runs with WORKING_DIRECTORY = engine root (CMake L99), pass the ntpacker icons
dir as a **new argv[3]** from the `add_custom_command` (CMakeLists L97) =
`${CMAKE_CURRENT_SOURCE_DIR}/assets/icons`, and join absolute in `build_packs.c`.

**Grid & scale survival.** Author on a **24px grid, 2px stroke**; export each master PNG at
**48px (2×)**; render into logical boxes `S(16)` (strip/menubar/row-action), `S(14)` (row
type icons), `S(12)` (chevrons). One 48px master downsamples crisply via LINEAR at scale
1.0 (48→16=physical24) and 1.5 (→24), and is ~native at 2.0 (→32). Hero empty-state icon:
separate 96px master rendered `S(48)`. **Avoid 1px hairlines** (vanish on downscale) — 2px
stroke min. This mirrors how the vector font already survives `g_ui_scale`; raster at 1.5×
is slightly soft but acceptable for 2px-stroke glyphs.

**Style:** stroke (outline), 2px, rounded joins, monochrome — matches the light UI type and
the Lucide house style. Filled variants only for status dots.

**SOURCE — recommend (b): vendor Lucide (MIT), pre-rasterized to PNG, attribution +
regen script checked in.** Rationale: the redesign's job is to stop looking amateur;
Lucide is a professional, internally-consistent 24px/2px set that covers every glyph below,
MIT needs only an attribution file, and committing the SVG sources + a `regen_icons.py`
(cairosvg/resvg → 48px PNG) keeps it license-clean AND regenerable — the same two properties
option (a) sells, without the risk that hand-built geometric PNGs (gear, folder, film-strip)
look worse than what we're replacing. Commit the rasterized PNGs (so the build needs no
Python), plus `apps/gui/assets/icons/{*.svg,*.png}`, `regen_icons.py`, `ATTRIBUTION.md`
(Lucide ISC/MIT + listed icon names). *(Rejected — (a) pure-Python geometric: only the ~5
primitives (chevron/plus/minus/x/triangle) render crisply that way; the pictographic glyphs
would regress the very "ugly" verdict we're fixing. If crispness at 1.5× ever bites, those 5
primitives MAY be pixel-snapped generated as a follow-up — one source is simpler for v1.)*

**Inventory** (Lucide glyph → placement → tint):

| Area | Icon (Lucide) | Placement / anchor | Tint |
|---|---|---|---|
| Strip | `layout-grid` | Pack button (declare_canvas_strip L2523) | text-strong / on-warn when stale |
| Strip | `alert-triangle` | prepended on Pack + chip when stale (L2523/2567) | warn |
| Strip | `download` | Export button (L2527) | text |
| Strip | `refresh-cw` | Refresh (L2530, replaces ⟳) | text-dim |
| Strip | `chevron-left`/`chevron-right` | page ◄/► (L2535/2542) | text-dim |
| Strip | `minus`/`plus` | zoom −/+ (L2548/2557) | text-dim |
| Strip | `scan` | zoom-100/reset (wrap the % label) | text-dim |
| Strip | `maximize-2` | Fit (L2560) | text-dim |
| Left | `layers` | atlas rows (declare_atlas_list L2075) | text / text-strong sel |
| Left | `folder` / `folder-open` | folder source + expand state (build_rows L1576) | warn-ish (smart) |
| Left | `image` | sprite/file leaf rows | text-dim |
| Left | `film` | animation rows (ANIMATIONS L2277) | text-dim |
| Left | `file-plus` / `folder-plus` | +Files / +Folder (L2176 area) | text |
| Left | `plus` | +Atlas / +Animation / +Target | text |
| Left | `x` | remove × on source rows | text-dim → danger hover |
| Left | `search` | filter field (future, audit C4) | text-faint |
| Right | `chevron-down`/`chevron-right` | section chevron (panel_header L3132) | text-dim |
| Right | `check` | checkbox tick (or keep font ✓, tp_checkbox L1876) | accent |
| Right | `link`/`external-link` | target out-path browse; About repo | link |
| Canvas | `square-dashed`/`crop`/`crosshair` | overlay toggles outline/trim/pivot (View menu) | text-dim/accent when on |
| Status | `info`/`check-circle`/`alert-triangle`/`octagon-alert` | status-bar + notices severity (§2.8) | dim/success/warn/danger |
| Empty | `folder-plus` (96px) | empty-state hero (declare_canvas) | text-faint |
| Menu (opt.) | `file-plus`/`folder-open`/`save`/`undo-2`/`redo-2` | File/Edit menu items | text-dim |

Menubar top-level (File/Edit/View/Help) stays **text** (icons on word-menus read as noise);
icons go on menu *items* only, optional.

---

## 4. Responsive behavior rules

Consistent with the overflow-fix packet (no overflow ever) — these say WHAT compacts, in
**logical px** (post-scale; use `scale.logical_w` already fed to `compute_panel_widths`,
main.c L4759/L688). Deterministic, three breakpoints, no reflow/wrapping (the strip must
never wrap "page 1/2" as it does at 1366×s1.5).

| Logical width | Strip | Panels | Labels |
|---|---|---|---|
| **≥ 1180** (wide) | Pack/Export = icon+label; all others icon-only | left 300 / right 300 fixed | full label column (`PANEL_LABEL_W` 116) |
| **900–1180** (mid) | Pack/Export icon+label; hide zoom-% label (keep −/+/Fit); chip → icon+2 words | right panel clamps `PANEL_LABEL_W` down to ~92 (already clamps) | ellipsize values (exists) |
| **< 900** (narrow) | **all strip buttons icon-only** (Pack/Export lose text; tooltip carries it) | side panels shrink to min (left 240 / right 240) before canvas gives up | label column ~80, ellipsize |

Rules: (1) icon-only buttons always keep a tooltip. (2) Never wrap the strip to two lines —
drop labels instead. (3) The right panel label column already runtime-clamps
(`compute_panel_widths`); just add the two width stops above. (4) Menubar project title
ellipsizes (right-aligned) rather than pushing menus. (5) Below the existing minimum where a
clip box would collapse (the `empty-scissor` guard, main.c L4799), keep the current
guard — that's the overflow packet's floor, not this spec's.

---

## 5. Prioritized implementation checklist (apps/gui only)

Three bounded packets, ordered by owner-visible impact. Each item: what / where / accept.

### Packet A — Icons pack + strip + button hierarchy  *(biggest "looks pro" jump)*
1. **Icon bake.** Add ~24 Lucide PNGs to `ntpacker_ui_atlas` in `build_packs.c` (after the
   `_white` add, L102); pass icons dir as argv[3] (CMakeLists `add_custom_command` L97; join
   absolute). *Accept:* codegen header lists `…__PACK` etc.; `--shot` shows crisp icons at
   scale 1.0/1.5/2.0, no tofu.
2. **Icon refs + helper.** Resolve each ref like `s_white_ref` (main.c L1806-1809, `ensure_ids`
   /`try_bind_resources`); add `ui_icon_btn(ctx,id,icon_ref,text_or_NULL,style,…)` extending
   `ui_btn` (L1823) to emit `nt_ui_image` + optional label. *Accept:* one call site renders
   icon-only and icon+label.
3. **Palette + button tokens.** Apply §2.1 RGBs to the `C_*`/`g_*_base` consts (L114-138) via
   `RGBA8`; add `g_btn_primary`, re-tint `g_btn_accent`→`warn`, `g_btn_stale`→`warn`
   (L152-170). *Accept:* Pack primary-blue when ready, amber+⚠ when stale; distinct from
   selection fill.
4. **Rebuild strip.** `declare_canvas_strip` (L2507): Pack/Export icon+label, Refresh/page/
   zoom/Fit icon-only ghost+tooltip; strip bottom border; §4 compaction booleans. *Accept:*
   no two-line wrap at 1366×1.5; tooltips on every icon-only button.

### Packet B — Panels + list hierarchy
5. **Section headers.** `panel_header` (L3118): `header` fill + 3px `accent` left-rule +
   chevron ICON + uppercase `section` title. Same rule on left ATLASES/SPRITES/ANIMATIONS
   captions (L2076/2176/2277). *Accept:* headers advance from panel; three left zones read
   as sections.
6. **Row icons + states.** `build_rows`/`declare_atlas_list` (L1576/L2075): leading
   `layers`/`folder`/`image`/`film` icon per row; selection = `sel` fill + `text-strong`;
   hover `hover`; optional zebra on odd rows. *Accept:* every row has a type icon; selection
   ≠ any button.
7. **Field wells + focus.** Inputs/dropdowns/checkbox (`s_num_input`, `tp_checkbox` L1851,
   `s_dd_style`): `input` fill + `border`, `border-strong` when focused/active. *Accept:*
   fields read as recessed wells; focused field ringed.
8. **Type scale.** Apply §2.3 sizes/tiers to `FS_*` (L106-111) and label styles (L126-138);
   retire dim-title. *Accept:* clear title>body>label>caption ladder.

### Packet C — States + feedback
9. **Empty state.** `declare_canvas`/`declare_canvas_preview` (L2645/2577): hero icon + "Add
   a folder to start" + primary "Add folder" (→`do_add_folder`) when no sources. *Accept:*
   fresh project shows it, not "press Pack".
10. **Stale visuals.** Canvas dim ~12% + corner "outdated" tag while stale (§2.9; canvas draw
    is `gui_canvas`, tag drawn in `declare_canvas`). *Accept:* stale canvas visibly dimmed +
    tagged, matches §3.3b.
11. **Status severity.** `declare_statusbar` (L2752): severity icon + tint per §2.8; stop
    overwriting/decoloring errors. *Accept:* warning amber, error red+icon, success green —
    the visual tokens region H (audit C2) will reuse.

**Order rationale:** A lands the icon toolbar + primary Pack — the single change that most
answers "стремные"; B fixes the flat hierarchy the shots show; C completes the feedback/empty
gaps. Each packet is independently shippable and touches only `apps/gui/` (+ committed
`assets/icons/`), engine tree untouched.

### Needs-engine-issue list
None. Everything above is buildable from shipped `nt_ui` widgets (`nt_ui_image` tint,
`nt_ui_button` content children, Clay `cornerRadius`/`border`/`backgroundColor`,
`nt_ui_tooltip`). Keyboard-focus ring and zebra rows are game-side Clay draws, not engine
gaps. The notices *panel* itself (region H) is audit C2's functional packet — this spec only
fixes its color/icon language so C2 inherits it.
