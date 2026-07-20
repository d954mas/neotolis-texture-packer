# UX Pattern Library for ntpacker

**Goal:** extract what makes Figma, Linear, Blender 2.8+, VS Code, Aseprite, Godot 4, and Raycast/Alfred *beloved*, and turn each proven pattern into a concrete design move for ntpacker (native desktop texture/atlas packer; 3 panels — atlas/sprite tree | packed-page canvas | settings inspector — plus menubar, toolbar, status bar; explicit Pack with amber stale state; snapshot undo; Defold-first).

**Method:** four parallel research threads over primary sources (official design/engineering blogs, release notes, docs, founder interviews, NN/g and classic HCI papers). Compiled 2026-07-13.

**Ranking:** patterns are ordered by expected impact for ntpacker = (how often a user benefits per session) x (how much it differentiates vs TexturePacker-class tools) x (feasibility in a custom C / Clay immediate-mode UI). Tiers group them; numbers are the global rank.

---

## Tier 1 — The core contract: speed, indexability, trust

### 1. Command palette as the capability index
- **Proven by:** VS Code (`Ctrl+Shift+P` — "access to all functionality within VS Code"), Blender F3 operator search (made a ~2,000-operator app navigable), Linear `Cmd+K` (contextual to selection), Figma Quick Actions (`Ctrl+/`), Godot 4 (`Ctrl+Shift+P`, deliberately copied VS Code).
- **Why loved:** operationalizes NN/g heuristic #6 (recognition over recall) — users type approximate words instead of hunting menus; it is simultaneously the fastest expert path and the best feature-discovery mechanism. Pattern lineage: Sublime Text 2011 → everywhere. Sources: https://code.visualstudio.com/docs/getstarted/userinterface ; https://docs.blender.org/manual/en/2.80/interface/controls/templates/operator_search.html ; https://www.nngroup.com/articles/recognition-and-recall/ ; https://www.vendr.com/blog/consumer-dev-tools-command-palette
- **ntpacker:** `Ctrl+Shift+P` palette indexes EVERY operation — menu items, toolbar actions, per-setting mutations with arguments ("set max page size 1024", "set padding 4", "add export target: defold"), and navigation ("go to sprite <fuzzy name>", "go to page 3"). Every row shows its keybinding inline. This is the single highest-leverage feature: it makes the whole tool searchable and self-documenting at once, and it is cheap in an immediate-mode UI (one list + fuzzy filter over an op registry — which the typed-operation engine already implies).

### 2. The 100ms budget + optimistic pack flow (performance-as-UX)
- **Proven by:** Nielsen's canonical limits (0.1s = "instantaneous", 1s = flow limit); Doherty threshold (<400ms → productivity inflection, IBM 1982); Superhuman's hard SLO ("every interaction should be faster than 100ms... aims for less than 50ms"); Linear (sync-engine-first architecture, most page loads <50ms, "fast software gets used and is loved" — Karri Saarinen); Figma deliberately delays loading spinners until 1+ second so fast operations never *look* slow.
- **Why loved:** latency never interrupts thought — users experience the app as responding "at the speed of intention," which is the literal substance of "feels native / feels great." Sources: https://www.nngroup.com/articles/response-times-3-important-limits/ ; https://lawsofux.com/doherty-threshold/ ; https://blog.superhuman.com/superhuman-is-built-for-speed/ ; https://performance.dev/how-is-linear-so-fast-a-technical-breakdown ; https://www.figma.com/blog/little-big-updates-dispatches-from-quality-week/
- **ntpacker:** adopt 100ms as a hard budget for every UI interaction (selection, tree scroll, inspector edits, zoom); run Pack on a worker thread with the amber-stale state as the optimistic-UI mechanism (edits apply instantly, canvas shows last pack + stale banner); never show a spinner for anything under ~1s. For small projects, add auto-repack-on-change under a threshold (e.g. <300ms measured pack time) so the canvas feels live — explicit Pack remains for big projects.

