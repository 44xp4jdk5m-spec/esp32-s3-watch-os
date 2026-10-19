#!/usr/bin/env python3
"""
build_parallax_face.py — Facebook-style 3D photo pre-processor for the ESP32-S3
watch face "Parallax" (components/os/watch_faces/watch_face_parallax.c).

The SAME pipeline Facebook 3D photos use:
  1. ONE 2D photo.
  2. An **AI monocular depth model** (Depth Anything V2 small, ONNX) predicts a
     per-pixel depth map that follows OBJECT EDGES (mountains, ridges, foreground
     objects) — NOT a flat gradient. This is the key difference from a naive
     "cut the image into horizontal strips".
  3. The photo is sliced into N depth layers along that real depth. Each layer's
     alpha is ~1 only where the scene is at that depth, so layers re-composite
     to the original photo at zero tilt and slide apart along the depth axis when
     the watch is tilted (QMI8658) → smooth 2.5D parallax. The MCU never warps
     anything; it only alpha-blends + translates pre-baked layers.

The model runs on the PC (onnxruntime) — the watch never runs an AI model.

Usage
-----
    python build_parallax_face.py                          # portrait CC demo + AI depth
    python build_parallax_face.py --input my.jpg           # your photo + AI depth
    python build_parallax_face.py --input my.jpg --depth depth.png   # your own depth map
    python build_parallax_face.py --no-ai                  # old heuristic vertical prior
    python build_parallax_face.py --synthetic              # offline generated demo scene

Options
-------
    --input IMG          source photo (portrait recommended; cropped to the canvas)
    --depth DEPTH        grayscale depth map (white = near); skips the AI model
    --depth-model PATH   use this .onnx instead of auto-downloading
    --no-ai              skip the AI model entirely (heuristic vertical prior)
    --depth-flip         invert the predicted depth (near<->far)
    --layers N           number of depth layers (default 6)
    --canvas-w H         full canvas width  (default 410 = full screen)
    --canvas-h H         full canvas height (default 502 = full screen)
    --margin M           extra pixels around the canvas for parallax shift (default 12)
    --max-shift S        max parallax shift px, must be <= margin (default 10)

Output
------
    spiffs/usr/share/watchfaces/parallax/layer0..N-1.bin   raw LVGL RGB565A8 bytes
                                     (pre-decoded; the watch reads these verbatim —
                                     no on-device PNG decoder)
    spiffs/usr/share/watchfaces/parallax/meta.txt          geometry the C loader reads
    spiffs/usr/share/watchfaces/parallax/ATTRIBUTION.txt   photo credit/licence
                                     (if downloaded)
    tmp/parallax_face/parallax_depth.png     the AI depth map (inspect this!)
    tmp/parallax_face/parallax_preview.png   composite at zero tilt
    tmp/parallax_face/parallax_exploded.png  layers side-by-side
    tmp/parallax_face/layerN_preview.png     per-layer RGBA crops (inspection only)
"""
import argparse
import math
import os
import shutil
import sys
import urllib.request

from PIL import Image, ImageFilter, ImageDraw
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# ── Default demo: portrait CC BY-SA photo on Wikimedia Commons ─────────────
# "Blue Mountains National Park (AU), Three Sisters" — Dietmar Rabich (XRay).
#   https://commons.wikimedia.org/wiki/File:Blue_Mountains_National_Park_(AU),_Three_Sisters_--_2019_--_1987-9.jpg
DEFAULT_IMG_URL = (
    "https://upload.wikimedia.org/wikipedia/commons/d/de/"
    "Blue_Mountains_National_Park_%28AU%29%2C_Three_Sisters_--_2019_--_1987-9.jpg"
)
DEFAULT_IMG_THUMB = (
    "https://upload.wikimedia.org/wikipedia/commons/thumb/d/de/"
    "Blue_Mountains_National_Park_%28AU%29%2C_Three_Sisters_--_2019_--_1987-9.jpg/"
    "960px-Blue_Mountains_National_Park_%28AU%29%2C_Three_Sisters_--_2019_--_1987-9.jpg"
)
DEFAULT_ATTR = (
    "Parallax demo image:\n"
    "\"Blue Mountains National Park (AU), Three Sisters\" by Dietmar Rabich (XRay),\n"
    "CC BY-SA 4.0, via Wikimedia Commons.\n"
    "https://commons.wikimedia.org/wiki/File:Blue_Mountains_National_Park_(AU),_"
    "Three_Sisters_--_2019_--_1987-9.jpg\n"
    "(layers derived from this photo are likewise CC BY-SA 4.0)"
)

