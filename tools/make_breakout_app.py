"""Generate the Breakout app package on the SD card template.

Produces, under sdcard/watchos/usr/app/breakout/:
  - app_icon.bin : raw 96x96 RGB565A8 (colour plane + inscribed-circle alpha)
                   — LVGL loads this into PSRAM and renders it as a circle
  - app_icon.png : 96x96 RGBA reference copy

Artwork: a classic Breakout scene — a navy disc with a rainbow brick wall at
the top, a yellow ball in the middle and a light paddle at the bottom.

Usage: python tools/make_breakout_app.py
"""
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "sdcard", "watchos", "usr", "app", "breakout")

SIZE = 96
CIRCLE_RADIUS = 48           # inscribed circle
C_BG = (10, 14, 26)          # 0x0A0E1A navy
C_PADDLE = (143, 211, 244)   # 0x8FD3F4 light blue
C_BALL = (246, 241, 213)     # 0xF6F1D5 cream
BALL_R = 5
ROW_COLORS = [
    (231, 76, 60),    # red     0xE74C3C
    (230, 126, 34),   # orange  0xE67E22
    (241, 196, 15),   # yellow  0xF1C40F
    (46, 204, 113),   # green   0x2ECC71
    (52, 152, 219),   # blue    0x3498DB
    (155, 89, 182),   # purple  0x9B59B6
]


def rounded(d, box, r, fill):
    d.rounded_rectangle(box, radius=r, fill=fill)


def main():
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Navy circular background (the alpha mask also trims the disc).
    rounded(d, [0, 0, SIZE - 1, SIZE - 1], 40, C_BG + (255,))

    # Brick wall: 3 rows squeezed into the top of the disc.  Rows are drawn
    # full-width and get trimmed by the circular mask near the edges.
    brick_h = 9
    row_gap = 3
    y = 10
    nrow = 3
    ncol = 6
    gap = 2
    brick_w = (SIZE - 2 * gap - (ncol - 1) * gap) / ncol
    for r in range(nrow):
        color = ROW_COLORS[r] + (255,)
        yy = y + r * (brick_h + row_gap)
        for c in range(ncol):
            x0 = gap + c * (brick_w + gap)
            rounded(d, [x0, yy, x0 + brick_w, yy + brick_h], 2, color)

    # Ball (slightly above the paddle).
    cx, cy = SIZE / 2, 52
    d.ellipse([cx - BALL_R, cy - BALL_R, cx + BALL_R, cy + BALL_R],
              fill=C_BALL + (255,))

    # Paddle at the bottom.
    pw, ph, pr = 60, 9, 5
    px = (SIZE - pw) / 2
    py = 68
    rounded(d, [px, py, px + pw, py + ph], pr, C_PADDLE + (255,))

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "app_icon.png"))

    px_l = list(img.getdata())

    # RGB565A8: raw colour plane + inscribed-circle alpha plane.
    half = (SIZE - 1) / 2.0
    r_outer = (CIRCLE_RADIUS + 1) ** 2
    r_inner = (CIRCLE_RADIUS - 1) ** 2
    r_band = r_outer - r_inner

    color = bytearray(SIZE * SIZE * 2)
    alpha = bytearray(SIZE * SIZE)
    for i, (r, g, b, a) in enumerate(px_l):
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
