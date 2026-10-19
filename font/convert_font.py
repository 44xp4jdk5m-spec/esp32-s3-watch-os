#!/usr/bin/env python3
"""
convert_font.py — Convert Noto + Material Symbols fonts to LVGL v9 C arrays.

Generates:
  main/app/fonts/noto_font_14.{c,h}   — Noto Sans CJK (EN/JP/TC), 14px
  main/app/fonts/noto_font_24.{c,h}   — Noto Sans CJK (EN/JP/TC), 24px
  main/app/fonts/noto_font_48.{c,h}   — Noto Sans CJK (EN/JP/TC), 48px
  main/app/fonts/mi_font_24.{c,h}     — Material Symbols Outlined icons, 24px
  main/app/fonts/fonts.h              — convenience include

Requirements: pip install fonttools pillow

Usage:
  python convert_font.py
"""

import os
import sys
import glob
import json
import re

from PIL import Image, ImageDraw, ImageFont
from fontTools import ttLib
from fontTools.varLib.instancer import instantiateVariableFont

# ── Paths ─────────────────────────────────────────────────────────────
HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = HERE
LANG_DIR = os.path.join(HERE, "..", "spiffs", "language")
OUT_DIR = os.path.join(HERE, "..", "main", "app", "fonts")

NOTO_JP   = os.path.join(FONT_DIR, "NotoSansJP-VF.ttf")
NOTO_TC   = os.path.join(FONT_DIR, "NotoSansTC-VF.ttf")
NOTO_LAT  = os.path.join(FONT_DIR, "NotoSans-VF.ttf")
ICON_FONT = os.path.join(FONT_DIR, "MaterialSymbolsOutlined-VF.ttf")

TEXT_SIZES = [14, 24, 48]
ICON_SIZE = 24

# Bits per pixel. 2 = 4 grey levels (small display, ~half the flash size).
# 4 = 16 grey levels (smoother). The LVGL plain-font reader keeps the
# nibble counter aligned across rows only when box_w is a multiple of
# (8 // BPP), so the converter pads box_w up to that alignment.
BPP = 2
_ALIGN = 8 // BPP   # pixels per byte; box_w is padded to a multiple of this

# ── Icon codepoints used by settings_ui.c (Material Symbols Outlined) ─
ICON_CODEPOINTS = [
    0xE63E, 0xE1A7, 0xE1A3, 0xE7F4, 0xE050, 0xE04D, 0xE897, 0xE896,
    0xE8B8, 0xE5C4, 0xE5C8, 0xE15B, 0xE145, 0xE5CA, 0xE002, 0xE5CD,
    0xE04F, 0xE8F4, 0xE0C8, 0xE518, 0xE1DB, 0xE894, 0xE88E, 0xEA4B,
    0xE868, 0xE8AC, 0xEB97, 0xE5C3, 0xE9B0, 0xE8B5, 0xE1C2, 0xE2BD,
    0xE1E0, 0xEB31, 0xE5D8, 0xE5DB, 0xE034, 0xE7F7, 0xE1EB, 0xE88A,
    # Control-center glyphs (airplane, flashlight, do-not-disturb moon)
    0xE195, 0xF00B, 0xE51C,
]

# ── Character collection ──────────────────────────────────────────────

def _walk(obj, chars):
    if isinstance(obj, str):
        chars.update(obj)
    elif isinstance(obj, dict):
        for v in obj.values():
            _walk(v, chars)
    elif isinstance(obj, list):
        for v in obj:
            _walk(v, chars)

