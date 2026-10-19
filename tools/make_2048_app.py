"""Generate the 2048 app package on the SD card template.

Produces, under sdcard/watchos/usr/app/2048/:
  - app_icon.bin : raw 96x96 RGB565A8 (colour plane + inscribed-circle alpha)
                   — LVGL loads this into PSRAM and renders it as a circle
  - app_icon.png : 96x96 RGBA reference copy

Artwork: a circle split into four quadrants using the classic 2048 block
colours (#EEE4DA / #EDE0C8 / #F2B179 / #F59563), with a white "2048"
centred on top.

Usage: python tools/make_2048_app.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "sdcard", "watchos", "usr", "app", "2048")

SIZE = 96
CIRCLE_RADIUS = 48           # inscribed circle

# Four classic 2048 block colours, clockwise from top-left.
QUADS = [
    (0xEE, 0xE4, 0xDA),   # 2   — top-left
    (0xED, 0xE0, 0xC8),   # 4   — top-right
    (0xF2, 0xB1, 0x79),   # 8   — bottom-left
    (0xF5, 0x95, 0x63),   # 16  — bottom-right
]
TEXT = (0xFF, 0xFF, 0xFF)   # white "2048"

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\arialbd.ttf",
    r"C:\Windows\Fonts\segoeuib.ttf",
    r"C:\Windows\Fonts\seguisb.ttf",
    r"C:\Windows\Fonts\arial.ttf",
]


def load_font(size):
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                continue
    return ImageFont.load_default()


def main():
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Four 90-degree pie slices fill the whole circle (PIL angles: 0 = 3 o'clock,
    # increasing clockwise).
    d.pieslice([0, 0, SIZE - 1, SIZE - 1], 180, 270, fill=QUADS[0])  # top-left
    d.pieslice([0, 0, SIZE - 1, SIZE - 1], 270, 360, fill=QUADS[1])  # top-right
    d.pieslice([0, 0, SIZE - 1, SIZE - 1], 0, 90,   fill=QUADS[2])   # bottom-left
    d.pieslice([0, 0, SIZE - 1, SIZE - 1], 90, 180,  fill=QUADS[3])   # bottom-right

    font = load_font(30)
    text = "2048"
    bbox = d.textbbox((0, 0), text, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((SIZE - tw) / 2 - bbox[0], (SIZE - th) / 2 - bbox[1]), text,
           font=font, fill=TEXT + (255,))

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "app_icon.png"))

    px = list(img.getdata())

    # RGB565A8: raw colour plane + inscribed-circle alpha plane.
    half = (SIZE - 1) / 2.0
    r_outer = (CIRCLE_RADIUS + 1) ** 2
    r_inner = (CIRCLE_RADIUS - 1) ** 2
    r_band = r_outer - r_inner

    color = bytearray(SIZE * SIZE * 2)
    alpha = bytearray(SIZE * SIZE)
    for i, (r, g, b, a) in enumerate(px):
        dx = (i % SIZE) - half
        dy = (i // SIZE) - half
        d2 = dx * dx + dy * dy
        if d2 <= r_inner:
            m = 255
        elif d2 >= r_outer:
            m = 0
        else:
            m = int(255 * (r_outer - d2) / r_band)
        al = (a * m) // 255
        r5 = (r >> 3) & 0x1F
        g6 = (g >> 2) & 0x3F
        b5 = (b >> 3) & 0x1F
        val = (r5 << 11) | (g6 << 5) | b5
        color[i * 2] = val & 0xFF
        color[i * 2 + 1] = (val >> 8) & 0xFF
        alpha[i] = al

    with open(os.path.join(OUT, "app_icon.bin"), "wb") as f:
        f.write(color)
        f.write(alpha)

    print("Wrote app_icon.png and app_icon.bin (%d bytes) -> %s"
          % (len(color) + len(alpha), OUT))


if __name__ == "__main__":
    main()