# Depth Anything V2 Small (int8 ONNX) — onnx-community export, Apache-2.0.
# Runs on the PC; the watch only uses the sliced layers.
DEPTH_MODEL_URL = (
    "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/"
    "onnx/model_quantized.onnx"
)
DEPTH_MODEL_NAME = "depth_anything_v2_vits_int8.onnx"
DAV2_SIZE = 518


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--input", help="source photo (portrait recommended)")
    p.add_argument("--depth", help="optional depth map (grayscale; white=near)")
    p.add_argument("--synthetic", action="store_true",
                   help="generate a synthetic mountain scene instead of downloading")
    p.add_argument("--depth-model", default=None, help="path to a .onnx depth model")
    p.add_argument("--no-ai", action="store_true",
                   help="skip the AI depth model (use the vertical heuristic)")
    p.add_argument("--depth-flip", action="store_true",
                   help="invert predicted depth (near<->far) if it looks backwards")
    p.add_argument("--out",
                   default=os.path.join(ROOT, "spiffs", "usr", "share",
                                        "watchfaces", "parallax"),
                   help="output dir for layer files "
                        "(default spiffs/usr/share/watchfaces/parallax)")
    p.add_argument("--canvas-w", type=int, default=410, help="canvas width px (full screen)")
    p.add_argument("--canvas-h", type=int, default=502, help="canvas height px (full screen)")
    p.add_argument("--layers", type=int, default=5,
                   help="number of depth layers (default 5; full-screen 410x502 needs ~2.2 MB PSRAM)")
    p.add_argument("--margin", type=int, default=12,
                   help="extra pixels around the canvas the parallax shifts into")
    p.add_argument("--max-shift", type=int, default=10,
                   help="max parallax shift px (must be <= margin)")
    p.add_argument("--overlap", type=float, default=0.5,
                   help="0..1 depth-feather overlap between adjacent layers")
    p.add_argument("--horizon", type=float, default=0.52,
                   help="far-plane fraction for the heuristic depth prior")
    p.add_argument("--crop-y", type=float, default=0.5,
                   help="vertical crop anchor for wrong-aspect sources, 0..1")
    return p.parse_args()


# ── Downloading ──────────────────────────────────────────────────────────
def _download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "parallax-face-builder/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f)


def load_source(args, workdir):
    """Return (PIL RGB image, depth_or_None).  Synthetic scenes return exact depth."""
    if args.synthetic:
        print("[source] generating synthetic mountain scene")
        return make_synthetic_scene(args)
    if args.input:
        img = Image.open(args.input).convert("RGB")
        print(f"[source] loaded {args.input} {img.size}")
        return img, None

    dest = os.path.join(workdir, "demo_source.jpg")
    if not os.path.exists(dest):
        print("[source] downloading portrait demo photo "
              "(Blue Mountains Three Sisters, CC BY-SA, Wikimedia)")
        try:
            _download(DEFAULT_IMG_URL, dest)
        except Exception as e:  # noqa: BLE001 — rate-limited? try the smaller thumb
            print(f"  full-res failed ({e}); trying 960px thumbnail")
            try:
                _download(DEFAULT_IMG_THUMB, dest)
            except Exception as e2:  # noqa: BLE001
                print(f"  thumb failed too ({e2}); using a synthetic scene")
                return make_synthetic_scene(args)
        with open(os.path.join(args.out, "ATTRIBUTION.txt"), "w", encoding="utf-8") as f:
            f.write(DEFAULT_ATTR + "\n")
    img = Image.open(dest).convert("RGB")
    print(f"[source] loaded {dest} {img.size}")
    return img, None


