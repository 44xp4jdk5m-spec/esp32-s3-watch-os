#!/usr/bin/env python3
"""Prep the 42mm hand PNGs for the analog watch faces (Analog Hands, California).

Each 314x392 source PNG (spiffs/usr/share/watchfaces/hands/*.png) is cropped to
its non-transparent content (+pad), upscaled by HAND_SCALE (410/314 ≈ 1.31) and
written as a raw A8 alpha mask (1 B/px):
  spiffs/usr/share/watchfaces/hands/layers/<role>.bin
The upscale makes the thin hand strokes (and, for California, the thin white
rim around the black inlay) occupy more screen pixels so they don't alias /
tear when rotated at 45°.  Both faces read these shared masks.

A reference (unscaled) <role>.png is also written for eyeballing.

Emits components/os/watch_faces/hands_assets_gen.h — per-asset w/h and the
rotation pivot inside the upscaled mask (the original canvas centre expressed
in the scaled crop).

Renders composed previews (tmp/hands_preview_black.png / _grey.png) at the
10:08:30 pose mirroring the runtime recolour model.

Re-run after editing any source PNG.  Idempotent.
"""
import os

from PIL import Image

ROOT = r"d:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\watch"
HANDS = os.path.join(ROOT, "spiffs", "usr", "share", "watchfaces", "hands")
LAYERS = os.path.join(HANDS, "layers")
GEN_H = os.path.join(ROOT, "components", "os", "watch_faces", "hands_assets_gen.h")
PREVIEW_OUT = os.path.join(ROOT, "tmp")

# Original canvas geometry (all sources are 314x392, pivot = canvas centre).
ORIG_W, ORIG_H = 314, 392
ORIG_CX, ORIG_CY = (ORIG_W - 1) / 2.0, (ORIG_H - 1) / 2.0   # 156.5, 195.5

PAD = 2          # extra px around the alpha bbox (protects AA / soft shadow)
ALPHA_MIN = 4    # pixels above this alpha count as "content"

# Source -> role + z order (bottom..top) + default palette colour (0xRRGGBB).
# Shadow slots use color None -> they keep their natural soft-black pixels.
ASSETS = [
    # (source filename, role, layer-colour or None)
    ("42mm Hour Hand Shadow.png",  "h_shadow",   None),      # z0 hour shadow
    ("42mm Hour Hand.png",         "h_hand",     0xE0E0E0),  # z1 hour
    ("42mm Hour Hand Inlay.png",   "h_inlay",    0xE0E0E0),  # z2 hour inlay
    ("42mm Minute Hand Shadow.png","m_shadow",   None),      # z3 minute shadow
    ("42mm Minute Hand.png",       "m_hand",     0xE0E0E0),  # z4 minute
    ("42mm Minute Hand Inlay.png", "m_inlay",    0xE0E0E0),  # z5 minute inlay
    ("42mm Second Hand Shadow.png","s_shadow",   None),      # z6 second shadow
    ("42mm Second Hand.png",       "s_hand",     0xFF3B30),  # z7 second (red)
    ("42mm Center Pin Large.png",  "pin_large",  0x000000),  # z8 (unused now)
    ("42mm Center Pin Small.png",  "pin_small",  0x000000),  # z9 top pin
]

# Face geometry (410x502, symmetric integer centre).
SCR_W, SCR_H = 410, 502
CX, CY = 205, 251
# Upscale the masks so hands match the screen resolution (same factor the
# dials are scaled by).  Fixes aliasing of thin features at 45° rotations.
HAND_SCALE = SCR_W / ORIG_W       # 410/314 ≈ 1.306


def crop_asset(src_name):
    """Crop a source PNG to content; return (im, box, pivot_in_crop)."""
    im = Image.open(os.path.join(HANDS, src_name)).convert("RGBA")
    w, h = im.size
    assert w == ORIG_W and h == ORIG_H, f"{src_name}: unexpected size {w}x{h}"
    alpha = im.getchannel("A")
    bbox = alpha.point(lambda a: 255 if a >= ALPHA_MIN else 0).getbbox()
    if bbox is None:
        raise RuntimeError(f"{src_name}: empty")
    x0, y0, x1, y1 = bbox
    x0 = max(0, x0 - PAD); y0 = max(0, y0 - PAD)
    x1 = min(w, x1 + PAD); y1 = min(h, y1 + PAD)
    crop = im.crop((x0, y0, x1, y1))
    px = int(round(ORIG_CX - x0))
    py = int(round(ORIG_CY - y0))
    return crop, (x0, y0, x1, y1), (px, py)