def collect_text_chars():
    """Collect all chars from language JSONs + ASCII printable range."""
    chars = set(chr(c) for c in range(0x20, 0x7F))  # ASCII printable

    # Calculator operator glyphs — written as hex escapes in
    # system_app/calculator/calculator_ui.c, so the string-literal scanner
    # below cannot decode them; add the codepoints explicitly.
    #   U+00D7 ×  U+00F7 ÷  U+2212 −  U+232B ⌫
    chars.update(chr(cp) for cp in (0x00D7, 0x00F7, 0x2212, 0x232B))

    for path in glob.glob(os.path.join(LANG_DIR, "*.json")):
        with open(path, encoding="utf-8") as f:
            _walk(json.load(f), chars)
    # Also scan source string literals for hardcoded UI text
    for pat in [
        os.path.join(HERE, "..", "main", "system_app", "settings", "*.c"),
        os.path.join(HERE, "..", "main", "system_app", "calculator", "*.c"),
        os.path.join(HERE, "..", "main", "app", "list_view_menu.c"),
        os.path.join(HERE, "..", "main", "app", "honeycomb_menu.c"),
    ]:
        for p in glob.glob(pat):
            try:
                with open(p, encoding="utf-8", errors="ignore") as f:
                    txt = f.read()
            except Exception:
                continue
            for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', txt):
                s = m.group(1)
                try:
                    s = bytes(s, "utf-8").decode("unicode_escape", errors="ignore")
                except Exception:
                    pass
                chars.update(s)
    # keep only BMP chars
    return sorted(c for c in chars if ord(c) < 0x10000)

# ── Font instantiation (variable → static) ────────────────────────────

_inst_cache = {}

def instantiate(path, coords):
    key = (path, tuple(sorted(coords.items())))
    if key in _inst_cache:
        return _inst_cache[key]
    out_path = os.path.join(FONT_DIR, "_static_" + os.path.splitext(os.path.basename(path))[0] +
                            "_" + "_".join(f"{k}{v}" for k, v in coords.items()) + ".ttf")
    if not os.path.exists(out_path):
        print(f"  Instantiating variable font: {os.path.basename(path)} {coords} ...")
        f = ttLib.TTFont(path)
        instantiateVariableFont(f, coords, inplace=True, updateFontNames=True)
        f.save(out_path)
    _inst_cache[key] = out_path
    return out_path

def load_fonts():
    """Prepare static TTFs and cache their fontTools objects."""
    fonts = {}
    fonts["jp"]  = instantiate(NOTO_JP,  {"wght": 400})
    fonts["tc"]  = instantiate(NOTO_TC,  {"wght": 400})
    fonts["lat"] = instantiate(NOTO_LAT, {"wght": 400, "wdth": 100})
    fonts["icon"] = instantiate(ICON_FONT, {"FILL": 0, "GRAD": 0, "opsz": 24, "wght": 400})
    return fonts

def has_glyph(ttf_path, ch):
    """Check if a static TTF contains the char (using fontTools cmap)."""
    font = ttLib.TTFont(ttf_path, lazy=True)
    cmap = font.getBestCmap()
    has = ord(ch) in cmap
    font.close() if hasattr(font, "close") else None
    return has

_glyph_cache_fonts = {}

def rasterize(ttf_path, ch, size):
    """Rasterize one char via Pillow. Returns (bitmap_rows, ofs_x, ofs_y, box_w, box_h, adv_w)."""
    key = (ttf_path, size)
    if key not in _glyph_cache_fonts:
        _glyph_cache_fonts[key] = ImageFont.truetype(ttf_path, size)
    font = _glyph_cache_fonts[key]

    ascent, descent = font.getmetrics()
    pad = size * 2
    W = size * 4
    H = size * 4
    img = Image.new("L", (W, H), 0)
    draw = ImageDraw.Draw(img)
    try:
        draw.text((pad, pad), ch, font=font, fill=255, anchor="la")
    except Exception:
        return None
    bbox = img.getbbox()
    if bbox is None:
        # empty glyph (space)
        adv_w = round(font.getlength(ch) * 16)
        return [], 0, 0, 0, 0, adv_w

    l, t, r, b = bbox
    ofs_x = l - pad
    # baseline image y = pad + ascent ; ofs_y = -(ink bottom - baseline)
    ofs_y = pad + ascent - b
    box_w = r - l
    box_h = b - t
    adv_w = round(font.getlength(ch) * 16)

    crop = img.crop((l, t, r, b))
    return crop, ofs_x, ofs_y, box_w, box_h, adv_w

def align_crop(crop, box_w):
    """Pad the crop's right side with transparent columns so box_w becomes
    a multiple of _ALIGN (pixels per byte). The LVGL plain-font reader keeps
    its nibble counter aligned across rows only when box_w is a multiple of
    8//bpp — without this, odd-width glyphs render with a strip."""
    pad = (-box_w) % _ALIGN
    if pad == 0:
        return crop, box_w
    new_w = box_w + pad
    padded = Image.new("L", (new_w, crop.height), 0)
    padded.paste(crop, (0, 0))
    return padded, new_w

