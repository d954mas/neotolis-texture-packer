# Icon attribution

The `.svg` files in this directory are vendored, unmodified, from the
[Lucide](https://lucide.dev/) icon set. The `.png` files are rasterized,
monochrome-white derivatives generated from those sources by
`regen_icons.py` for use as tint masks in the ntpacker GUI.

- Upstream repository: https://github.com/lucide-icons/lucide
- Vendored from: `main` branch, commit `078a0b23397ef579d48e860af8f26af62996006e`
  (2026-07-10)
- Corresponding published package version at time of vendoring:
  `lucide-static@1.24.0` (per https://registry.npmjs.org/lucide-static/latest;
  the repo's own `packages/lucide/package.json` shows an unreleased `0.0.1`
  placeholder on `main`, so the npm registry version is the more meaningful
  reference point)

## License

Lucide is published under the ISC License:

```
ISC License

Copyright (c) 2026 Lucide Icons and Contributors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

A subset of Lucide icons are additionally derived from the
[Feather](https://github.com/feathericons/feather) icon set and carry an
extra MIT notice in upstream's `LICENSE` file. Several icons vendored here
fall in that dual-licensed set (`check`, `chevron-down`, `chevron-left`,
`chevron-right`, `crosshair`, `download`, `external-link`, `info`, `link`,
`minus`, `search`, `x`); both the ISC grant above and the Feather-derived MIT
grant below apply to those files:

```
The MIT License (MIT) (for the icons listed above)

Copyright (c) 2013-present Cole Bemis

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Full upstream license file (for reference):
https://raw.githubusercontent.com/lucide-icons/lucide/main/LICENSE

## Vendored icons

All 32 names below resolved directly under their primary upstream name at
vendoring time (no alias fallback was actually needed, though `regen_icons.py`
still carries alias fallbacks for `triangle-alert` -> `alert-triangle`,
`square-dashed` -> `box-select`, `circle-check` -> `check-circle`, and
`octagon-alert` -> `alert-octagon` in case a future re-run hits an upstream
rename):

```
layout-grid, triangle-alert, download, refresh-cw, chevron-left,
chevron-right, chevron-down, minus, plus, scan, maximize-2, layers, folder,
folder-open, image, film, file-plus, folder-plus, x, search, check, link,
external-link, square-dashed, crop, crosshair, info, circle-check,
octagon-alert, save, undo-2, redo-2
```

`folder-plus` is additionally rasterized at 96x96 as `folder-plus-hero.png`
for the empty-state hero graphic.

## Regenerating

See `regen_icons.py` in this directory — it re-downloads the SVGs and
re-rasterizes the PNGs from scratch, and documents its rasterizer dependency
at the top of the file.
