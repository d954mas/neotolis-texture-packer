# Bench assets — attribution

These sprites drive the **U-01 owner-scale bench fixture** (`docs/plans/master-spec-implementation-plan.md` §U-01).
They are real game assets — not synthetic — so decode, hashing, packing, thumbnail,
and cache behaviour are measured against realistic pixel data and a realistic size
distribution (16 px tiles → 1024 px backgrounds).

## Source & license

All packs are by **Kenney** (<https://kenney.nl>) and released under
**Creative Commons CC0 1.0 (public domain)** — no attribution required, but credited
here as good practice. CC0: <https://creativecommons.org/publicdomain/zero/1.0/>.

| Folder | Pack | Role in the fixture | Download (CC0) |
|---|---|---|---|
| `kenney/tiny-dungeon/` | Tiny Dungeon | 16 px tiles — many tiny sprites | <https://kenney.nl/assets/tiny-dungeon> |
| `kenney/ui-pack/` | UI Pack | UI buttons/panels — varied mid sizes | <https://kenney.nl/assets/ui-pack> |
| `kenney/platformer-art-deluxe/` | Platformer Art Deluxe | characters/tiles/items — broad size range | <https://kenney.nl/assets/platformer-art-deluxe> |
| `kenney/prototype-textures/` | Prototype Textures | up to 1024×1024 — large images / multi-page atlases | <https://kenney.nl/assets/prototype-textures> |

## Curation

Only individual sprite PNGs are kept. Excluded from every pack: pre-baked
spritesheets/tilesheets, `Vector/` (SVG sources), `Font/`, `Sounds/`, and the
`preview.png` / `sample.png` / `*_packed.png` / `tilemap.png` marketing images.

Result: **2020 distinct PNGs, ~3.00 MB**, stored in Git LFS. Every file is recorded
in `manifest.tsv` (relpath · width · height · bytes · sha256), sorted and LF-terminated,
which is the deterministic input the fixture generator reads. Regenerate the manifest
only after an intentional asset change; a mismatch fails the bench-project contract test.

Do not hand-edit files here. To refresh, re-download the packs above, re-run the
curation, and re-commit the manifest.
