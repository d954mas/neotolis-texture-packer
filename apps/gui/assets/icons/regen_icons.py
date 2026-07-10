#!/usr/bin/env python3
"""Regenerate the ntpacker GUI icon set from upstream Lucide SVGs.

What this does
--------------
1. Downloads each icon's SVG source verbatim from the Lucide GitHub repo
   (raw.githubusercontent.com/lucide-icons/lucide/main/icons/<name>.svg)
   and saves it unmodified as ``<name>.svg`` next to this script. These are
   the vendored upstream sources — kept as-is (still using
   ``stroke="currentColor"``) so the license/provenance trail stays honest.
2. For rasterization only, takes the SVG text in memory and forces
   ``stroke="currentColor"`` / ``fill="currentColor"`` to ``#FFFFFF`` — the
   PNGs are monochrome white-on-transparent tint masks, not colored icons.
3. Rasterizes the white-forced SVG to a 48x48 RGBA PNG (the Lucide 24px grid
   at 2x, so the 2px stroke becomes 4 physical px). ``folder-plus`` is
   additionally rasterized at 96x96 as ``folder-plus-hero.png`` for the
   empty-state hero graphic.
4. Post-processes every PNG so alpha stays straight (non-premultiplied) and
   every pixel with alpha > 0 has RGB pinned to pure white (255,255,255) —
   some rasterizers (and always PNG re-encoders) can leave slightly-grey
   anti-aliased edges; this step corrects that so the PNGs are safe to use
   as tint masks (multiply by any UI color).
5. Verifies size, mode, non-empty alpha, and colorlessness for every PNG.

Tool dependency (rasterizer)
-----------------------------
This script shells out to the `resvg` CLI to rasterize SVG -> PNG. Tried, in
the order recommended by the vendoring task, on a Windows dev box with none
of ImageMagick / rsvg-convert / cairosvg's native cairo lib installed:

  a. `magick` (ImageMagick)        -> not installed, skipped.
  b. `rsvg-convert`                -> not installed, skipped.
  c. `pip install cairosvg`        -> installs fine, but importing it fails
                                      at runtime with "no library called
                                      cairo-2/cairo/libcairo-2 was found"
                                      (cairosvg needs the native libcairo
                                      DLL via cairocffi; Windows has no
                                      system cairo by default). Skipped.
  d. `resvg` prebuilt binary       -> WORKED. Used for this vendoring pass.

resvg is NOT vendored into the repo (per task instructions — binaries must
not land in the repo). To re-run this script you need a `resvg` executable
available; get one for your OS from:

    https://github.com/linebender/resvg/releases   (used: v0.47.0, resvg-win64.zip)

Then either put `resvg`/`resvg.exe` on PATH, or point this script at it:

    python regen_icons.py --resvg "C:/path/to/resvg.exe"
    # or
    RESVG_BIN="/path/to/resvg" python regen_icons.py

Requires: Python 3.8+ stdlib (urllib) for fetching, and Pillow
(`pip install pillow`) for the PNG post-processing / verification step.

Usage
-----
    python regen_icons.py            # fetch + rasterize + verify everything
    python regen_icons.py --no-fetch # reuse the .svg files already on disk
                                      # (skip re-downloading from GitHub)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RAW_BASE = "https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/{}.svg"

# (name to save as, alias to try on 404). Alias is only used as a fallback;
# every one of these resolved via its primary name as of the vendoring date
# below, so no aliases were actually needed this run — kept here so a future
# re-run survives an upstream rename.
ICONS = [
    ("layout-grid", None),
    ("triangle-alert", "alert-triangle"),
    ("download", None),
    ("refresh-cw", None),
    ("chevron-left", None),
    ("chevron-right", None),
    ("chevron-down", None),
    ("minus", None),
    ("plus", None),
    ("scan", None),
    ("maximize-2", None),
    ("layers", None),
    ("folder", None),
    ("folder-open", None),
    ("image", None),
    ("film", None),
    ("file-plus", None),
    ("folder-plus", None),
    ("x", None),
    ("search", None),
    ("check", None),
    ("link", None),
    ("external-link", None),
    ("square-dashed", "box-select"),
    ("crop", None),
    ("crosshair", None),
    ("info", None),
    ("circle-check", "check-circle"),
    ("octagon-alert", "alert-octagon"),
    ("save", None),
    ("undo-2", None),
    ("redo-2", None),
]

HERO_ICON = "folder-plus"
HERO_SIZE = 96
BASE_SIZE = 48


def fetch_svg(name, alias):
    """Return (used_name, svg_text) fetched verbatim from upstream Lucide."""
    for candidate in (name, alias):
        if not candidate:
            continue
        url = RAW_BASE.format(candidate)
        try:
            with urllib.request.urlopen(url, timeout=15) as resp:
                if resp.status == 200:
                    return candidate, resp.read().decode("utf-8")
        except Exception:
            continue
    raise RuntimeError(f"could not fetch '{name}' (alias '{alias}') from upstream")


def force_white(svg_text):
    """Replace currentColor stroke/fill with pure white for rasterization."""
    out = svg_text.replace('stroke="currentColor"', 'stroke="#FFFFFF"')
    out = out.replace('fill="currentColor"', 'fill="#FFFFFF"')
    return out


def find_resvg(explicit):
    if explicit:
        return explicit
    env = os.environ.get("RESVG_BIN")
    if env:
        return env
    found = shutil.which("resvg") or shutil.which("resvg.exe")
    if found:
        return found
    raise RuntimeError(
        "no resvg binary found; pass --resvg <path> or set RESVG_BIN "
        "(see the tool-dependency note at the top of this script)"
    )


def rasterize(resvg_bin, svg_text, out_png, size):
    with tempfile.NamedTemporaryFile(
        suffix=".svg", mode="w", encoding="utf-8", delete=False
    ) as tmp:
        tmp.write(svg_text)
        tmp_path = tmp.name
    try:
        cmd = [
            resvg_bin,
            "--width", str(size),
            "--height", str(size),
            tmp_path,
            out_png,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"resvg failed for {out_png}: {result.stdout}\n{result.stderr}"
            )
    finally:
        os.unlink(tmp_path)


def force_straight_white_alpha(png_path):
    """Pin RGB to pure white on every pixel with alpha > 0, straight alpha.

    Guards against rasterizers/re-encoders that leave premultiplied-looking
    grey anti-aliased edges. Safe to run even when RGB is already white.
    """
    img = Image.open(png_path).convert("RGBA")
    pixels = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if a > 0:
                pixels[x, y] = (255, 255, 255, a)
            else:
                pixels[x, y] = (0, 0, 0, 0)
    img.save(png_path)


def verify_png(png_path, expected_size):
    img = Image.open(png_path)
    errors = []
    if img.mode != "RGBA":
        errors.append(f"mode is {img.mode}, expected RGBA")
        img = img.convert("RGBA")
    if img.size != (expected_size, expected_size):
        errors.append(f"size is {img.size}, expected {expected_size}x{expected_size}")
    pixels = img.load()
    w, h = img.size
    has_alpha = False
    colored = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if a > 0:
                has_alpha = True
                if (r, g, b) != (255, 255, 255):
                    colored += 1
    if not has_alpha:
        errors.append("alpha channel is entirely empty")
    if colored:
        errors.append(f"{colored} px have alpha>0 but RGB != white")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resvg", help="path to resvg executable", default=None)
    parser.add_argument(
        "--no-fetch",
        action="store_true",
        help="reuse existing <name>.svg files instead of re-downloading",
    )
    args = parser.parse_args()

    resvg_bin = find_resvg(args.resvg)
    print(f"using resvg: {resvg_bin}")

    all_errors = {}
    count = 0

    for name, alias in ICONS:
        svg_path = os.path.join(SCRIPT_DIR, f"{name}.svg")

        if args.no_fetch:
            with open(svg_path, "r", encoding="utf-8") as f:
                svg_text = f.read()
        else:
            used_name, svg_text = fetch_svg(name, alias)
            with open(svg_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(svg_text)
            if used_name != name:
                print(f"  {name}: fetched via alias '{used_name}'")

        white_svg = force_white(svg_text)

        png_path = os.path.join(SCRIPT_DIR, f"{name}.png")
        rasterize(resvg_bin, white_svg, png_path, BASE_SIZE)
        force_straight_white_alpha(png_path)
        errs = verify_png(png_path, BASE_SIZE)
        if errs:
            all_errors[png_path] = errs
        count += 1

        if name == HERO_ICON:
            hero_path = os.path.join(SCRIPT_DIR, f"{HERO_ICON}-hero.png")
            rasterize(resvg_bin, white_svg, hero_path, HERO_SIZE)
            force_straight_white_alpha(hero_path)
            errs = verify_png(hero_path, HERO_SIZE)
            if errs:
                all_errors[hero_path] = errs
            count += 1

    print(f"processed {len(ICONS)} icons -> {count} PNGs")
    if all_errors:
        print("VERIFICATION FAILURES:")
        for path, errs in all_errors.items():
            print(f"  {path}:")
            for e in errs:
                print(f"    - {e}")
        sys.exit(1)
    print("all PNGs verified OK (correct size, RGBA, non-empty alpha, white-only RGB)")


if __name__ == "__main__":
    if Image is None:
        print("Pillow is required: pip install pillow", file=sys.stderr)
        sys.exit(1)
    main()