def crop_to_ratio(img, target_w, target_h, anchor_y=0.5):
    """Crop to the target aspect ratio (anchor_y 0..1 controls vertical pos)."""
    sw, sh = img.size
    target_ratio = target_w / target_h
    ratio = sw / sh
    if ratio > target_ratio:              # too wide -> crop width
        new_w = int(sh * target_ratio)
        x0 = (sw - new_w) // 2
        box = (x0, 0, x0 + new_w, sh)
    else:                                  # too tall -> crop height
        new_h = int(sw / target_ratio)
        y0 = int((sh - new_h) * anchor_y)
        box = (0, y0, sw, y0 + new_h)
    return img.crop(box)


# ── Synthetic scene (offline fallback) ──────────────────────────────────
def make_synthetic_scene(args):
    layer_w = args.canvas_w + 2 * args.margin
    layer_h = args.canvas_h + 2 * args.margin
    W, H = layer_w, layer_h
    rng = np.random.default_rng(7)

    top = np.array([26, 52, 110])
    bot = np.array([255, 180, 120])
    yy = np.linspace(0, 1, H)[:, None]
    grad = (top[None, :] * (1 - yy) + bot[None, :] * yy).astype(np.uint8)
    sky = np.tile(grad[:, None, :], (1, W, 1))

    img = sky.copy()
    depth = np.full((H, W), 0.0)

    def ridge(base_y, amp, color, d, jag=1.0):
        nonlocal img, depth
        x = np.arange(W)
        y = (np.sin(x / (W / 3.0) * math.pi * jag + rng.uniform(0, 6)) * 0.5 +
             np.sin(x / (W / 7.0) * math.pi * jag + rng.uniform(0, 6)) * 0.3 +
             np.sin(x / (W / 1.3) * math.pi * jag) * 0.2)
        y = 0.5 + 0.5 * y
        peak = np.clip(base_y - amp * y, 0, H).astype(int)
        rows = np.arange(H)[:, None]
        mask = rows >= peak[None, :]
        col = np.array(color, dtype=np.uint8)
        img = np.where(mask[:, :, None], col[None, None, :], img)
        depth = np.where(mask, d, depth)
        for xx in range(W):
            p0 = int(peak[xx])
            if p0 < H:
                g = np.linspace(0.8, 1.05, H - p0)
                img[p0:, xx] = np.clip(img[p0:, xx].astype(np.float32) * g[:, None],
                                       0, 255).astype(np.uint8)

    ridge(int(H * 0.30), int(H * 0.16), (180, 205, 235), 0.18, jag=1.1)
    ridge(int(H * 0.42), int(H * 0.14), (150, 175, 215), 0.36, jag=1.4)
    ridge(int(H * 0.60), int(H * 0.16), (70, 110, 90), 0.58, jag=1.7)
    ridge(int(H * 0.74), int(H * 0.14), (45, 80, 70), 0.78, jag=2.0)

    lake_top = int(H * 0.82)
    img[lake_top:] = np.array([40, 70, 110])
    depth[lake_top:] = 0.95
    sun = Image.fromarray(img)
    ImageDraw.Draw(sun).ellipse([int(W * 0.68), int(H * 0.10),
                                 int(W * 0.80), int(H * 0.26)], fill=(255, 230, 170))
    print(f"[synthetic] {(W, H)}")
    return Image.fromarray(np.array(sun)).convert("RGB"), depth


