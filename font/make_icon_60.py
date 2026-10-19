#!/usr/bin/env python3
"""Generate mi_font_60.{c,h} — Material Symbols Outlined icons at 60px,
reusing convert_font.py's icon pipeline (same codepoints as mi_font_24).

Output: components/os/fonts/mi_font_60.{c,h}
Run with the project venv (has fonttools + Pillow):
  .venv\\Scripts\\python.exe font\\make_icon_60.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import convert_font as cf

OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "components", "os", "fonts"))
cf.OUT_DIR = OUT_DIR

SIZE = 60


def gen():
    print("Preparing fonts (variable -> static)...")
    fonts = cf.load_fonts()
    print("Generating %dpx icon font..." % SIZE)
    cf.generate_icon_font(fonts, SIZE)
    print("Wrote %s/mi_font_%d.{c,h}" % (OUT_DIR, SIZE))


if __name__ == "__main__":
    gen()