# ── LVGL output ───────────────────────────────────────────────────────

def pick_text_source(ch):
    cp = ord(ch)
    if 0x3040 <= cp <= 0x30FF or 0x31F0 <= cp <= 0x31FF:
        return "jp"          # Kana → Japanese
    if 0x4E00 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF:
        return "tc"          # CJK ideographs → Traditional Chinese forms
    return "lat"             # Latin / ASCII → Noto Sans

def pack_bitmap(crop, box_w, bpp=BPP):
    """Convert an L-mode crop into bpp-bit packed bytes (stride=0, continuous rows).

    Pixels are packed most-significant-first, `_ALIGN` pixels per byte.
    box_w is expected to already be a multiple of `_ALIGN` so the LVGL
    reader's row counter stays aligned across rows.
    """
    px = crop.load()
    per_byte = 8 // bpp
    mask = (1 << bpp) - 1
    out = bytearray()
    for y in range(crop.height):
        row = bytearray()
        for x in range(box_w):
            v = px[x, y]
            # scale 8-bit alpha -> bpp-bit value (0..mask)
            nib = (v * mask + 127) >> 8
            if nib > mask:
                nib = mask
            slot = x % per_byte
            shift = 8 - bpp * (slot + 1)
            if slot == 0:
                row.append(nib << shift)
            else:
                row[-1] |= nib << shift
        out += row
    return bytes(out)

def c_array_bytes(data, per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk))
    return ",\n".join(lines)

def generate_text_font(static_fonts, size):
    chars = collect_text_chars()
    name = f"noto_font_{size}"

    # For metrics (line height / baseline) use the Latin font (or TC)
    font_ref = ImageFont.truetype(static_fonts["lat"], size)
    ascent, descent = font_ref.getmetrics()
    line_height = ascent + descent
    base_line = descent

    bitmaps = bytearray()
    glyph_dsc = []
    unicode_list = []

    # glyph 0 reserved
    glyph_dsc.append((0, 0, 0, 0, 0, 0))

    for ch in chars:
        src = pick_text_source(ch)
        ttf = static_fonts[src]
        # fallback if char missing
        if not has_glyph(ttf, ch):
            for alt in ("tc", "jp", "lat"):
                if has_glyph(static_fonts[alt], ch):
                    ttf = static_fonts[alt]
                    break
            else:
                continue
        res = rasterize(ttf, ch, size)
        if res is None:
            continue
        crop, ofs_x, ofs_y, box_w, box_h, adv_w = res
        if box_w == 0 or box_h == 0:
            glyph_dsc.append((len(bitmaps), adv_w, 0, 0, ofs_x, ofs_y))
            unicode_list.append(ord(ch))
            continue
        crop, box_w = align_crop(crop, box_w)
        data = pack_bitmap(crop, box_w, BPP)
        idx = len(bitmaps)
        bitmaps += data
        glyph_dsc.append((idx, adv_w, box_w, box_h, ofs_x, ofs_y))
        unicode_list.append(ord(ch))

    write_font_files(name, size, bitmaps, glyph_dsc, unicode_list, line_height, base_line)

def generate_icon_font(static_fonts, size):
    name = f"mi_font_{size}"
    ttf = static_fonts["icon"]
    font_ref = ImageFont.truetype(ttf, size)
    ascent, descent = font_ref.getmetrics()
    line_height = ascent + descent
    base_line = descent

    bitmaps = bytearray()
    glyph_dsc = []
    unicode_list = []
    glyph_dsc.append((0, 0, 0, 0, 0, 0))

    # CRITICAL: the sparse cmap uses binary search, so codepoints MUST be
    # in ascending order or glyphs will not be found (fallback to default).
    for cp in sorted(ICON_CODEPOINTS):
        ch = chr(cp)
        if not has_glyph(ttf, ch):
            print(f"  WARN: icon U+{cp:04X} missing in Material Symbols")
            continue
        res = rasterize(ttf, ch, size)
        if res is None:
            continue
        crop, ofs_x, ofs_y, box_w, box_h, adv_w = res
        if box_w == 0 or box_h == 0:
            glyph_dsc.append((len(bitmaps), adv_w, 0, 0, ofs_x, ofs_y))
            unicode_list.append(cp)
            continue
        crop, box_w = align_crop(crop, box_w)
        data = pack_bitmap(crop, box_w, BPP)
        idx = len(bitmaps)
        bitmaps += data
        glyph_dsc.append((idx, adv_w, box_w, box_h, ofs_x, ofs_y))
        unicode_list.append(cp)

    write_font_files(name, size, bitmaps, glyph_dsc, unicode_list, line_height, base_line)