# ── AI depth (Depth Anything V2 small, ONNX) ────────────────────────────
def ensure_model(args, workdir):
    """Return a path to an ONNX depth model (downloading if needed), or None."""
    if args.depth_model:
        if not os.path.exists(args.depth_model):
            print(f"error: --depth-model '{args.depth_model}' not found", file=sys.stderr)
            sys.exit(1)
        return args.depth_model
    dest = os.path.join(workdir, DEPTH_MODEL_NAME)
    if not os.path.exists(dest):
        print(f"[model] downloading {DEPTH_MODEL_NAME} "
              "(Depth Anything V2 small int8, ~25 MB, Apache-2.0)...")
        try:
            _download(DEPTH_MODEL_URL, dest)
            print(f"[model] {os.path.getsize(dest)//1024//1024} MB -> {dest}")
        except Exception as e:  # noqa: BLE001
            print(f"[model] download failed ({e}); will use the heuristic depth")
            return None
    return dest


def estimate_depth_onnx(img_rgb, model_path):
    """Depth Anything V2 small (ONNX) → disparity map HxW float32 in [0,1], 1 = near.
    Returns a numpy array at the *layer* resolution."""
    import onnxruntime as ort
    MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
    STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)
    S = DAV2_SIZE

    x = img_rgb.resize((S, S), Image.BILINEAR)
    x = np.asarray(x, dtype=np.float32) / 255.0
    x = (x - MEAN) / STD
    x = np.transpose(x, (2, 0, 1))[None].astype(np.float32)      # 1,3,S,S

    sess = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    inp = sess.get_inputs()[0].name
    out = sess.run(None, {inp: x})[0]                            # (1,S,S) or (1,1,S,S)
    d = out[0]
    if d.ndim == 3:                                              # (1,S,S) -> (S,S)
        d = d[0]
    d = d.astype(np.float32)
    dmin, dmax = float(d.min()), float(d.max())
    if dmax - dmin < 1e-6:
        return np.zeros_like(d)
    d = (d - dmin) / (dmax - dmin)
    return d


def build_depth(args, source_rgb, workdir, synthetic_depth, model_path):
    """Return float HxW in [0,1]: 0 = far, 1 = near."""
    W, H = source_rgb.size
    if synthetic_depth is not None:
        d = synthetic_depth.astype(np.float32)
        blur = 0
    elif args.depth:
        dm = Image.open(args.depth).convert("L").resize((W, H), Image.BILINEAR)
        d = np.asarray(dm, dtype=np.float32) / 255.0
        blur = 2
    elif model_path:
        print("[depth] running Depth Anything V2 (this takes a few seconds)...")
        try:
            d = estimate_depth_onnx(source_rgb, model_path)
            if args.depth_flip:
                d = 1.0 - d
            blur = 3   # light smoothing so layer bands are soft
        except Exception as e:  # noqa: BLE001 — missing onnxruntime etc.
            print(f"[depth] AI model failed ({e}); using the heuristic vertical prior")
            d = heuristic_depth(W, H, args.horizon)
            blur = 24
    else:
        d = heuristic_depth(W, H, args.horizon)
        blur = 24

    # upscale the (maybe 518x518) AI map to layer size + soften band edges
    if d.shape != (H, W):
        d = np.asarray(Image.fromarray((d * 255).astype(np.uint8))
                       .resize((W, H), Image.BILINEAR), dtype=np.float32) / 255.0
    if blur > 0:
        d = np.asarray(Image.fromarray((d * 255).astype(np.uint8))
                       .filter(ImageFilter.GaussianBlur(blur)), dtype=np.float32) / 255.0

    lo, hi = float(d.min()), float(d.max())
    if hi - lo > 1e-6:
        d = (d - lo) / (hi - lo)
    return d


def heuristic_depth(W, H, horizon):
    y = np.linspace(0, 1, H)[:, None]
    d = np.clip((y - horizon) / (1.0 - horizon), 0.0, 1.0)
    return np.tile(d, (1, W)).astype(np.float32)


