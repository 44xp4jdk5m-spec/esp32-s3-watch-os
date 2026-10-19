#!/usr/bin/env python3
"""Regenerate the Material Symbols icon fonts (24px + 60px) after changing
ICON_CODEPOINTS in convert_font.py.

Output: components/os/fonts/mi_font_24.{c,h} and mi_font_60.{c,h}
Run with the project venv (has fonttools + Pillow):
  .venv\\Scripts\\python.exe font\\make_icons.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import convert_font as cf

OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "components", "os", "fonts"))
cf.OUT_DIR = OUT_DIR


def gen():
    print("Preparing fonts (variable -> static)...")
    fonts = cf.load_fonts()
    for size in (24, 60):
        print("Generating %dpx icon font..." % size)
        cf.generate_icon_font(fonts, size)
        print("Wrote %s/mi_font_%d.{c,h}" % (OUT_DIR, size))


if __name__ == "__main__":
    gen()
