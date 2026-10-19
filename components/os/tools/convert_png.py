# convert_png.py - Convert PNG files to LVGL C arrays (lv_image_dsc_t)
# Run: python convert_png.py
#
# Place your PNG icon files (64x64 recommended) in a folder,
# then set PNG_DIR below to that folder.  Output will be
# png_images.h and png_images.c in the output directory.
#
# Once generated, add png_images.c to CMakeLists.txt and
# #include "png_images.h" in honeycomb_menu.c.  Then replace
# the placeholder icon generation with the real image descriptors.

import os
import sys

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# ── Configuration ─────────────────────────────────────────────────────
# Directory containing PNG icon files
PNG_DIR = os.path.join(_SCRIPT_DIR, "..", "app_icon")

# Output paths (adjust for your project layout)
OUTPUT_H = os.path.join(_SCRIPT_DIR, "..", "png_images.h")
OUTPUT_C = os.path.join(_SCRIPT_DIR, "..", "png_images.c")

# ── Script ─────────────────────────────────────────────────────────────

png_files = sorted([f for f in os.listdir(PNG_DIR) if f.endswith(".png")])

if not png_files:
    print(f"ERROR: No .png files found in {PNG_DIR}")
    sys.exit(1)

with open(OUTPUT_H, "w") as h, open(OUTPUT_C, "w") as c:
    h.write("""#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

""")
    c.write('#include "png_images.h"\n\n')

    for png in png_files:
        path = os.path.join(PNG_DIR, png)
        with open(path, "rb") as f:
            data = f.read()

        # Create valid C identifier from filename
        var_name = png.replace(".png", "").replace("-", "_").replace(" ", "_")
        data_name = f"{var_name}_data"

        # Write raw PNG byte array
        c.write(f"/* {png} - {len(data)} bytes */\n")
        c.write(f"static const uint8_t {data_name}[] = {{\n  ")
        for i, b in enumerate(data):
            c.write(f"0x{b:02X},")
            if (i + 1) % 16 == 0 and i + 1 < len(data):
                c.write("\n  ")
        c.write("\n};\n\n")

        # Write lv_image_dsc_t in LV_COLOR_FORMAT_RAW format
        # (requires LVGL PNG decoder enabled in lv_conf.h:
        #  LV_USE_PNG must be 1, and lodepng or libpng linked)
        c.write(f"const lv_image_dsc_t {var_name} = {{\n")
        c.write(f"  .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        c.write(f"  .header.cf = LV_COLOR_FORMAT_RAW,\n")
        c.write(f"  .header.w = 0,\n")
        c.write(f"  .header.h = 0,\n")
        c.write(f"  .data_size = sizeof({data_name}),\n")
        c.write(f"  .data = {data_name},\n")
        c.write(f"}};\n\n")

        # Write extern declaration in header
        h.write(f"extern const lv_image_dsc_t {var_name};\n")

    h.write("""
#ifdef __cplusplus
}
#endif
""")

print(f"Generated {OUTPUT_H} and {OUTPUT_C} with {len(png_files)} images")
for p in png_files:
    print(f"  {p}")
print("\nIMPORTANT: Ensure LV_USE_PNG=1 in lv_conf.h for LV_COLOR_FORMAT_RAW decoding!")

for p in png_files:
    print(f"  {p}")

if not PRESCALE_MODE:
    print("\nIMPORTANT: Ensure LV_USE_PNG=1 in lv_conf.h for LV_COLOR_FORMAT_RAW decoding!")
    print("For pre-scaled 96×96 RGB565 (faster):  python convert_png.py --prescale")