def build_layers(args, source_rgb, depth):
    """Slice into N RGBA layers sized (canvas+2*margin)^2, alpha = depth bands.
    Linear (equal-depth-range) bands — keeps layer bboxes small for this board's
    tight PSRAM (quantile/equal-area bands make full-frame bands and blow up RAM)."""
    W, H = source_rgb.size
    N = args.layers
    src = np.asarray(source_rgb, dtype=np.uint8)
    layers = []
    for i in range(N):
        c = (i + 0.5) / N
        half = (1.0 / N) * (1.0 + args.overlap)
        alpha = np.clip(1.0 - np.abs(depth - c) / half, 0.0, 1.0)
        a8 = (alpha * 255.0 + 0.5).astype(np.uint8)
        layers.append(Image.fromarray(np.dstack([src, a8]), "RGBA"))
    return layers


def rgb_to_rgb565(img_rgb):
    """PIL RGB → packed LVGL RGB565 bytes (2 B/px, little-endian).  Used for the
    opaque full-photo background layer (layer0), which needs no alpha map."""
    arr = np.asarray(img_rgb.convert("RGB"))
    r, g, b = arr[..., 0].astype(np.uint32), arr[..., 1].astype(np.uint32), arr[..., 2].astype(np.uint32)
    rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    color = np.empty((*rgb565.shape, 2), np.uint8)
    color[..., 0] = (rgb565 & 0xff).astype(np.uint8)
    color[..., 1] = (rgb565 >> 8).astype(np.uint8)
    return color.tobytes()


def layer_to_rgb565a8(lay):
    """PIL RGBA layer → packed LVGL RGB565A8 bytes: RGB565 color plane (2 B/px,
    little-endian) immediately followed by an A8 alpha plane (1 B/px).  The C
    loader reads this verbatim (no on-device decode)."""
    arr = np.asarray(lay)
    r, g, b, a = arr[..., 0], arr[..., 1], arr[..., 2], arr[..., 3]
    rgb565 = (((r.astype(np.uint32) >> 3) << 11) |
              ((g.astype(np.uint32) >> 2) << 5) |
              (b.astype(np.uint32) >> 3))
    color = np.empty((*rgb565.shape, 2), np.uint8)
    color[..., 0] = (rgb565 & 0xff).astype(np.uint8)
    color[..., 1] = (rgb565 >> 8).astype(np.uint8)
    return color.tobytes() + a.tobytes()


def write_layers(args, source_rgb, layers):
    """Write spiffs/usr/share/watchfaces/parallax/layerN.bin:
      layer0 = the FULL photo, opaque, RGB565 (2 B/px) — the background that fills
               any parallax gaps (also the farthest plane).
      layer1..N-1 = depth bands, RGB565A8 (3 B/px), cropped to their alpha bbox.
    Returns crop records (off_x, off_y, w, h) for meta.txt."""
    layer_w, layer_h = source_rgb.size
    preview_dir = os.path.join(ROOT, "tmp", "parallax_face")
    os.makedirs(preview_dir, exist_ok=True)

    # layer 0: opaque full-photo background
    with open(os.path.join(args.out, "layer0.bin"), "wb") as f:
        f.write(rgb_to_rgb565(source_rgb))
    source_rgb.convert("RGB").save(os.path.join(preview_dir, "layer0_preview.png"))
    recs = [(0, 0, layer_w, layer_h)]

    # layers 1..N-1: depth bands
    for i in range(1, args.layers):
        lay = layers[i]
        a = np.asarray(lay)[..., 3]
        ys, xs = np.nonzero(a > 0)
        if len(xs) == 0:
            crop = lay.crop((0, 0, 1, 1))
            recs.append((0, 0, 1, 1))
        else:
            x0, y0 = int(xs.min()), int(ys.min())
            x1, y1 = int(xs.max()) + 1, int(ys.max()) + 1
            crop = lay.crop((x0, y0, x1, y1))
            recs.append((x0, y0, x1 - x0, y1 - y0))
        with open(os.path.join(args.out, f"layer{i}.bin"), "wb") as f:
            f.write(layer_to_rgb565a8(crop))
        crop.save(os.path.join(preview_dir, f"layer{i}_preview.png"))   # inspection only
    return recs


