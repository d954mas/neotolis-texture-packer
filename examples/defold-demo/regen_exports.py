#!/usr/bin/env python3
"""Regenerate the demo's ntpacker exports (the *_nt.* files in examples/<name>/).

Runs the `tp_demo_export` driver over defold-demo.ntpacker_project (defold +
json-neotolis targets for every atlas). The Defold exporter now emits the
`.tpatlas` `file:` field as a project-absolute Defold resource path directly (it
walks up for `game.project`, found at this demo's root), so NO post-hoc rewrite
is needed -- the previous shim was removed once tp_export_defold.c was fixed.

Usage:
  python regen_exports.py [path/to/tp_demo_export(.exe)] [work_dir]
The driver path defaults to a search under <repo>/build/; work_dir defaults to
<repo>/build/tp_demo_work (transient, gitignored .ntpack files).
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PROJ = os.path.join(HERE, "defold-demo.ntpacker_project")


def find_driver():
    for name in ("tp_demo_export.exe", "tp_demo_export"):
        hits = glob.glob(os.path.join(REPO, "build", "**", name), recursive=True)
        if hits:
            return hits[0]
    return None


def main():
    drv = sys.argv[1] if len(sys.argv) > 1 else find_driver()
    if not drv or not os.path.exists(drv):
        sys.exit(
            "tp_demo_export driver not found -- build it first:\n"
            "  cmake --preset native-release && "
            "cmake --build --preset native-release --target tp_demo_export"
        )
    work = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "build", "tp_demo_work")
    os.makedirs(work, exist_ok=True)
    print(f"driver:  {drv}")
    print(f"project: {PROJ}")
    subprocess.run([drv, PROJ, work], check=True)
    print("regen_exports: OK")


if __name__ == "__main__":
    main()