def recolor_alpha(alpha, rgb):
    """alpha (L image) -> RGBA filled with one colour at that alpha."""
    out = Image.new("RGBA", alpha.size,
                    (rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF, 0))
    out.putalpha(alpha)
    return out


def main():
    os.makedirs(LAYERS, exist_ok=True)
    rows = []
    for src_name, role, color in ASSETS:
        crop, box, pivot = crop_asset(src_name)
        png_path = os.path.join(LAYERS, role + ".png")
        bin_path = os.path.join(LAYERS, role + ".bin")
        crop.save(png_path)                       # reference (unscaled) art
        nw = max(1, int(round(crop.width * HAND_SCALE)))
        nh = max(1, int(round(crop.height * HAND_SCALE)))
        alpha = crop.getchannel("A").resize((nw, nh), Image.BILINEAR)
        with open(bin_path, "wb") as f:
            f.write(alpha.tobytes())
        spx = int(round(pivot[0] * HAND_SCALE))
        spy = int(round(pivot[1] * HAND_SCALE))
        rows.append((role, nw, nh, box[0], box[1], spx, spy))
        print(f"{role:12s} crop {box[0]:3d},{box[1]:3d}  {nw:3d}x{nh:<3d}  "
              f"pivot ({spx},{spy})  -> {os.path.relpath(bin_path, ROOT)}")

    # ---- emit the generated C table ----------------------------------
    lines = []
    lines.append("/* Generated by tools/build_hands_face.py — do not edit by hand. */")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("typedef enum {")
    for i, (role, *_ ) in enumerate(rows):
        lines.append(f"    HANDS_ASSET_{role.upper()} = {i},")
    lines.append("    HANDS_ASSET_COUNT")
    lines.append("} hands_asset_id_t;")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    const char *file;      /* raw A8 mask name under hands/layers/ */")
    lines.append("    int16_t     w, h;      /* mask size (1 B/px)                 */")
    lines.append("    int16_t     pivot_x;   /* LVGL rotation pivot in the crop  */")
    lines.append("    int16_t     pivot_y;")
    lines.append("} hands_asset_meta_t;")
    lines.append("")
    lines.append("static const hands_asset_meta_t hands_asset_meta[HANDS_ASSET_COUNT] = {")
    for role, w, h, ox, oy, px, py in rows:
        lines.append(f'    [HANDS_ASSET_{role.upper()}] = {{ "{role}.bin", {w}, {h}, {px}, {py} }},')
    lines.append("};")
    lines.append("")
    open(GEN_H, "w", encoding="utf-8").write("\n".join(lines))
    print(f"\nwrote {GEN_H}")

    # ---- composed previews (mirror the runtime recolor model) --------
    def layer_angle(role):
        if role.startswith("h_"):
            return (10 % 12) * 300 + 8 * 5          # 0.1-deg units (hour)
        if role.startswith("m_"):
            return 8 * 60 + 30                       # minute + seconds frac
        if role.startswith("s_"):
            return 30 * 60                            # second
        return 0

    loaded = {}
    for role, nw, nh, ox, oy, px, py in rows:
        raw = open(os.path.join(LAYERS, role + ".bin"), "rb").read()
        alpha = Image.frombytes("L", (nw, nh), raw)
        color = next(c for s, r, c in ASSETS if r == role)
        if color is None:
            color = 0x000000          # shadows are recoloured black on device
        loaded[role] = (alpha, px, py, color)

    def compose(bg_hex):
        bg = Image.new("RGBA", (SCR_W, SCR_H),
                       (bg_hex >> 16 & 0xFF, bg_hex >> 8 & 0xFF, bg_hex & 0xFF, 255))
        for src_name, role, color in ASSETS:
            if role not in loaded:
                continue
            alpha, px, py, color = loaded[role]
            layer = recolor_alpha(alpha, color)
            full = Image.new("RGBA", (SCR_W, SCR_H), (0, 0, 0, 0))
            full.alpha_composite(layer, (CX - px, CY - py))
            deg = layer_angle(role) / 10.0
            if deg:
                full = full.rotate(-deg, center=(CX, CY), resample=Image.BILINEAR)
            bg.alpha_composite(full)
        return bg

    compose(0x000000).save(os.path.join(PREVIEW_OUT, "hands_preview_black.png"))
    compose(0x14161A).save(os.path.join(PREVIEW_OUT, "hands_preview_grey.png"))
    print("wrote previews to", PREVIEW_OUT)


if __name__ == "__main__":
    main()