### 3. The UI teaches its own shortcuts (novice-to-expert ramp everywhere)
- **Proven by:** Blender 2.8 (tooltips show shortcut + Python name; status bar live-shows what each mouse button does per mode/tool); VS Code (palette shows keybindings; keybinding editor lists commands *without* bindings to advertise capability); Linear (every context-menu row prints its shortcut — "the mouse path trains the keyboard path"); Figma (shortcut panel highlights shortcuts you've already used); Raycast (action panel shows each action's shortcut "so you can learn to trigger them directly over time"). Theory: Alan Cooper's "perpetual intermediates" — design for the middle, give beginners a ramp and experts automation.
- **Why loved:** the ramp is embedded in normal use rather than in documentation; users get faster every week without ever reading a manual. Sources: https://developer.blender.org/docs/release_notes/2.80/ui/ ; https://linear.app/now/invisible-details ; https://help.figma.com/hc/en-us/articles/360040328653 ; https://blog.codinghorror.com/defending-perpetual-intermediacy/
- **ntpacker:** every tooltip shows the shortcut; every menu and context-menu row prints its accelerator; the status bar's right side shows contextual mouse/key hints for the hovered panel ("LMB select · RMB menu · Ctrl+Scroll zoom"). One vector font + Clay makes this a rendering convention, not a feature.

### 4. Canvas navigation grammar + canvas as a first-class selection surface
- **Proven by:** Figma's spatial grammar — scroll = pan, `Ctrl+Scroll` = zoom-at-cursor, Space-drag = pan, `Shift+1` = fit, `Shift+2` = zoom-to-selection, `Shift+0` = 100%, double-click = deep select, right-click → "Select layer" lists everything under the cursor.
- **Why loved:** deep documents stay navigable; "go look at this thing" is one keystroke; you never fight the selection model. Sources: https://help.figma.com/hc/en-us/articles/360041065034 ; https://help.figma.com/hc/en-us/articles/360040449873
- **ntpacker:** adopt the Figma grammar verbatim (users already know it). Critically: clicking a packed sprite on the canvas selects it in the tree AND loads it in the inspector (and vice versa — selecting in tree highlights + optionally zooms on canvas via `Shift+2`/`F`); right-click on overlapping trimmed sprites lists all rects under the cursor; `Shift+0` shows the atlas at true 100% texel scale. This closes the tree↔canvas↔inspector triangle that most packers leave disconnected.

### 5. Everything undoable, with named undo (undo as exploration license)
- **Proven by:** NN/g heuristic #3 — reliable undo "encourages exploration, which facilitates learning and discovery of features"; Shneiderman's direct-manipulation criteria require "rapid, incremental, reversible" operations; optimistic UI is only safe when paired with visible revert.
- **Why loved:** undo converts a cautious user into an experimenting one; it's what makes keyboard-speed destructive operations acceptable at all. Sources: https://www.nngroup.com/articles/user-control-and-freedom/ ; https://www.nngroup.com/articles/direct-manipulation/
- **ntpacker:** extend snapshot undo to literally every mutation — including inspector setting changes, export-target edits, and multi-sprite operations — and *name* each step: the undo toast and Edit menu read "Undo: set padding 4 (3 sprites)". An undo-history dropdown (Photoshop-style) is a cheap add on top of snapshots. Advertise "everything is undoable" as a product promise.

