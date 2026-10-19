# convert_icon_circle.py - Crop a source image into a circle for the honeycomb menu
#
# Usage:
#   python convert_icon_circle.py <input.png> <output.png> [scale_pct]
#
# - Resizes nothing (keeps source size), scales the art by scale_pct (default 85%)
#   and centers it, then applies an inscribed-circle alpha mask with a soft edge.
# - Transparent outside pixels are written as alpha=0 (the honeycomb prescale
#   step converts those to black, which blends into the dark menu background).
#
# Requires Pillow.

import os
import sys
import math

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    print("ERROR: This script requires Pillow. Install: pip install Pillow")
    sys.exit(1)


def crop_circle(input_path, output_path, scale_pct=85):
    img = Image.open(input_path).convert("RGBA")
    w, h = img.size
    side = min(w, h)

    # Scale art down and center it so the whole subject fits inside the circle
    scale = scale_pct / 100.0
    art_w = int(w * scale)
    art_h = int(h * scale)
    img = img.resize((art_w, art_h), Image.LANCZOS)

    # Canvas same size as the input
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    canvas.paste(img, ((w - art_w) // 2, (h - art_h) // 2), img)

    # Inscribed circle mask with a soft (anti-aliased) edge
    mask = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(mask)
    margin = 2  # keep a 2px transparent ring so the edge is clean
    radius = side / 2 - margin
    cx, cy = w / 2, h / 2
    draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=255)

    # Soft edge: blur the mask a tiny bit for anti-aliasing
    mask = mask.filter(ImageFilter.GaussianBlur(0.8))

    out = canvas.copy()
    out.putalpha(mask)
    out.save(output_path)
    print(f"Saved {output_path} ({w}x{h}, circle scale {scale_pct}%)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    inp = sys.argv[1]
    out = sys.argv[2]
    scale = float(sys.argv[3]) if len(sys.argv) > 3 else 85.0
    crop_circle(inp, out, scale)