def main():
    args = parse_args()
    if args.max_shift > args.margin:
        print(f"error: --max-shift {args.max_shift} > --margin {args.margin}", file=sys.stderr)
        sys.exit(1)

    workdir = os.path.join(ROOT, "tmp", "parallax_face")
    os.makedirs(workdir, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)

    # 1) source -> crop/scale to the LAYER size (canvas + 2*margin)
    source_rgb, synthetic_depth = load_source(args, workdir)
    layer_w = args.canvas_w + 2 * args.margin
    layer_h = args.canvas_h + 2 * args.margin
    source_rgb = crop_to_ratio(source_rgb, layer_w, layer_h, args.crop_y)
    source_rgb = source_rgb.resize((layer_w, layer_h), Image.LANCZOS)
    print(f"[canvas] {args.canvas_w}x{args.canvas_h} + margin {args.margin} "
          f"-> layer {layer_w}x{layer_h}")

    # 2) depth (AI unless --no-ai / --depth / --synthetic)
    model_path = None
    if not (args.no_ai or args.depth or args.synthetic):
        model_path = ensure_model(args, workdir)
    depth = build_depth(args, source_rgb, workdir, synthetic_depth, model_path)

    # 3) slice -> write raw .bin layers (layer0 = opaque RGB565 bg, rest = bands)
    layers = build_layers(args, source_rgb, depth)
    crops = write_layers(args, source_rgb, layers)

    with open(os.path.join(args.out, "meta.txt"), "w", encoding="ascii") as f:
        f.write(f"{args.layers} {layer_w} {layer_h} {args.canvas_w} {args.canvas_h} "
                f"{args.margin} {args.max_shift}\n")
        for (ox, oy, w, h) in crops:
            f.write(f"{ox} {oy} {w} {h}\n")

    spiffs_total = 0
    for i in range(args.layers):
        p = os.path.join(args.out, f"layer{i}.bin")
        spiffs_total += os.path.getsize(p)
        print(f"  layer{i}.bin {os.path.getsize(p)//1024:5d} KB  "
              f"crop {crops[i][2]}x{crops[i][3]} @({crops[i][0]},{crops[i][1]})")
    print(f"[spiffs] layers total {spiffs_total//1024} KB")
    print(f"[memory] resident = {spiffs_total/1024/1024:.2f} MB "
          f"({spiffs_total//1024} KB, includes opaque RGB565 background)")

    # 4) previews (full-size layers, zero tilt == the original crop)
    comp = Image.alpha_composite(Image.new("RGBA", (layer_w, layer_h), (0, 0, 0, 255)),
                                 layers[0])
    for lay in layers[1:]:
        comp = Image.alpha_composite(comp, lay)
    x0, y0 = (layer_w - args.canvas_w) // 2, (layer_h - args.canvas_h) // 2
    comp.crop((x0, y0, x0 + args.canvas_w, y0 + args.canvas_h)).convert("RGB").save(
        os.path.join(workdir, "parallax_preview.png"))

    thumb_w = layer_w // 2
    exploded = Image.new("RGBA", (args.layers * (thumb_w + 4) - 4, layer_h // 2),
                         (20, 20, 20, 255))
    for i, lay in enumerate(layers):
        t = lay.resize((thumb_w, layer_h // 2), Image.LANCZOS)
        exploded.paste(t, (i * (thumb_w + 4), 0), t)
    exploded.convert("RGB").save(os.path.join(workdir, "parallax_exploded.png"))

    # depth map preview (0=far black, 1=near white)
    Image.fromarray((depth * 255).astype(np.uint8)).convert("RGB").save(
        os.path.join(workdir, "parallax_depth.png"))

    print(f"[ok] layers in {args.out}; previews in {workdir} "
          f"(see parallax_depth.png = the AI depth map)")
    print("Flash note: layers are embedded by the build "
          "(spiffs_create_partition_image) automatically.")


if __name__ == "__main__":
    main()