### 6. Non-default markers + one-click revert on every inspector property
- **Proven by:** Godot 4 inspector — every non-default value gets a revert icon; "Expand Non-Default" shows only what you changed; property pinning (PR #52943) built on one universal notion of a property's default. VS Code settings — "colored bar on the left of the setting, similar to modified lines" + `@modified` filter.
- **Why loved:** hundreds of properties stay navigable and "what did I change?" is answerable at a glance — the revert arrow doubles as a diff marker. Sources: https://docs.godotengine.org/en/stable/tutorials/editor/inspector_dock.html ; https://github.com/godotengine/godot/pull/52943 ; https://code.visualstudio.com/docs/configure/settings
- **ntpacker:** every inspector field that differs from the default (or from the atlas-level value, for per-sprite overrides) gets an accent-colored left bar + revert glyph; a "modified only" filter atop the inspector. This is doubly powerful with settings inheritance (project → atlas → sprite): the marker shows *where* a value comes from.

### 7. Problems panel: aggregated, clickable diagnostics at three zoom levels
- **Proven by:** VS Code — squiggle (character) + overview ruler (file) + Problems panel `Ctrl+Shift+M` (project), with the status-bar error/warning count as the entry point; every representation is a navigation affordance.
- **Why loved:** the same diagnostic is visible at every zoom level and always click-to-jump; nothing gets lost in a log. Source: https://code.visualstudio.com/docs/editing/editingevolved
- **ntpacker:** aggregate all pack/export diagnostics — sprite exceeds max page size, duplicate sprite name, missing source file, page overflow, NPOT warnings, Defold-specific export issues — into a Problems panel; clicking an entry selects the sprite in the tree, zooms the canvas to it, and highlights the offending inspector field. Status bar shows "⚠ 3" as the always-visible entry point.

### 8. Status bar as ambient, clickable state
- **Proven by:** VS Code — branch, error counts, language mode, indentation; every segment is an action opening a picker; codified placement rules (workspace-level left, contextual right) keep it calm.
- **Why loved:** state you'd otherwise hunt for is one glance away, and one click deep. Sources: https://code.visualstudio.com/docs/getstarted/userinterface ; https://code.visualstudio.com/api/ux-guidelines/status-bar
- **ntpacker:** left side = project state: pack status (fresh/stale/packing), page count + fill % ("3 pages · 87% fill"), problems count; right side = context: cursor texel coords, zoom %, selected-sprite dims, key hints. Every segment clickable: fill % → per-page breakdown popover; stale → Pack; ⚠ → Problems panel.

---

## Tier 2 — First-run and the zero-to-packed path

### 9. Opinionated defaults: zero-config to first result
- **Proven by:** Linear Method — "A tool should be simple to get started with and grow more powerful as you scale"; "Your tools should not make you the designer and maintainer of them". Aseprite — review analysis shows time-to-competency ~15 minutes; "the default screen already IS the workflow". Blender 2.8 shipped world-aligned defaults (left-click select) while keeping the old way as a preference.
- **Why loved:** users escape config swamps (Jira/TexturePacker settings walls); the tool's opinion doubles as guidance. Sources: https://linear.app/method/introduction ; https://vaporlens.app/app/431730/aseprite ; https://projects.blender.org/blender/blender/issues/56702
- **ntpacker:** the golden path must be: launch → drag folder → packed atlas on screen with good defaults (max 2048, padding 2, trim on, Defold target preselected) → Export — under 60 seconds, zero dialogs. Every default should be the value a Defold dev would have chosen; deviations are what the inspector is for.

### 10. Drag-and-drop as the primary ingestion gesture (direct manipulation)
- **Proven by:** Shneiderman's direct-manipulation properties — act on the visible object with physical actions, not dialog syntax; Figma's alt-drag duplicate / drag-everything ethos.
- **Why loved:** when the object stays visible and every operation is small, instant, and reversible, users act on the thing itself instead of negotiating with a form. Source: https://www.nngroup.com/articles/direct-manipulation/
- **ntpacker:** drag files/folders from the OS onto the window → new atlas (or into a specific atlas node in the tree); drag sprites between atlases in the tree; drag a sprite out of the tree onto an export-target to test membership. Every drop is one undo step.

### 11. Empty states teach the first action
- **Proven by:** NN/g — empty states should "provide direct pathways for getting users started with key tasks"; in-context cues at the moment of need beat up-front tutorials. Linear/Slack/Notion first-run panels.
- **Why loved:** the blank panel is the one screen every new user is guaranteed to see; it's the cheapest onboarding a tool can ship. Source: https://www.nngroup.com/articles/empty-state-interface-design/
- **ntpacker:** empty tree panel: "Drop a folder of PNGs here — or Ctrl+O to open, Ctrl+Shift+P for all commands." Empty canvas: "Nothing packed yet — press P to pack." Empty export panel: "Add a target: Defold · JSON · ..." Each empty state names both the mouse path and the keystroke.

### 12. No dead ends: every state offers the next action
- **Proven by:** Alfred fallback searches — "when he can't find results locally," the empty result converts into forwarding actions; Raycast's uniform `Cmd+K` action panel means no result is ever inert.
- **Why loved:** every keystroke sequence terminates in a useful action; typing is never wasted. Sources: https://www.alfredapp.com/help/features/default-results/fallback-searches/ ; https://manual.raycast.com/action-panel
- **ntpacker:** palette queries with no command match fall back to "search sprites for '<query>'" and "search help"; a failed pack always ends in actionable diagnostics ("sprite 'boss.png' 4096px > max page 2048 → [set max page 4096] [scale sprite] [exclude]") — offered as buttons in the problem entry, never as a bare error toast.

---

## Tier 3 — Inspector and canvas craft (the daily-driver details)

### 13. Calculator-grade numeric inputs: math expressions + scrubbing
- **Proven by:** Figma — every numeric field accepts `+ - * / ^ ( )` (type `2048/2`, or `+10` relative to current), and every field is scrubbable by dragging its label, with vertical cursor position switching drag precision (2x/1x/1/2/1/4).
- **Why loved:** users think in relationships ("half this, plus 8px gutter") and express them directly; scrubbing turns precise fields into direct-manipulation sliders. Source: https://help.figma.com/hc/en-us/articles/360039956914
- **ntpacker:** all inspector numerics (padding, extrude, max size, scale) accept math and label-scrubbing with Shift/Alt precision modifiers; power-of-two fields scrub in POT steps. In immediate-mode UI this is one widget upgraded once, paying off everywhere.

### 14. Single-key contextual shortcut grammar
- **Proven by:** Linear — bare letters operate on the focused item (C create, S status, P priority, A assign), two-letter sequences for navigation; "Keyboard shortcuts... let you control all of the common functionality and make the application a lot faster to use" (Linear changelog). Aseprite's tap-to-switch tools.
- **Why loved:** triage becomes typing — no modifier gymnastics on the hot path, and the letter=verb grammar is guessable, so skills transfer. Sources: https://linear.app/changelog/2021-03-25-keyboard-shortcuts-help ; https://www.aseprite.org/docs/keyboard-shortcuts/
- **ntpacker:** with sprite(s) selected: `T` toggle trim, `X` exclude/include from pack, `E` cycle extrude, `R` rename, `Del` remove, `F` zoom-to-sprite; global: `P` pack, `Ctrl+E` export. `?` opens a searchable shortcut sheet. Multi-select + one key = batch operation (one undo step).

### 15. Alt-hover measurement on the canvas
- **Proven by:** Figma — select a layer, hold Alt, hover another: red guides with exact px distances appear; the feature engineers cite most for handoff, a zero-UI gesture.
- **Why loved:** spacing audits with no tool to invoke and no mode to enter. Source: https://help.figma.com/hc/en-us/articles/360039956974
- **ntpacker:** hold Alt with a sprite selected → live px distances from its rect to the hovered sprite's rect and to page edges; instantly answers "did padding/extrude actually apply?" and "why did these two end up adjacent?" — the packer-specific spacing audit.

### 16. Always-live preview at final scale (kill the export-and-check loop)
- **Proven by:** Aseprite's F7 preview window — "shows you the animation preview in realtime while you edit" at native size, while you work zoomed at 800%; Godot's remote/live-edit while the game runs. Aseprite's tiled mode does the same for seamless textures.
- **Why loved:** artists work zoomed but ship at 100%; seeing both simultaneously eliminates the edit→export→check ritual — the single most-cited delight mechanic in that research thread. Sources: https://www.aseprite.org/docs/preview-window/ ; https://www.aseprite.org/docs/tiled-mode/
- **ntpacker:** a detachable/floating 1:1 preview pane showing the selected sprite (or page region) at true texel scale with the chosen filter mode (nearest/linear) — what it will look like in-engine — while the main canvas stays zoomed for inspection. Live-updates on repack.

### 17. Animation tags → engine clips (Defold flipbooks as first-class objects)
- **Proven by:** Aseprite — multiple animations per file via tags (loop/forward/reverse/ping-pong); "tags map 1:1 onto game-engine animation clips"; #1-cited workflow in reviews.
- **Why loved:** the tool's grouping concept matches the engine's runtime concept exactly, so nothing is lost in export. Source: https://www.aseprite.org/docs/timeline/
- **ntpacker:** auto-detect sequences (`walk_01..walk_08`) into animation groups in the tree, editable (fps, loop, ping-pong); play them in the live preview (pattern 16) rendered *from the packed atlas*; export as Defold `.atlas` animation groups. TexturePacker treats animations as an afterthought — for a Defold-first tool this is a headline differentiator.

### 18. Right-click context menus everywhere — with shortcuts and a safe triangle
- **Proven by:** Blender 2.8 ("Throughout Blender, there is now a right-click context menu... quick access to important commands in the given context"); Linear's contextual menus with a triangular safe area for diagonal submenu travel and shortcuts printed on every row ("hundreds of interactions a day" justify shaving 1-2s).
- **Why loved:** this is the texture users describe as "everything just feels right" — sub-perceptual mechanics removing micro-failures, plus an ambient shortcut tutorial. Sources: https://developer.blender.org/docs/release_notes/2.80/ui/ ; https://linear.app/now/invisible-details
- **ntpacker:** right-click works on every object — tree nodes, canvas sprites, page tabs, inspector fields (→ "reset to default", "copy value", "apply to all selected"), status-bar segments — each menu contextual, each row printing its accelerator.

### 19. Momentary (hold-key) tools
- **Proven by:** Aseprite's dual activation — tap = switch, hold = "quick" mode that returns to the previous tool on release (hold Alt = eyedropper, hold Space = hand); Figma's Space-drag pan.
- **Why loved:** your "home" tool is never lost; hands never leave position. Source: https://www.aseprite.org/docs/keyboard-shortcuts/
- **ntpacker:** hold Space = pan, hold Z = zoom, hold Alt = measure (pattern 15), hold Tab = temporarily solo the selected sprite's overlay — all momentary, all restoring the previous state on release.

### 20. Neutral grayscale chrome that doesn't distort the art
- **Proven by:** Godot 4.6's default theme went deliberately grayscale so it "allows you to do color-sensitive work on your game without the blue tint altering your perception"; Blender's principle "the UI chrome always defers to the user-created content."
- **Why loved:** the tool must not distort the craft — for artists judging textures, tinted chrome is a real defect. Sources: https://godotengine.org/releases/4.6/ ; https://code.blender.org/2018/04/tools-toolbar-and-tool-widgets/
- **ntpacker:** keep the dark theme strictly neutral-gray around the canvas; offer canvas backdrop options (checkerboard / black / white / mid-gray / custom) one click away in the canvas corner, since sprite edges read differently against each.

---

## Tier 4 — Power users and pipelines

### 21. CLI/GUI parity: the project file packs headlessly
- **Proven by:** Aseprite CLI (`aseprite -b sprite.ase --sheet atlas.png --data atlas.json`, `--sheet-pack`, `--tag`, `--trim`) — "integrate Aseprite in your assets pipeline"; top Steam reviews explicitly praise pipeline integration.
- **Why loved:** the file becomes a source-of-truth asset in build pipelines; the tool graduates from "app" to "infrastructure." Source: https://www.aseprite.org/docs/cli/
- **ntpacker:** `ntpacker --pack project.ntp --out build/` produces byte-identical output to the GUI's Export (same engine code path); a "Copy as CLI command" action in the export panel teaches the pipeline form. Defold teams will wire this into CI on day one.

### 22. Human-readable project file + GUI as two views of one model
- **Proven by:** VS Code settings — the GUI editor and `settings.json` edit the same data with no feature gap, user/workspace scoping, and `@modified` filtering.
- **Why loved:** beginners get a searchable GUI, power users get diffable/committable text — reviewable in PRs, mergeable, greppable. Source: https://code.visualstudio.com/docs/configure/settings
- **ntpacker:** the `.ntp` project format is stable, ordered, human-readable text (deterministic serialization → clean git diffs); "Open project as text" in the menu; every inspector change maps to an obvious diff line. Combined with pattern 21, the project file becomes the team's shared contract.

### 23. Frecency ranking + aliases in the palette
- **Proven by:** Raycast/Alfred — results ranked by frequency+recency (Mozilla's frecency algorithm); Raycast aliases ("gc" → Chrome) and per-command hotkeys form an explicit frequency tier: search rare things, alias frequent things, hotkey constant things.
- **Why loved:** after a few uses, 2-3 characters reliably resolve to the intended command — ranking quality, not search speed, makes "type two letters, Enter" work. Sources: https://manual.raycast.com/command-aliases-and-hotkeys ; https://wiki.mozilla.org/User:Jesse/NewFrecency
- **ntpacker:** the command palette (pattern 1) ranks by frecency per project; users can assign a custom keybinding to any palette command from inside the palette row (Raycast/VS Code style).

### 24. Quick Favorites: user-curated quick menu by pointing at the thing
- **Proven by:** Blender 2.8's Q menu — right-click any button or menu item → "Add to Quick Favorites"; customization with zero configuration UI.
- **Why loved:** users build their own accelerator layer by pointing at the thing itself. Source: https://developer.blender.org/docs/release_notes/2.80/ui/
- **ntpacker:** right-click any inspector field, menu item, or toolbar button → "Add to Quick Menu"; `Q` pops the personal menu at the cursor. Cheap to build once actions are a registry (pattern 1 prerequisite).

### 25. Remappable keys, searchable binding editor, deliberately sparse defaults
- **Proven by:** Blender 2.8 (three named keymap profiles incl. "Industry Compatible"; "assigning fewer shortcut keys by default so users can map their own" — empty key space as a user resource); VS Code's searchable keybinding editor listing commands *without* bindings.
- **Why loved:** migrants from other tools get a profile instead of a fight; unused capability is advertised, not hidden. Sources: https://developer.blender.org/docs/release_notes/2.80/ui/ ; https://code.visualstudio.com/docs/configure/keybindings
- **ntpacker:** a searchable shortcuts editor over the same command registry; ship sparse, conventional defaults (Figma-canvas + VS Code-palette conventions users already know) and leave letters free; optionally a "TexturePacker-compatible" keymap profile to de-risk migration.

### 26. Progressive disclosure in the inspector: ≤2 levels, basic upfront
- **Proven by:** NN/g — "defers advanced or rarely used features to a secondary screen, making applications easier to learn and less error-prone," with the hard guideline that >2 disclosure levels breaks usability; Raycast's one flat action panel is the same rule in launcher form.
- **Why loved:** the initial screen carries only high-frequency options, improving both learnability AND error rates. Source: https://www.nngroup.com/articles/progressive-disclosure/
- **ntpacker:** inspector sections show the 5-6 settings everyone touches (max size, padding, trim, extrude, algorithm, format); each section has one "Advanced" disclosure (heuristics, rotation policies, alpha handling) — never nested further. A search box atop the inspector (VS Code settings style) bypasses the hierarchy entirely.

---

## Tier 5 — Meta-patterns (process, not pixels)

### 27. "Little Big Updates": QoL as a release strategy
- **Proven by:** Figma — batches 30+ tiny fixes into named, celebrated releases and runs internal Quality Weeks; Sho Kuwamoto: "when you celebrate the small things... it gives people a chance to recognize how important each one is." Outcomes included a 1000%-faster typing edge case.
- **Why loved:** users spend 40+ hrs/week in a pro tool, so shaving clicks off repeated actions compounds more than headline features — and public celebration signals the vendor notices the same annoyances users do. Source: https://www.figma.com/blog/little-big-updates-2023/
- **ntpacker:** maintain a standing "paper cuts" backlog; periodically ship and *announce* a batch ("14 quality-of-life fixes") in release notes/devlogs. For a tool whose pitch is "all other packers feel bad by comparison," this cadence IS the marketing.

### 28. Interaction path length is a performance metric (design speed x engineering speed)
- **Proven by:** the Linear teardown's synthesis — "A perfectly built sync engine still loses to a slow input model: if the fastest path to an action requires a mouse, three menus, and a click, the user pays for those steps"; Linear treats path length to every action as a metric alongside render latency.
- **Why loved:** users experience "fast" as engineering latency x interaction path length; both must be optimized — this is the unifying pattern behind every beloved app studied. Source: https://performance.dev/how-is-linear-so-fast-a-technical-breakdown
- **ntpacker:** audit the top ~10 workflows (add sprites → pack → export; retarget max size; find a sprite; fix a warning) and count keystrokes/clicks; set explicit budgets ("re-export after a source change: ≤2 actions") and regression-test them like performance numbers.

### 29. Extensibility that can't degrade the core (when scripting arrives)
- **Proven by:** VS Code — extensions run out-of-process so "the VS Code core is less impacted... save happens in the core so if an extension does some weird things it cannot impact the save" (Erich Gamma); Raycast renders third-party extensions through native components so keyboard behavior stays uniform.
- **Why loved:** users trust that adding capability never costs baseline speed or data safety. Sources: https://www.theregister.com/2021/01/28/erich_gamma_on_vs_code/ ; https://www.raycast.com/blog/how-raycast-api-extensions-work
- **ntpacker:** when custom exporters/scripts arrive (Lua like Aseprite is the natural fit), sandbox them so a bad script can never corrupt the project file or block the UI thread; script-provided commands register into the same palette/registry as native ones.

---

## Validation notes
- Blender 2.8 is the outcome-proven case: the UX overhaul is directly credited by adopting studios (Ubisoft Animation Studio joined the Dev Fund citing "a revamped UX"; Khara announced migration) and downloads nearly doubled to >10M in 2019 (blender.org official press). A UX-led repositioning of an expert tool measurably changes adoption.
- Numbers to design against: 100ms (feels instant — Nielsen/Superhuman), 400ms (Doherty productivity inflection), 1s (flow break; also Figma's minimum before showing a spinner), ≤2 disclosure levels (NN/g).
- Dropped/uncertain claims: Superhuman's 100ms is attributed to Paul Buchheit via Superhuman's own blog (not the game-design essay); the "47ms vs Jira" comparison circulating online traces to an unreliable source and was excluded; Aseprite's "own UI is pixel art" coherence point lacked a primary Capello quote and is used only as background.
