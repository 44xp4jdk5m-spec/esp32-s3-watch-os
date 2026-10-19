#!/usr/bin/env python3
"""Generate noto_font_48_bold.{c,h} — Noto Sans CJK (EN/JP/TC) 48px,
weight 700, width 100 — reusing convert_font.py's pipeline.

Output: components/os/fonts/noto_font_48_bold.{c,h}
Run with the project venv (has fonttools + Pillow):
  .venv\\Scripts\\python.exe font\\make_bold_48.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = HERE
sys.path.insert(0, FONT_DIR)

import convert_font as cf

OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "components", "os", "fonts"))
cf.OUT_DIR = OUT_DIR
# the language JSONs the char collector should walk live here now:
cf.LANG_DIR = os.path.normpath(os.path.join(HERE, "..", "spiffs", "usr", "share", "lang"))

NAME = "noto_font_48_bold"
SIZE = 48
WEIGHT = 700


def gen_bold():
    print("Instantiating wght=%d fonts ..." % WEIGHT)
    fonts = {}
    fonts["jp"] = cf.instantiate(cf.NOTO_JP, {"wght": WEIGHT})
    fonts["tc"] = cf.instantiate(cf.NOTO_TC, {"wght": WEIGHT})
    fonts["lat"] = cf.instantiate(cf.NOTO_LAT, {"wght": WEIGHT, "wdth": 100})

    print("Collecting characters ...")
    chars = cf.collect_text_chars()

    from PIL import ImageFont
    font_ref = ImageFont.truetype(fonts["lat"], SIZE)
    ascent, descent = font_ref.getmetrics()
    line_height = ascent + descent
    base_line = descent

    bitmaps = bytearray()
    glyph_dsc = []
    unicode_list = []
    glyph_dsc.append((0, 0, 0, 0, 0, 0))   # reserved glyph 0

    for ch in chars:
        src = cf.pick_text_source(ch)
        ttf = fonts[src]
        if not cf.has_glyph(ttf, ch):
            for alt in ("tc", "jp", "lat"):
                if cf.has_glyph(fonts[alt], ch):
                    ttf = fonts[alt]
                    break
            else:
                continue
        res = cf.rasterize(ttf, ch, SIZE)
        if res is None:
            continue
        crop, ofs_x, ofs_y, box_w, box_h, adv_w = res
        if box_w == 0 or box_h == 0:
            glyph_dsc.append((len(bitmaps), adv_w, 0, 0, ofs_x, ofs_y))
            unicode_list.append(ord(ch))
            continue
        crop, box_w = cf.align_crop(crop, box_w)
        data = cf.pack_bitmap(crop, box_w, cf.BPP)
        idx = len(bitmaps)
        bitmaps += data
        glyph_dsc.append((idx, adv_w, box_w, box_h, ofs_x, ofs_y))
        unicode_list.append(ord(ch))

    os.makedirs(OUT_DIR, exist_ok=True)
    cf.write_font_files(NAME, SIZE, bitmaps, glyph_dsc, unicode_list,
                        line_height, base_line)
    print("Wrote %d glyphs -> %s/%s.{c,h}" % (len(unicode_list), OUT_DIR, NAME))


if __name__ == "__main__":
    gen_bold()