def write_font_files(name, size, bitmaps, glyph_dsc, unicode_list, line_height, base_line):
    os.makedirs(OUT_DIR, exist_ok=True)
    c_path = os.path.join(OUT_DIR, name + ".c")
    h_path = os.path.join(OUT_DIR, name + ".h")

    header = f"""/* Auto-generated by convert_font.py — DO NOT EDIT */
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

extern const lv_font_t {name};

#ifdef __cplusplus
}}
#endif
"""
    with open(h_path, "w", encoding="utf-8") as f:
        f.write(header)

    # glyph dsc lines
    dsc_lines = []
    for idx, (bi, adv, bw, bh, ox, oy) in enumerate(glyph_dsc):
        dsc_lines.append(
            "    {{ .bitmap_index = {bi}, .adv_w = {adv}, .box_w = {bw}, .box_h = {bh}, .ofs_x = {ox}, .ofs_y = {oy} }}, /* U+{cp:04X} */".format(
                bi=bi, adv=adv, bw=bw, bh=bh, ox=ox, oy=oy, cp=unicode_list[idx-1] if idx > 0 else 0))

    # unicode list for sparse cmap
    ul_lines = []
    for i in range(0, len(unicode_list), 16):
        chunk = unicode_list[i:i + 16]
        ul_lines.append("    " + ", ".join("0x%04x" % c for c in chunk) + ",")

    c = f"""/* Auto-generated by convert_font.py — DO NOT EDIT */
/* Font: {name} ({size}px, bpp={BPP}, LVGL v9 plain) */

#include "{name}.h"

/* Glyph bitmaps ({len(bitmaps)} bytes) */
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
{c_array_bytes(bytes(bitmaps))}
}};

/* Glyph descriptors */
static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{chr(10).join(dsc_lines)}
}};

/* Unicode map (sparse, range 0x0000-0xFFFF) */
static const uint16_t unicode_list[] = {{
{chr(10).join(ul_lines)}
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{ .range_start = 0, .range_length = 0xFFFF, .glyph_id_start = 1,
      .unicode_list = unicode_list, .glyph_id_ofs_list = NULL,
      .list_length = {len(unicode_list)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY }},
}};

/* Font descriptor */
static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = {BPP},
    .kern_classes = 0,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
    .stride = 0,
}};

/* Font object */
const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
    .subpx = LV_FONT_SUBPX_NONE,
    .dsc = &font_dsc,
    .fallback = NULL,
}};
"""
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(c)

    print(f"  wrote {name}: {len(glyph_dsc)-1} glyphs, {len(bitmaps)} bytes bitmap, "
          f"line_height={line_height}, base_line={base_line}")

def write_fonts_header():
    os.makedirs(OUT_DIR, exist_ok=True)
    names = [f"noto_font_{s}" for s in TEXT_SIZES] + [f"mi_font_{ICON_SIZE}"]
    with open(os.path.join(OUT_DIR, "fonts.h"), "w", encoding="utf-8") as f:
        f.write("""/* Auto-generated by convert_font.py — DO NOT EDIT */
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

""")
        for n in names:
            f.write(f'#include "{n}.h"\n')
        f.write("""
/* Shorthand: use these fonts in your UI.
 * noto_font_XX supports English / Japanese / Traditional Chinese.
 * mi_font_24 is the Material Symbols icon font. */

#ifdef __cplusplus
}
#endif
""")

if __name__ == "__main__":
    print("== convert_font.py ==")
    print("Collecting text characters ...")
    chars = collect_text_chars()
    print(f"  {len(chars)} unique text codepoints")
    print("Instantiating variable fonts ...")
    fonts = load_fonts()
    print("Generating text fonts ...")
    for size in TEXT_SIZES:
        generate_text_font(fonts, size)
    print("Generating icon font ...")
    generate_icon_font(fonts, ICON_SIZE)
    write_fonts_header()
    print("Done.")
