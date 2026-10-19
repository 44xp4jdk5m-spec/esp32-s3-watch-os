#!/usr/bin/env python3
"""Bake the "California" dial masks + render a full-face preview (410x502).

DIALS — the California Fullscreen 42mm dial PNGs (assets/watchfaces/california,
kept OUTSIDE spiffs: their long filenames exceed SPIFFS's 64-char object-name
limit) are 314x392 white-on-transparent masks on the same canvas/centre as the
hands.  Each chosen layer is affinely re-scaled, centre-anchored so the source
centre (156.5,195.5) maps to the screen centre (205,251), then written as a raw
A8 alpha mask:
  spiffs/usr/share/watchfaces/california/{small_ticks,big_ticks,symbol_california}.bin

The Small Ticks + Big Ticks files are the TWO parts of ONE tick-ring entity
(split only for recolouring) and are always drawn together; the symbol is drawn
on top and contains NO ticks.

HANDS — the shared scaled-up masks from tools/build_hands_face.py are used by
the device; this script only reads them to compose the preview.

Re-run after editing any source PNG or tuning geometry.  Idempotent.
"""
import os
import re

from PIL import Image

ROOT = r"d:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\watch"
CAL_SRC = os.path.join(ROOT, "assets", "watchfaces", "california")
CAL_OUT = os.path.join(ROOT, "spiffs", "usr", "share", "watchfaces", "california")
HAND_LAYERS = os.path.join(ROOT, "spiffs", "usr", "share", "watchfaces", "hands", "layers")
GEN_H = os.path.join(ROOT, "components", "os", "watch_faces", "hands_assets_gen.h")
PREVIEW_OUT = os.path.join(ROOT, "tmp")

SRC_W, SRC_H = 314, 392
DST_W, DST_H = 410, 502
CX, CY = 205, 251

DIALS = [
    ("California Fullscreen 42mm - Small Ticks.png",         "small_ticks",       0x9A9A9A),
    ("California Fullscreen 42mm - Big Ticks.png",           "big_ticks",         0xFFFFFF),
    ("California Fullscreen 42mm - Symbols - California.png","symbol_california", 0xFFFFFF),
]

# Hand preview colours (mirror of watch_face_california.c).
HAND_COLORS = {
    "h_shadow": 0x000000, "h_hand": 0xFFFFFF, "h_inlay": 0x000000,
    "m_shadow": 0x000000, "m_hand": 0xFFFFFF, "m_inlay": 0x000000,
    "s_shadow": 0x000000, "s_hand": 0xFF3B30, "pin_small": 0x000000,
}
HAND_ORDER = ["h_shadow", "h_hand", "h_inlay", "m_shadow", "m_hand",
              "m_inlay", "s_shadow", "s_hand", "pin_small"]


# ── helpers ──────────────────────────────────────────────────────────
def recolor_alpha(alpha, rgb):
    out = Image.new("RGBA", alpha.size,
                    (rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF, 0))
    out.putalpha(alpha)
    return out


def parse_hand_meta():
    """role -> (file, w, h, pivot_x, pivot_y) from hands_assets_gen.h (scaled)."""
    text = open(GEN_H, encoding="utf-8").read()
    meta = {}
    for m in re.finditer(
            r'HANDS_ASSET_([A-Z0-9_]+)\]\s*=\s*\{\s*"([^"]+\.bin)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}',
            text):
        meta[m.group(1).lower()] = (m.group(2), int(m.group(3)), int(m.group(4)),
                                    int(m.group(5)), int(m.group(6)))
    return meta


def dial_mask_410x502(src_png):
    im = Image.open(src_png).convert("RGBA")
    assert im.size == (SRC_W, SRC_H), f"{os.path.basename(src_png)}: {im.size}"
    sx = DST_W / SRC_W
    sy = DST_H / SRC_H
    a, e = 1.0 / sx, 1.0 / sy
    c = (SRC_W - 1) / 2.0 - CX / sx
    f = (SRC_H - 1) / 2.0 - CY / sy
    scaled = im.transform((DST_W, DST_H), Image.AFFINE, (a, 0, c, 0, e, f),
                          resample=Image.BILINEAR)
    return scaled.getchannel("A")


# ── main ─────────────────────────────────────────────────────────────
def main():
    os.makedirs(CAL_OUT, exist_ok=True)
    dial_alphas = {}
    for src_name, role, color in DIALS:
        alpha = dial_mask_410x502(os.path.join(CAL_SRC, src_name))
        with open(os.path.join(CAL_OUT, role + ".bin"), "wb") as f:
            f.write(alpha.tobytes())
        dial_alphas[role] = (alpha, color)
        print(f"{role:18s} 410x502 A8 {alpha.size[0] * alpha.size[1]} B")

    # full-face preview (shared scaled hands)
    hand_meta = parse_hand_meta()

    def layer_angle(role):
        if role.startswith("h_"):
            return (10 % 12) * 300 + 8 * 5
        if role.startswith("m_"):
            return 8 * 60 + 30
        if role.startswith("s_"):
            return 30 * 60
        return 0

    def compose():
        bg = Image.new("RGBA", (DST_W, DST_H), (0, 0, 0, 255))
        for _s, role, color in DIALS:
            alpha, _ = dial_alphas[role]
            bg.alpha_composite(recolor_alpha(alpha, color))
        for role in HAND_ORDER:
            if role not in hand_meta:
                continue
            file, nw, nh, px, py = hand_meta[role]
            raw = open(os.path.join(HAND_LAYERS, file), "rb").read()
            alpha = Image.frombytes("L", (nw, nh), raw)
            color = HAND_COLORS[role]
            col = recolor_alpha(alpha, color)
            full = Image.new("RGBA", (DST_W, DST_H), (0, 0, 0, 0))
            full.alpha_composite(col, (CX - px, CY - py))
            deg = layer_angle(role) / 10.0
            if deg:
                full = full.rotate(-deg, center=(CX, CY), resample=Image.BILINEAR)
            bg.alpha_composite(full)
        return bg

    compose().save(os.path.join(PREVIEW_OUT, "california_preview.png"))
    print("wrote previews to", PREVIEW_OUT)


if __name__ == "__main__":
    main()
