#!/usr/bin/env python3
"""Convert the PPM files uipreview writes into PNGs, for looking at them.

    python3 tooling/uipreview/ppm2png.py out

Needs Pillow (python3 -m pip install pillow).
"""
import glob
import os
import sys

from PIL import Image


def main() -> int:
    folder = sys.argv[1] if len(sys.argv) > 1 else "out"
    files = sorted(glob.glob(os.path.join(folder, "*.ppm")))
    if not files:
        print(f"no .ppm files in {folder} - run ./uipreview {folder} first")
        return 1

    for path in files:
        png = path[:-4] + ".png"
        Image.open(path).save(png)
        print(png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
