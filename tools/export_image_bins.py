#!/usr/bin/env python3
"""Export generated image C arrays to raw .bin files for SPIFFS.

The generated pixel arrays below used to be compiled into the firmware
(and, with CONFIG_SPIRAM_RODATA=y, sat in PSRAM for the whole session).
They are now excluded from the build; this script extracts each byte
array and writes it under spiffs/usr/share/ so the runtime image loader
(components/os/app_image_loader.c) can read the pixels into PSRAM only
while the image is actually displayed.

  watch-face images  -> spiffs/usr/share/watchfaces/<name>.bin
  activity app icons -> spiffs/usr/share/activity/<name>.bin

Run:  python tools/export_image_bins.py
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# (source file, output subdir under spiffs/usr/share/)
JOBS = [
    # Watch-face images -> /watchfaces
    ("components/os/watch_faces/ui/ui_img_137261120.c", "watchfaces"),
    ("components/os/watch_faces/ui/ui_img_charge_png.c", "watchfaces"),
    ("components/os/watch_faces/flux_digits.c", "watchfaces"),
    # Activity app icons -> /activity
    ("components/apps/activity/ui_img_weekly_summary_png.c", "activity"),
    ("components/apps/activity/ui_img_workload_png.c", "activity"),
    ("components/apps/activity/ui_img_share_png.c", "activity"),
    ("components/apps/activity/ui_img_award_png.c", "activity"),
    ("components/apps/activity/ui_img_exercise_symbol_png.c", "activity"),
    ("components/apps/activity/ui_img_move_symbol_png.c", "activity"),
    # Shutdown screen power icon (I8 60x61) -> /shutdown
    ("tmp/shutdown/ui_img_power_icon_png.c", "shutdown"),
]
OUT_ROOT = os.path.join(ROOT, "spiffs", "usr", "share")

# Matches: [static ]const [<attrs...>] uint8_t <name>_data[] = { ... };  OR
#          <name>_map[] = { ... };   (SquareLine uses _map for some icons)
# <attrs> can be LV_ATTRIBUTE_MEM_ALIGN, LV_ATTRIBUTE_LARGE_CONST and/or a
# per-file LV_ATTRIBUTE_IMAGE_* macro.
ARRAY_RE = re.compile(
    r"(?:const|static)\s+(?:(?:const|static|LV_ATTRIBUTE_[A-Z0-9_]+)\s+)*"
    r"uint8_t\s+(\w+)_(?:data|map)\[\]\s*=\s*\{(.*?)\};",
    re.S,
)
BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})")
COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)


def main() -> int:
    total = 0
    for rel, sub in JOBS:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print(f"!! missing: {path}")
            continue
        out_dir = os.path.join(OUT_ROOT, sub)
        os.makedirs(out_dir, exist_ok=True)
        text = open(path, "r", encoding="utf-8", errors="replace").read()
        found = 0
        for m in ARRAY_RE.finditer(text):
            name = m.group(1)
            body = COMMENT_RE.sub("", m.group(2))
            raw = bytes(int(b, 16) for b in BYTE_RE.findall(body))
            out = os.path.join(out_dir, name + ".bin")
            with open(out, "wb") as f:
                f.write(raw)
            print(f"{name:26s} {len(raw):>9d} B  ->  {os.path.relpath(out, ROOT)}")
            total += len(raw)
            found += 1
        if not found:
            print(f"!! no arrays found in {os.path.basename(path)}")
    print(f"\nTotal: {total} bytes  ({total / 1024:.1f} KB) -> {OUT_ROOT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
