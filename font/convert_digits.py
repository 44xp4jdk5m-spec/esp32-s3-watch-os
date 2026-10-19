#!/usr/bin/env python3
"""
convert_digits.py — Render Stadium font digits 0-9 as LVGL A8 images
(solid, 1-bit, no anti-aliasing) for the Flux watch face.

Font: Stadium (squarish display face) from the user's Stadium SquareGear pack.

Usage: python convert_digits.py
Requires: pip install pillow
"""

import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
# Stadium font — user-provided squarish display font.
FONT_SRC = r"D:\download\stadium_squaregear\stadium.ttf"
OUT_DIR = os.path.join(HERE, "..", "main", "watch_faces")
OUT_H = os.path.join(OUT_DIR, "flux_digits.h")
OUT_C = os.path.join(OUT_DIR, "flux_digits.c")

# Font size chosen so the digit ink bbox matches the largest digit
# container height (minutex = 263).  Keeps the biggest digit at ~1:1
# scale so nearest-neighbour stretching stays crisp.
PIXEL_HEIGHT = 360

DIGITS = "0123456789"


def static_font():
    return FONT_SRC


def render_digit(ch, ttf):
    font = ImageFont.truetype(ttf, PIXEL_HEIGHT)
    # Draw on a 1-bit image: binary glyph, no anti-aliasing (solid edges)
    pad = PIXEL_HEIGHT // 2
    W = PIXEL_HEIGHT * 2
    H = PIXEL_HEIGHT * 2
    img = Image.new("1", (W, H), 0)
    d = ImageDraw.Draw(img)
    d.text((pad, pad), ch, font=font, fill=1)
    bbox = img.getbbox()
    if bbox is None:
        raise RuntimeError("empty glyph for %r" % ch)
    return img.crop(bbox)


def pack_a8(img):
    w, h = img.size
    px = img.load()
    data = bytearray()
    for y in range(h):
        for x in range(w):
            # mode "1" stores pixels as 0/1 (not 0/255), so treat any
            # non-zero pixel as opaque (solid, no anti-aliasing)
            data.append(0xFF if px[x, y] else 0x00)
    return w, h, bytes(data)


def c_hex(data, per=16):
    lines = []
    for i in range(0, len(data), per):
        chunk = data[i:i + per]
        lines.append("    " + ", ".join("0x%02X" % b for b in chunk) + ",")
    return "\n".join(lines)


def main():
    ttf = static_font()

    h_lines = [
        "#pragma once",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    c_lines = ['#include "flux_digits.h"', ""]

    for ch in DIGITS:
        img = render_digit(ch, ttf)
        w, h, data = pack_a8(img)
        name = "flux_digit_%c" % ch
        c_lines.append("/* digit '%s' %dx%d */" % (ch, w, h))
        c_lines.append("static const uint8_t %s_data[] = {" % name)
        c_lines.append(c_hex(data))
        c_lines.append("};")
        c_lines.append("")
        c_lines.append("const lv_image_dsc_t %s = {" % name)
        c_lines.append("    .header.w = %d," % w)
        c_lines.append("    .header.h = %d," % h)
        c_lines.append("    .data_size = sizeof(%s_data)," % name)
        c_lines.append("    .header.cf = LV_COLOR_FORMAT_A8,")
        c_lines.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
        c_lines.append("    .data = %s_data," % name)
        c_lines.append("};")
        c_lines.append("")
        h_lines.append("extern const lv_image_dsc_t %s;" % name)

    h_lines += ["", "#ifdef __cplusplus", "}", "#endif", ""]

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_H, "w") as f:
        f.write("\n".join(h_lines))
    with open(OUT_C, "w") as f:
        f.write("\n".join(c_lines))

    print("Wrote %s and %s" % (OUT_H, OUT_C))


if __name__ == "__main__":
    main()
