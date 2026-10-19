"""Reconstruct glyphs from a generated LVGL font .c using the EXACT LVGL v9
bpp=4 plain reader algorithm, and render a test string to an image.

Usage: python _render_font.py <font.c> <out.png> <string...>
"""
import re, sys, os
from PIL import Image, ImageDraw

def parse_font(path):
    with open(path, encoding="utf-8") as f:
        txt = f.read()
    # glyph bitmaps
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", txt, re.S)
    body = "".join(re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))
    bitmap = bytes.fromhex(body)
    # glyph dsc
    m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\};", txt, re.S)
    dsc = []
    for mm in re.finditer(r"\{\s*\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}", m.group(1)):
        dsc.append(tuple(int(x) for x in mm.groups()))
    # font dsc (bpp, stride, bitmap_format)
    fbpp = int(re.search(r"\.bpp\s*=\s*(\d+)", txt).group(1))
    m = re.search(r"\.stride\s*=\s*(\d+)", txt)
    fstride = int(m.group(1)) if m else 0
    # cmaps
    m = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\};", txt, re.S)
    cmap = {}
    cm_body = m.group(1)
    for cm in re.finditer(r"\.range_start\s*=\s*(\d+|0x[0-9a-fA-F]+),\s*\.range_length\s*=\s*(\d+|0x[0-9a-fA-F]+),\s*\.glyph_id_start\s*=\s*(\d+).*?\.type\s*=\s*(\w+)", cm_body, re.S):
        def to_int(s):
            return int(s, 16) if s.lower().startswith("0x") else int(s)
        rs, rl, gs, typ = to_int(cm.group(1)), to_int(cm.group(2)), int(cm.group(3)), cm.group(4)
        # find unicode_list for this cmap (assume single sparse)
        ul = None
        m2 = re.search(r"unicode_list\[\]\s*=\s*\{(.*?)\};", txt, re.S)
        if m2:
            ul = [int(x,16) for x in re.findall(r"0x([0-9a-fA-F]{4})", m2.group(1))]
        if typ.endswith("SPARSE_TINY") and ul:
            for ofs, cp in enumerate(ul):
                cmap[cp] = gs + ofs
        else:
            for cp in range(rs, rs+rl):
                cmap[cp] = gs + (cp - rs)
    return bitmap, dsc, fbpp, fstride, cmap

# LVGL opa tables (same as LVGL)
def make_opa4():
    # opa4_table[i] = (i*17) mapped 0..255 (LVGL: (i*17) & 0xFF)
    return [(i * 17) & 0xFF for i in range(16)]

def make_opa2():
    return [0, 85, 170, 255]

OPA4 = make_opa4()
OPA2 = make_opa2()

def render_glyph(bitmap, dsc, fbpp, fstride, gid):
    """Replicate lv_font_get_bitmap_fmt_txt PLAIN for bpp 1/2/4/8 with stride."""
    if gid == 0 or gid >= len(dsc):
        return None
    bi, adv, bw, bh, ox, oy = dsc[gid]
    if bw == 0 or bh == 0:
        return None
    out = bytearray(bw * bh)
    if fstride == 0:
        stride_in = 0
    else:
        stride_in = ((bw + fstride - 1) // fstride) * fstride
    bit_in = bi
    idx = 0
    i = 0
    if fbpp == 4:
        for y in range(bh):
            line_rem = stride_in if stride_in != 0 else bw
            for x in range(bw):
                i = i & 0x1
                if i == 0:
                    out[idx] = OPA4[bitmap[bit_in] >> 4]
                elif i == 1:
                    out[idx] = OPA4[bitmap[bit_in] & 0xF]
                    line_rem -= 1
                    bit_in += 1
                idx += 1
            if stride_in:
                i = 0
                bit_in += line_rem
    elif fbpp == 2:
        for y in range(bh):
            line_rem = stride_in if stride_in != 0 else bw
            for x in range(bw):
                i = i & 0x3
                if i == 0: out[idx] = ((bitmap[bit_in] >> 6) & 0x3) * 85
                elif i == 1: out[idx] = ((bitmap[bit_in] >> 4) & 0x3) * 85
                elif i == 2: out[idx] = ((bitmap[bit_in] >> 2) & 0x3) * 85
                elif i == 3:
                    out[idx] = (bitmap[bit_in] & 0x3) * 85
                    line_rem -= 1; bit_in += 1
                idx += 1
            if stride_in:
                i = 0
                bit_in += line_rem
    elif fbpp == 1:
        for y in range(bh):
            line_rem = stride_in if stride_in != 0 else bw
            for x in range(bw):
                i = i & 0x7
                sh = 7 - i
                if i != 7:
                    out[idx] = 255 if (bitmap[bit_in] >> sh) & 1 else 0
                else:
                    out[idx] = 255 if (bitmap[bit_in] & 1) else 0
                    line_rem -= 1; bit_in += 1
                idx += 1
            if stride_in:
                i = 0
                bit_in += line_rem
    elif fbpp == 8:
        for y in range(bh):
            for x in range(bw):
                out[idx] = bitmap[bit_in]; bit_in += 1; idx += 1
            if stride_in:
                bit_in += line_rem
    return out, bw, bh, ox, oy, adv

def main():
    path = sys.argv[1]
    outpng = sys.argv[2]
    strings = sys.argv[3:]
    bitmap, dsc, fbpp, fstride, cmap = parse_font(path)
    print(f"font={os.path.basename(path)} bpp={fbpp} stride={fstride} glyphs={len(dsc)}")
    # composite
    W, H = 1000, 200
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)
    pen_x = 10
    pen_y = 120  # baseline
    for s in strings:
        for ch in s:
            cp = ord(ch)
            gid = cmap.get(cp, 0)
            if gid == 0:
                print(f"  MISSING U+{cp:04X} '{ch}'")
                pen_x += 12
                continue
            r = render_glyph(bitmap, dsc, fbpp, fstride, gid)
            if r is None:
                pen_x += 8
                continue
            data, bw, bh, ox, oy, adv = r
            g = Image.new("L", (bw, bh), 0)
            g.putdata(data)
            # paste at baseline + oy
            img.paste(g, (pen_x + ox, pen_y + oy), g)
            pen_x += (adv + 15) // 16
    img.save(outpng)
    print(f"wrote {outpng}")

if __name__ == "__main__":
    main()
