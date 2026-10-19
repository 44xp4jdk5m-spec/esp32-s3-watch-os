"""Render each glyph separately, upscaled, to inspect for strip artifacts."""
import re, sys
from PIL import Image
sys.path.insert(0, r"d:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\watch\font")
from _render_font import parse_font, render_glyph

path = r"d:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\watch\main\app\fonts\noto_font_24.c"
bitmap, dsc, fbpp, fstride, cmap = parse_font(path)
print(f"bpp={fbpp} stride={fstride}")

chars = "ilapsagp中あH0mw1"
SCALE = 8
composite = Image.new("L", (len(chars)*40*SCALE, 40*SCALE), 255)
for ci, ch in enumerate(chars):
    cp = ord(ch)
    gid = cmap.get(cp, 0)
    if gid == 0:
        print(f"MISSING {ch}")
        continue
    r = render_glyph(bitmap, dsc, fbpp, fstride, gid)
    data, bw, bh, ox, oy, adv = r
    g = Image.new("L", (bw, bh), 0)
    g.putdata(data)
    # upscale
    g2 = g.resize((bw*SCALE, bh*SCALE), Image.NEAREST)
    # place on composite with baseline at bottom
    x0 = ci*40*SCALE + 4*SCALE
    y0 = 30*SCALE + oy*SCALE
    composite.paste(g2, (x0, y0))
    # print glyph dims
    print(f"{ch} U+{cp:04X} gid={gid} bw={bw} bh={bh} ox={ox} oy={oy} adv={adv}")

composite.save(r"d:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\watch\font\_glyphs.png")
print("saved _glyphs.png")
